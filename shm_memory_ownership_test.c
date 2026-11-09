/** Real SHM creation, device mappings and memory ownership, with host views. */
#include <machine.h>
#include <shm/shm.h>
#include <shm/device.h>
#include <handles/handle.h>
#include "../kernel/memory/shm/private.h"
#include "../kernel/memory/space/private.h"
#include <assert.h>
#include <log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 0x1000
#define REGION_BASE 0x100000
#define REGION_LENGTH (8 * PAGE_SIZE)

static SystemMachine_t g_machine;
static MemorySpace_t   g_space;
static uintptr_t        g_storage[128];
static _Alignas(PAGE_SIZE) unsigned char g_ram[REGION_LENGTH];
static unsigned int    g_heapLive;
static unsigned int    g_heapCalls;
static unsigned int    g_failHeapAt;
static bool            g_failHandle;
static bool            g_failMap;
static oserr_t         g_unmapFailure;
static uuid_t          g_failDrop;
static unsigned int    g_dropCalls[16];
static unsigned int    g_unmapCalls;
static unsigned int    g_generalFrees;
static uuid_t          g_operationID;
static unsigned int    g_operationReferences;
static unsigned int    g_operationResolves;

struct __TestHandle {
    void*              Resource;
    HandleDestructorFn Destructor;
    unsigned int       References;
};
static struct __TestHandle g_handles[16];

struct __TestView {
    vaddr_t Address;
    size_t  Length;
};
static struct __TestView g_views[16];

static void
__CheckOperationReference(void)
{
    if (g_operationID != 0) {
        assert(g_handles[g_operationID].References == g_operationReferences + 1);
    }
}

static void
__CheckOperationExit(void)
{
    assert(g_handles[g_operationID].References == g_operationReferences);
    assert(g_operationResolves == 1);
    g_operationResolves = 0;
}

void
LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}
MemorySpace_t*
GetDomainMemorySpace(void)
{
    return GetCurrentMemorySpace();
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}
size_t
GetMemorySpacePageSize(void)
{
    return PAGE_SIZE;
}

void*
kmalloc(
    size_t length)
{
    void* memory;
    if (++g_heapCalls == g_failHeapAt) {
        return NULL;
    }
    memory = malloc(length);
    assert(memory != NULL);
    g_heapLive++;
    return memory;
}
void
kfree(
    void* memory)
{
    assert(memory != NULL && g_heapLive != 0);
    g_heapLive--;
    free(memory);
}
void
SpinlockConstruct(
    Spinlock_t* lock)
{
    atomic_init(&lock->Current, 0);
}
void
SpinlockAcquireIrq(
    Spinlock_t* lock)
{
    assert(atomic_exchange(&lock->Current, 1) == 0);
}
void
SpinlockReleaseIrq(
    Spinlock_t* lock)
{
    assert(atomic_exchange(&lock->Current, 0) == 1);
}
void
MutexConstruct(
    Mutex_t* mutex,
    unsigned int flags)
{
    (void)flags;
    mutex->ReferenceCount = 0;
}
void
MutexDestruct(
    Mutex_t* mutex)
{
    assert(mutex->ReferenceCount == 0);
}
void
MutexLock(
    Mutex_t* mutex)
{
    assert(mutex->ReferenceCount++ == 0);
}
void
MutexUnlock(
    Mutex_t* mutex)
{
    assert(--mutex->ReferenceCount == 0);
}

uuid_t
CreateHandle(
    HandleType_t type,
    HandleDestructorFn grantCleanup,
    HandleDestructorFn destructor,
    void* resource)
{
    (void)grantCleanup;
    assert(type == HandleTypeSHM);
    if (g_failHandle) {
        return UUID_INVALID;
    }
    for (unsigned int i = 1; i < 16; i++) {
        if (g_handles[i].References == 0) {
            g_handles[i] = (struct __TestHandle){resource, destructor, 1};
            return i;
        }
    }
    abort();
}
void*
LookupHandle(
    uuid_t id,
    HandleType_t type)
{
    if (id >= 16 || type != HandleTypeSHM || g_handles[id].References == 0) {
        return NULL;
    }
    return g_handles[id].Resource;
}

