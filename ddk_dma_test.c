// Real DMA wrappers with controlled syscall and provider transport failures.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef assert
#define assert(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include <os/types/syscall.h>
#include <os/handle.h>

_Static_assert(sizeof(struct OSDmaMappingResult) == 24, "DMA mapping result ABI");
_Static_assert(offsetof(struct OSDmaMappingResult, InlineSegment) == 8, "DMA inline segment ABI");

static int g_allocations;
static int g_failAfter = -1;

static void*
__Allocate(size_t size)
{
    void* result;

    // Fail a chosen bookkeeping allocation to check that no kernel owner leaks.
    if (g_failAfter == 0) {
        return NULL;
    }
    if (g_failAfter > 0) {
        g_failAfter--;
    }
    result = malloc(size);
    assert(result != NULL);
    g_allocations++;
    return result;
}

static void*
__Calloc(size_t count, size_t size)
{
    void* result = __Allocate(count * size);

    // Mirror calloc so the production wrapper gets its usual zeroed owners.
    if (result != NULL) {
        memset(result, 0, count * size);
    }
    return result;
}

static void
__Free(void* pointer)
{
    // Track wrapper allocations independently of the fake hardware storage.
    if (pointer != NULL) {
        g_allocations--;
        free(pointer);
    }
}

#define malloc __Allocate
#define calloc __Calloc
#define free __Free
#include "../librt/libddk/dma.c"
#include "../librt/libddk/dma_context.c"
#undef free
#undef calloc
#undef malloc


static uint64_t g_session = 41;
static int g_releaseFailure;
static int g_releases;

oserr_t
OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest request, void* data, size_t length)
{
    assert(device == 7);
    if (request == OSIOCTLREQUEST_DMA_QUERY) {
        struct OSDmaConfiguration* result = data;
        assert(length == sizeof(*result));
        memset(result, 0, sizeof(*result));
        result->Session = g_session;
        result->Count = 1;
        result->CachePolicy = SHMDeviceCacheCoherent;
        result->Ranges[0].Length = UINT64_MAX;
        return OS_EOK;
    }
    assert(request == OSIOCTLREQUEST_DMA_RELEASE && length == sizeof(uint64_t));
    assert(*(uint64_t*)data == g_session);
    g_releases++;
    return g_releaseFailure ? OS_EUNKNOWN : OS_EOK;
}
#ifdef MOLLENOS
void usched_call_once(struct usched_once_flag* flag, void (*fn)(void)) { static int done; (void)flag; if (!done) { done = 1; fn(); } }
void usched_mtx_init(struct usched_mtx* m, int type) { (void)m; (void)type; }
void usched_mtx_lock(struct usched_mtx* m) { (void)m; }
void usched_mtx_unlock(struct usched_mtx* m) { (void)m; }
#endif

static int g_calls;
static int g_segmentCalls;
static int g_contexts;
static int g_mappings;
static int g_shmOwners;
static int g_deviceOwned;
static int g_badCount;
static uint32_t g_resultSegmentCount = 1;
static int g_failSyscall = -1;
static oserr_t g_failStatus;
static size_t g_lastOffset;
static size_t g_lastLength;
static unsigned char g_buffer[4096];

oserr_t
OSHandleWrap(uuid_t id, enum OSHandleType type, void* payload, bool ownership, OSHandle_t* out)
{
    // Reproduce the infallible wrapper operation; SHM's actual attachment
    // lifetime is covered by the existing SHM memory ownership tests.
    assert(type == OSHANDLE_SHM && ownership && payload != NULL);
    *out = (OSHandle_t) { .ID = id, .Type = type, .Payload = payload };
    g_shmOwners++;
    return OS_EOK;
}

