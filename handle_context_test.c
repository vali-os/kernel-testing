// Exercise the real context lookup and handle code together, without a scheduler.
#define CreateGlobalHandle __CreateGlobalHandleForTest
#include "../kernel/handle.c"
#include "handle_set_stubs.h"
#undef CreateGlobalHandle
#include "../kernel/memory/space/private.h"
#include <arch/x86/memory.h>
#include <machine.h>
#include <stdio.h>
#include <stdlib.h>

#undef __MODULE
#define THREAD_GET(id) ((Thread_t*)LookupHandle(id, HandleTypeThread))
#include "../kernel/api/memory.c"
#include "../kernel/sync/userevent.c"

/** Check the published context before another group could resolve its ID. */
uuid_t
CreateGlobalHandle(
    _In_ HandleType_t type,
    _In_ HandleDestructorFn cleanup,
    _In_ void* resource)
{
    struct MSContext* context = resource;

    if (type == HandleTypeMSContext && cleanup != NULL) {
        assert(context->Security.HandleTable != NULL);
        assert(context->Security.GlobalHandleID == UUID_INVALID);
        assert(context->Security.Capabilities == 0);
        assert(context->SignalHandler == 0);
        assert(context->Allocations.count == 0);
    }
    return __CreateGlobalHandleForTest(type, cleanup, resource);
}

static size_t g_copyLimit = SIZE_MAX;
static oserr_t g_capabilityStatus = OS_EOK;
static MemorySpace_t g_space;
static uuid_t g_currentSpaceHandle = UUID_INVALID;
static unsigned int g_live;
static int          g_failAllocationAfter = -1;
static bool         g_failTableAllocation;
static oserr_t      g_cloneResult;
static unsigned int g_mutexDestructions;
static unsigned int g_groupGrantCleanups;
static unsigned int g_eventNotifications;
static uuid_t       g_lastEvent;

oserr_t
MarkHandle(
    _In_ uuid_t       handle,
    _In_ unsigned int flags)
{
    assert(flags == IOSETSYN);
    g_lastEvent = handle;
    g_eventNotifications++;
    return OS_EOK;
}

oserr_t
FutexWake(
    _In_ atomic_int* futex,
    _In_ int         count,
    _In_ int         flags)
{
    abort();
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* memory;

    if (g_failAllocationAfter == 0) {
        return NULL;
    }
    if (g_failAllocationAfter > 0) {
        g_failAllocationAfter--;
    }
    memory = calloc(1, size);

    if (memory != NULL) {
        g_live++;
    }
    return memory;
}

void
kfree(
    _In_ void* memory)
{
    if (memory != NULL) {
        assert(g_live != 0);
        g_live--;
        free(memory);
    }
}

void*
dsalloc(
    _In_ size_t size)
{
    return g_failTableAllocation ? NULL : malloc(size);
}

void
dsfree(
    _In_ void* memory)
{
    free(memory);
}

SystemMachine_t*
GetMachine(void)
{
    static SystemMachine_t machine = {
        .MemoryMap = {
            .UserHeap = { .Start = 0x1000, .Length = 0x10000 },
            .ThreadLocal = { .Start = 0x20000, .Length = 0xffff }
        },
        .MemoryGranularity = 0x1000
    };

    return &machine;
}

MemorySpace_t*
GetDomainMemorySpace(void)
{
    return &GetMachine()->SystemSpace;
}

uuid_t
GetCurrentMemorySpaceHandle(void)
{
    assert(g_currentSpaceHandle != UUID_INVALID);
    return g_currentSpaceHandle;
}

size_t
GetMemorySpacePageSize(void)
{
    return 0x1000;
}

void
MutexConstruct(
    _In_ Mutex_t*     mutex,
    _In_ unsigned int configuration)
{
    (void)mutex;
    (void)configuration;
}

void
MutexDestruct(
    _In_ Mutex_t* mutex)
{
    (void)mutex;
    g_mutexDestructions++;
}

void
list_construct(
    _In_ list_t* list)
{
    memset(list, 0, sizeof(*list));
}

