// The real wrappers run against both the real unavailable gate and a controlled
// future-kernel model. The model tests transport and ownership, not hardware DMA.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <os/types/syscall.h>
#include <os/handle.h>

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
#undef free
#undef calloc
#undef malloc
#include "../kernel/api/dma.c"

static int g_mock;
static int g_calls;
static int g_segmentCalls;
static int g_contexts;
static int g_mappings;
static int g_shmOwners;
static int g_deviceOwned;
static int g_badCount;
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
    // supplied only in the model; normal calls reach each production gate.
    g_calls++;
    assert(arg4 == 0);
    if (!g_mock) {
        switch (function) {
            case 63:
                return ScDmaOpen((uuid_t)arg0, (const void*)(uintptr_t)arg1, (void*)(uintptr_t)arg2);
            case 64:
                return ScDmaAllocate((uuid_t)arg0, requirements, allocation);
            case 65:
                return ScDmaMapShm((uuid_t)arg0, (uuid_t)arg1, parameters, mapping);
            case 66:
                return ScDmaGetSegments((uuid_t)arg0, (void*)(uintptr_t)arg1,
                    (uint32_t)arg2, (void*)(uintptr_t)arg3);
            case 67:
                return ScDmaSyncForDevice((uuid_t)arg0, (size_t)arg1, (size_t)arg2);
            case 68:
                return ScDmaSyncForCpu((uuid_t)arg0, (size_t)arg1, (size_t)arg2);
            case 69:
                return ScDmaUnmap((uuid_t)arg0);
            case 70:
                return ScDmaClose((uuid_t)arg0);
            default:
                assert(0);
        }
    }
    if (function == g_failSyscall) {
        return g_failStatus;
    }
    switch (function) {
        case 63:
            assert(arg0 == 7 && arg3 == 0);
            assert(*(const uint64_t*)(uintptr_t)arg1 == UINT64_MAX);
            *(uuid_t*)(uintptr_t)arg2 = 101;
            g_contexts++;
            break;
        case 64:
            assert(arg0 == 101 && arg3 == 0 && g_contexts == 1 && g_mappings == 0);
            assert(requirements->Length == 256 && requirements->Alignment == 64);
            assert(requirements->Boundary == 0x10000 && requirements->Contiguous);
            allocation->Buffer = (SHMHandle_t) { .ID = 303, .Buffer = g_buffer, .Length = 4096, .Capacity = 4096 };
            allocation->Mapping.Handle = 202;
            allocation->Mapping.SegmentCount = 1;
            g_mappings++;
            break;
        case 65:
            assert(arg0 == 101 && g_contexts == 1 && g_mappings == 0);
            assert(arg1 == 303 && parameters->Offset == 128 && parameters->Length == 512);
            assert(parameters->Direction == SHMDeviceFromDevice);
            mapping->Handle = 202;
            mapping->SegmentCount = 1;
            g_mappings++;
            break;
        case 66:
            assert(arg0 == 202 && g_mappings == 1 && arg2 == 1);
            ((struct SHMDeviceSegment*)(uintptr_t)arg1)[0] = (struct SHMDeviceSegment) { 0x100001234ULL, 512 };
            *(uint32_t*)(uintptr_t)arg3 = g_badCount ? 2 : 1;
            g_segmentCalls++;
            break;
        case 67:
            assert(arg0 == 202 && g_mappings == 1 && arg3 == 0);
            g_lastOffset = (size_t)arg1;
            g_lastLength = (size_t)arg2;
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_deviceOwned = 1;
            break;
        case 68:
            assert(arg0 == 202 && g_mappings == 1 && arg3 == 0);
            g_lastOffset = (size_t)arg1;
            g_lastLength = (size_t)arg2;
            if (!g_deviceOwned) {
                return OS_EINVALPARAMS;
            }
            g_deviceOwned = 0;
            break;
        case 69:
            assert(arg0 == 202 && g_mappings == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_mappings--;
            break;
        case 70:
            assert(arg0 == 101 && g_contexts == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
            g_contexts--;
            break;
        default:
            assert(0);
    }
    return OS_EOK;
}

static void
__TestGate(void)
{
    struct OSDmaAllocationResult allocation;
    struct OSDmaAllocationResult before;
    struct OSDmaMappingResult mapping = { 123, 456 };
    struct SHMDeviceSegment segment = { 789, 987 };
    uuid_t handle = 55;
    uuid_t dmaHandle;
    uint32_t count = 66;
    int calls;

    // Every operation has its own unavailable entry point. Guessed handles and
    // even inaccessible pointer arguments must leave output untouched while gated.
    memset(&allocation, 0xa5, sizeof(allocation));
    before = allocation;
    assert(Syscall_DmaOpen(7, NULL, &handle) == OS_ENOTSUPPORTED && handle == 55);
    assert(Syscall_DmaAllocate(101, NULL, &allocation) == OS_ENOTSUPPORTED);
    assert(memcmp(&before, &allocation, sizeof(allocation)) == 0);
    assert(Syscall_DmaMapShm(101, 303, NULL, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping.Handle == 123 && mapping.SegmentCount == 456);
    assert(Syscall_DmaGetSegments(202, &segment, 1, &count) == OS_ENOTSUPPORTED);
    assert(segment.Address == 789 && segment.Length == 987 && count == 66);
    assert(Syscall_DmaSyncForDevice(202, 0, 512) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaSyncForCpu(202, 0, 0) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaUnmap(202) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaClose(101) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaOpen(7, (void*)1, (void*)1) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaAllocate(101, (void*)1, (void*)1) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaMapShm(101, 303, (void*)1, (void*)1) == OS_ENOTSUPPORTED);
    assert(Syscall_DmaGetSegments(202, (void*)1, UINT32_MAX, (void*)1) == OS_ENOTSUPPORTED);
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_ENOTSUPPORTED && dmaHandle == UUID_INVALID);
    calls = g_calls;
    assert(DMAOpen(UUID_INVALID, UINT64_MAX, &dmaHandle) == OS_EINVALPARAMS);
    assert(DMAOpen(7, UINT64_MAX, NULL) == OS_EINVALPARAMS);
    g_failAfter = 0;
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EOOM);
    g_failAfter = -1;
    assert(g_calls == calls && g_allocations == 0);
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
    g_mock = 1;
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
    g_failSyscall = 64;
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
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && count == 1);
    assert(segments[0].Address == 0x100001234ULL);
    calls = g_calls;
    assert(DMASyncForDevice(mapping, 0, 256) == OS_EINVALPARAMS);
    assert(DMASyncForCpu(mapping, 0, 256) == OS_EINVALPARAMS);
    assert(g_calls == calls);
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL && g_shmOwners == 1);
    // Stand in for libos's SHM destructor after the mapping has gone.
    assert(g_mappings == 0);
    __Free(buffer.Payload);
    g_shmOwners--;
    assert(DMAClose(dmaHandle) == OS_EOK && dmaHandle == UUID_INVALID);
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

    // Importing SHM must not create a new userspace SHM owner. Segment queries
    // may fail and be retried without losing the mapping or changing outputs.
    assert(DMAOpen(7, UINT64_MAX, &dmaHandle) == OS_EOK);
    g_failAfter = 0;
    assert(DMAMapSHM(dmaHandle, &buffer, 128, 512, SHMDeviceFromDevice, &mapping) == OS_EOOM);
    g_failAfter = -1;
    g_failSyscall = 65;
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
    g_failSyscall = 66;
    g_failStatus = OS_EBUFFER;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EBUFFER);
    g_failSyscall = -1;
    g_badCount = 1;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EUNKNOWN);
    assert(segments == NULL && count == 99);
    g_badCount = 0;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && count == 1);
    assert(segments[0].Address > UINT32_MAX);
    cached = segments;
    calls = g_segmentCalls;
    assert(DMAGetSegments(mapping, &segments, &count) == OS_EOK && cached == segments);
    assert(g_segmentCalls == calls);

    // A failed close retains its owner; successful close does not close mappings.
    g_failSyscall = 70;
    g_failStatus = OS_EBUSY;
    assert(DMAClose(dmaHandle) == OS_EBUSY && dmaHandle != UUID_INVALID);
    g_failSyscall = -1;
    assert(DMAClose(dmaHandle) == OS_EOK && dmaHandle == UUID_INVALID && g_mappings == 1);
    assert(DMAClose(dmaHandle) == OS_EOK && DMAClose(UUID_INVALID) == OS_EOK);
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
    g_failSyscall = 69;
    g_failStatus = OS_EUNKNOWN;
    assert(DMAUnmap(&mapping) == OS_EUNKNOWN && mapping == saved);
    g_failSyscall = -1;
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL);
    assert(DMAUnmap(&mapping) == OS_EOK && DMAUnmap(NULL) == OS_EOK);
    assert(g_allocations == 0 && g_contexts == 0 && g_mappings == 0);
}

int
main(void)
{
    // Both the production unavailable path and the staged success contract matter.
    __TestGate();
    __TestAllocation();
    __TestStreaming();
    puts("DDK DMA: eight separate syscalls, unavailable gates, separate ownership, query retries, 64-bit addresses and busy cleanup passed");
    return 0;
}
