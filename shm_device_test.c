/**
 * Exercise the public kernel interface against the real SHM device context.
 * Only the heap is replaced, so failures and outstanding allocations can be
 * checked without booting a kernel or enabling a device.
 */
#include <shm_device.h>
#include <handle.h>
#include "../kernel/memory/private.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint g_liveAllocations;
static unsigned int g_allocationCalls;
static bool g_failAllocation;
static unsigned int g_failAtCall;

// One fake SHM buffer stands in for the kernel's handle table.
static struct SHMBuffer* g_buffer;
static int g_bufferReferences;
static bool g_bufferLocked;

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
AcquireHandleOfType(
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

void
MutexLock(
    _In_ Mutex_t* mutex)
{
    // Pages must only be read while the buffer's own lock is held.
    assert(g_buffer != NULL && mutex == &g_buffer->Mutex && !g_bufferLocked);
    g_bufferLocked = true;
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    assert(g_buffer != NULL && mutex == &g_buffer->Mutex && g_bufferLocked);
    g_bufferLocked = false;
}

size_t
GetMemorySpacePageSize(void)
{
    return 0x1000;
}

// Fake memory. Kernel views of buffer pages point into g_sourceMemory, one
// page per entry in the buffer's page list. Bounce pages get host memory and
// fake physical addresses starting at g_bouncePhysical.
static uint8_t  g_sourceMemory[4 * 0x1000];
static paddr_t  g_bouncePhysical = 0x708000;
static uint8_t* g_bounceMemory;
static int      g_liveViews;
static int      g_mapCalls;
static int      g_failMapAt;

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return NULL;
}

