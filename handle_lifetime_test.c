/** Retain janitor work across real hash-table relocation. No scheduler timing
 * is needed: tests decide when queued destructors run, while using the same
 * handle, hash-table and intrusive queue code as the kernel.
 */
#include "../kernel/handle.c"
#include <stdio.h>
#include <stdlib.h>

static MSContextSecurity_t* g_currentContext;
static unsigned int g_contextLookups;

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    g_contextLookups++;
    return g_currentContext;
}

static unsigned g_destroyed[1024];
static size_t g_live;
static bool g_failRecords, g_failTable;
void *kmalloc(size_t size)
{
    if (g_failRecords)
        return NULL;
    void *p = malloc(size);
    if (p)
        g_live++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        assert(g_live);
        g_live--;
        free(p);
    }
}
void *dsalloc(size_t size)
{
    return g_failTable ? NULL : malloc(size);
}
void dsfree(void *p)
{
    free(p);
}

static void DestroyResource(void *value)
{
    unsigned index = (unsigned)(uintptr_t)value;
    assert(index < 1024 && !g_destroyed[index]);
    g_destroyed[index]++;
}

static void DrainJanitor(void)
{
    element_t *entry;
    while ((entry = queue_pop(&g_cleanQueue)) != NULL)
    {
        __CleanupHandle((struct ResourceHandle *)entry);
    }
}

/** Destructors may allocate more handles before returning to janitor cleanup. */
static void DestroyWithChurn(void *unused)
{
    (void)unused;
    uuid_t temporary[128];
    for (unsigned i = 0; i < 128; ++i)
    {
        temporary[i] = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
        assert(temporary[i] != UUID_INVALID);
    }
    for (unsigned i = 0; i < 128; ++i)
        assert(DestroyHandle(temporary[i]) == OS_EOK);
}

static unsigned int g_exitCalls;
static unsigned int g_globalCleanupCalls;

static void
__TestGlobalCleanup(
    _In_ void* resource)
{
    assert(resource == (void*)42);
    assert(!g_handlesLock.held);
    g_globalCleanupCalls++;
}