void*
LookupGlobalHandle(
    _In_ uuid_t       id,
    _In_ HandleType_t type)
{
    return LookupHandle(id, type);
}

struct MSAllocation*
MSAllocationLookup(
    _In_ struct MSContext* context,
    _In_ vaddr_t           address)
{
    // This harness maps ordinary buffers without a process allocation context.
    abort();
}

oserr_t
MSAllocationFindSHM(
    _In_  struct MSContext*     context,
    _In_  vaddr_t               address,
    _In_  size_t                length,
    _In_  bool                  allowFreedPages,
    _Out_ struct MSAllocation** allocationOut)
{
    // Restricted commit is covered by the real VM test.
    abort();
}

struct MSAllocation*
MSAllocationAcquire(
    _In_ struct MSContext* context,
    _In_ vaddr_t           address)
{
    abort();
}

oserr_t
MSAllocationRelease(
    _In_ struct MSContext*    context,
    _In_ struct MSAllocation* allocation)
{
    abort();
}

oserr_t
MSMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mappingOut)
{
    // This harness maps ordinary buffers; restricted maps use the real VM test.
    abort();
}

oserr_t
MSUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    // Restricted cleanup is covered by shm_restricted_test.c with the real VM.
    abort();
}

oserr_t
AcquireHandle(
    uuid_t id,
    HandleType_t type,
    void** resource)
{
    void* found = LookupHandle(id, type);
    if (found == NULL) {
        return OS_ENOENT;
    }
    if (resource != NULL) {
        *resource = found;
    }
    g_handles[id].References++;
    return OS_EOK;
}
oserr_t
DestroyHandle(
    uuid_t id)
{
    struct __TestHandle* handle;
    assert(id < 16);
    g_dropCalls[id]++;
    // Inject failure only for the final reference, after temporary lookups end.
    if (id == g_failDrop && g_handles[id].References == 1) {
        return OS_EBUSY;
    }
    handle = &g_handles[id];
    assert(handle->References != 0);
    if (--handle->References != 0) {
        return OS_EINCOMPLETE;
    }
    handle->Destructor(handle->Resource);
    handle->Resource = NULL;
    return OS_EOK;
}
oserr_t
AcquireHandleReference(
    _In_  uuid_t       id,
    _In_  HandleType_t type,
    _Out_ void**       resource)
{
    if (id == g_operationID) {
        g_operationResolves++;
    }
    return AcquireHandle(id, type, resource);
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t id)
{
    return DestroyHandle(id);
}

oserr_t
AreMemorySpacesRelated(
    MemorySpace_t* first,
    MemorySpace_t* second)
{
    return first == second ? OS_EOK : OS_EPERMISSIONS;
}
oserr_t
AllocatePhysicalMemory(
    size_t mask,
    int count,
    uintptr_t* pages)
{
    return SystemMemoryAllocate(&g_machine.PhysicalMemory, mask, count, pages);
}
void
FreePhysicalMemory(
    int count,
    const uintptr_t* pages)
{
    (void)count;
    (void)pages;
#ifdef IPC_SECURITY_TEST
    for (int i = 0; i < count; i++) {
        assert(pages[i] == 0);
    }
    return;
#endif
    // Every buffer in this test has reserved backing; this is always a bug.
    g_generalFrees++;
    abort();
}

oserr_t
MemorySpaceMap(
    MemorySpace_t* space,
    struct MemorySpaceMapOptions* options,
    vaddr_t* output)
{
    paddr_t           base;
    struct SHMBuffer* buffer;
    (void)space;
    __CheckOperationReference();
    if (g_failMap) {
        return OS_EOOM;
    }
    assert(options->Pages != NULL);
#ifdef IPC_SECURITY_TEST
    buffer = LookupHandle(options->SHMTag, HandleTypeSHM);
    if (buffer != NULL && SHM_KIND(buffer->Flags) == SHM_IPC) {
        if (buffer->Allocation.Owner == NULL) {
            assert(options->Flags & MAPPING_USERSPACE);
            assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, options->Length,
                PAGE_SIZE, 0, &buffer->Allocation) == OS_EOK);
        }
        for (size_t i = 0; i < DIVUP(options->Length, PAGE_SIZE); i++) {
            options->Pages[i] = buffer->Allocation.PhysicalBase + i * PAGE_SIZE;
        }
    }