oserr_t
ArchSHMTypeToPageMask(
    _In_  enum OSMemoryConformity conformity,
    _Out_ size_t*                 pageMaskOut)
{
    // Bounce pages are expected to come from low memory.
    assert(conformity == OSMEMORYCONFORMITY_LOW);
    *pageMaskOut = 0xFFFFFFFF;
    return OS_EOK;
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

    // Views must live in kernel memory, be backed right away and cover pages.
    assert(options->PlacementFlags & MAPPING_VIRTUAL_GLOBAL);
    assert(options->Flags & MAPPING_COMMIT);
    assert(options->Length % 0x1000 == 0);
    if (options->PlacementFlags & MAPPING_PHYSICAL_FIXED) {
        // A view of the buffer must never free the buffer's pages.
        assert(options->Flags & MAPPING_PERSISTENT);
        index = (size_t)(options->Pages - g_buffer->Pages);
        assert(index * 0x1000 + options->Length <= sizeof(g_sourceMemory));
        *mappingOut = (vaddr_t)&g_sourceMemory[index * 0x1000];
    } else {
        // Bounce pages must be zeroed and come from low memory.
        assert(g_bounceMemory == NULL);
        assert((options->Flags & MAPPING_CLEAN) && options->Mask == 0xFFFFFFFF);
        g_bounceMemory = calloc(1, options->Length);
        assert(g_bounceMemory != NULL);
        for (size_t i = 0; i < options->Length / 0x1000; i++) {
            options->Pages[i] = g_bouncePhysical + i * 0x1000;
        }
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
static struct __CacheCall g_cacheCalls[8];
static int                g_cacheCallCount;

static void
__RecordCacheCall(
    _In_ enum __CacheOperation operation,
    _In_ uintptr_t             address,
    _In_ size_t                length)
{
    assert(g_cacheCallCount < 8);
    g_cacheCalls[g_cacheCallCount++] = (struct __CacheCall){ operation, address, length };
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
    _In_ unsigned int   flags,
    _In_ bool           exported)
{
    // The test owns the first reference, like the process that created it.
    assert(g_buffer == NULL);
    g_buffer = calloc(1, sizeof(*g_buffer) + pageCount * sizeof(paddr_t));
    assert(g_buffer != NULL);
    g_buffer->ID = 0x20;
    g_buffer->Offset = offset;
    g_buffer->Length = length;
    g_buffer->Flags = flags;
    g_buffer->Exported = exported;
    g_buffer->PageCount = pageCount;
    memcpy(g_buffer->Pages, pages, pageCount * sizeof(paddr_t));
    g_bufferReferences = 1;
}

// Pages the fake SHMCreate hands out, and how many times it was called.
static paddr_t g_createPages[4];
static int     g_createCalls;
static oserr_t g_createStatus = OS_EOK;

oserr_t
SHMCreate(
    _In_ SHM_t*       shm,
    _In_ SHMHandle_t* handle)
{
    int pageCount = (int)(shm->Size / 0x1000);

    // Device allocations must be zeroed, uncached, from low memory, in pages.
    g_createCalls++;
    assert(shm->Flags == (SHM_DEVICE | SHM_CLEAN));
    assert(shm->Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    assert(shm->Conformity == OSMEMORYCONFORMITY_LOW);
    assert(shm->Size % 0x1000 == 0 && pageCount <= 4 && shm->Key == NULL);
    if (g_createStatus != OS_EOK) {
        return g_createStatus;
    }

    __CreateBuffer(g_createPages, pageCount, 0, shm->Size, shm->Flags, false);
    *handle = (SHMHandle_t){
        .ID = g_buffer->ID,
        .SourceID = UUID_INVALID,
        .Capacity = shm->Size,
        .Buffer = g_sourceMemory,
        .Length = shm->Size
    };
    return OS_EOK;
}

oserr_t
SHMDetach(
    _In_ SHMHandle_t* handle)
{
    (void)DestroyHandle(handle->ID);
    return OS_EOK;
}

static void
__ExpectTranslation(
    _In_ const struct SHMDeviceContext* context,
    _In_ uint64_t                       physical,
    _In_ uint64_t                       length,
    _In_ oserr_t                        expectedStatus,
    _In_ uint64_t                       expectedAddress)
{
    uint64_t address = 0xfeed;

    // Failed lookups must not leave a plausible but incomplete device address.
    assert(SHMDeviceContextTranslate(context, physical, length, &address) == expectedStatus);
    assert(address == (expectedStatus == OS_EOK ? expectedAddress : 0xfeed));
}

static struct SHMDeviceContext*
__CreateContext(
    _In_ const struct SHMDeviceRange* ranges,
    _In_ uint32_t                     count,
    _In_ enum SHMDeviceCachePolicy    policy,
    _In_ uint64_t                     limit)
{
    struct SHMDeviceContext* context = NULL;

    assert(SHMDeviceContextCreate(ranges, count, policy, limit, &context) == OS_EOK);
    return context;
}

static void
__ExpectLimitedTranslation(
    _In_ const struct SHMDeviceRange* ranges,
    _In_ uint32_t                     count,
    _In_ uint64_t                     limit,
    _In_ uint64_t                     physical,
    _In_ uint64_t                     length,
    _In_ oserr_t                      expectedStatus,
    _In_ uint64_t                     expectedAddress)
{
    // The limit belongs to the context, so each limit needs its own context.
    struct SHMDeviceContext* context = __CreateContext(ranges, count, SHMDeviceCacheCoherent, limit);

    __ExpectTranslation(context, physical, length, expectedStatus, expectedAddress);
    SHMDeviceContextRelease(&context);
}

static void
__TestTranslation(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x1000, 0x2000 },
        { 0x702000, 0x3000, 0x1000 },
        { 0x704000, 0x5000, 0x1000 }
    };
    struct SHMDeviceContext* context;

    // Include touching ranges and a physical hole: a fitting first byte is
    // insufficient, even if a later range would cover the rest of the request.
    context = __CreateContext(ranges, 3, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __ExpectTranslation(context, 0x700300, 0x100, OS_EOK, 0x1300);
    __ExpectLimitedTranslation(ranges, 3, 0x2fff, 0x700000, 0x2000, OS_EOK, 0x1000);
    __ExpectLimitedTranslation(ranges, 3, 0x2fff, 0x701fff, 1, OS_EOK, 0x2fff);
    __ExpectTranslation(context, 0x701fff, 2, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x700000, 0x2001, OS_ENOENT, 0);
    __ExpectLimitedTranslation(ranges, 3, 0x13fe, 0x700300, 0x100, OS_ENOENT, 0);
    __ExpectLimitedTranslation(ranges, 3, 0x13ff, 0x700300, 0x100, OS_EOK, 0x1300);
    __ExpectLimitedTranslation(ranges, 3, 0x12ff, 0x700300, 1, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x6fffff, 2, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x703000, 1, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x705000, 1, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x704fff, 1, OS_EOK, 0x5fff);
    __ExpectTranslation(context, 0x700000, 0, OS_EINVALPARAMS, 0);
    __ExpectTranslation(context, UINT64_MAX, 2, OS_EINVALPARAMS, 0);
    __ExpectTranslation(NULL, 0, 1, OS_EINVALPARAMS, 0);
    assert(SHMDeviceContextTranslate(context, 0, 1, NULL) == OS_EINVALPARAMS);
    SHMDeviceContextRelease(&context);
}

static void
__TestAliases(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x100000000ULL, 0x2000 },
        { 0x700000, 0x1000, 0x1000 },
        { 0x700800, 0x8000, 0x1000 }
    };
    struct SHMDeviceRange reversed[3];
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* other;
    unsigned int i;

    // A 32-bit controller can use the low alias even when a high alias appears
    // first. A short low alias must not hide another alias covering more bytes.
    context = __CreateContext(ranges, 3, SHMDeviceCacheCoherent, UINT64_MAX);
    for (i = 0; i < 3; i++) {
        reversed[i] = ranges[2 - i];
    }
    other = __CreateContext(reversed, 3, SHMDeviceCacheCoherent, UINT64_MAX);
    __ExpectLimitedTranslation(ranges, 3, UINT32_MAX, 0x700300, 0x100, OS_EOK, 0x1300);
    __ExpectTranslation(other, 0x700300, 0x100, OS_EOK, 0x1300);
    __ExpectLimitedTranslation(ranges, 3, UINT32_MAX, 0x700800, 0x1000, OS_EOK, 0x8000);
    __ExpectLimitedTranslation(reversed, 3, UINT32_MAX, 0x700800, 0x1000, OS_EOK, 0x8000);
    __ExpectTranslation(context, 0x700000, 0x2000, OS_EOK, 0x100000000ULL);
    __ExpectLimitedTranslation(ranges, 3, UINT32_MAX, 0x700000, 0x2000, OS_ENOENT, 0);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(&other);
}

