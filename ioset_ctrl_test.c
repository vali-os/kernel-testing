// Exercise ioset_ctrl event validation with the target headers. The kernel
// accepts a NULL event for IOSET_DEL, so the C library must as well. IOSET_MOD
// and IOSET_DEL must find a descriptor anywhere in the set, not only first.
// IOSET_MOD replaces the events and the data, as the kernel does, and a call
// the kernel refuses leaves the C library's copy of the set unchanged.
#ifndef IOSET_SOURCE
#define IOSET_SOURCE "../librt/libc/compat/ioset.c"
#endif
#include IOSET_SOURCE
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define SET_DESCRIPTOR 3
#define IO_DESCRIPTOR  4
// Descriptors IO_DESCRIPTOR up to IO_DESCRIPTOR + IO_OBJECT_COUNT - 1 are
// valid I/O descriptors. The tests that need several use all of them.
#define IO_OBJECT_COUNT 4

static stdio_handle_t      g_setObject;
static stdio_handle_t      g_ioObjects[IO_OBJECT_COUNT];
static struct IOSet        g_set;
static unsigned int        g_kernelCalls;
static int                 g_kernelOperation;
static OSHandle_t*         g_kernelHandle;
static struct ioset_event* g_kernelEvent;
static oserr_t             g_kernelResult = OS_EOK;

int*
__errno(void)
{
    static int error;

    return &error;
}

stdio_handle_t*
stdio_handle_get(
    _In_ int iod)
{
    if (iod == SET_DESCRIPTOR) {
        return &g_setObject;
    }
    if (iod >= IO_DESCRIPTOR && iod < IO_DESCRIPTOR + IO_OBJECT_COUNT) {
        return &g_ioObjects[iod - IO_DESCRIPTOR];
    }
    return NULL;
}

unsigned int
stdio_handle_signature(
    _In_ stdio_handle_t* handle)
{
    return handle->Signature;
}

// Models the kernel's ControlHandleSet. It needs the event for adding and
// modifying, and ignores it for removal.
oserr_t
OSNotificationQueueCtrl(
    _In_ OSHandle_t*         setHandle,
    _In_ int                 operation,
    _In_ OSHandle_t*         handle,
    _In_ struct ioset_event* event)
{
    CHECK(setHandle == &g_setObject.OSHandle);
    CHECK(handle >= &g_ioObjects[0].OSHandle && handle <= &g_ioObjects[IO_OBJECT_COUNT - 1].OSHandle);
    g_kernelCalls++;
    g_kernelOperation = operation;
    g_kernelHandle = handle;
    g_kernelEvent = event;
    if (operation != IOSET_DEL && event == NULL) {
        return OS_EINVALPARAMS;
    }
    return g_kernelResult;
}

int
OsErrToErrNo(
    _In_ oserr_t status)
{
    if (status == OS_EOK) {
        return 0;
    }
    errno = status == OS_ENOENT ? ENOENT : EINVAL;
    return -1;
}

// Returns the set entry for iod and checks that it is at position index.
static struct IOSetEntry*
__EntryAt(
    _In_ int index,
    _In_ int iod)
{
    struct IOSetEntry* entry = g_set.entries;

    for (int i = 0; i < index && entry != NULL; i++) {
        entry = entry->link;
    }
    CHECK(entry != NULL && entry->iod == iod);
    return entry;
}

// IOSET_MOD and IOSET_DEL look the descriptor up in the set before calling the
// kernel. The lookup must walk the whole list. It used to check only the first
// entry and then loop on it forever, so a change to any later entry hung.
static void
__TestEntryLookup(void)
{
    struct ioset_event added[3];
    struct ioset_event changed[3];
    struct ioset_event missing = { .events = IOSETIN, .data.iod = IO_DESCRIPTOR + 3 };
    unsigned int       calls;
    int                i;
    int                j;

    CHECK(g_set.entries == NULL);
    for (i = 0; i < 3; i++) {
        added[i] = (struct ioset_event) { .events = IOSETIN, .data.iod = IO_DESCRIPTOR + i };
        CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR + i, &added[i]) == 0);
    }

    // Change the first, the middle and the last entry. Each change reaches
    // the kernel for the right descriptor and touches only that entry.
    for (i = 0; i < 3; i++) {
        changed[i] = (struct ioset_event) { .events = IOSETOUT | IOSETLVT, .data.iod = 100 + i };
        calls = g_kernelCalls;
        CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_MOD, IO_DESCRIPTOR + i, &changed[i]) == 0);
        CHECK(g_kernelCalls == calls + 1 && g_kernelOperation == IOSET_MOD);
        CHECK(g_kernelHandle == &g_ioObjects[i].OSHandle && g_kernelEvent == &changed[i]);
        for (j = 0; j < 3; j++) {
            struct ioset_event* expected = j <= i ? &changed[j] : &added[j];

            CHECK(__EntryAt(j, IO_DESCRIPTOR + j)->event.events == expected->events);
            CHECK(__EntryAt(j, IO_DESCRIPTOR + j)->event.data.iod == expected->data.iod);
        }
    }

    // A valid descriptor that is not in the set is not found, and the kernel
    // is not asked.
    errno = 0;
    calls = g_kernelCalls;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_MOD, IO_DESCRIPTOR + 3, &missing) == -1);
    CHECK(errno == ENOENT && g_kernelCalls == calls);
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR + 3, NULL) == -1);
    CHECK(errno == ENOENT && g_kernelCalls == calls);

    // Removal finds the middle, then the last, then the first entry, and keeps
    // the order of the others.
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR + 1, NULL) == 0);
    CHECK(g_kernelHandle == &g_ioObjects[1].OSHandle);
    (void)__EntryAt(0, IO_DESCRIPTOR);
    CHECK(__EntryAt(1, IO_DESCRIPTOR + 2)->link == NULL);
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR + 2, NULL) == 0);
    CHECK(g_kernelHandle == &g_ioObjects[2].OSHandle);
    CHECK(__EntryAt(0, IO_DESCRIPTOR)->link == NULL);
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == 0);
    CHECK(g_kernelHandle == &g_ioObjects[0].OSHandle && g_set.entries == NULL);
    puts("ioset_ctrl: IOSET_MOD and IOSET_DEL on first, middle, last and missing entries passed");
}