void
list_clear(
    _In_ list_t* list,
    _In_ void (*cleanup)(element_t*, void*),
    _In_ void* context)
{
    (void)cleanup;
    (void)context;
    // These lifetime cases do not create memory allocations inside the group.
    assert(list->head == NULL && list->tail == NULL && list->count == 0);
}

void
DynamicMemoryPoolConstruct(
    _In_ DynamicMemoryPool_t* pool,
    _In_ uintptr_t            start,
    _In_ size_t               length,
    _In_ size_t               chunkSize)
{
    (void)pool;
    (void)start;
    (void)length;
    (void)chunkSize;
}

void
DynamicMemoryPoolDestroy(
    _In_ DynamicMemoryPool_t* pool)
{
    (void)pool;
}

void
DynamicMemoryPoolFree(
    _In_ DynamicMemoryPool_t* pool,
    _In_ uintptr_t            address)
{
    (void)pool;
    (void)address;
    assert(false);
}

oserr_t
MmuCloneVirtualSpace(
    _In_ MemorySpace_t* parent,
    _In_ MemorySpace_t* child,
    _In_ int            inherit)
{
    (void)parent;
    (void)child;
    (void)inherit;
    return g_cloneResult;
}

oserr_t
MmuDestroyVirtualSpace(
    _In_ MemorySpace_t* memorySpace)
{
    (void)memorySpace;
    return OS_EOK;
}