static void
__TestAddressEdges(void)
{
    struct SHMDeviceRange ranges[] = {
        { UINT64_MAX - 15, UINT64_MAX - 15, 16 },
        { 0, 0, 1 }
    };
    struct SHMDeviceContext* context;

    // Inclusive end checks must accept the final representable byte and zero.
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    __ExpectTranslation(context, UINT64_MAX - 15, 16, OS_EOK, UINT64_MAX - 15);
    __ExpectTranslation(context, UINT64_MAX, 1, OS_EOK, UINT64_MAX);
    __ExpectLimitedTranslation(ranges, 2, UINT64_MAX - 1, UINT64_MAX, 1, OS_ENOENT, 0);
    __ExpectLimitedTranslation(ranges, 2, 0, 0, 1, OS_EOK, 0);
    __ExpectTranslation(context, 0, UINT64_MAX, OS_ENOENT, 0);
    SHMDeviceContextRelease(&context);
}

static void
__TestCreationFailures(void)
{
    const struct SHMDeviceRange invalid[][2] = {
        { { 0, 0, 0 }, { 0, 10, 1 } },
        { { UINT64_MAX, 0, 2 }, { 0, 10, 1 } },
        { { 0, UINT64_MAX, 2 }, { 0, 10, 1 } },
        { { 0, 10, 10 }, { 100, 19, 10 } },
        { { 100, 19, 10 }, { 0, 10, 10 } },
        { { 0, 10, 10 }, { 100, 10, 10 } },
        { { 0, 10, 10 }, { 100, 12, 1 } },
        { { 0, 12, 1 }, { 100, 10, 10 } }
    };
    struct SHMDeviceRange valid = { 0, 0, 1 };
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceContext* saved;
    unsigned int calls = g_allocationCalls;
    size_t i;

    // Reject malformed descriptions before allocation, including an oversized
    // count with only one accessible entry (the count must be checked first).
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(SHMDeviceContextCreate(invalid[i], 2, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EINVALPARAMS);
        assert(context == NULL);
    }
    assert(SHMDeviceContextCreate(NULL, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 0, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, UINT64_MAX, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 65, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_ENOTSUPPORTED);
    assert(SHMDeviceContextCreate(&valid, UINT32_MAX, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_ENOTSUPPORTED);
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheUnknown, UINT64_MAX, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 1, (enum SHMDeviceCachePolicy)99, UINT64_MAX, &context) == OS_EINVALPARAMS);
    assert(context == NULL && g_allocationCalls == calls);

    // Allocation failure leaves no owner behind and permits a later retry.
    g_failAllocation = true;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EOOM);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 0);
    g_failAllocation = false;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EOK);
    saved = context;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EBUSY);
    assert(context == saved);
    SHMDeviceContextRelease(&context);
}

static void
__TestCopiedStorageAndLifetime(void)
{
    struct SHMDeviceRange* ranges;
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceContext* retained;
    struct SHMDeviceContext* other = NULL;
    unsigned int i;

    // Fill the maximum map in reverse order to exercise bounded unsorted input.
    ranges = malloc(SHM_DEVICE_MAX_RANGES * sizeof(*ranges));
    assert(ranges != NULL);
    for (i = 0; i < SHM_DEVICE_MAX_RANGES; i++) {
        ranges[i] = (struct SHMDeviceRange){ i * 0x1000, (64 - i) * 0x1000, 0x1000 };
    }
    assert(SHMDeviceContextCreate(ranges, 64, SHMDeviceCacheNonCoherent, UINT64_MAX, &context) == OS_EOK);
    ranges[0].DeviceBase = 0;
    assert(SHMDeviceContextCreate(ranges, 1, SHMDeviceCacheCoherent, UINT64_MAX, &other) == OS_EOK);
    memset(ranges, 0, SHM_DEVICE_MAX_RANGES * sizeof(*ranges));
    free(ranges);

    // Neither source destruction nor another context's rules may change the
    // original. A retained reference must also outlive the creating owner.
    assert(SHMDeviceContextAcquire(context) == OS_EOK);
    retained = context;
    SHMDeviceContextRelease(&context);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 2);
    SHMDeviceContextRelease(&context);
    assert(atomic_load(&g_liveAllocations) == 2);
    __ExpectTranslation(retained, 0, 0x1000, OS_EOK, 0x40000);
    __ExpectTranslation(retained, 63 * 0x1000, 0x1000, OS_EOK, 0x1000);
    __ExpectTranslation(other, 0, 1, OS_EOK, 0);
    assert(SHMDeviceContextGetCachePolicy(retained) == SHMDeviceCacheNonCoherent);
    assert(SHMDeviceContextGetCachePolicy(other) == SHMDeviceCacheCoherent);
    SHMDeviceContextRelease(&retained);
    assert(atomic_load(&g_liveAllocations) == 1);
    SHMDeviceContextRelease(&other);
    assert(atomic_load(&g_liveAllocations) == 0);
    SHMDeviceContextRelease(NULL);
    assert(SHMDeviceContextAcquire(NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextGetCachePolicy(NULL) == SHMDeviceCacheUnknown);
}

