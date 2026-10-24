// Compiler instrumentation counts the real translation walks without kernel hooks.
#include "../kernel/memory/ms_shm_device.c"
#define main __ExistingShmDeviceTests
#include "shm_device_test.c"
#undef main

static unsigned int g_segmentWalks;

__attribute__((no_instrument_function)) void
__cyg_profile_func_enter(
    void* function,
    void* caller)
{
    (void)caller;
    if (function == (void*)__WalkSegments) {
        g_segmentWalks++;
    }
}

__attribute__((no_instrument_function)) void
__cyg_profile_func_exit(
    void* function,
    void* caller)
{
    (void)function;
    (void)caller;
}

int
main(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct SHMDeviceSegment* segments;
    paddr_t page;
    uint32_t count;
    unsigned int mode;

    __UsePool(0x708000, false);
    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    for (mode = 0; mode < 2; mode++) {
        page = mode == 0 ? 0x700000 : 0x900000;
        __CreateBuffer(&page, 1, 0, 4096, 0, false);
        g_segmentWalks = 0;
        assert(SHMDeviceMap(context, 0x20, 0, 4096, SHMDeviceFromDevice, &mapping) == OS_EOK);
        // Direct buffers need count + fill. Bouncing adds one reachability probe.
        assert(g_segmentWalks == (mode == 0 ? 2 : 3));
        segments = SHMDeviceMappingSegments(mapping, &count);
        assert(count == 1 && segments[0].Length == 4096);
        assert(segments[0].Address == (mode == 0 ? 0x1000 : 0x9000));
        assert(SHMDeviceUnmap(&mapping) == OS_EOK);
        assert(DestroyHandle(0x20) == OS_EOK);
    }
    SHMDeviceContextRelease(&context);
    assert(atomic_load(&g_liveAllocations) == 0 && g_liveViews == 0 && __PoolAllFree());
    puts("DMA translation walks: direct=2, bounced=3, addresses and cleanup passed");
    return __ExistingShmDeviceTests();
}