#endif
    base = options->Pages[0];
    assert(base >= REGION_BASE && base - REGION_BASE + options->Length <= REGION_LENGTH);
    for (size_t i = 0; i < DIVUP(options->Length, PAGE_SIZE); i++) {
        assert(options->Pages[i] == base + i * PAGE_SIZE);
    }
    assert((options->PlacementFlags & MAPPING_PHYSICAL_FIXED) ||
           SHM_KIND(((struct SHMBuffer*)LookupHandle(options->SHMTag, HandleTypeSHM))->Flags) == SHM_IPC);
    assert(options->Flags & MAPPING_PERSISTENT);
    buffer = LookupHandle(options->SHMTag, HandleTypeSHM);
    assert(buffer != NULL && buffer->Allocation.Owner == &g_machine.PhysicalMemory);
    assert(((options->Flags & MAPPING_NOCACHE) != 0) == !buffer->Allocation.Cached);
    if (options->Flags & MAPPING_CLEAN) {
        memset(g_ram + (base - REGION_BASE), 0, options->Length);
    }
    *output = (vaddr_t)(g_ram + (base - REGION_BASE));
    for (int i = 0; i < 16; i++) {
        if (g_views[i].Length == 0) {
            g_views[i] = (struct __TestView){*output, options->Length};
            return OS_EOK;
        }
    }
    abort();
}
oserr_t
MemorySpaceUnmap(
    MemorySpace_t* space,
    vaddr_t address,
    size_t length)
{
    (void)space;
    __CheckOperationReference();
    g_unmapCalls++;
    if (g_unmapFailure != OS_EOK) {
        return g_unmapFailure;
    }
    for (int i = 0; i < 16; i++) {
        if (g_views[i].Length == length && g_views[i].Address == address) {
            g_views[i].Length = 0;
            return OS_EOK;
        }
    }
    abort();
}

#ifndef IPC_SECURITY_TEST
oserr_t
MemorySpaceCommit(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ uintptr_t*     pages,
    _In_ size_t         length,
    _In_ size_t         mask,
    _In_ unsigned int   flags)
{
    __CheckOperationReference();
    assert(length == 2 * PAGE_SIZE);
    return g_failMap ? OS_EOOM : OS_EOK;
}
#endif

size_t
CpuDataCacheLineSize(void)
{
    return 64;
}
void
CpuDataCacheClean(
    uintptr_t physical,
    size_t length)
{
    (void)physical;
    (void)length;
    abort();
}
void
CpuDataCacheInvalidate(
    uintptr_t physical,
    size_t length)
{
    (void)physical;
    (void)length;
    abort();
}
void
CpuDataCacheCleanInvalidate(
    uintptr_t physical,
    size_t length)
{
    (void)physical;
    (void)length;
    abort();
}

static void
__ExpectEmpty(void)
{
    for (int i = 0; i < 16; i++) {
        assert(g_handles[i].References == 0 && g_views[i].Length == 0);
    }
    assert(MemoryStackFreeBlockCount(&g_machine.PhysicalMemory.DeviceMemory.Stack) == 8);
    assert(g_generalFrees == 0 && g_heapLive == 0);
}