static void*
__ReferenceWorker(
    _In_ void* argument)
{
    struct SHMDeviceContext* context = argument;
    struct SHMDeviceContext* temporary;
    unsigned int i;

    // Each worker already owns a reference, allowing safe retain/release and
    // translation while other workers and the creator drop their references.
    for (i = 0; i < 1000; i++) {
        assert(SHMDeviceContextAcquire(context) == OS_EOK);
        temporary = context;
        __ExpectTranslation(temporary, 0x700300, 0x100, OS_EOK, 0x1300);
        SHMDeviceContextRelease(&temporary);
    }
    SHMDeviceContextRelease(&context);
    return NULL;
}

static void
__TestConcurrentOwners(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x2000 };
    struct SHMDeviceContext* context = NULL;
    pthread_t workers[4];
    unsigned int i;

    // Pass actual owning references to threads, then let the creator leave.
    assert(SHMDeviceContextCreate(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX, &context) == OS_EOK);
    for (i = 0; i < 4; i++) {
        assert(SHMDeviceContextAcquire(context) == OS_EOK);
        assert(pthread_create(&workers[i], NULL, __ReferenceWorker, context) == 0);
    }
    SHMDeviceContextRelease(&context);
    for (i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__ExpectSegments(
    _In_ const struct SHMDeviceContext*  context,
    _In_ const SHMSG_t*                  extents,
    _In_ int                             extentCount,
    _In_ const struct SHMDeviceSegment*  expected,
    _In_ uint32_t                        expectedCount)
{
    struct SHMDeviceSegment segments[8];
    uint32_t count = 0;

    // Check both passes: the count must match exactly what the fill writes.
    assert(SHMDeviceContextBuildSegments(context, extents, extentCount, &count, NULL) == OS_EOK);
    assert(count == expectedCount && count <= 8);
    count = 8;
    assert(SHMDeviceContextBuildSegments(context, extents, extentCount, &count, segments) == OS_EOK);
    assert(count == expectedCount);
    assert(memcmp(segments, expected, count * sizeof(*segments)) == 0);
}

static void
__TestSegments(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x1000, 0x2000 },
        { 0x702000, 0x3800, 0x1000 },
        { 0x703000, 0x4800, 0x1000 },
        { 0x710000, 0x100000000ULL, 0x2000 },
        { 0x710000, 0x9000, 0x1000 }
    };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* limited;

    context = __CreateContext(ranges, 5, SHMDeviceCacheNonCoherent, UINT64_MAX);
    limited = __CreateContext(ranges, 5, SHMDeviceCacheNonCoherent, UINT32_MAX);

    // One physical block crossing into a range with a different device base
    // splits; crossing into one that continues the device addresses joins.
    __ExpectSegments(context, (SHMSG_t[]){ { 0x701800, 0x2000 } }, 1,
                     (struct SHMDeviceSegment[]){ { 0x2800, 0x800 }, { 0x3800, 0x1800 } }, 2);

    // Separate physical pages that land next to each other for the device join.
    __ExpectSegments(context, (SHMSG_t[]){ { 0x702000, 0x1000 }, { 0x703000, 0x100 } }, 2,
                     (struct SHMDeviceSegment[]){ { 0x3800, 0x1100 } }, 1);

    // Pages that are out of order for the device must stay apart, in order.
    __ExpectSegments(context, (SHMSG_t[]){ { 0x701000, 0x1000 }, { 0x700000, 0x1000 } }, 2,
                     (struct SHMDeviceSegment[]){ { 0x2000, 0x1000 }, { 0x1000, 0x1000 } }, 2);

    // The longer high alias wins without a limit; a 32-bit controller must
    // use the low alias instead, which only reaches the first page.
    __ExpectSegments(context, (SHMSG_t[]){ { 0x710000, 0x2000 } }, 1,
                     (struct SHMDeviceSegment[]){ { 0x100000000ULL, 0x2000 } }, 1);
    __ExpectSegments(limited, (SHMSG_t[]){ { 0x710000, 0x1000 } }, 1,
                     (struct SHMDeviceSegment[]){ { 0x9000, 0x1000 } }, 1);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(&limited);
}

