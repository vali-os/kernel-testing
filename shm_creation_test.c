// Exercise SHM creation rollback with real handles and deferred destruction.
#include "../kernel/handle.c"
#include "handle_set_stubs.h"
#include <machine.h>
#include <mutex.h>
#include <shm/shm.h>
#include "../kernel/memory/space/private.h"
#include <ds/streambuffer.h>
#include <stdio.h>
#include <stdlib.h>

static MSContextSecurity_t g_security;
static SystemMachine_t g_machine;
static MemorySpace_t g_space;
static _Alignas(4096) unsigned char g_storage[4096];
static unsigned int g_live;
static int g_failAllocationAfter = -1;
static unsigned int g_mapCalls;
static unsigned int g_failMapAt;
static unsigned int g_unmapCalls;
static bool g_failUserUnmap;
static bool g_userMapped;
static bool g_kernelMapped;
static bool g_pagesAllocated;
static bool g_pinKernelMapping;
static unsigned int g_pageFrees;
static unsigned int g_mutexDestructions;
static uuid_t g_createdHandleID;

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return &g_security;
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

size_t
GetMemorySpacePageSize(void)
{
    return sizeof(g_storage);
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
    assert(memory != NULL);
    g_live++;
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
    return malloc(size);
}

void
dsfree(
    _In_ void* memory)
{
    free(memory);
}

void
MutexConstruct(
    _In_ Mutex_t*     mutex,
    _In_ unsigned int flags)
{
    (void)mutex;
    (void)flags;
}

void
MutexDestruct(
    _In_ Mutex_t* mutex)
{
    (void)mutex;
    g_mutexDestructions++;
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mappingOut)
{
    assert(space == &g_space && options->Length == sizeof(g_storage));
    g_mapCalls++;
    g_createdHandleID = options->SHMTag;
    if (g_mapCalls == g_failMapAt) {
        return OS_EOOM;
    }
    if (options->Flags & MAPPING_USERSPACE) {
        assert(!g_userMapped && !g_pagesAllocated);
        g_userMapped = true;
        if (options->Pages != NULL) {
            options->Pages[0] = 0x1000;
            g_pagesAllocated = true;
        }
    } else {
        assert(g_userMapped && g_pagesAllocated && !g_kernelMapped);
        g_kernelMapped = true;
        if (g_pinKernelMapping) {
            assert(AcquireGlobalHandle(g_createdHandleID, HandleTypeSHM, NULL) == OS_EOK);
        }
    }
    *mappingOut = (vaddr_t)g_storage;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    assert(space == &g_space && address == (vaddr_t)g_storage && length == sizeof(g_storage));
    g_unmapCalls++;
    if (g_userMapped) {
        if (g_failUserUnmap) {
            return OS_EBUSY;
        }
        g_userMapped = false;
    } else {
        assert(g_kernelMapped);
        g_kernelMapped = false;
    }
    return OS_EOK;
}

void
FreePhysicalMemory(
    _In_ int            count,
    _In_ const paddr_t* pages)
{
    assert(count == 1);
    assert(!g_userMapped && !g_kernelMapped);
    if (pages[0] != 0) {
        assert(pages[0] == 0x1000 && g_pagesAllocated);
        g_pagesAllocated = false;
        g_pageFrees++;
    }
}

oserr_t
GetMemorySpaceMapping(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ int            count,
    _Out_ paddr_t*      pages)
{
    (void)pages;
    assert(space == &g_space && address == (vaddr_t)g_storage && count == 1);
    return OS_ENOENT;
}

