// Mapping constraints apply to translated segments, independently of cache policy.
static void
__TestAlignmentCopies(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    const size_t offsets[] = { 0, 1, 4095 };
    struct DMAMapRequirements requirements = { 2, 2 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    enum SHMDeviceCachePolicy policy;
    enum DMADirection direction;
    uint32_t count;
    uint32_t i;
    size_t offset;
    size_t j;
    int backing;

    for (policy = SHMDeviceCacheCoherent; policy <= SHMDeviceCacheNonCoherent; policy++) {
        context = __CreateContext(&range, 1, policy, UINT64_MAX);
        for (backing = 0; backing < 3; backing++) {
            __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
            if (backing != 0) {
                g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
                g_buffer->Allocation.Cached = backing == 1;
            }
            for (j = 0; j < sizeof(offsets) / sizeof(offsets[0]); j++) {
                offset = offsets[j];
                for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
                    memset(g_sourceMemory, 0x11, sizeof(g_sourceMemory));
                    assert(SHMDeviceMap(context, 0x20, offset, 512, direction,
                        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOK);
                    segments = SHMDeviceMappingSegments(mapping, &count);
                    for (i = 0; i < count; i++) {
                        assert(segments[i].Address % 2 == 0 && segments[i].Length % 2 == 0);
                    }
                    assert((g_bounceMemory != NULL) == (offset != 0));
                    assert(SHMDeviceSyncForDevice(mapping, 0, 512, NULL) == OS_EOK);
                    if (offset != 0) {
                        if (direction != DMAFromDevice) {
                            assert(memcmp(g_bounceMemory, g_sourceMemory + offset, 512) == 0);
                        } else {
                            assert(g_bounceMemory[0] == 0);
                        }
                        memset(g_bounceMemory, 0xab, 512);
                    }
                    assert(SHMDeviceSyncForCpu(mapping, 127) == OS_EOK);
                    if (offset != 0) {
                        assert(g_sourceMemory[offset] == (direction == DMAToDevice ? 0x11 : 0xab));
                        assert(g_sourceMemory[offset + 126] == (direction == DMAToDevice ? 0x11 : 0xab));
                        assert(g_sourceMemory[offset - 1] == 0x11 && g_sourceMemory[offset + 127] == 0x11);
                    }
                    g_cacheCallCount = 0;
                    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
                    assert(g_liveViews == 0 && __PoolAllFree());
                }
            }
            assert(DestroyHandle(0x20) == OS_EOK);
        }
        SHMDeviceContextRelease(&context);
    }
}

static void
__TestAlignmentFailures(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    const struct DMAMapRequirements invalid[] = { {3, 2}, {2, 3}, {2, 1024} };
    struct DMAMapRequirements requirements = { 2, 2 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SystemMemoryAllocation full = { 0 };
    unsigned int calls;
    unsigned int live;
    unsigned int i;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    calls = g_allocationCalls;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
            SHMDeviceMappingStreamingBounce, &invalid[i], &mapping) == OS_EINVALPARAMS);
    }
    assert(g_allocationCalls == calls && mapping == NULL && g_bufferReferences == 1);
    assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
        SHMDeviceMappingStreamingDirect, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(SHMDeviceMap(context, 0x20, 1, 512, DMAFromDevice,
        SHMDeviceMappingPersistent, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_bufferReferences == 1 && g_liveViews == 0 && __PoolAllFree());

    // Each allocation and view failure must release every intermediate resource.
    live = atomic_load(&g_liveAllocations);
    for (i = 1; i <= 3; i++) {
        g_failAtCall = g_allocationCalls + i;
        assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
            SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOOM);
        g_failAtCall = 0;
        assert(mapping == NULL && g_bufferReferences == 1);
        assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0 && __PoolAllFree());
    }
    for (i = 1; i <= 2; i++) {
        g_failMapAt = g_mapCalls + i;
        assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
            SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOOM);
        g_failMapAt = 0;
        assert(mapping == NULL && g_bufferReferences == 1);
        assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0 && __PoolAllFree());
    }
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 0x8000, 0, 0, &full) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOOM);
    assert(SystemMemoryReleaseAllocation(&full) == OS_EOK);
    assert(mapping == NULL && g_bufferReferences == 1 && g_liveViews == 0 && __PoolAllFree());
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}

