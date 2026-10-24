// Exercise the production syscall registry with an instrumented SHM backend.
// Translation and cache operations themselves are covered by shm_device_test.
#include "../kernel/api/dma.c"
#include "../librt/libds/hashtable.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

struct SHMDeviceContext { int References; };
struct SHMDeviceMapping { struct SHMDeviceContext* Context; int Owned; };
static MSContextSecurity_t g_security = { .Handle = 10 };
static int g_live, g_contexts, g_maps, g_buffers, g_locked;
static int g_copyFailure, g_engineFailure, g_oom;
static int g_indexAllocations;
static int g_indexAllocationCalls;
static int g_indexUnlockedFrees;
static int g_mutexAcquisitions;
static void (*g_beforeIndexFree)(void);
static int g_indexFailAfter = -1;
static int g_hostErrno;
int*
__errno(void)
{
    return &g_hostErrno;
}
static struct SHMDeviceSegment g_segments[] = {
    { 0x100001000ULL, 4096 },
    { 0x200001000ULL, 4096 }
};
static uint32_t g_segmentCount = 1;

void* kmalloc(size_t length) { if (g_oom) return NULL; g_live++; return calloc(1, length); }
void kfree(void* p) { CHECK(p != NULL && !g_locked); g_live--; free(p); }
void*
dsalloc(
    _In_ size_t length)
{
    void* memory;

    g_indexAllocationCalls++;
    if (g_indexFailAfter == 0) {
        return NULL;
    }
    if (g_indexFailAfter > 0) {
        g_indexFailAfter--;
    }
    memory = calloc(1, length);
    CHECK(memory != NULL);
    g_indexAllocations++;
    return memory;
}

void
dsfree(
    _In_ void* memory)
{
    void (*beforeFree)(void);

    CHECK(memory != NULL);
    if (g_beforeIndexFree != NULL) {
        CHECK(!g_locked);
        beforeFree = g_beforeIndexFree;
        g_beforeIndexFree = NULL;
        beforeFree();
    }
    g_indexAllocations--;
    if (!g_locked) {
        g_indexUnlockedFrees++;
    }
    free(memory);
}
void SpinlockAcquireIrq(Spinlock_t* lock) { (void)lock; CHECK(0); }
void SpinlockReleaseIrq(Spinlock_t* lock) { (void)lock; CHECK(0); }
int
list_cmp_default(
    _In_ void* left,
    _In_ void* right)
{
    return left != right;
}

void
MutexLock(
    _In_ Mutex_t* lock)
{
    CHECK(lock == &g_dmaLock && !g_locked);
    CHECK(lock->Flags == MUTEX_FLAG_PLAIN && lock->Owner == UUID_INVALID);
    CHECK(lock->BlockQueue.head == NULL && lock->BlockQueue.tail == NULL);
    g_locked = 1;
    g_mutexAcquisitions++;
}
void
MutexUnlock(
    _In_ Mutex_t* lock)
{
    CHECK(lock == &g_dmaLock && g_locked);
    g_locked = 0;
}
MSContextSecurity_t* MemorySpaceGetSecurityContext(void) { return &g_security; }
oserr_t CapabilityVerify(Capabilities_t c) { CHECK(c == CAPABILITY_DEVICEIO_ACCESS); return OS_EOK; }
oserr_t MemorySpaceCopyUser(void* user, void* kernel, size_t length, bool toUser)
{
    CHECK(!g_locked);
    if (!user || (toUser && g_copyFailure)) return OS_EINVALPARAMS;
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}
oserr_t SHMDeviceContextCreate(const struct SHMDeviceRange* ranges, uint32_t count,
    enum SHMDeviceCachePolicy policy, uint64_t limit, struct SHMDeviceContext** out)
{
    CHECK(count == 1 && ranges[0].DeviceBase == 0x100000000ULL);
    CHECK(policy == SHMDeviceCacheNonCoherent && limit == UINT64_MAX);
    *out = calloc(1, sizeof(**out)); (*out)->References = 1; g_contexts++; return OS_EOK;
}
void SHMDeviceContextRelease(struct SHMDeviceContext** context)
{
    if (*context && --(*context)->References == 0) { free(*context); g_contexts--; }
    *context = NULL;
}
oserr_t SHMDeviceMap(struct SHMDeviceContext* context, uuid_t shm, size_t offset,
    size_t length, enum SHMDeviceDirection direction, struct SHMDeviceMapping** out)
{
    CHECK(!g_locked);
    CHECK(shm == 99 && offset == 16 && length == 64 && direction == SHMDeviceFromDevice);
    if (g_engineFailure) return OS_EOOM;
    *out = calloc(1, sizeof(**out)); (*out)->Context = context; context->References++; g_maps++;
    return OS_EOK;
}
oserr_t SHMDeviceAllocate(struct SHMDeviceContext* context,
    const struct SHMDeviceRequirements* req, SHMHandle_t* buffer, struct SHMDeviceMapping** out)
{
    CHECK(req->Length == 4096);
    if (g_engineFailure) return OS_EOOM;
    *out = calloc(1, sizeof(**out)); (*out)->Context = context; context->References++; g_maps++;
    buffer->ID = 99; g_buffers++; return OS_EOK;
}
oserr_t SHMDetach(SHMHandle_t* buffer) { CHECK(buffer->ID == 99); g_buffers--; return OS_EOK; }
const struct SHMDeviceSegment* SHMDeviceMappingSegments(const struct SHMDeviceMapping* m, uint32_t* count)
{ CHECK(m != NULL); *count = g_segmentCount; return g_segments; }
oserr_t SHMDeviceSyncForDevice(struct SHMDeviceMapping* m, size_t offset, size_t length)
{ CHECK(offset == 0 && length == 64); if (m->Owned) return OS_EBUSY; m->Owned = 1; return OS_EOK; }
oserr_t SHMDeviceSyncForCpu(struct SHMDeviceMapping* m, size_t offset, size_t length)
{ CHECK(offset == 0 && length <= 64); if (!m->Owned) return OS_EINVALPARAMS; m->Owned = 0; return OS_EOK; }
oserr_t SHMDeviceUnmap(struct SHMDeviceMapping** mapping)
{
    CHECK(!g_locked);
    if (*mapping == NULL) return OS_EOK;
    if ((*mapping)->Owned) return OS_EBUSY;
    SHMDeviceContextRelease(&(*mapping)->Context); free(*mapping); *mapping = NULL; g_maps--; return OS_EOK;
}