SCTYPE
_syscall(SCTYPE function, SCTYPE arg0, SCTYPE arg1, SCTYPE arg2, SCTYPE arg3, SCTYPE arg4)
{
    const struct SHMDeviceRequirements* requirements = (const void*)(uintptr_t)arg1;
    const struct OSDmaMapParameters* parameters = (const void*)(uintptr_t)arg2;
    struct OSDmaAllocationResult* allocation = (void*)(uintptr_t)arg2;
    struct OSDmaMappingResult* mapping = (void*)(uintptr_t)arg3;

    // Use the real syscall macros and runtime argument forwarding. Success is
    // supplied by the model; production registry coverage is a separate test.
    g_calls++;
    assert(arg4 == 0);
    if (function == g_failSyscall) {
        return g_failStatus;
    }
    switch (function) {
        case 78:
            assert(arg0 == 7 && arg3 == 0);
            assert(*(const uint64_t*)(uintptr_t)arg1 == UINT64_MAX);
            *(uuid_t*)(uintptr_t)arg2 = 101;
            g_contexts++;
            break;
        case 79:
            assert(arg0 == 101 && arg3 == 0 && g_contexts == 1 && g_mappings == 0);
            assert(requirements->Length == 256 && requirements->Alignment == 64);
            assert(requirements->Boundary == 0x10000 && requirements->Contiguous);
            allocation->Buffer = (SHMHandle_t) { .ID = 303, .Buffer = g_buffer, .Length = 4096, .Capacity = 4096 };
            allocation->Mapping.Handle = 202;
            allocation->Mapping.SegmentCount = 1;
            allocation->Mapping.InlineSegment = (struct SHMDeviceSegment) { 0x100001234ULL, 4096 };
            g_mappings++;
            break;
        case 80:
            assert(arg0 == 101 && g_contexts == 1 && g_mappings == 0);
            assert(arg1 == 303 && parameters->Offset == 128 && parameters->Length == 512);
            assert(parameters->Direction == SHMDeviceFromDevice);
            mapping->Handle = 202;
            mapping->SegmentCount = g_resultSegmentCount;
            mapping->InlineSegment = (struct SHMDeviceSegment) { 0 };
            if (g_resultSegmentCount == 1) {
                mapping->InlineSegment = (struct SHMDeviceSegment) { 0x100001234ULL, 512 };
            }
            g_mappings++;
            break;
        case 81:
            assert(arg0 == 202 && g_mappings == 1 && arg2 == 2);
            ((struct SHMDeviceSegment*)(uintptr_t)arg1)[0] = (struct SHMDeviceSegment) { 0x100001234ULL, 256 };
            ((struct SHMDeviceSegment*)(uintptr_t)arg1)[1] = (struct SHMDeviceSegment) { 0x200001234ULL, 256 };
            *(uint32_t*)(uintptr_t)arg3 = g_badCount ? 3 : 2;
            g_segmentCalls++;
            break;
        case 82:
            assert(arg0 == 202 && g_mappings == 1 && arg3 == 0);
            g_lastOffset = (size_t)arg1;
            g_lastLength = (size_t)arg2;
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_deviceOwned = 1;
            break;
        case 83:
            assert(arg0 == 202 && g_mappings == 1 && arg3 == 0);
            g_lastOffset = (size_t)arg1;
            g_lastLength = (size_t)arg2;
            if (!g_deviceOwned) {
                return OS_EINVALPARAMS;
            }
            g_deviceOwned = 0;
            break;
        case 84:
            assert(arg0 == 202 && g_mappings == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_mappings--;
            break;
        case 85:
            assert(arg0 == 101 && g_contexts == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
            if (g_mappings != 0) return OS_EBUSY;
            g_contexts--;
            break;
        default:
            assert(0);
    }
    return OS_EOK;
}

static void
__TestAllocation(void)
{
    struct DMAMapping* mapping = NULL;
    struct SHMDeviceRequirements requirements = { 256, 64, 0x10000, true };
    OSHandle_t buffer = { 0 };
    OSHandle_t before;
    const struct SHMDeviceSegment* segments;
    uint32_t count;
    int calls;
    int allocations;
    uuid_t dmaHandle = UUID_INVALID;

    // An allocation has a separately owned SHM handle. Validate both outputs
    // before creation and reserve local storage before asking for kernel pages.
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EOK);
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EBUSY);
    allocations = g_allocations;
    calls = g_calls;
    for (int i = 0; i < 2; i++) {
        g_failAfter = i;
        assert(DMAAllocate(dmaHandle, &requirements, &buffer, &mapping) == OS_EOOM);
        assert(mapping == NULL && buffer.ID == UUID_INVALID && g_allocations == allocations);
    }
    g_failAfter = -1;
    assert(g_calls == calls);
    g_failSyscall = 79;
    g_failStatus = OS_ENOENT;
    before = buffer;
    assert(DMAAllocate(dmaHandle, &requirements, &buffer, &mapping) == OS_ENOENT);
    assert(memcmp(&buffer, &before, sizeof(buffer)) == 0 && mapping == NULL);
    assert(g_allocations == allocations);
    g_failSyscall = -1;
    buffer.ID = 55;
    assert(DMAAllocate(dmaHandle, &requirements, &buffer, &mapping) == OS_EBUSY);
    buffer.ID = 0;
    assert(DMAAllocate(dmaHandle, &requirements, &buffer, &mapping) == OS_EOK);
    assert(buffer.Type == OSHANDLE_SHM && buffer.ID == 303 && g_shmOwners == 1);
    assert(((SHMHandle_t*)buffer.Payload)->Buffer == g_buffer);
    assert(DMAAllocate(dmaHandle, &requirements, &buffer, &mapping) == OS_EBUSY);
    calls = g_calls;
    allocations = g_allocations;
    g_failAfter = 0;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && count == 1);
    g_failAfter = -1;
    assert(g_calls == calls && g_allocations == allocations);
    assert(segments[0].Address == 0x100001234ULL && segments[0].Length == 4096);
    calls = g_calls;
    assert(DMASyncForDevice(mapping, 0, 256) == OS_EINVALPARAMS);
    assert(DMASyncForCpu(mapping, 0, 256) == OS_EINVALPARAMS);
    assert(g_calls == calls);
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL && g_shmOwners == 1);
    // Stand in for libos's SHM destructor after the mapping has gone.
    assert(g_mappings == 0);
    __Free(buffer.Payload);
    g_shmOwners--;
    g_releaseFailure = 1;
    assert(DMAClose(dmaHandle) == OS_EUNKNOWN && g_contexts == 0);
    calls = g_calls;
    g_releaseFailure = 0;
    assert(DMAClose(dmaHandle) == OS_EOK && g_calls == calls);
    assert(g_allocations == 0);
}

