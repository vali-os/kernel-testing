// Exercise the real local registry and SHM destructor, including callbacks
// that grow the registry while the final owner is reserved for release.
#include "../librt/libos/handles.c"
#include <stdlib.h>
static void __TestShmFree(void* memory);
#define free __TestShmFree
#include "../librt/libos/shm.c"
#undef free
#include "../librt/libds/hashtable.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

const OSHandleOps_t g_osFileOps = { 0 };
const OSHandleOps_t g_eventOps = { 0 };
const OSHandleOps_t g_hqueueOps = { 0 };
const OSHandleOps_t g_socketOps = { 0 };
static int g_locked;
static int g_detaches;
static int g_drops;
static int g_warnings;
static int g_failDetach;
static int g_reenter;
static OSHandle_t* g_owner;
static OSHandle_t g_extra[128];

static void
__TestShmFree(
    _In_ void* memory)
{
    struct __LocalHandle key = { .OSHandle = {
        .ID = 42, .Type = OSHANDLE_SHM, .Payload = memory
    } };

    CHECK(!g_locked);
    // The payload address cannot become reusable while its old key remains.
    CHECK(hashtable_get(&g_osHandles, &key) == NULL);
    free(memory);
}

int*
__errno(
    void)
{
    static int error;
    return &error;
}

void*
dsalloc(
    size_t length)
{
    return malloc(length);
}

void
dsfree(
    void* memory)
{
    free(memory);
}