static struct SHMDeviceContext*
__CreateContext(void)
{
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceRange    range = {REGION_BASE, 0x200000, REGION_LENGTH};
    assert(SHMDeviceContextCreate(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EOK);
    return context;
}

static void
__TestResolvedOperations(
    _InOut_ SHMHandle_t* handle)
{
    SHMHandle_t original;

    g_operationID = handle->ID;
    g_operationReferences = g_handles[handle->ID].References;
    original = *handle;
    g_failMap = true;
    assert(SHMMap(handle, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOOM);
    assert(memcmp(handle, &original, sizeof(original)) == 0);
    __CheckOperationExit();
    g_failMap = false;

    // Ordinary mappings still clamp requests to the available buffer length.
    assert(SHMMap(handle, 0, 3 * PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
    assert(handle->Length == 2 * PAGE_SIZE);
    __CheckOperationExit();
    original = *handle;

    // Updating an existing mapping must release the operation reference on error.
    g_failMap = true;
    assert(SHMMap(handle, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOOM);
    assert(memcmp(handle, &original, sizeof(original)) == 0);
    __CheckOperationExit();
#ifndef IPC_SECURITY_TEST
    // The IPC fixture supplies its own commit stub and does not exercise commit.
    assert(SHMCommit(handle->ID, handle->Buffer, handle->Buffer, 3 * PAGE_SIZE) == OS_EOOM);
    __CheckOperationExit();
    g_failMap = false;
    assert(SHMCommit(handle->ID, handle->Buffer, handle->Buffer, 3 * PAGE_SIZE) == OS_EOK);
    __CheckOperationExit();
#endif
    g_failMap = false;

    // Remapping uses the same reference while removing the previous mapping.
    assert(SHMMap(handle, PAGE_SIZE, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
    assert(handle->Offset == PAGE_SIZE && handle->Length == PAGE_SIZE);
    __CheckOperationExit();
    assert(SHMMap(handle, 0, 2 * PAGE_SIZE, SHM_ACCESS_READ | SHM_ACCESS_WRITE) == OS_EOK);
    __CheckOperationExit();
    g_operationID = 0;
}

static void
__TestLifetime(
    bool cached)
{
    struct SHMDeviceContext*      context;
    struct SHMDeviceMapping*      mapping = NULL;
    struct DMARequirements  requirements = {0x1800, 0x4000, 0x10000, true};
    struct SHMBuffer*             buffer;
    SHMHandle_t                   original;
    SHMHandle_t                   attached;
    struct SystemMemoryAllocation remaining = { 0 };
    struct SystemMemoryAllocation unavailable = { 0 };
    uuid_t                        id;

    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, PAGE_SIZE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, REGION_BASE, REGION_LENGTH, cached,
                                              g_storage, sizeof(g_storage)) == OS_EOK);
    memset(g_ram, 0xab, sizeof(g_ram));
    context = __CreateContext();
    assert(SHMDeviceAllocate(context, &requirements, &original, &mapping) == OS_EOK);
    id = original.ID;
    buffer = LookupHandle(id, HandleTypeSHM);
    assert(buffer != NULL && buffer->Allocation.Owner == &g_machine.PhysicalMemory);
    assert(buffer->Allocation.Length == 0x2000 && buffer->Allocation.Cached == cached);
    assert(SHM_KIND(buffer->Flags) == 0 && (buffer->Flags & SHM_COMMIT));
    for (size_t i = 0; i < original.Length; i++) {
        assert(((unsigned char*)original.Buffer)[i] == 0);
    }
    assert(SHMAttach(id, &attached) == OS_EOK);
    __TestResolvedOperations(&attached);

    // Failed detach retains the view, handle and backing for a later retry.
    g_unmapFailure = OS_EBUSY;
    assert(SHMDetach(&original) == OS_EBUSY);
    assert(g_handles[id].References == 3 && original.Buffer != NULL);
    // EINCOMPLETE from unmap is still a failure; only a reference drop uses
    // that status to report successful release with other owners remaining.
    g_unmapFailure = OS_EINCOMPLETE;
    assert(SHMDetach(&original) == OS_EINCOMPLETE);
    assert(g_handles[id].References == 3 && original.Buffer != NULL);
    g_unmapFailure = OS_EOK;
    assert(SHMDetach(&original) == OS_EOK);
    assert(SHMDetach(&attached) == OS_EOK);
    assert(g_handles[id].References == 1);

    // The device mapping alone keeps its pages unavailable after all process
    // attachments disappear. Only its final release returns the allocation.
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 0x6000, 0, 0, &remaining) == OS_EOK);
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 1, 0, 0, &unavailable) == OS_EOOM);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 0x2000, 0, 0, &unavailable) == OS_EOK);
    assert(SystemMemoryReleaseAllocation(&unavailable) == OS_EOK);
    assert(SystemMemoryReleaseAllocation(&remaining) == OS_EOK);
    SHMDeviceContextRelease(&context);
    __ExpectEmpty();
}