static void
__TestGlobalHandles(void)
{
    MSContextSecurity_t    first = { .GlobalHandleID = 1 };
    MSContextSecurity_t    second = { .GlobalHandleID = 2 };
    struct ResourceHandle* record;
    unsigned int          contextLookups;
    uuid_t                handle;
    void*                 resource = NULL;

    first.HandleTable = CreateHandleGrantsTable();
    second.HandleTable = CreateHandleGrantsTable();
    assert(first.HandleTable != NULL && second.HandleTable != NULL);

    g_currentContext = &first;
    contextLookups = g_contextLookups;
    handle = CreateGlobalHandle(HandleTypeSHM, __TestGlobalCleanup, (void*)42);
    assert(handle != UUID_INVALID && g_contextLookups == contextLookups);
    assert(LookupHandle(handle, HandleTypeSHM) == NULL);
    assert(AcquireHandle(handle, HandleTypeSHM, NULL) == OS_ENOENT);
    assert(AcquireHandleReference(handle, HandleTypeSHM, &resource) == OS_ENOENT);

    // A different process can run kernel work without receiving a grant.
    g_currentContext = &second;
    contextLookups = g_contextLookups;
    assert(AcquireGlobalHandle(handle, HandleTypeSHM, &resource) == OS_EOK);
    assert(resource == (void*)42);
    assert(LookupGlobalHandle(handle, HandleTypeSHM) == resource);
    assert(LookupGlobalHandle(handle, HandleTypeThread) == NULL);
    assert(ReleaseGlobalHandle(handle) == OS_EINCOMPLETE);
    assert(g_contextLookups == contextLookups);
    assert(DestroyHandle(handle) == OS_ENOENT);

    // Failed checks must not add a reference or replace the output pointer.
    assert(AcquireGlobalHandle(handle, HandleTypeThread, &resource) == OS_EUNKNOWN);
    assert(AcquireGlobalHandle(UUID_INVALID, HandleTypeSHM, &resource) == OS_ENOENT);
    record = __LookupGlobalSafe(handle);
    assert(record->References == 1 && resource == (void*)42);
    record->References = INT_MAX;
    assert(AcquireGlobalHandle(handle, HandleTypeSHM, &resource) == OS_EOVERFLOW);
    assert(record->References == INT_MAX && resource == (void*)42);
    record->References = 1;

    // Process teardown must not release the independent kernel reference.
    DestroyHandleGrantsTable(first.HandleTable);
    DestroyHandleGrantsTable(second.HandleTable);
    g_currentContext = NULL;
    DrainJanitor();
    assert(g_globalCleanupCalls == 0);
    assert(LookupGlobalHandle(handle, HandleTypeSHM) == resource);
    assert(AcquireGlobalHandle(handle, HandleTypeSHM, NULL) == OS_EOK);
    assert(ReleaseGlobalHandle(handle) == OS_EINCOMPLETE);
    assert(ReleaseGlobalHandle(handle) == OS_EOK);
    assert(g_globalCleanupCalls == 0);
    assert(LookupGlobalHandle(handle, HandleTypeSHM) == NULL);
    assert(AcquireGlobalHandle(handle, HandleTypeSHM, NULL) == OS_ENOENT);
    assert(ReleaseGlobalHandle(handle) == OS_ENOENT);
    DrainJanitor();
    assert(g_globalCleanupCalls == 1);
    assert(g_handles.element_count == 0 && g_live == 0);

    g_failRecords = true;
    assert(CreateGlobalHandle(HandleTypeSHM, __TestGlobalCleanup, (void*)42) == UUID_INVALID);
    g_failRecords = false;
    assert(g_globalCleanupCalls == 1);
    assert(g_handles.element_count == 0 && g_live == 0);
}

static void
__TestContextExit(
    _In_ void* resource)
{
    (void)resource;
    assert(!g_currentContext->HandleTable->Lock.held);
    assert(!g_handlesLock.held);
    g_exitCalls++;
}