oserr_t
SystemMemoryReleaseAllocation(
    _In_ struct SystemMemoryAllocation* allocation)
{
    (void)allocation;
    assert(false);
    return OS_ENOTSUPPORTED;
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
__CheckFailedCreation(
    _In_ SHM_t* configuration,
    _In_ oserr_t expected)
{
    SHMHandle_t handle;
    SHMHandle_t saved;
    unsigned int live = g_live;
    size_t records = g_handles.element_count;

    memset(&handle, 0xa5, sizeof(handle));
    memcpy(&saved, &handle, sizeof(saved));
    assert(SHMCreate(configuration, &handle) == expected);
    assert(memcmp(&saved, &handle, sizeof(saved)) == 0);
    assert(!g_userMapped);
    __DrainJanitor();
    assert(g_live == live && g_handles.element_count == records);
    assert(!g_kernelMapped && !g_pagesAllocated);
}

int
main(void)
{
    SHM_t configuration = {
        .Key = "/test/creation",
        .Flags = SHM_IPC,
        .Size = sizeof(g_storage)
    };
    SHMHandle_t handle;
    SHMHandle_t saved;
    uuid_t occupiedHandleID;
    uuid_t resolvedGlobalHandleID;
    unsigned int unmaps;
    unsigned int frees;
    unsigned int destructors;
    int failure;

    assert(InitializeHandles() == OS_EOK);
    g_security.HandleTable = CreateHandleGrantsTable();
    assert(g_security.HandleTable != NULL);
    // The old device kind and all unknown kinds fail before allocating or
    // mapping anything, including when ordinary creation flags are present.
    for (configuration.Flags = 0x300U; configuration.Flags <= SHM_KIND_MASK;
         configuration.Flags += 0x100U) {
        __CheckFailedCreation(&configuration, OS_ENOTSUPPORTED);
        configuration.Flags |= SHM_COMMIT | SHM_CLEAN;
        __CheckFailedCreation(&configuration, OS_ENOTSUPPORTED);
        configuration.Flags &= SHM_KIND_MASK;
    }
    assert(g_mapCalls == 0 && g_unmapCalls == 0 && g_mutexDestructions == 0);
    configuration.Flags = SHM_IPC;

    for (failure = 0; failure < 2; failure++) {
        g_failAllocationAfter = failure;
        __CheckFailedCreation(&configuration, OS_EOOM);
        g_failAllocationAfter = -1;
    }

    g_mapCalls = 0;
    g_failMapAt = 1;
    __CheckFailedCreation(&configuration, OS_EOOM);
    assert(LookupHandleByPath(configuration.Key, &resolvedGlobalHandleID) == OS_ENOENT);
    g_mapCalls = 0;
    g_failMapAt = 2;
    unmaps = g_unmapCalls;
    __CheckFailedCreation(&configuration, OS_EOOM);
    assert(g_unmapCalls == unmaps + 1);
    g_failMapAt = 0;

    occupiedHandleID = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
    assert(RegisterHandlePath(occupiedHandleID, configuration.Key) == OS_EOK);
    unmaps = g_unmapCalls;
    frees = g_pageFrees;
    destructors = g_mutexDestructions;
    __CheckFailedCreation(&configuration, OS_EEXISTS);
    assert(g_unmapCalls == unmaps + 2 && g_pageFrees == frees + 1);
    assert(g_mutexDestructions == destructors + 1);
    assert(LookupGlobalHandle(g_createdHandleID, HandleTypeSHM) == NULL);
    assert(LookupHandleByPath(configuration.Key, &resolvedGlobalHandleID) == OS_EOK);
    assert(resolvedGlobalHandleID == occupiedHandleID);

    // The common SHMCreate rollback also applies to non-IPC user mappings.
    configuration.Flags = SHM_COMMIT;
    __CheckFailedCreation(&configuration, OS_EEXISTS);

    // Another kernel operation may already hold a reference when setup fails.
    configuration.Flags = SHM_IPC;
    g_pinKernelMapping = true;
    destructors = g_mutexDestructions;
    assert(SHMCreate(&configuration, &handle) == OS_EEXISTS);
    g_pinKernelMapping = false;
    __DrainJanitor();
    assert(!g_userMapped && g_kernelMapped && g_pagesAllocated);
    assert(LookupHandle(g_createdHandleID, HandleTypeSHM) == NULL);
    assert(LookupGlobalHandle(g_createdHandleID, HandleTypeSHM) != NULL);
    assert(g_mutexDestructions == destructors);
    assert(ReleaseGlobalHandle(g_createdHandleID) == OS_EOK);
    __DrainJanitor();
    assert(!g_kernelMapped && !g_pagesAllocated);
    assert(g_mutexDestructions == destructors + 1);

    memset(&handle, 0xa5, sizeof(handle));
    memcpy(&saved, &handle, sizeof(saved));
    frees = g_pageFrees;

    // Failed unmap keeps the creator's grant until its address space is gone.
    configuration.Flags = SHM_IPC;
    g_failUserUnmap = true;
    assert(SHMCreate(&configuration, &handle) == OS_EEXISTS);
    assert(memcmp(&saved, &handle, sizeof(saved)) == 0);
    __DrainJanitor();
    assert(g_userMapped && g_kernelMapped && g_pagesAllocated);
    assert(LookupHandle(g_createdHandleID, HandleTypeSHM) != NULL);
    assert(g_pageFrees == frees);

    // Memory-space teardown clears user mappings before destroying group grants.
    g_userMapped = false;
    g_failUserUnmap = false;
    DestroyHandleGrantsTable(g_security.HandleTable);
    __DrainJanitor();
    assert(!g_kernelMapped && !g_pagesAllocated);
    assert(g_live == 0 && g_handles.element_count == 0);
    assert(LookupHandleByPath(configuration.Key, &resolvedGlobalHandleID) == OS_ENOENT);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("SHM creation: allocation/map/path rollback, deferred cleanup and failed-unmap preservation passed");
    return 0;
}

// This creation-only fixture has no process VM allocations.
oserr_t
MSAllocationFindSHM(
    _In_ struct MSContext* context,
    _In_ vaddr_t address,
    _In_ size_t length,
    _In_ bool allowFreedPages,
    _Out_ struct MSAllocation** allocationOut)
{
    assert(context == NULL);
    *allocationOut = NULL;
    return OS_EOK;
}

void
MutexLock(
    _In_ Mutex_t* mutex)
{
    abort();
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    abort();
}
