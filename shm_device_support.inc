/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Shared memory/cache fixtures without test cases. SHM_TEST_REAL_HANDLES
// omits the heap, handle and lock stubs, and the buffer creation/detach wrappers.
// The including suite supplies those functions around its real handle table.

#include <shm/device.h>
#include <handles/handle.h>
#include <machine.h>
#include "../kernel/memory/shm/private.h"
#include <shm/view.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct SHMBuffer* g_buffer;

#ifndef SHM_TEST_REAL_HANDLES
static atomic_uint g_liveAllocations;
static unsigned int g_allocationCalls;
static bool g_failAllocation;
static unsigned int g_failAtCall;

// One fake SHM buffer stands in for the kernel's handle table.
static int g_bufferReferences;
static int g_bufferKernelReferences;
static struct SHMView* g_restricted;
static int g_restrictedReferences;
static bool g_bufferLocked;
static pthread_mutex_t g_accessMutex = PTHREAD_MUTEX_INITIALIZER;

void*
kmalloc(
    _In_ size_t size)
{
    void* allocation;

    // Count attempts as well as successes to prove validation precedes storage.
    g_allocationCalls++;
    if (g_failAllocation || g_allocationCalls == g_failAtCall) {
        return NULL;
    }
    allocation = malloc(size);
    assert(allocation != NULL);
    atomic_fetch_add(&g_liveAllocations, 1);
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    // Track actual destruction to catch both leaks and premature final release.
    assert(allocation != NULL);
    assert(atomic_fetch_sub(&g_liveAllocations, 1) > 0);
    free(allocation);
}

oserr_t
AcquireHandle(
    _In_  uuid_t       handleId,
    _In_  HandleType_t handleType,
    _Out_ void**       resourceOut)
{
    // Mirror the kernel: a live handle of the right type gains a reference.
    if (g_buffer == NULL || handleId != g_buffer->ID || handleType != HandleTypeSHM) {
        return OS_ENOENT;
    }
    assert(g_bufferReferences > 0);
    g_bufferReferences++;
    *resourceOut = g_buffer;
    return OS_EOK;
}

oserr_t
DestroyHandle(
    _In_ uuid_t handleId)
{
    // The last reference frees the buffer, as __SHMBufferDelete would.
    assert(g_buffer != NULL && handleId == g_buffer->ID);
    assert(g_bufferReferences > 0);
    if (--g_bufferReferences == 0) {
        free(g_buffer);
        g_buffer = NULL;
        return OS_EOK;
    }
    return OS_EINCOMPLETE;
}

oserr_t
AcquireHandleReference(
    _In_  uuid_t       handleId,
    _In_  HandleType_t handleType,
    _Out_ void**       resourceOut)
{
    oserr_t status;

    if (handleId == 0x21 && g_restricted != NULL) {
        if (handleType != HandleTypeSHMView) {
            return OS_EUNKNOWN;
        }
        g_restrictedReferences++;
        *resourceOut = g_restricted;
        return OS_EOK;
    }
    status = AcquireHandle(handleId, handleType, resourceOut);

    if (status == OS_EOK) {
        g_bufferKernelReferences++;
    }
    return status;
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t handleId)
{
    if (handleId == 0x21 && g_restricted != NULL) {
        assert(g_restrictedReferences > 0);
        g_restrictedReferences--;
        return OS_EOK;
    }
    assert(g_bufferKernelReferences > 0);
    g_bufferKernelReferences--;
    return DestroyHandle(handleId);
}

static pthread_mutex_t g_quarantineMutex = PTHREAD_MUTEX_INITIALIZER;

void
MutexConstruct(Mutex_t* mutex, unsigned int flags)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->Flags = flags;
}

void
MutexDestruct(Mutex_t* mutex)
{
    (void)mutex;
}

void
MutexLock(
    _In_ Mutex_t* mutex)
{
    if (g_buffer == NULL || mutex != &g_buffer->Mutex) {
        assert(pthread_mutex_lock(&g_quarantineMutex) == 0);
        return;
    }
    assert(pthread_mutex_lock(&g_accessMutex) == 0);
    assert(!g_bufferLocked);
    g_bufferLocked = true;
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    if (g_buffer == NULL || mutex != &g_buffer->Mutex) {
        assert(pthread_mutex_unlock(&g_quarantineMutex) == 0);
        return;
    }
    assert(g_bufferLocked);
    g_bufferLocked = false;
    assert(pthread_mutex_unlock(&g_accessMutex) == 0);
}

#endif

size_t
GetMemorySpacePageSize(void)
{
    return 0x1000;
}

// Fake memory. Kernel views of buffer pages point into g_sourceMemory, one
// page per entry in the buffer's page list. Bounce pages come from the real
// device pool below and get host memory for their kernel view.
static uint8_t  g_sourceMemory[4 * 0x1000];
static uint8_t* g_bounceMemory;
static int      g_liveViews;
static int      g_mapCalls;
static int      g_failMapAt;