static void
__TestSegmentFailures(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x2000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* limited;
    struct SHMDeviceSegment segments[2] = { { 0xfeed, 0xfeed }, { 0xfeed, 0xfeed } };
    SHMSG_t split[] = { { 0x700000, 0x100 }, { 0x701000, 0x100 } };
    SHMSG_t hole[] = { { 0x700000, 0x100 }, { 0x702000, 0x100 } };
    uint32_t count;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    limited = __CreateContext(&range, 1, SHMDeviceCacheCoherent, 0x1fff);

    // A too-small array reports the needed size and is left untouched.
    count = 1;
    assert(SHMDeviceContextBuildSegments(context, split, 2, &count, segments) == OS_EBUFFER);
    assert(count == 2 && segments[0].Address == 0xfeed);

    // A later unreachable byte must fail before earlier segments are written,
    // and so must a buffer that passes the controller limit partway through.
    count = 2;
    assert(SHMDeviceContextBuildSegments(context, hole, 2, &count, segments) == OS_ENOENT);
    assert(SHMDeviceContextBuildSegments(limited, split, 2, &count, segments) == OS_ENOENT);
    assert(count == 2 && segments[0].Address == 0xfeed);

    assert(SHMDeviceContextBuildSegments(NULL, split, 2, &count, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextBuildSegments(context, NULL, 2, &count, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextBuildSegments(context, split, 0, &count, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextBuildSegments(context, split, 2, NULL, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextBuildSegments(context, (SHMSG_t[]){ { 0x700000, 0 } }, 1,
                                         &count, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextBuildSegments(context, (SHMSG_t[]){ { UINTPTR_MAX, 2 } }, 1,
                                         &count, NULL) == OS_EINVALPARAMS);
    assert(count == 2);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(&limited);
}

static void
__TestSegmentAddressEdges(void)
{
    struct SHMDeviceRange ranges[] = {
        { UINTPTR_MAX - 0xfff, 0, 0x1000 },
        { 0, 0x1000, 0x1000 }
    };
    struct SHMDeviceContext* context;

    // The final physical page is valid, and its device addresses run straight
    // into the next extent's, so the two join even across the physical wrap.
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    __ExpectSegments(context, (SHMSG_t[]){ { UINTPTR_MAX - 0xfff, 0x1000 }, { 0, 0x1000 } }, 2,
                     (struct SHMDeviceSegment[]){ { 0, 0x2000 } }, 1);
    SHMDeviceContextRelease(&context);
}

static void
__TestMapping(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x701000, 0x705000 };
    const struct SHMDeviceSegment expected[] = { { 0x1300, 0x1d00 }, { 0x6000, 0xd00 } };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct SHMDeviceSegment* segments;
    uint32_t count = 0;

    // The buffer starts 0x100 into its first page, and the range 0x200 later.
    // The first two pages continue each other physically; the third does not.
    __CreateBuffer(pages, 3, 0x100, 0x2e00, SHM_DEVICE, false);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0x200, 0x2a00, SHMDeviceBidirectional, &mapping) == OS_EOK);
    assert(g_bufferReferences == 2 && !g_bufferLocked);

    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 2 && memcmp(segments, expected, sizeof(expected)) == 0);

    // Both the process and the context creator may leave; the mapping still
    // keeps the pages and the address rules (context, mapping, physical blocks).
    assert(DestroyHandle(0x20) == OS_EINCOMPLETE);
    SHMDeviceContextRelease(&context);
    assert(g_buffer != NULL && atomic_load(&g_liveAllocations) == 3);

    SHMDeviceUnmap(&mapping);
    assert(mapping == NULL && g_buffer == NULL);
    assert(atomic_load(&g_liveAllocations) == 0);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SHMDeviceUnmap(NULL) == OS_EOK);
    assert(SHMDeviceMappingSegments(NULL, &count) == NULL);
}

static void
__TestSync(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x701000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;

    __CreateBuffer(pages, 2, 0, 0x2000, SHM_DEVICE, false);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0x1000, 0x1000, SHMDeviceFromDevice, &mapping) == OS_EOK);

    // Ranges count from the mapping, which starts 0x1000 into the buffer.
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1001) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForDevice(mapping, 0x1001, 0) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForDevice(mapping, 0x800, SIZE_MAX) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0) == OS_EINVALPARAMS);

    // While the device owns the mapping it can be neither handed over again
    // nor released, because the device may still be using its pages.
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1000) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheCleanInvalidate, 0x701000, 0x1000 } }, 1);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1000) == OS_EBUSY);
    saved = mapping;
    assert(SHMDeviceUnmap(&mapping) == OS_EBUSY);
    assert(mapping == saved && g_bufferReferences == 2);

    // A short transfer hands back only what was written; a bad range does not
    // change the owner. After that, the cycle can repeat.
    assert(SHMDeviceSyncForCpu(mapping, 0x800, 0x801) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x200) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheInvalidate, 0x701000, 0x200 } }, 1);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x200) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForDevice(mapping, 0x800, 0x800) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheCleanInvalidate, 0x701800, 0x800 } }, 1);

    // A stopped transfer that wrote nothing still gives the mapping back.
    assert(SHMDeviceSyncForCpu(mapping, 0x1000, 0) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK && mapping == NULL);

    assert(SHMDeviceSyncForDevice(NULL, 0, 1) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForCpu(NULL, 0, 0) == OS_EINVALPARAMS);
    assert(SHMDeviceMap(context, 0x20, 0, 0x1000, (enum SHMDeviceDirection)99, &mapping) == OS_EINVALPARAMS);
    assert(mapping == NULL && g_bufferReferences == 1);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}