static void
__TestGrants(void)
{
    MSContextSecurity_t first = { .GlobalHandleID = 1 };
    MSContextSecurity_t second = { .GlobalHandleID = 2 };
    struct ResourceHandle* global;
    uuid_t handle;
    uuid_t local;
    void* resource;
    size_t live;
    size_t capacity;
    uuid_t id;

    g_failTable = true;
    assert(CreateHandleGrantsTable() == NULL);
    g_failTable = false;
    first.HandleTable = CreateHandleGrantsTable();
    second.HandleTable = CreateHandleGrantsTable();
    assert(first.HandleTable != NULL && second.HandleTable != NULL);
    g_currentContext = &first;
    handle = CreateHandle(HandleTypeSHM, NULL, NULL, (void*)42);
    assert(handle != UUID_INVALID);
    assert(AcquireHandle(handle, HandleTypeSHM, &resource) == OS_EOK && resource == (void*)42);
    assert(GrantHandle(&second, handle) == OS_EOK);
    global = __LookupGlobalSafe(handle);
    assert(global->References == 2);
    g_currentContext = &second;
    assert(AcquireHandle(handle, HandleTypeSHM, NULL) == OS_EOK);
    g_currentContext = &first;
    assert(GrantHandle(&second, handle) == OS_EOK);
    assert(GrantHandle(&first, handle) == OS_EOK);
    assert(global->References == 2);
    assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    assert(AcquireHandleReference(handle, HandleTypeSHM, &resource) == OS_EOK);
    assert(global->References == 3);

    g_currentContext = &second;
    assert(AcquireGlobalHandle(handle, HandleTypeSHM, NULL) == OS_EOK);
    assert(ReleaseGlobalHandle(handle) == OS_EINCOMPLETE);
    assert(global->References == 3);
    // A duplicate grant must preserve the outstanding destination reference.
    assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    assert(AcquireHandle(handle, HandleTypeThread, NULL) == OS_EUNKNOWN);
    assert(AcquireHandle(handle, HandleTypeSHM, &resource) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    // Teardown must drop the specified table, not the caller's matching grant.
    DestroyHandleGrantsTable(first.HandleTable);
    assert(global->References == 2);
    assert(LookupHandle(handle, HandleTypeSHM) == (void*)42);
    assert(DestroyHandle(handle) == OS_EOK);
    assert(global->References == 1);
    assert(LookupHandle(handle, HandleTypeSHM) == NULL);
    assert(ReleaseGlobalHandle(handle) == OS_EOK);
    DrainJanitor();

    local = CreateHandle(HandleTypeSHMAccess, __TestContextExit, NULL, NULL);
    assert(local != UUID_INVALID);
    assert(GrantHandle(&second, local) == OS_EPERMISSIONS);
    assert(AcquireHandleReference(local, HandleTypeSHMAccess, &resource) == OS_EPERMISSIONS);
    assert(AcquireGlobalHandle(local, HandleTypeSHMAccess, &resource) == OS_EPERMISSIONS);
    assert(AcquireHandle(local, HandleTypeSHMAccess, NULL) == OS_EOK);
    assert(DestroyHandle(local) == OS_EINCOMPLETE);
    assert(g_exitCalls == 0);
    assert(DestroyHandle(local) == OS_EOK);
    assert(g_exitCalls == 1);
    DrainJanitor();

    // Killed async calls can leave extra grant-local references behind.
    local = CreateHandle(HandleTypeSHMAccess, __TestContextExit, NULL, NULL);
    assert(AcquireHandle(local, HandleTypeSHMAccess, NULL) == OS_EOK);
    assert(AcquireHandle(local, HandleTypeSHMAccess, NULL) == OS_EOK);
    DestroyHandleGrantsTable(second.HandleTable);
    assert(g_exitCalls == 2);
    g_currentContext = NULL;
    DrainJanitor();
    assert(g_handles.element_count == 0 && g_live == 0);

    // Grant allocation failure must undo the already inserted global entry.
    first.HandleTable = CreateHandleGrantsTable();
    g_currentContext = &first;
    while (first.HandleTable->Table.element_count < first.HandleTable->Table.grow_count) {
        handle = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
        assert(handle != UUID_INVALID);
    }
    capacity = first.HandleTable->Table.capacity;
    assert(AcquireHandle(handle, HandleTypeGeneric, NULL) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    assert(first.HandleTable->Table.capacity == capacity);
    // Give the global table spare capacity without growing the grant table.
    g_currentContext = NULL;
    while (g_handles.capacity <= first.HandleTable->Table.capacity) {
        assert(CreateHandle(HandleTypeGeneric, NULL, NULL, NULL) != UUID_INVALID);
    }
    g_currentContext = &first;
    live = g_live;
    g_failTable = true;
    assert(CreateHandle(HandleTypeGeneric, NULL, NULL, NULL) == UUID_INVALID);
    assert(g_live == live);
    g_failTable = false;
    DestroyHandleGrantsTable(first.HandleTable);
    g_currentContext = NULL;
    DrainJanitor();
    // Remove the kernel-only handles created to grow the global table.
    for (id = 1; id < atomic_load(&g_nextHandleId); id++) {
        if (__LookupGlobalSafe(id) != NULL) {
            assert(DestroyHandle(id) == OS_EOK);
        }
    }
    DrainJanitor();
    assert(g_handles.element_count == 0 && g_live == 0);
}

#ifdef TEST_CPU_SESSIONS
#include "cpu_session_test.inc"
#endif

int main(void)
{
    uuid_t handles[512];
    assert(InitializeHandles() == OS_EOK);
    handles[0] = CreateHandle(HandleTypeThread, NULL, DestroyResource, (void *)(uintptr_t)1);
    assert(handles[0] != UUID_INVALID);
    assert(DestroyHandle(handles[0]) == OS_EOK);
    // An acquired record must survive table movement even outside the lock.
    uuid_t pinnedId = CreateHandle(HandleTypeGeneric, NULL, NULL, (void *)(uintptr_t)1000);
    struct ResourceHandle *pinned = __AcquireHandle(pinnedId);
    assert(pinned != NULL);
    size_t originalCapacity = g_handles.capacity;
    // Rehash the table while the janitor still owns its first queue entry.
    for (unsigned i = 1; i < 512; ++i)
    {
        handles[i] = CreateHandle(HandleTypeSHM, NULL, DestroyResource, (void *)(uintptr_t)(i + 1));
        assert(handles[i] != UUID_INVALID);
    }
    assert(g_handles.capacity > originalCapacity);
    assert(pinned->ID == pinnedId && pinned->Resource == (void *)(uintptr_t)1000);
    assert(DestroyHandle(pinnedId) == OS_EINCOMPLETE);
    assert(DestroyHandle(pinnedId) == OS_EOK);
    DrainJanitor();
    assert(g_destroyed[1] == 1);
    for (unsigned i = 1; i < 512; ++i)
    {
        assert(!g_destroyed[i + 1]);
        assert(LookupHandle(handles[i], HandleTypeSHM) == (void *)(uintptr_t)(i + 1));
        assert(DestroyHandle(handles[i]) == OS_EOK);
        assert(DestroyHandle(handles[i]) == OS_ENOENT);
    }
    // A failed optional table shrink must not retain dangling index entries.
    g_failTable = true;
    DrainJanitor();
    g_failTable = false;
    for (unsigned i = 1; i <= 512; ++i)
        assert(g_destroyed[i] == 1);
    assert(g_handles.element_count == 0 && !g_live);
    uuid_t reentrant = CreateHandle(HandleTypeGeneric, NULL, DestroyWithChurn, NULL);
    assert(DestroyHandle(reentrant) == OS_EOK);
    DrainJanitor();
    assert(g_handles.element_count == 0 && !g_live);
    g_failRecords = true;
    assert(CreateHandle(HandleTypeGeneric, NULL, NULL, NULL) == UUID_INVALID);
    g_failRecords = false;
    // Fill precisely to the next growth boundary, then fail its allocation.
    size_t count = g_handles.grow_count;
    uuid_t *growth = malloc(count * sizeof(*growth));
    assert(growth);
    for (size_t i = 0; i < count; ++i)
    {
        growth[i] = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
        assert(growth[i] != UUID_INVALID);
    }
    g_failTable = true;
    assert(CreateHandle(HandleTypeGeneric, NULL, NULL, NULL) == UUID_INVALID);
    assert(g_live == count && g_handles.element_count == count);
    g_failTable = false;
    for (size_t i = 0; i < count; ++i)
        assert(DestroyHandle(growth[i]) == OS_EOK);
    DrainJanitor();
    assert(g_handles.element_count == 0 && !g_live);
    free(growth);
    __TestGlobalHandles();
    __TestGrants();
#ifdef TEST_CPU_SESSIONS
    __TestCpuSessions();
#endif
    handles[0] = CreateHandle(HandleTypeGeneric, NULL, NULL, (void*)42);
    atomic_store(&g_nextHandleId, UINT32_MAX);
    assert(AllocateHandleId() == UINT32_MAX);
    assert(AllocateHandleId() == UUID_INVALID);
    assert(LookupHandle(handles[0], HandleTypeGeneric) == (void*)42);
    assert(DestroyHandle(handles[0]) == OS_EOK);
    DrainJanitor();
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("Handle lifetime: global references, grant isolation and pending cleanup passed");
    return 0;
}
