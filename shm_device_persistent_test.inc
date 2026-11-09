// Persistent use counts protect the mapping while cache work occurs only at
// the CPU/device boundaries, never between queued jobs using the same pages.
static void
__TestPersistentMappings(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x100000000ULL, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    const paddr_t outside[] = { 0x700000, 0x800000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    const struct DMASegment* segments;
    struct __CacheCall prepare[2];
    const struct __CacheCall complete[] = {
        { __CacheInvalidate, 0x700f00, 0x100 },
        { __CacheInvalidate, 0x702000, 0x100 }
    };
    enum SHMDeviceCachePolicy policy;
    enum DMADirection direction;
    uint32_t count;
    unsigned int allocations;
    unsigned int live;
    unsigned int i;
    int views;

    assert(SHMDeviceBeginUse(NULL, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceEndUse(NULL) == OS_EINVALPARAMS);
    for (policy = SHMDeviceCacheNonCoherent; policy <= SHMDeviceCacheCoherent; policy++) {
        for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
            context = __CreateContext(&range, 1, policy, UINT64_MAX);
            __CreateBuffer(pages, 2, 0, 0x2000, 0);
            assert(SHMDeviceMap(context, 0x20, 0xf00, 0x200, direction, SHMDeviceMappingPersistent, NULL, &mapping) == OS_EOK);
            saved = mapping;
            segments = SHMDeviceMappingSegments(mapping, &count);
            assert(count == 2 && segments[0].Address == 0x100000f00ULL);
            assert(segments[1].Address == 0x100002000ULL);
            assert(segments[0].Length == 0x100 && segments[1].Length == 0x100);
            assert(g_bounceMemory == NULL && g_liveViews == 0 && __PoolAllFree());
            __ExpectCacheCalls(NULL, 0);
            assert(SHMDeviceMap(context, 0x20, 0, 64, direction, SHMDeviceMappingPersistent, NULL, &mapping) == OS_EBUSY);
            assert(mapping == saved);
            assert(SHMDeviceSyncForDevice(mapping, 0, 64, NULL) == OS_EINVALPARAMS);
            assert(SHMDeviceSyncForCpu(mapping, 64) == OS_EINVALPARAMS);
            assert(SHMDeviceEndUse(mapping) == OS_EINVALPARAMS);

            // The buffer and context must survive after the caller drops them.
            SHMDeviceContextRelease(&context);
            assert(DestroyHandle(0x20) == OS_EINCOMPLETE);
            allocations = g_allocationCalls;
            views = g_mapCalls;
            for (i = 0; i < 16; i++) {
                assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK);
                prepare[0] = (struct __CacheCall) {
                    direction == DMAToDevice ? __CacheClean : __CacheCleanInvalidate, 0x700f00, 0x100
                };
                prepare[1] = (struct __CacheCall) { prepare[0].Operation, 0x702000, 0x100 };
                __ExpectCacheCalls(prepare, policy == SHMDeviceCacheNonCoherent ? 2 : 0);
                assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK);
                assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK);
                assert(SHMDeviceUnmap(&mapping) == OS_EBUSY && mapping == saved);
                assert(SHMDeviceSyncForCpu(mapping, 0x200) == OS_EINVALPARAMS);
                assert(SHMDeviceEndUse(mapping) == OS_EOK);
                assert(SHMDeviceEndUse(mapping) == OS_EOK);
                __ExpectCacheCalls(NULL, 0);
                assert(SHMDeviceUnmap(&mapping) == OS_EBUSY && g_bufferReferences == 1);
                assert(SHMDeviceMappingSegments(mapping, &count) == segments && count == 2);
                assert(SHMDeviceEndUse(mapping) == OS_EOK);
                __ExpectCacheCalls(complete,
                    policy == SHMDeviceCacheNonCoherent && direction != DMAToDevice ? 2 : 0);
                assert(SHMDeviceEndUse(mapping) == OS_EINVALPARAMS);
                assert(g_allocationCalls == allocations && g_mapCalls == views);
            }
            assert(SHMDeviceUnmap(&mapping) == OS_EOK && mapping == NULL && g_buffer == NULL);
        }
    }

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    live = atomic_load(&g_liveAllocations);
    for (i = 1; i <= 2; i++) {
        g_failAtCall = g_allocationCalls + i;
        assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &mapping) == OS_EOOM);
        g_failAtCall = 0;
        assert(mapping == NULL && g_bufferReferences == 1 && atomic_load(&g_liveAllocations) == live);
    }
    assert(SHMDeviceMap(context, 0x20, 1, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_bufferReferences == 1 && __PoolAllFree());
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAToDevice, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceEndUse(mapping) == OS_EINVALPARAMS);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(DestroyHandle(0x20) == OS_EOK);
    __CreateBuffer(outside, 2, 0, 0x2000, 0);
    assert(SHMDeviceMap(context, 0x20, 0, 0x2000, DMAToDevice, SHMDeviceMappingPersistent, NULL, &mapping) == OS_ENOENT);
    assert(mapping == NULL && g_bufferReferences == 1 && g_bounceMemory == NULL && __PoolAllFree());
    assert(DestroyHandle(0x20) == OS_EOK);

    // An uncached backing still needs counted use protection but no cache work.
    __CreateBuffer(pages, 2, 0, 0x2000, SHM_COMMIT);
    g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
    g_buffer->Allocation.Cached = false;
    assert(SHMDeviceMap(context, 0x20, 1, 63, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceBeginUse(mapping, NULL) == OS_EOK);
    assert(SHMDeviceEndUse(mapping) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}