// Each nonzero count makes that many of the next unmaps of a bounce view or a
// source view fail. A failed unmap leaves the view in place, as the kernel does.
static int g_failBounceUnmaps;
static int g_failSourceUnmaps;

static bool
__FailViewUnmap(
    _In_ bool bounceView)
{
    int* failures = bounceView ? &g_failBounceUnmaps : &g_failSourceUnmaps;

    if (*failures == 0) {
        return false;
    }
    (*failures)--;
    return true;
}

// The real device pool allocator, with only its spinlock faked.
static SystemMachine_t  g_machine;
#define g_pool (g_machine.PhysicalMemory.DeviceMemory)

SystemMachine_t* GetMachine(void)
{
    return &g_machine;
}

static uintptr_t          g_poolStorage[128];
static int               g_poolLockHeld;

#if !defined(SHM_TEST_REAL_HANDLES) && !defined(SHM_TEST_REAL_SPINLOCK)
void
SpinlockConstruct(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
}

void
SpinlockAcquireIrq(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
    assert(g_poolLockHeld == 0);
    g_poolLockHeld = 1;
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
    assert(g_poolLockHeld == 1);
    g_poolLockHeld = 0;
}

#endif

static void
__UsePool(
    _In_ paddr_t base,
    _In_ bool    cached)
{
    // Eight pages are enough. Tests move the pool to put bounce pages within
    // or out of a device's reach.
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, 0x1000, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, base,
        8 * 0x1000, cached, g_poolStorage, sizeof(g_poolStorage)) == OS_EOK);
}

static bool
__PoolAllFree(void)
{
    return MemoryStackFreeBlockCount(&g_pool.Stack) == (g_pool.Length / 0x1000);
}

MemorySpace_t*
GetDomainMemorySpace(void)
{
    return GetCurrentMemorySpace();
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return NULL;
}

static bool
__IsBufferPageList(
    _In_ const paddr_t* pages)
{
    return g_buffer != NULL && pages >= g_buffer->Pages && pages < g_buffer->Pages + g_buffer->PageCount;
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*                memorySpace,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mappingOut)
{
    size_t index;

    (void)memorySpace;
    if (++g_mapCalls == g_failMapAt) {
        return OS_EOOM;
    }

    // Views must live in kernel memory, use given pages, be backed right away,
    // cover whole pages, and never free their pages when removed.
    assert(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_GLOBAL));
    assert(options->Flags & MAPPING_COMMIT);
    assert(options->Flags & MAPPING_PERSISTENT);
    assert(options->Length % 0x1000 == 0);
    if (__IsBufferPageList(options->Pages)) {
        assert(((options->Flags & MAPPING_NOCACHE) != 0) == SHMBufferUncached(g_buffer));
        index = (size_t)(options->Pages - g_buffer->Pages);
        assert(index * 0x1000 + options->Length <= sizeof(g_sourceMemory));
        *mappingOut = (vaddr_t)&g_sourceMemory[index * 0x1000];
    } else {
        // Bounce pages are one zeroed run from the pool, cached as it says.
        assert(g_bounceMemory == NULL && (options->Flags & MAPPING_CLEAN));
        assert(((options->Flags & MAPPING_NOCACHE) != 0) == !g_machine.PhysicalMemory.DeviceMemory.Cached);
        for (size_t i = 0; i < options->Length / 0x1000; i++) {
            assert(options->Pages[i] == options->Pages[0] + i * 0x1000);
            assert(options->Pages[i] >= g_pool.Base && options->Pages[i] < g_pool.Base + 8 * 0x1000);
        }
        g_bounceMemory = calloc(1, options->Length);
        assert(g_bounceMemory != NULL);
        *mappingOut = (vaddr_t)g_bounceMemory;
    }
    g_liveViews++;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* memorySpace,
    _In_ vaddr_t        address,
    _In_ size_t         size)
{
    (void)memorySpace;
    (void)size;
    assert(g_liveViews > 0);
    if (__FailViewUnmap((uint8_t*)address == g_bounceMemory)) {
        return OS_EUNKNOWN;
    }
    if ((uint8_t*)address == g_bounceMemory) {
        free(g_bounceMemory);
        g_bounceMemory = NULL;
    } else {
        assert((uint8_t*)address >= g_sourceMemory &&
               (uint8_t*)address < g_sourceMemory + sizeof(g_sourceMemory));
    }
    g_liveViews--;
    return OS_EOK;
}

enum __CacheOperation {
    __CacheClean,
    __CacheInvalidate,
    __CacheCleanInvalidate
};

struct __CacheCall {
    enum __CacheOperation Operation;
    uintptr_t             Address;
    size_t                Length;
};