static void
__TestCacheMaintenance(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceContext* coherent;
    struct SHMDeviceMapping* mapping = NULL;

    // An ordinary cached buffer whose two pages are apart in physical memory,
    // so cache work for one range must be split across both pages.
    __CreateBuffer(pages, 2, 0, 0x2000, 0, false);
    nonCoherent = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    coherent = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);

    // A device that only reads gets cached data written back, nothing more,
    // and its range need not sit on cache line boundaries.
    assert(SHMDeviceMap(nonCoherent, 0x20, 0xf10, 0x1f0, SHMDeviceToDevice, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1f0) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){
        { __CacheClean, 0x700f10, 0xf0 },
        { __CacheClean, 0x702000, 0x100 } }, 2);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x1f0) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);

    // A device that writes gets lines written back and thrown away before,
    // and only the completed bytes thrown away after.
    assert(SHMDeviceMap(nonCoherent, 0x20, 0xf00, 0x200, SHMDeviceFromDevice, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(mapping, 0x80, 0x100) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){
        { __CacheCleanInvalidate, 0x700f80, 0x80 },
        { __CacheCleanInvalidate, 0x702000, 0x80 } }, 2);
    assert(SHMDeviceSyncForCpu(mapping, 0x100, 0x10) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheInvalidate, 0x702000, 0x10 } }, 1);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);

    // A coherent device sees the CPU cache, so no cache work happens at all.
    assert(SHMDeviceMap(coherent, 0x20, 0, 0x2000, SHMDeviceBidirectional, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x2000) == OS_EOK);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x2000) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);

    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&nonCoherent);
    SHMDeviceContextRelease(&coherent);
}

static void
__ExpectMapFailure(struct SHMDeviceContext*, uuid_t, size_t, size_t, oserr_t);

static void
__TestBounce(void)
{
    struct SHMDeviceRange wide = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceRange narrow = { 0x700000, 0x1000, 0x2000 };
    const paddr_t pages[] = { 0x700000, 0x7f0000 };
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceContext* coherent;
    struct SHMDeviceMapping* mapping = NULL;
    const struct SHMDeviceSegment* segments;
    uint32_t count;

    // The second page lies outside both contexts' ranges.
    __CreateBuffer(pages, 2, 0, 0x2000, 0, false);
    nonCoherent = __CreateContext(&wide, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    coherent = __CreateContext(&narrow, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    memset(g_sourceMemory, 0x11, sizeof(g_sourceMemory));

    // A device that writes, off cache line boundaries: it gets zeroed bounce
    // pages, and cache work happens on those instead of the buffer.
    assert(SHMDeviceMap(nonCoherent, 0x20, 0x10, 0x1e0, SHMDeviceFromDevice, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x9000 && segments[0].Length == 0x1e0);
    assert(g_liveViews == 2 && g_bounceMemory[0] == 0);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1e0) == OS_EOK);
    assert(g_bounceMemory[0] == 0);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheCleanInvalidate, 0x708000, 0x1e0 } }, 1);

    // The device fills the range but completes only 0x20 bytes: only those
    // reach the buffer, and its neighbouring bytes stay untouched.
    memset(g_bounceMemory, 0xab, 0x1e0);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x20) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheInvalidate, 0x708000, 0x20 } }, 1);
    assert(g_sourceMemory[0xf] == 0x11 && g_sourceMemory[0x10] == 0xab);
    assert(g_sourceMemory[0x2f] == 0xab && g_sourceMemory[0x30] == 0x11);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(g_liveViews == 0 && g_bounceMemory == NULL);

    // Part of the range is out of reach: what the device reads is copied to
    // reachable bounce pages, across the buffer's page boundary.
    g_bouncePhysical = 0x701000;
    for (size_t i = 0; i < sizeof(g_sourceMemory); i++) {
        g_sourceMemory[i] = (uint8_t)(i * 7);
    }
    assert(SHMDeviceMap(coherent, 0x20, 0x800, 0x1000, SHMDeviceToDevice, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x2000 && segments[0].Length == 0x1000);
    assert(SHMDeviceSyncForDevice(mapping, 0, 0x1000) == OS_EOK);
    assert(memcmp(g_bounceMemory, &g_sourceMemory[0x800], 0x1000) == 0);
    __ExpectCacheCalls(NULL, 0);

    // A device that only reads never changes the buffer.
    memset(g_bounceMemory, 0xcd, 0x1000);
    assert(SHMDeviceSyncForCpu(mapping, 0, 0x1000) == OS_EOK);
    assert(g_sourceMemory[0x801] == (uint8_t)(0x801 * 7));
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);

    // Bounce pages out of reach as well: the mapping fails cleanly.
    g_bouncePhysical = 0x900000;
    __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_ENOENT);
    g_bouncePhysical = 0x701000;

    // Failing to map the buffer view, the bounce pages, or to allocate the
    // page list or the mapping leaves nothing behind either.
    for (int i = 1; i <= 2; i++) {
        g_failMapAt = g_mapCalls + i;
        __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_EOOM);
        g_failMapAt = 0;
    }
    for (unsigned int i = 2; i <= 3; i++) {
        g_failAtCall = g_allocationCalls + i;
        __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_EOOM);
        g_failAtCall = 0;
    }

    g_bouncePhysical = 0x708000;
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&nonCoherent);
    SHMDeviceContextRelease(&coherent);
}