// The kernel is asked first. When it refuses, our copy of the set must not
// change, or ioset_wait would report level-triggered events the kernel does
// not have, with data it does not have.
static void
__TestKernelRefusal(void)
{
    struct ioset_event added = { .events = IOSETIN | IOSETLVT, .data.iod = 200 };
    struct ioset_event changed = { .events = IOSETOUT, .data.iod = 201 };
    struct IOSetEntry* entry;

    CHECK(g_set.entries == NULL);

    // A refused add leaves no entry behind.
    g_kernelResult = OS_EINVALPARAMS;
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR, &added) == -1);
    CHECK(errno == EINVAL && g_set.entries == NULL);

    // A refused change keeps the old events and data.
    g_kernelResult = OS_EOK;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR, &added) == 0);
    entry = __EntryAt(0, IO_DESCRIPTOR);
    g_kernelResult = OS_EINVALPARAMS;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_MOD, IO_DESCRIPTOR, &changed) == -1);
    CHECK(entry->event.events == added.events && entry->event.data.iod == added.data.iod);

    // A refused removal keeps the entry.
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == -1);
    CHECK(__EntryAt(0, IO_DESCRIPTOR) == entry);

    // When the kernel already dropped the subscription, the removal reports
    // ENOENT and our stale entry goes too.
    g_kernelResult = OS_ENOENT;
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == ENOENT && g_set.entries == NULL);
    g_kernelResult = OS_EOK;
    puts("ioset_ctrl: IOSET_MOD data and refused kernel calls passed");
}

int
main(void)
{
    struct ioset_event event = { .events = IOSETSYN, .data.iod = IO_DESCRIPTOR };
    unsigned int       calls;
    int                i;

    g_setObject.Signature = IOSET_SIGNATURE;
    g_setObject.OpsContext = &g_set;
    for (i = 0; i < IO_OBJECT_COUNT; i++) {
        g_ioObjects[i].Signature = EVENT_SIGNATURE;
    }

    // Adding needs the event. A NULL event is rejected before the set or the
    // kernel is touched.
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == EINVAL && g_kernelCalls == 0 && g_set.entries == NULL);

    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR, &event) == 0);
    CHECK(g_kernelCalls == 1 && g_kernelOperation == IOSET_ADD && g_kernelEvent == &event);
    CHECK(g_set.entries != NULL && g_set.entries->iod == IO_DESCRIPTOR);
    CHECK(g_set.entries->event.events == IOSETSYN);

    // Modifying needs the event too.
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_MOD, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == EINVAL && g_kernelCalls == 1);
    CHECK(g_set.entries->event.events == IOSETSYN);

    // An unknown operation stays invalid, with or without an event.
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, 0, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == EINVAL && g_kernelCalls == 1);

    // Removal does not use the event. NULL succeeds, takes the entry out of
    // the set and is passed on to the kernel as NULL.
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == 0);
    CHECK(g_kernelCalls == 2 && g_kernelOperation == IOSET_DEL && g_kernelEvent == NULL);
    CHECK(g_set.entries == NULL);

    // A second removal finds nothing, and the kernel is not asked.
    errno = 0;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == ENOENT && g_kernelCalls == 2);

    // Removal with an event still works, as callers written for the older
    // behaviour pass one.
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_ADD, IO_DESCRIPTOR, &event) == 0);
    calls = g_kernelCalls;
    CHECK(ioset_ctrl(SET_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, &event) == 0);
    CHECK(g_kernelCalls == calls + 1 && g_kernelEvent == &event && g_set.entries == NULL);

    // A descriptor that is not a set is rejected, also on removal.
    errno = 0;
    CHECK(ioset_ctrl(IO_DESCRIPTOR, IOSET_DEL, IO_DESCRIPTOR, NULL) == -1);
    CHECK(errno == EBADF);

    __TestEntryLookup();
    __TestKernelRefusal();
    puts("ioset_ctrl: NULL event accepted for removal, rejected for add and modify passed");
    return 0;
}
