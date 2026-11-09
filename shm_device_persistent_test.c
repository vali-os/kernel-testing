// Include the engine to reach the use-count boundary without billions of calls.
// All normal behavior still runs through the public interface in the shared suite.
#include "../kernel/memory/shm/device.c"
#define main __RunSHMDeviceTests
#include "shm_device_test.c"
#undef main

int
main(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct DMARequirements requirements = { .Length = 4096 };
    SHMHandle_t buffer = { 0 };

    assert(__RunSHMDeviceTests() == 0);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) { { __CacheCleanInvalidate, 0x700000, 64 } }, 1);
    mapping->Persistent.Uses = UINT_MAX - 1;
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK && mapping->Persistent.Uses == UINT_MAX);
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOVERFLOW && mapping->Persistent.Uses == UINT_MAX);
    assert(SHMDeviceUnmap(&mapping) == OS_EBUSY);
    assert(SHMDeviceEndUse(mapping) == OS_EOK && mapping->Persistent.Uses == UINT_MAX - 1);
    __ExpectCacheCalls(NULL, 0);
    mapping->Persistent.Uses = 1;
    assert(SHMDeviceEndUse(mapping) == OS_EOK && mapping->Persistent.Uses == 0);
    __ExpectCacheCalls((struct __CacheCall[]) { { __CacheInvalidate, 0x700000, 64 } }, 1);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    // A streaming transfer remains active after abandonment until completion.
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(mapping, 0, 64, NULL) == OS_EOK);
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    g_expectQuarantine = true;
    SHMDeviceReleaseQuarantine(context);
    g_expectQuarantine = false;
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) {
        { __CacheCleanInvalidate, 0x700000, 64 },
        { __CacheInvalidate, 0x700000, 64 }
    }, 2);
    // A reset must discard bounce bytes whose completion was never reported.
    assert(SHMDeviceMap(context, 0x20, 1, 63, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    memset(g_sourceMemory, 0x11, 64);
    assert(SHMDeviceSyncForDevice(mapping, 0, 63, NULL) == OS_EOK);
    memset(g_bounceMemory, 0x77, 63);
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    assert(g_buffer->Access.Faults == 1);
    SHMDeviceReleaseQuarantine(context);
    assert(g_sourceMemory[1] == 0x11 && g_sourceMemory[63] == 0x11);
    assert(g_buffer->Access.Faults == 0 && g_buffer->Access.First == NULL);
    __ExpectCacheCalls(NULL, 0);
    assert(DestroyHandle(0x20) == OS_EOK);

    // Continuously shared allocations must also reject the counted-use API.
    assert(SHMDeviceAllocate(context, &requirements, &buffer, &mapping) == OS_EOK);
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceEndUse(mapping) == OS_EINVALPARAMS);
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    // The fixture confirms a stop before releasing shared storage.
    SHMDeviceReleaseQuarantine(context);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK);
    SHMDeviceContextRelease(&context);
    assert(atomic_load(&g_liveAllocations) == 0 && __PoolAllFree());
    puts("Persistent DMA: counted lifetime, cache boundaries, overflow and mapping kinds passed");
    return 0;
}