static void
__ExpectAllocateFailure(
    _In_ struct SHMDeviceContext*            context,
    _In_ const struct SHMDeviceRequirements* requirements,
    _In_ oserr_t                             expected)
{
    struct SHMDeviceMapping* mapping = NULL;
    SHMHandle_t              buffer = { 0 };
    unsigned int             live = atomic_load(&g_liveAllocations);

    // A failed allocation must leave no buffer, mapping, view or bounce behind.
    assert(SHMDeviceAllocate(context, requirements, &buffer, &mapping) == expected);
    assert(mapping == NULL && buffer.ID == 0 && g_buffer == NULL);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0 && g_bounceMemory == NULL);
}

static void
__TestAllocate(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    const struct SHMDeviceSegment* segments;
    SHMHandle_t buffer;
    uint32_t count;
    int calls;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);

    // Two neighbouring pages meet every requirement; 0x1800 bytes round up to
    // two whole pages, all of which the single segment covers.
    g_createPages[0] = 0x700000;
    g_createPages[1] = 0x701000;
    assert(SHMDeviceAllocate(context, &(struct SHMDeviceRequirements){ 0x1800, 0x1000, 0x10000, true },
                             &buffer, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x1000 && segments[0].Length == 0x2000);
    assert(buffer.ID == 0x20 && buffer.Buffer != NULL && buffer.Length == 0x2000);
    assert(g_bufferReferences == 2 && g_bounceMemory == NULL);

    // An occupied slot is refused before anything is created.
    saved = mapping;
    calls = g_createCalls;
    assert(SHMDeviceAllocate(context, &(struct SHMDeviceRequirements){ 0x1000, 0, 0, false },
                             &buffer, &mapping) == OS_EBUSY);
    assert(mapping == saved && g_createCalls == calls);

    // Release the mapping, then the buffer, as a caller would.
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK && g_buffer == NULL);

    // Pages apart in memory are two segments: fine unless one is required.
    g_createPages[1] = 0x703000;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x2000, 0, 0, true }, OS_ENOTSUPPORTED);
    assert(SHMDeviceAllocate(context, &(struct SHMDeviceRequirements){ 0x2000, 0, 0, false },
                             &buffer, &mapping) == OS_EOK);
    (void)SHMDeviceMappingSegments(mapping, &count);
    assert(count == 2);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK);

    // The memory obtained misses the alignment, or crosses the boundary.
    g_createPages[1] = 0x701000;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 0x2000, 0, false }, OS_ENOTSUPPORTED);
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x2000, 0, 0x1000, false }, OS_ENOTSUPPORTED);

    // Unreachable memory is reported, and never bounced.
    g_createPages[0] = 0x900000;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 0, 0, false }, OS_ENOENT);
    g_createPages[0] = 0x700000;

    // Requirements no memory could meet are refused without creating anything.
    calls = g_createCalls;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 3, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 0, 0x6000, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x2000, 0, 0x1000, true }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ SIZE_MAX, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(NULL, &(struct SHMDeviceRequirements){ 0x1000, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, NULL, OS_EINVALPARAMS);
    assert(g_createCalls == calls);

    // Failing to create the buffer, or to map it, leaves nothing behind.
    g_createStatus = OS_EOOM;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 0, 0, false }, OS_EOOM);
    g_createStatus = OS_EOK;
    g_failAtCall = g_allocationCalls + 2;
    __ExpectAllocateFailure(context, &(struct SHMDeviceRequirements){ 0x1000, 0, 0, false }, OS_EOOM);
    g_failAtCall = 0;
    SHMDeviceContextRelease(&context);
}