#include "dma_index_test.inc"

int main(void)
{
    struct OSDmaOpenParameters parameters = { .AddressLimit = UINT64_MAX,
        .Configuration = { .Count = 1, .CachePolicy = SHMDeviceCacheNonCoherent,
            .Ranges = { { .DeviceBase = 0x100000000ULL, .Length = 0x10000 } } } };
    struct OSDmaMapParameters map = { .Offset = 16, .Length = 64, .Direction = SHMDeviceFromDevice };
    struct OSDmaMappingResult result = { 0 };
    struct OSDmaAllocationResult allocation = { 0 };
    struct SHMDeviceRequirements requirements = { .Length = 4096 };
    struct SHMDeviceSegment segment;
    uuid_t context = 0;
    uint32_t count = 0;

    g_oom = 1;
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EOOM && context == 0);
    g_oom = 0; g_copyFailure = 1;
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EINVALPARAMS && g_contexts == 0 && g_live == 0);
    g_copyFailure = 0;
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EOK);
    g_security.Handle = 11;
    CHECK(ScDmaClose(context) == OS_EPERMISSIONS);
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EPERMISSIONS);
    g_security.Handle = 10; g_copyFailure = 1;
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EINVALPARAMS);
    CHECK(g_buffers == 0 && g_maps == 0 && g_contexts == 1 && g_live == 1);
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS && g_maps == 0);
    g_copyFailure = 0; g_engineFailure = 1;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOOM && g_live == 1);
    g_engineFailure = 0;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    CHECK(result.SegmentCount == 1 && result.InlineSegment.Address == g_segments[0].Address);
    CHECK(result.InlineSegment.Length == g_segments[0].Length);
    CHECK(ScDmaClose(context) == OS_EBUSY);
    CHECK(ScDmaClose(result.Handle) == OS_EINVALPARAMS);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 0, &count) == OS_EBUFFER && count == 1);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 1, &count) == OS_EOK && segment.Address == g_segments[0].Address);
    g_security.Handle = 11;
    CHECK(ScDmaUnmap(result.Handle) == OS_EPERMISSIONS);
    g_security.Handle = 10;
    CHECK(ScDmaSyncForDevice(result.Handle, 0, 64) == OS_EOK);
    CHECK(ScDmaUnmap(result.Handle) == OS_EBUSY);
    CHECK(ScDmaSyncForCpu(result.Handle, 0, 7) == OS_EOK);
    CHECK(ScDmaUnmap(result.Handle) == OS_EOK);
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EOK);
    CHECK(allocation.Mapping.SegmentCount == 1);
    CHECK(allocation.Mapping.InlineSegment.Address == g_segments[0].Address);
    CHECK(allocation.Mapping.InlineSegment.Length == g_segments[0].Length);
    CHECK(ScDmaUnmap(allocation.Mapping.Handle) == OS_EOK);
    CHECK(SHMDetach(&allocation.Buffer) == OS_EOK);
    // Multi-segment replies must initialize the unused inline bytes rather
    // than copy kernel stack contents to the caller.
    g_segmentCount = 2;
    memset(&result, 0xff, sizeof(result));
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    CHECK(result.SegmentCount == 2);
    CHECK(result.InlineSegment.Address == 0 && result.InlineSegment.Length == 0);
    CHECK(ScDmaUnmap(result.Handle) == OS_EOK);
    g_segmentCount = 1;
    CHECK(ScDmaClose(context) == OS_EOK);
    CHECK(ScDmaClose(context) == OS_ENOENT);
    CHECK(g_live == 0 && g_maps == 0 && g_contexts == 0 && g_buffers == 0);
    __TestHandleIndex(&parameters, &map);
    __TestDefaultTableShrinking();
    __TestDetachedIndex(parameters);
    puts("DMA syscalls: ownership, live-map close, copy rollback, translation and busy unmap passed");
    return 0;
}