static void
__DrainJanitor(void)
{
    element_t* entry;

    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

static void
__CountGroupGrantCleanup(
    _In_ void* resource)
{
    (void)resource;
    g_groupGrantCleanups++;
}

static void
__TestGroupAfterCreatorExit(void)
{
    MemorySpace_t*    memorySpace;
    struct MSContext* context;
    uuid_t            creatorGlobalHandleID;
    uuid_t            otherGlobalHandleID;
    uuid_t            groupGlobalHandleID;
    uuid_t            memorySpaceHandleID;
    uuid_t            securityGroupID;

    creatorGlobalHandleID = MSContextNew();
    otherGlobalHandleID = MSContextNew();
    assert(creatorGlobalHandleID != UUID_INVALID && otherGlobalHandleID != UUID_INVALID);

    g_space.GroupGlobalHandleID = creatorGlobalHandleID;
    assert(CreateMemorySpace(
        MEMORY_SPACE_APPLICATION,
        &memorySpaceHandleID,
        &securityGroupID
    ) == OS_EOK);
    assert(AcquireHandleReference(
        memorySpaceHandleID,
        HandleTypeMemorySpace,
        (void**)&memorySpace
    ) == OS_EOK);
    groupGlobalHandleID = memorySpace->GroupGlobalHandleID;
    assert(securityGroupID == groupGlobalHandleID);
    context = LookupGlobalHandle(groupGlobalHandleID, HandleTypeMSContext);
    assert(context != NULL && context->Security.GlobalHandleID == groupGlobalHandleID);
    assert(LookupHandle(groupGlobalHandleID, HandleTypeMSContext) == NULL);
    assert(AcquireHandle(groupGlobalHandleID, HandleTypeMSContext, NULL) == OS_ENOENT);

    // The group's grant cleanup must run once, when its memory space is gone.
    g_space.GroupGlobalHandleID = groupGlobalHandleID;
    assert(CreateHandle(HandleTypeGeneric, __CountGroupGrantCleanup, NULL, NULL) != UUID_INVALID);
    g_space.GroupGlobalHandleID = otherGlobalHandleID;
    assert(ReleaseGlobalHandle(creatorGlobalHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_mutexDestructions == 2 && g_groupGrantCleanups == 0);
    assert(LookupGlobalHandle(groupGlobalHandleID, HandleTypeMSContext) == context);
    assert(LookupGlobalHandle(memorySpaceHandleID, HandleTypeMemorySpace) == memorySpace);

    // Cleanup runs in another context with no grant to the memory-space group.
    assert(ReleaseGlobalHandle(memorySpaceHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_mutexDestructions == 4 && g_groupGrantCleanups == 1);
    assert(LookupGlobalHandle(groupGlobalHandleID, HandleTypeMSContext) == NULL);
    __DrainJanitor();
    assert(g_groupGrantCleanups == 1);

    g_space.GroupGlobalHandleID = UUID_INVALID;
    assert(ReleaseGlobalHandle(otherGlobalHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_mutexDestructions == 6);
    assert(g_live == 0 && g_handles.element_count == 0);
}

static void
__TestGroupCreationFailures(void)
{
    uuid_t       creatorGlobalHandleID;
    uuid_t       memorySpaceHandleID;
    uuid_t       securityGroupID;
    unsigned int live;
    int          failure;

    creatorGlobalHandleID = MSContextNew();
    assert(creatorGlobalHandleID != UUID_INVALID);
    g_space.GroupGlobalHandleID = creatorGlobalHandleID;
    live = g_live;

    // Fail context allocation, global record allocation and grant-table allocation.
    for (failure = 0; failure < 3; failure++) {
        g_failAllocationAfter = failure;
        assert(MSContextNew() == UUID_INVALID);
        g_failAllocationAfter = -1;
        __DrainJanitor();
        assert(g_live == live && g_handles.element_count == 1);
    }

    g_failTableAllocation = true;
    assert(MSContextNew() == UUID_INVALID);
    g_failTableAllocation = false;
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == 1);

    g_cloneResult = OS_EOOM;
    assert(CreateMemorySpace(
        MEMORY_SPACE_APPLICATION,
        &memorySpaceHandleID,
        &securityGroupID
    ) == OS_EOOM);
    g_cloneResult = OS_EOK;
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == 1);

    // The fifth allocation creates the memory-space handle after its group.
    g_failAllocationAfter = 4;
    assert(CreateMemorySpace(
        MEMORY_SPACE_APPLICATION,
        &memorySpaceHandleID,
        &securityGroupID
    ) == OS_EOOM);
    g_failAllocationAfter = -1;
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == 1);

    g_space.GroupGlobalHandleID = UUID_INVALID;
    assert(ReleaseGlobalHandle(creatorGlobalHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_live == 0 && g_handles.element_count == 0);
}

oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    assert(capabilities == CAPABILITY_MEMORYSPACE_MANAGEMENT);
    return g_capabilityStatus;
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       user,
    _In_  const void* kernel,
    _In_  size_t      length)
{
    memcpy(user, kernel, g_copyLimit < length ? g_copyLimit : length);
    return g_copyLimit < length ? OS_EPERMISSIONS : OS_EOK;
}

static void
__TestCreationSyscall(void)
{
    MemorySpaceCreateResult_t result;
    MemorySpaceCreateResult_t sentinel = { 0x12345678, 0x23456789 };
    MemorySpace_t*           space;
    uuid_t                   creator;
    unsigned int             live;
    size_t                   prefix;

    creator = MSContextNew();
    assert(creator != UUID_INVALID);
    g_space.GroupGlobalHandleID = creator;
    live = g_live;

    result = sentinel;
    assert(ScCreateMemorySpace(0, NULL) == OS_EUNKNOWN);
    g_capabilityStatus = OS_EPERMISSIONS;
    assert(ScCreateMemorySpace(0, &result) == OS_EPERMISSIONS);
    assert(memcmp(&result, &sentinel, sizeof(result)) == 0);
    assert(g_live == live && g_handles.element_count == 1);
    g_capabilityStatus = OS_EOK;

    g_cloneResult = OS_EOOM;
    assert(ScCreateMemorySpace(0, &result) == OS_EOOM);
    assert(memcmp(&result, &sentinel, sizeof(result)) == 0);
    g_cloneResult = OS_EOK;
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == 1);

    // Even a copy that published the complete handle must undo its grant.
    for (prefix = 0; prefix < sizeof(result); prefix++) {
        result = sentinel;
        g_copyLimit = prefix;
        assert(ScCreateMemorySpace(0, &result) == OS_EPERMISSIONS);
        if (prefix >= sizeof(result.Handle)) {
            assert(LookupHandle(result.Handle, HandleTypeMemorySpace) == NULL);
        }
        __DrainJanitor();
        assert(g_live == live && g_handles.element_count == 1);
    }
    g_copyLimit = SIZE_MAX;

    assert(ScCreateMemorySpace(0, &result) == OS_EOK);
    space = LookupHandle(result.Handle, HandleTypeMemorySpace);
    assert(space != NULL && (space->Flags & MEMORY_SPACE_APPLICATION));
    assert(result.SecurityGroupID == space->GroupGlobalHandleID);
    assert(result.SecurityGroupID != creator);
    assert(LookupHandle(result.SecurityGroupID, HandleTypeMSContext) == NULL);
    assert(DestroyHandle(result.Handle) == OS_EOK);
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == 1);

    g_space.GroupGlobalHandleID = UUID_INVALID;
    assert(ReleaseGlobalHandle(creator) == OS_EOK);
    __DrainJanitor();
    assert(g_live == 0 && g_handles.element_count == 0);
}