static void
__TestStreaming(void)
{
    struct DMAMapping* mapping = NULL;
    struct DMAMapping* saved;
    OSHandle_t buffer = { .ID = 303, .Type = OSHANDLE_SHM };
    const struct SHMDeviceSegment* segments = NULL;
    const struct SHMDeviceSegment* cached;
    uint32_t count = 99;
    int calls;
    uuid_t dmaHandle = UUID_INVALID;

    // Fragmented SHM must not create a new userspace SHM owner. Segment queries
    // may fail and be retried without losing the mapping or changing outputs.
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EOK);
    g_resultSegmentCount = 2;
    g_failAfter = 0;
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EOOM);
    g_failAfter = -1;
    g_failSyscall = 80;
    g_failStatus = OS_EINCOMPLETE;
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EINCOMPLETE);
    assert(mapping == NULL);
    g_failSyscall = -1;
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EOK);
    assert(g_shmOwners == 0);
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EBUSY);
    g_failAfter = 0;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOOM);
    g_failAfter = -1;
    g_failSyscall = 81;
    g_failStatus = OS_EBUFFER;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EBUFFER);
    g_failSyscall = -1;
    g_badCount = 1;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EUNKNOWN);
    assert(segments == NULL && count == 99);
    g_badCount = 0;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && count == 2);
    assert(segments[0].Address == 0x100001234ULL && segments[0].Length == 256);
    assert(segments[1].Address == 0x200001234ULL && segments[1].Length == 256);
    cached = segments;
    calls = g_segmentCalls;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && cached == segments);
    assert(g_segmentCalls == calls);

    // A failed close retains its owner; successful close does not close mappings.
    g_failSyscall = 85;
    g_failStatus = OS_EBUSY;
    assert(DMAClose(dmaHandle) == OS_EBUSY && dmaHandle != UUID_INVALID);
    g_failSyscall = -1;
    assert(DMAClose(dmaHandle) == OS_EBUSY && g_mappings == 1);
    calls = g_calls;
    assert(DMASyncForDevice(mapping, 0, 0) == OS_EINVALPARAMS);
    assert(DMASyncForDevice(mapping, 500, 20) == OS_EINVALPARAMS);
    assert(DMASyncForCpu(mapping, SIZE_MAX, 0) == OS_EINVALPARAMS);
    assert(g_calls == calls);
    assert(DMASyncForDevice(mapping, 16, 256) == OS_EOK);
    assert(g_lastOffset == 16 && g_lastLength == 256);
    assert(DMASyncForDevice(mapping, 0, 512) == OS_EBUSY);
    saved = mapping;
    assert(DMAUnmap(&mapping) == OS_EBUSY && mapping == saved);
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && segments == cached);
    assert(DMASyncForCpu(mapping, 16, 7) == OS_EOK);
    assert(g_lastOffset == 16 && g_lastLength == 7);
    assert(DMASyncForDevice(mapping, 0, 512) == OS_EOK);
    assert(DMASyncForCpu(mapping, 0, 0) == OS_EOK && g_lastLength == 0);
    g_failSyscall = 84;
    g_failStatus = OS_EUNKNOWN;
    assert(DMAUnmap(&mapping) == OS_EUNKNOWN && mapping == saved);
    g_failSyscall = -1;
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL);
    assert(DMAUnmap(&mapping) == OS_EOK && DMAUnmap(NULL) == OS_EOK);
    assert(DMAClose(dmaHandle) == OS_EOK);
    assert(g_allocations == 0 && g_contexts == 0 && g_mappings == 0);
    g_resultSegmentCount = 1;
}