static void
__TestTranslatedAlignment(void)
{
    // The source is physically aligned but translates to an odd address. The
    // pool translates separately, so only the bounce segments satisfy alignment.
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x1001, 0x2000 },
        { 0x708000, 0x9000, 0x8000 },
        { 0x710000, 0x20000, 0x1000 }
    };
    const paddr_t pages[] = { 0x700000, 0x701000 };
    struct DMAMapRequirements requirements = { 2, 2 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t count;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0, 512, DMAToDevice,
        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOK);
    assert(g_bounceMemory != NULL);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    SHMDeviceContextRelease(&context);

    // If the pool also translates to odd addresses, bouncing cannot fix it.
    ranges[1].DeviceBase++;
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0, 512, DMAToDevice,
        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_bufferReferences == 1 && g_liveViews == 0 && __PoolAllFree());
    SHMDeviceContextRelease(&context);

    // Joined translated pieces must be checked as one segment. An odd range
    // boundary inside a continuous segment does not require a bounce copy.
    ranges[0] = (struct SHMDeviceRange){ 0x700000, 0x1000, 1 };
    ranges[1] = (struct SHMDeviceRange){ 0x700001, 0x1001, 0x1fff };
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0, 512, DMAToDevice,
        SHMDeviceMappingStreamingDirect, &requirements, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x1000 && segments[0].Length == 512);
    assert(g_bounceMemory == NULL);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    SHMDeviceContextRelease(&context);

    // Length constraints apply to every segment, even when addresses align.
    ranges[0] = (struct SHMDeviceRange){ 0x700000, 0x1000, 1 };
    ranges[1] = (struct SHMDeviceRange){ 0x700001, 0x3000, 0x1fff };
    context = __CreateContext(ranges, 2, SHMDeviceCacheCoherent, UINT64_MAX);
    requirements.AddressAlignment = 0;
    assert(SHMDeviceMap(context, 0x20, 0, 512, DMAToDevice,
        SHMDeviceMappingStreamingDirect, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_bufferReferences == 1);
    SHMDeviceContextRelease(&context);

    // A separate pool range repairs segment lengths without padding the request.
    ranges[2] = (struct SHMDeviceRange){ 0x708000, 0x9000, 0x8000 };
    context = __CreateContext(ranges, 3, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceMap(context, 0x20, 0, 512, DMAToDevice,
        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Length == 512 && g_bounceMemory != NULL);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    SHMDeviceContextRelease(&context);
    assert(DestroyHandle(0x20) == OS_EOK);
    puts("DMA alignment: all cache policies, copies, translated segments and failure cleanup passed");
}

static void
__TestLargeMappingAlignment(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x700000, 0x10000 };
    const paddr_t pages[] = { 0x700000 };
    struct DMAMapRequirements requirements = { 8192, 2 };
    struct SystemMemoryAllocation reserved = {0};
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t count;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 1, 0, 4096, SHM_COMMIT);
    // Reserve the first pool page so an allocation with only page alignment
    // would choose an address that fails the requested 8192-byte alignment.
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 4096, 0, 0, &reserved) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 1, 512, DMAToDevice,
        SHMDeviceMappingStreamingBounce, &requirements, &mapping) == OS_EOK);
    segments = SHMDeviceMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x70a000 && segments[0].Length == 512);
    assert(SHMDeviceUnmap(&mapping) == OS_EOK);
    assert(SystemMemoryReleaseAllocation(&reserved) == OS_EOK);
    assert(__PoolAllFree());
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}