static void
__TestInheritedPageTables(
    _In_ MemorySpace_t* root,
    _In_ MemorySpace_t* child)
{
    PageMasterTable_t  rootTable = { 0 };
    PageMasterTable_t  childTable = { 0 };
    PageMasterTable_t* parent;
    PageMasterTable_t* table;
    int                isCurrent;

    root->PlatformData.Cr3VirtualAddress = (uintptr_t)&rootTable;
    child->PlatformData.Cr3VirtualAddress = (uintptr_t)&childTable;
    assert(LookupHandle(child->ParentHandle, HandleTypeMemorySpace) == NULL);

    // The child's kernel reference must resolve the shared process mappings,
    // even when the current grant table has no access to the root handle.
    table = MmVirtualGetMasterTable(
        child,
        MEMORY_LOCATION_RING3_CODE,
        &parent,
        &isCurrent
    );
    assert(table == &childTable && parent == &rootTable && isCurrent == 0);
    table = MmVirtualGetMasterTable(
        child,
        MEMORY_LOCATION_RING3_THREAD_START - 1,
        &parent,
        &isCurrent
    );
    assert(table == &childTable && parent == &rootTable && isCurrent == 0);

    // Kernel mappings and thread-local mappings do not use the parent's table.
    table = MmVirtualGetMasterTable(child, MEMORY_LOCATION_KERNEL, &parent, &isCurrent);
    assert(table == &childTable && parent == NULL && isCurrent == 0);
    table = MmVirtualGetMasterTable(
        child,
        MEMORY_LOCATION_RING3_THREAD_START,
        &parent,
        &isCurrent
    );
    assert(table == &childTable && parent == NULL && isCurrent == 0);

    // The process root has no parent table of its own.
    table = MmVirtualGetMasterTable(root, MEMORY_LOCATION_RING3_CODE, &parent, &isCurrent);
    assert(table == &rootTable && parent == NULL && isCurrent == 0);

    root->PlatformData.Cr3VirtualAddress = 0;
    child->PlatformData.Cr3VirtualAddress = 0;
}

