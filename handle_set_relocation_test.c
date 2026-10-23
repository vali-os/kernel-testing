// Exercise the production handle-set code and containers with a competing
// table mutation scheduled at the first release of the registry lock.
void _ReadWriteBarrier(void);
#include "../kernel/handle_set.c"
#include "../librt/libds/list.c"
#include "../librt/libds/hashtable.c"
#include "../librt/libds/rbtree.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

enum Mutation {
    MutationNone,
    MutationGrow,
    MutationInsert,
    MutationRemove
};

static struct handle_set g_set;
static enum Mutation    g_mutation;
static uuid_t           g_mutationHandle;
static void*            g_retired;
static size_t           g_retiredSize;
static int              g_failAllocation;
static int              g_liveElements;
static int              g_wakes;
static int              g_errno;

int*
__errno(void)
{
    return &g_errno;
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
}

void*
kmalloc(
    _In_ size_t size)
{
    void* allocation = calloc(1, size);
    if (allocation) {
        g_liveElements++;
    }
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    g_liveElements--;
    free(allocation);
}

void*
dsalloc(
    _In_ size_t size)
{
    return g_failAllocation ? NULL : malloc(size);
}

void
dsfree(
    _In_ void* allocation)
{
    // Keep relocated storage readable so a stale lock access fails explicitly.
    if (allocation != g_retired) {
        free(allocation);
    }
}