// Record cache work instead of doing it, so tests can check exactly which
// physical bytes each sync call asked the CPU to write back or throw away.
#define __CACHE_CALL_LOG 128
static struct __CacheCall g_cacheCalls[__CACHE_CALL_LOG];
static int                g_cacheCallCount;
static bool               g_expectQuarantine;
static void (*g_cacheHook)(void);

static void
__RecordCacheCall(
    _In_ enum __CacheOperation operation,
    _In_ uintptr_t             address,
    _In_ size_t                length)
{
    if (g_expectQuarantine) {
        assert(atomic_load(&g_buffer->Access.Faults) != 0);
    }
    assert(g_cacheCallCount < __CACHE_CALL_LOG);
    g_cacheCalls[g_cacheCallCount++] = (struct __CacheCall){ operation, address, length };
    if (g_cacheHook != NULL) {
        void (*hook)(void) = g_cacheHook;

        g_cacheHook = NULL;
        hook();
    }
}

size_t
CpuDataCacheLineSize(void)
{
    return 64;
}

void
CpuDataCacheClean(
    _In_ uintptr_t physical,
    _In_ size_t    length)
{
    __RecordCacheCall(__CacheClean, physical, length);
}

void
CpuDataCacheInvalidate(
    _In_ uintptr_t physical,
    _In_ size_t    length)
{
    __RecordCacheCall(__CacheInvalidate, physical, length);
}

void
CpuDataCacheCleanInvalidate(
    _In_ uintptr_t physical,
    _In_ size_t    length)
{
    __RecordCacheCall(__CacheCleanInvalidate, physical, length);
}

static void
__ExpectCacheCalls(
    _In_ const struct __CacheCall* expected,
    _In_ int                       count)
{
    assert(g_cacheCallCount == count);
    for (int i = 0; i < count; i++) {
        assert(g_cacheCalls[i].Operation == expected[i].Operation);
        assert(g_cacheCalls[i].Address == expected[i].Address);
        assert(g_cacheCalls[i].Length == expected[i].Length);
    }
    g_cacheCallCount = 0;
}

static void
__CreateBuffer(
    _In_ const paddr_t* pages,
    _In_ int            pageCount,
    _In_ size_t         offset,
    _In_ size_t         length,
    _In_ unsigned int   flags)
{
    // The test owns the first reference, like the process that created it.
    assert(g_buffer == NULL);
    g_buffer = calloc(1, sizeof(*g_buffer) + pageCount * sizeof(paddr_t));
    assert(g_buffer != NULL);
    g_buffer->ID = 0x20;
    g_buffer->Offset = offset;
    g_buffer->Length = length;
    g_buffer->Flags = flags;
    g_buffer->PageCount = pageCount;
    memcpy(g_buffer->Pages, pages, pageCount * sizeof(paddr_t));
#ifndef SHM_TEST_REAL_HANDLES
    assert(g_bufferKernelReferences == 0);
    g_bufferReferences = 1;
#endif
}

// Pages the fake pool-backed creation hands out, and what it was asked for.
static paddr_t g_createPages[4];
static int     g_createCalls;
static size_t  g_createAlignment;
static size_t  g_createBoundary;
static oserr_t g_createStatus = OS_EOK;

static oserr_t
__CreateDeviceBuffer(
    _In_  size_t       size,
    _In_  size_t       alignment,
    _In_  size_t       boundary,
    _Out_ SHMHandle_t* handle)
{
    int pageCount = (int)(size / 0x1000);

    // Device allocations come in whole pages. The pool itself is tested on
    // its own, so this fake hands out chosen pages to shape the device view.
    g_createCalls++;
    g_createAlignment = alignment;
    g_createBoundary = boundary;
    assert(size % 0x1000 == 0 && pageCount <= 4);
    if (g_createStatus != OS_EOK) {
        return g_createStatus;
    }

    __CreateBuffer(g_createPages, pageCount, 0, size, SHM_COMMIT | SHM_CLEAN);
    g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
    g_buffer->Allocation.Cached = false;
    *handle = (SHMHandle_t){
        .ID = g_buffer->ID,
        .Capacity = size,
        .Buffer = g_sourceMemory,
        .Length = size
    };
    return OS_EOK;
}

#ifndef SHM_TEST_REAL_HANDLES
oserr_t
SHMCreateDeviceBuffer(size_t size, size_t alignment, size_t boundary, SHMHandle_t* handle)
{
    return __CreateDeviceBuffer(size, alignment, boundary, handle);
}

oserr_t
SHMDetach(
    _In_ SHMHandle_t* handle)
{
    (void)DestroyHandle(handle->ID);
    return OS_EOK;
}

#endif

#ifndef SHM_TEST_REAL_HANDLES
#include "shm_device_binding_support.inc"
#endif