static void
__TestInheritedSpaceWithoutGrant(void)
{
    MemorySpace_t* root;
    MemorySpace_t* child;
    uuid_t         rootHandle;
    uuid_t         childHandle;
    uuid_t         group;
    uuid_t         childGroup;
    uuid_t         otherGroup;
    unsigned int   live;

    assert(CreateMemorySpace(MEMORY_SPACE_APPLICATION, &rootHandle, &group) == OS_EOK);
    root = LookupGlobalHandle(rootHandle, HandleTypeMemorySpace);
    assert(root != NULL);
    g_space = *root;
    g_currentSpaceHandle = rootHandle;

    // Match bootstrap: the thread has a kernel reference to its space, but
    // the process grant table does not contain that space's handle.
    assert(AcquireHandle(rootHandle, HandleTypeMemorySpace, NULL) == OS_ENOENT);
    live = g_live;
    g_cloneResult = OS_EOOM;
    assert(CreateMemorySpace(MEMORY_SPACE_APPLICATION | MEMORY_SPACE_INHERIT,
                            &childHandle, &childGroup) == OS_EOOM);
    g_cloneResult = OS_EOK;
    __DrainJanitor();
    assert(g_live == live);
    assert(__LookupGlobalSafe(rootHandle)->References == 1);

    assert(CreateMemorySpace(MEMORY_SPACE_APPLICATION | MEMORY_SPACE_INHERIT,
                            &childHandle, &childGroup) == OS_EOK);
    assert(childGroup == group);
    assert(AcquireHandleReference(childHandle, HandleTypeMemorySpace,
                                  (void**)&child) == OS_EOK);
    assert(child->ParentHandle == rootHandle);
    assert(DestroyHandle(childHandle) == OS_EOK);
    assert(ReleaseGlobalHandle(rootHandle) == OS_EINCOMPLETE);
    __TestInheritedPageTables(root, child);
    assert(__LookupGlobalSafe(rootHandle)->References == 1);

    // Final cleanup runs outside the process. The child's parent reference
    // must still be released, including the root's security context.
    otherGroup = MSContextNew();
    assert(otherGroup != UUID_INVALID);
    g_space.GroupGlobalHandleID = otherGroup;
    g_currentSpaceHandle = UUID_INVALID;
    __TestInheritedPageTables(root, child);
    assert(__LookupGlobalSafe(rootHandle)->References == 1);
    assert(ReleaseGlobalHandle(childHandle) == OS_EOK);
    __DrainJanitor();
    assert(g_handles.element_count == 1);
    memset(&g_space, 0, sizeof(g_space));
    g_space.GroupGlobalHandleID = UUID_INVALID;
    assert(ReleaseGlobalHandle(otherGroup) == OS_EOK);
    __DrainJanitor();
    assert(g_live == 0 && g_handles.element_count == 0);
}

static void
__TestEventSignalWithoutGrant(
    _In_ uuid_t driverGroup,
    _In_ uuid_t interruptedGroup)
{
    atomic_int counter = 0;
    UserEvent_t event = {
        .InitialValue = 1,
        .flags = EVT_RESET_EVENT,
        .KernelMapping = &counter
    };
    uuid_t handle;

    g_space.GroupGlobalHandleID = driverGroup;
    handle = CreateHandle(HandleTypeUserEvent, NULL, NULL, &event);
    assert(handle != UUID_INVALID);
    assert(AcquireHandleReference(handle, HandleTypeUserEvent, NULL) == OS_EOK);
    assert(__LookupGlobalSafe(handle)->References == 2);

    g_space.GroupGlobalHandleID = interruptedGroup;
    assert(LookupHandle(handle, HandleTypeUserEvent) == NULL);
    assert(AcquireHandleReference(handle, HandleTypeUserEvent, NULL) == OS_ENOENT);
    assert(UserEventSignal(handle) == OS_EINCOMPLETE);
    assert(atomic_load(&counter) == 1 && g_eventNotifications == 1 && g_lastEvent == handle);

    // The kernel reference must survive the driver's final grant close.
    g_space.GroupGlobalHandleID = driverGroup;
    assert(DestroyHandle(handle) == OS_EOK);
    assert(__LookupGlobalSafe(handle)->References == 1);
    g_space.GroupGlobalHandleID = interruptedGroup;
    atomic_store(&counter, 0);
    assert(UserEventSignal(handle) == OS_EINCOMPLETE);
    assert(atomic_load(&counter) == 1 && g_eventNotifications == 2);
    assert(__LookupGlobalSafe(handle)->References == 1);

    assert(ReleaseGlobalHandle(handle) == OS_EOK);
    assert(UserEventSignal(handle) == OS_EINVALPARAMS);
    __DrainJanitor();
    g_space.GroupGlobalHandleID = driverGroup;
}