void
spinlock_init(
    spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_acquire(
    spinlock_t* lock)
{
    (void)lock;
    CHECK(!g_locked);
    g_locked = 1;
}

void
spinlock_release(
    spinlock_t* lock)
{
    (void)lock;
    CHECK(g_locked);
    g_locked = 0;
}

SCTYPE
_syscall(
    SCTYPE function,
    SCTYPE arg0,
    SCTYPE arg1,
    SCTYPE arg2,
    SCTYPE arg3,
    SCTYPE arg4)
{
    SHMHandle_t* payload = (void*)(uintptr_t)arg0;
    OSHandle_t found;
    unsigned int i;

    (void)arg1;
    (void)arg2;
    (void)arg3;
    (void)arg4;
    CHECK(!g_locked);
    if (function == 57) {
        CHECK(arg0 == 42);
        ((SHMHandle_t*)(uintptr_t)arg1)->ID = 42;
        return OS_EOK;
    }
    if (function == 0) {
        g_warnings++;
        return OS_EOK;
    }
    if (function == 64) {
        g_drops++;
        return OS_EINCOMPLETE;
    }
    CHECK(function == 61);
    g_detaches++;
    if (g_reenter) {
        g_reenter = 0;
        CHECK(SHMRelease(g_owner) == OS_EBUSY);
        CHECK(OSHandlesAcquire(g_owner) == OS_EBUSY);
        CHECK(OSHandlesFind(g_owner->ID, &found) == OS_EBUSY);
        for (i = 0; i < 128; i++) {
            CHECK(OSHandleWrap(100 + i, OSHANDLE_NULL, NULL, false, &g_extra[i]) == OS_EOK);
        }
    }
    // Simulate successful unmap followed by a failed reference drop. The real
    // kernel stages are covered separately by shm_memory_ownership_test.
    if (g_failDetach) {
        CHECK(payload->Buffer != NULL);
        payload->Buffer = NULL;
        g_failDetach = 0;
        return OS_EUNKNOWN;
    }
    CHECK(payload->Buffer == NULL);
    payload->ID = UUID_INVALID;
    return OS_EOK;
}

static void
__MakeOwner(
    OSHandle_t* owner)
{
    CHECK(SHMAttach(42, owner) == OS_EOK);
}

int
main(
    void)
{
    OSHandle_t owner;
    OSHandle_t alias;
    OSHandle_t found;
    void* payload;
    unsigned int i;
    int calls;

    OSHandlesInitialize();
    __MakeOwner(&owner);
    alias = owner;
    CHECK(OSHandlesAcquire(&alias) == OS_EOK);
    CHECK(SHMRelease(&alias) == OS_EOK && alias.ID == UUID_INVALID && alias.Payload == NULL);
    CHECK(g_detaches == 0);
    CHECK(OSHandlesFind(owner.ID, &found) == OS_EOK && found.Payload == owner.Payload);
    CHECK(SHMRelease(&owner) == OS_EOK && owner.ID == UUID_INVALID && owner.Payload == NULL);
    CHECK(g_detaches == 1 && SHMRelease(&owner) == OS_EOK && g_detaches == 1);

    __MakeOwner(&owner);
    payload = owner.Payload;
    ((SHMHandle_t*)payload)->Buffer = (void*)0x1000;
    g_owner = &owner;
    g_failDetach = 1;
    g_reenter = 1;
    CHECK(SHMRelease(&owner) == OS_EUNKNOWN);
    CHECK(owner.ID == 42 && owner.Payload == payload);
    CHECK(((SHMHandle_t*)payload)->Buffer == NULL);
    CHECK(OSHandlesAcquire(&owner) == OS_EBUSY);
    CHECK(OSHandlesFind(owner.ID, &found) == OS_EBUSY);
    CHECK(SHMRelease(&owner) == OS_EOK && owner.Payload == NULL);
    CHECK(g_detaches == 3 && OSHandlesFind(42, &found) == OS_ENOENT);
    for (i = 0; i < 128; i++) {
        CHECK(OSHandleRelease(&g_extra[i]) == OS_EOK);
    }

    __MakeOwner(&owner);
    ((SHMHandle_t*)owner.Payload)->Buffer = (void*)0x1000;
    g_failDetach = 1;
    OSHandleDestroy(&owner);
    CHECK(owner.ID == 42 && owner.Payload != NULL && g_warnings == 1);
    CHECK(SHMRelease(&owner) == OS_EOK);
    CHECK(OSHandleWrap(43, OSHANDLE_NULL, NULL, true, &owner) == OS_EOK);
    calls = g_detaches;
    CHECK(SHMRelease(&owner) == OS_EINVALPARAMS && g_detaches == calls);
    CHECK(OSHandleRelease(&owner) == OS_EOK && owner.ID == UUID_INVALID && g_drops == 1);
    CHECK(SHMRelease(NULL) == OS_EOK && g_osHandles.element_count == 0);

    // Independent attachments to the same storage must keep separate views
    // and references. Only explicit local aliases share their attachment.
    __MakeOwner(&owner);
    __MakeOwner(&found);
    CHECK(owner.Payload != found.Payload);
    alias = owner;
    CHECK(OSHandlesAcquire(&alias) == OS_EOK);
    CHECK(OSHandlesFind(42, &g_extra[0]) == OS_EEXISTS);
    calls = g_detaches;
    CHECK(SHMRelease(&alias) == OS_EOK && g_detaches == calls);
    ((SHMHandle_t*)owner.Payload)->Buffer = (void*)0x1000;
    g_failDetach = 1;
    CHECK(SHMRelease(&owner) == OS_EUNKNOWN);
    CHECK(OSHandlesAcquire(&owner) == OS_EBUSY);
    alias = found;
    CHECK(OSHandlesAcquire(&alias) == OS_EOK);
    CHECK(SHMRelease(&found) == OS_EOK && alias.ID == 42);
    CHECK(SHMRelease(&owner) == OS_EOK);
    CHECK(OSHandlesFind(42, &found) == OS_EOK && found.Payload == alias.Payload);
    CHECK(SHMRelease(&alias) == OS_EOK && g_detaches == calls + 3);
    CHECK(g_osHandles.element_count == 0);
    hashtable_destroy(&g_osHandles);
    puts("SHM release: aliases, retry, partial cleanup, reentrancy, registry growth and syscall budgets passed");
    return 0;
}