static void
__ExpectMapFailure(
    _In_ struct SHMDeviceContext* context,
    _In_ uuid_t                   id,
    _In_ size_t                   offset,
    _In_ size_t                   length,
    _In_ oserr_t                  expected)
{
    struct SHMDeviceMapping* mapping = NULL;
    unsigned int live = atomic_load(&g_liveAllocations);

    // A failure must give back every reference, allocation and kernel view.
    assert(SHMDeviceMap(context, id, offset, length, SHMDeviceToDevice, &mapping) == expected);
    assert(mapping == NULL && g_bufferReferences == 1 && !g_bufferLocked);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0);
}

static void
__TestMappingFailures(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0, 0x702000 };
    const paddr_t outside[] = { 0x800000 };
    struct SHMDeviceContext* coherent;
    struct SHMDeviceContext* limited;
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    unsigned int calls;

    coherent = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    limited = __CreateContext(&range, 1, SHMDeviceCacheCoherent, 0x2fff);
    nonCoherent = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);

    // The middle page was never allocated, so only ranges avoiding it work.
    __CreateBuffer(pages, 3, 0, 0x3000, 0, false);
    __ExpectMapFailure(coherent, 0x21, 0, 0x1000, OS_ENOENT);
    __ExpectMapFailure(coherent, 0x20, 0, 0x2000, OS_EINCOMPLETE);
    __ExpectMapFailure(coherent, 0x20, 0, 0, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x3001, 1, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x2000, 0x1001, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x2000, SIZE_MAX, OS_EINVALPARAMS);
    __ExpectMapFailure(limited, 0x20, 0x2000, 0x1000, OS_ENOENT);

    // A device that writes on a non-coherent path, off cache line boundaries,
    // is given bounce pages; a device that only reads, or a coherent one, uses
    // the buffer itself.
    assert(SHMDeviceMap(nonCoherent, 0x20, 0x20, 0x40, SHMDeviceFromDevice, &mapping) == OS_EOK);
    assert(g_bounceMemory != NULL);
    SHMDeviceUnmap(&mapping);
    assert(SHMDeviceMap(nonCoherent, 0x20, 0, 0x30, SHMDeviceBidirectional, &mapping) == OS_EOK);
    assert(g_bounceMemory != NULL);
    SHMDeviceUnmap(&mapping);
    assert(SHMDeviceMap(nonCoherent, 0x20, 0x20, 0x30, SHMDeviceToDevice, &mapping) == OS_EOK);
    assert(g_bounceMemory == NULL);
    SHMDeviceUnmap(&mapping);
    assert(SHMDeviceMap(coherent, 0x20, 0x20, 0x30, SHMDeviceFromDevice, &mapping) == OS_EOK);
    assert(g_bounceMemory == NULL);
    SHMDeviceUnmap(&mapping);
    assert(g_liveViews == 0);

    assert(SHMDeviceMap(coherent, 0x20, 0x2000, 0x1000, SHMDeviceToDevice, &mapping) == OS_EOK);
    saved = mapping;
    assert(SHMDeviceMap(coherent, 0x20, 0, 0x1000, SHMDeviceToDevice, &mapping) == OS_EBUSY);
    assert(mapping == saved && g_bufferReferences == 2);
    SHMDeviceUnmap(&mapping);

    // Fail each allocation in turn: the temporary extents, then the mapping.
    for (unsigned int i = 1; i <= 2; i++) {
        calls = g_allocationCalls;
        g_failAtCall = calls + i;
        __ExpectMapFailure(coherent, 0x20, 0x2000, 0x1000, OS_EOOM);
        g_failAtCall = 0;
    }
    assert(DestroyHandle(0x20) == OS_EOK);

    // Memory SHM does not own is refused even when it is reachable.
    __CreateBuffer(outside, 1, 0, 0x1000, SHM_DEVICE, true);
    __ExpectMapFailure(coherent, 0x20, 0, 0x1000, OS_ENOTSUPPORTED);
    assert(DestroyHandle(0x20) == OS_EOK);

    assert(SHMDeviceMap(NULL, 0x20, 0, 1, SHMDeviceToDevice, &mapping) == OS_EINVALPARAMS);
    assert(SHMDeviceMap(coherent, 0x20, 0, 1, SHMDeviceToDevice, NULL) == OS_EINVALPARAMS);
    SHMDeviceContextRelease(&coherent);
    SHMDeviceContextRelease(&limited);
    SHMDeviceContextRelease(&nonCoherent);
}

int
main(void)
{
    // Keep the tests ordered so each case must finish with no live contexts.
    __TestTranslation();
    __TestAliases();
    __TestAddressEdges();
    __TestCreationFailures();
    __TestCopiedStorageAndLifetime();
    __TestConcurrentOwners();
    __TestSegments();
    __TestSegmentFailures();
    __TestSegmentAddressEdges();
    __TestMapping();
    __TestSync();
    __TestCacheMaintenance();
    __TestBounce();
    __TestAllocate();
    __TestMappingFailures();
    assert(atomic_load(&g_liveAllocations) == 0 && g_buffer == NULL && g_liveViews == 0);
    puts("SHM device context: translation, segments, mappings, validation and lifetime passed");
    return 0;
}