static void
__TestCreationFailures(void)
{
    SHMHandle_t                   handle;
    SHMHandle_t                   saved;
    struct SystemMemoryAllocation full = { 0 };
    struct SHMDeviceContext*      context;
    struct SHMDeviceMapping*      mapping = NULL;
    struct DMARequirements  requirements = {PAGE_SIZE, 0, 0, true};
    unsigned int                  baseline;

    memset(&handle, 0xa5, sizeof(handle));
    saved = handle;
    g_failHeapAt = g_heapCalls + 1;
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 0, 0, &handle) == OS_EOOM);
    g_failHeapAt = 0;
    __ExpectEmpty();
    g_failHandle = true;
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 0, 0, &handle) == OS_EOOM);
    g_failHandle = false;
    __ExpectEmpty();
    g_failMap = true;
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 0, 0, &handle) == OS_EOOM);
    g_failMap = false;
    __ExpectEmpty();
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, REGION_LENGTH, 0, 0, &full) == OS_EOK);
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 0, 0, &handle) == OS_EOOM);
    assert(SystemMemoryReleaseAllocation(&full) == OS_EOK);
    assert(SHMCreateDeviceBuffer(1, 0, 0, &handle) == OS_EINVALPARAMS);
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 3, 0, &handle) == OS_EINVALPARAMS);
    assert(memcmp(&handle, &saved, sizeof(saved)) == 0);
    __ExpectEmpty();

    // Inject every heap failure in the combined SHM/device allocation path.
    context = __CreateContext();
    baseline = g_heapLive;
    for (unsigned int fail = 1; fail <= 3; fail++) {
        g_failHeapAt = g_heapCalls + fail;
        assert(SHMDeviceAllocate(context, &requirements, &handle, &mapping) == OS_EOOM);
        g_failHeapAt = 0;
        assert(mapping == NULL && g_heapLive == baseline);
        assert(MemoryStackFreeBlockCount(&g_machine.PhysicalMemory.DeviceMemory.Stack) == 8);
        assert(memcmp(&handle, &saved, sizeof(saved)) == 0);
        for (int i = 0; i < 16; i++) {
            assert(g_handles[i].References == 0 && g_views[i].Length == 0);
        }
    }
    SHMDeviceContextRelease(&context);
    __ExpectEmpty();
}

static void
__TestDetachStages(void)
{
    SHMHandle_t handle;
    uuid_t      id;
    unsigned int drops;
    unsigned int unmaps;

    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, PAGE_SIZE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory,
        REGION_BASE, REGION_LENGTH, false, g_storage, sizeof(g_storage)) == OS_EOK);
    assert(SHMCreateDeviceBuffer(PAGE_SIZE, 0, 0, &handle) == OS_EOK);
    id = handle.ID;

    // A failed reference release follows a successful unmap. The next detach
    // must retry only the reference release, leaving that completed unmap alone.
    g_failDrop = id;
    unmaps = g_unmapCalls;
    assert(SHMDetach(&handle) == OS_EBUSY);
    assert(handle.Buffer == NULL && handle.ID == id);
    assert(g_unmapCalls == unmaps + 1);
    assert(g_handles[id].References == 1);

    g_failDrop = UUID_INVALID;
    drops = g_dropCalls[id];
    assert(SHMDetach(&handle) == OS_EOK && handle.ID == UUID_INVALID);
    assert(g_dropCalls[id] == drops + 1);
    assert(g_unmapCalls == unmaps + 1);
    assert(SHMDetach(&handle) == OS_EOK && g_dropCalls[id] == drops + 1);
    __ExpectEmpty();
}

int
main(void)
{
    __TestLifetime(false);
    __TestLifetime(true);
    __TestCreationFailures();
    __TestDetachStages();
    puts("SHM ownership: real creation, cache policy, attachment/device lifetime and failure "
         "cleanup passed");
    return 0;
}