void
SpinlockConstruct(
    _In_ Spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

void
SpinlockAcquireIrq(
    _In_ Spinlock_t* lock)
{
    uintptr_t address = (uintptr_t)lock;
    uintptr_t retired = (uintptr_t)g_retired;

    CHECK(!g_retired || address < retired || address >= retired + g_retiredSize);
    CHECK(atomic_load(&lock->Current) == atomic_load(&lock->Next));
    atomic_fetch_add(&lock->Next, 1);
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* lock)
{
    enum Mutation mutation;
    struct handle_sets entry = { .id = g_mutationHandle, .sets = LIST_INIT };

    CHECK(atomic_load(&lock->Current) + 1 == atomic_load(&lock->Next));
    atomic_fetch_add(&lock->Current, 1);
    if (lock != &g_handleSetsLock || g_mutation == MutationNone) {
        return;
    }

    mutation = g_mutation;
    g_mutation = MutationNone;
    SpinlockAcquireIrq(&g_handleSetsLock);
    if (mutation == MutationGrow) {
        g_retired = g_handleSets.elements;
        g_retiredSize = g_handleSets.capacity * g_handleSets.element_size;
        CHECK(hashtable_resize(&g_handleSets, g_handleSets.capacity * 2) == 0);
    } else if (mutation == MutationInsert) {
        hashtable_set(&g_handleSets, &entry);
        CHECK(hashtable_get(&g_handleSets, &entry) != NULL);
    } else {
        CHECK(hashtable_remove(&g_handleSets, &entry) != NULL);
    }
    SpinlockReleaseIrq(&g_handleSetsLock);
}

void*
LookupHandleOfType(
    _In_ uuid_t       handle,
    _In_ HandleType_t type)
{
    CHECK(type == HandleTypeSet);
    return &g_set;
}

uuid_t
CreateHandle(
    _In_ HandleType_t       type,
    _In_ HandleDestructorFn destroy,
    _In_ void*              resource)
{
    abort();
}

oserr_t
FutexWait(
    _In_ OSAsyncContext_t* asyncContext,
    _In_ _Atomic(int)*    futex,
    _In_ int              value,
    _In_ int              flags,
    _In_ _Atomic(int)*    futex2,
    _In_ int              count,
    _In_ int              operation,
    _In_ OSTimestamp_t*   deadline)
{
    abort();
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* futex,
    _In_ int           count,
    _In_ int           flags)
{
    CHECK(futex == &g_set.events_pending && atomic_load(futex) > 0);
    g_wakes++;
    return OS_EOK;
}

static void
__ReleaseRetiredStorage(void)
{
    free(g_retired);
    g_retired = NULL;
    g_retiredSize = 0;
}

static void
__Add(
    _In_ uuid_t handle)
{
    struct ioset_event event = { .events = IOSETIN };

    event.data.context = (void*)(uintptr_t)handle;
    CHECK(ControlHandleSet(1, IOSET_ADD, handle, &event) == OS_EOK);
    CHECK(g_mutation == MutationNone);
    __ReleaseRetiredStorage();
}

static void
__MarkAndDrain(
    _In_ uuid_t handle)
{
    struct ioset_event event;
    int               count;
    int               wakes = g_wakes;

    CHECK(MarkHandle(handle, IOSETIN) == OS_EOK);
    CHECK(g_mutation == MutationNone && g_wakes == wakes + 1);
    CHECK(WaitForHandleSet(1, NULL, &event, 1, 0, NULL, &count) == OS_EOK);
    CHECK(count == 1 && event.events == IOSETIN);
    CHECK(event.data.context == (void*)(uintptr_t)handle);
    CHECK(list_count(&g_set.events) == 0 && !atomic_load(&g_set.events_pending));
    __ReleaseRetiredStorage();
}

static void
__Remove(
    _In_ uuid_t handle)
{
    CHECK(ControlHandleSet(1, IOSET_DEL, handle, NULL) == OS_EOK);
    CHECK(g_mutation == MutationNone);
    CHECK(MarkHandle(handle, IOSETIN) == OS_ENOENT);
    __ReleaseRetiredStorage();
}

int
main(void)
{
    struct ioset_event event = { .events = IOSETIN };
    struct handle_sets entry;
    void*              oldEntry;
    size_t             capacity;
    int                liveElements;
    uuid_t             handle;

    CHECK(HandleSetsInitialize() == OS_EOK);
    list_construct(&g_set.events);
    rb_tree_construct(&g_set.handles);

    // Each operation must finish using its bucket before a competing resize.
    g_mutation = MutationGrow;
    __Add(2);
    g_mutation = MutationGrow;
    __MarkAndDrain(2);
    g_mutation = MutationGrow;
    __Remove(2);

    // Robin Hood insertion moves an entry without reallocating the table.
    capacity = g_handleSets.capacity;
    __Add(1);
    __Add(2);
    entry = (struct handle_sets){ .id = 2 };
    oldEntry = hashtable_get(&g_handleSets, &entry);
    g_mutationHandle = (uuid_t)capacity + 1;
    g_mutation = MutationInsert;
    __MarkAndDrain(2);
    CHECK(hashtable_get(&g_handleSets, &entry) != oldEntry);

    // Removing that collision shifts the marked entry back into its old slot.
    g_mutation = MutationRemove;
    __MarkAndDrain(2);
    CHECK(hashtable_get(&g_handleSets, &entry) == oldEntry);
    __Remove(1);
    __Remove(2);

    // Failed table growth must release the lock and the unpublished element.
    for (handle = 1; g_handleSets.element_count < g_handleSets.grow_count; handle++) {
        __Add(handle);
    }
    liveElements = g_liveElements;
    g_failAllocation = 1;
    CHECK(ControlHandleSet(1, IOSET_ADD, handle, &event) == OS_EOOM);
    CHECK(g_liveElements == liveElements);
    CHECK(MarkHandle(handle, IOSETIN) == OS_ENOENT);
    g_failAllocation = 0;
    __Add(handle);
    __MarkAndDrain(handle);

    // Duplicate registration also rolls back without losing the original.
    CHECK(ControlHandleSet(1, IOSET_ADD, handle, &event) == OS_EINVALPARAMS);
    CHECK(g_liveElements == liveElements + 1);
    __MarkAndDrain(handle);
    while (handle) {
        __Remove(handle--);
    }
    CHECK(g_liveElements == 0 && g_handleSets.element_count == 0);
    hashtable_destroy(&g_handleSets);
    puts("Handle sets: relocation during add/mark/remove, collision/backshift, OOM and rollback passed");
    return 0;
}