int
main(void)
{
    struct MSContext first = { 0 };
    struct MSContext second = { 0 };
    MSContextSecurity_t* checkedSecurity;
    uuid_t           firstGlobalHandleID;
    uuid_t           secondGlobalHandleID;
    uuid_t           buffer;
    void*            resource;
    element_t*       entry;

    assert(InitializeHandles() == OS_EOK);
    assert(MemorySpaceGetSecurityContext() == NULL);
    firstGlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, NULL, &first);
    secondGlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, NULL, &second);
    first.Security.GlobalHandleID = firstGlobalHandleID;
    second.Security.GlobalHandleID = secondGlobalHandleID;
    first.Security.HandleTable = CreateHandleGrantsTable();
    second.Security.HandleTable = CreateHandleGrantsTable();
    assert(first.Security.HandleTable != NULL && second.Security.HandleTable != NULL);
    __TestEventSignalWithoutGrant(firstGlobalHandleID, secondGlobalHandleID);
    g_space.GroupGlobalHandleID = UUID_INVALID;

    assert(MSContextAcquireSecurity(firstGlobalHandleID, &checkedSecurity) == OS_EOK);
    assert(checkedSecurity == &first.Security);
    assert(ReleaseGlobalHandle(firstGlobalHandleID) == OS_EINCOMPLETE);
    checkedSecurity = NULL;
    assert(MSContextAcquireSecurity((1ULL << 32) | firstGlobalHandleID, &checkedSecurity) == OS_EINVALPARAMS);
    assert(checkedSecurity == NULL);
    assert(MSContextAcquireSecurity(0, &checkedSecurity) == OS_EINVALPARAMS);

    // Context discovery must work even though the context has no grant to itself.
    g_space.GroupGlobalHandleID = firstGlobalHandleID;
    assert(MemorySpaceGetSecurityContext() == &first.Security);
    assert(LookupHandle(firstGlobalHandleID, HandleTypeMSContext) == NULL);
    assert(LookupGlobalHandle(firstGlobalHandleID, HandleTypeMSContext) == &first);
    assert(LookupGlobalHandle(firstGlobalHandleID, HandleTypeSHM) == NULL);
    buffer = CreateHandle(HandleTypeSHM, NULL, NULL, (void*)42);
    assert(AcquireHandleReference(buffer, HandleTypeSHM, &resource) == OS_EOK);
    assert(resource == (void*)42);

    g_space.GroupGlobalHandleID = secondGlobalHandleID;
    assert(MemorySpaceGetSecurityContext() == &second.Security);
    assert(AcquireHandle(buffer, HandleTypeSHM, NULL) == OS_ENOENT);
    assert(AcquireHandleReference(buffer, HandleTypeSHM, &resource) == OS_ENOENT);
    assert(LookupHandle(buffer, HandleTypeSHM) == NULL);
    // Explicit global lookup is for trusted kernel references, not authorization.
    assert(LookupGlobalHandle(buffer, HandleTypeSHM) == (void*)42);

    g_space.GroupGlobalHandleID = firstGlobalHandleID;
    assert(GrantHandle(&second.Security, buffer) == OS_EOK);
    g_space.GroupGlobalHandleID = secondGlobalHandleID;
    assert(AcquireHandle(buffer, HandleTypeSHM, &resource) == OS_EOK && resource == (void*)42);
    assert(DestroyHandle(buffer) == OS_EINCOMPLETE);
    DestroyHandleGrantsTable(first.Security.HandleTable);
    assert(LookupHandle(buffer, HandleTypeSHM) == (void*)42);
    assert(DestroyHandle(buffer) == OS_EOK);
    assert(LookupHandle(buffer, HandleTypeSHM) == NULL);
    assert(ReleaseGlobalHandle(buffer) == OS_EOK);
    DestroyHandleGrantsTable(second.Security.HandleTable);

    g_space.GroupGlobalHandleID = UUID_INVALID;
    assert(ReleaseGlobalHandle(firstGlobalHandleID) == OS_EOK);
    assert(ReleaseGlobalHandle(secondGlobalHandleID) == OS_EOK);
    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
    assert(g_live == 0 && g_handles.element_count == 0);
    __TestGroupAfterCreatorExit();
    __TestGroupCreationFailures();
    __TestCreationSyscall();
    __TestInheritedSpaceWithoutGrant();
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("Handle contexts: grant isolation, interrupt events, inherited page tables, group references and cleanup passed");
    return 0;
}