static void
__TestInlineStreaming(void)
{
    struct DMAMapping* mapping = NULL;
    OSHandle_t buffer = { .ID = 303, .Type = OSHANDLE_SHM };
    const struct SHMDeviceSegment* segments;
    const struct SHMDeviceSegment* cached;
    uint32_t count;
    uuid_t dmaHandle = UUID_INVALID;
    int calls;
    int allocations;

    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EOK);
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EOK);
    calls = g_calls;
    allocations = g_allocations;
    // Neither allocation failure nor a segment-query failure can affect the
    // inline path. Its borrowed pointer remains valid until successful unmap.
    g_failAfter = 0;
    g_failSyscall = 81;
    g_failStatus = OS_EUNKNOWN;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && count == 1);
    assert(segments[0].Address == 0x100001234ULL && segments[0].Length == 512);
    cached = segments;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && segments == cached);
    assert(g_calls == calls && g_allocations == allocations);
    g_failAfter = -1;
    g_failSyscall = -1;
    assert(DMASyncForDevice(mapping, 0, 512) == OS_EOK);
    assert(DMAUnmap(&mapping) == OS_EBUSY);
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && segments == cached);
    assert(DMASyncForCpu(mapping, 0, 7) == OS_EOK);
    assert(DMAUnmap(&mapping) == OS_EOK);
    assert(DMAClose(dmaHandle) == OS_EOK);
    assert(g_allocations == 0 && g_contexts == 0 && g_mappings == 0);
}

int
main(void)
{
    // Check wrappers and both stages of context teardown.

    __TestAllocation();
    __TestStreaming();
    __TestInlineStreaming();
    puts("DDK DMA: allocation-free inline segments, fragmented query retries, 64-bit addresses and busy cleanup passed");
    return 0;
}
