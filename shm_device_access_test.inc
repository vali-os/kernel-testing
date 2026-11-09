#include <shm/access.h>
#include <ddk/barrier.h>

// Model the CPU call site's state checks and ordering around the common core.
static oserr_t
__TestAccessBeginCpu(
    _In_ struct SHMAccess*           access,
    _In_ bool                        write,
    _In_ const struct SHMAccessWait* wait)
{
    oserr_t status;

    status = SHMAccessBegin(
        access,
        0,
        access->Length,
        SHMAccessHolderCpu,
        write,
        wait
    );
    if (status == OS_EOK) {
        dma_mb();
    }
    return status;
}

static oserr_t
__TestAccessEndCpu(
    _In_ struct SHMAccess* access)
{
    if (access->Phase != SHMAccessActive || access->Holder != SHMAccessHolderCpu) {
        return OS_EINVALPARAMS;
    }
    dma_mb();
    SHMAccessEnd(access);
    return OS_EOK;
}

static void
__TestAccessCancelCpu(
    _In_ struct SHMAccess* access)
{
    dma_mb();
    SHMAccessEnd(access);
}

static void
__TestBufferAccess(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* otherContext;
    struct SHMDeviceMapping* reader = NULL;
    struct SHMDeviceMapping* otherReader = NULL;
    struct SHMDeviceMapping* writer = NULL;
    struct SHMDeviceMapping* stream = NULL;
    struct SHMAccess* cpu = NULL;
    struct SHMAccess* otherCpu = NULL;
    struct SHMAccess* streamAccess;
    struct SHMAccess* tail;
    unsigned int allocations;
    int views;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    otherContext = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAToDevice, SHMDeviceMappingPersistent, NULL, &reader) == OS_EOK);
    assert(SHMDeviceMap(otherContext, 0x20, 0, 64, DMAToDevice, SHMDeviceMappingPersistent, NULL, &otherReader) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &writer) == OS_EOK);
    assert(SHMAccessCreate(0x20, 0, 64, &cpu) == OS_EOK);
    assert(SHMAccessCreate(0x20, 0, 64, &otherCpu) == OS_EOK);
    // This stream needs a bounce copy. It must participate in the same gate.
    assert(SHMDeviceMap(context, 0x20, 1, 63, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &stream) == OS_EOK);
    allocations = g_allocationCalls;
    views = g_mapCalls;
    assert(SHMDeviceBeginUse(reader, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) { { __CacheClean, 0x700000, 64 } }, 1);
    assert(SHMDeviceBeginUse(otherReader, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(otherCpu) == OS_EINVALPARAMS);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EBUSY);
    assert(SHMDeviceSyncForDevice(stream, 0, 63, NULL) == OS_EBUSY);
    assert(__TestAccessBeginCpu(otherCpu, true, NULL) == OS_EBUSY);
    assert(SHMAccessDestroy(&cpu) == OS_EBUSY);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);
    assert(__TestAccessEndCpu(cpu) == OS_EINVALPARAMS);
    assert(SHMDeviceEndUse(reader) == OS_EOK);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EBUSY);
    assert(SHMDeviceEndUse(otherReader) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);

    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) { { __CacheCleanInvalidate, 0x700000, 64 } }, 1);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(reader, NULL) == OS_EBUSY);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    __ExpectCacheCalls(NULL, 0);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) { { __CacheInvalidate, 0x700000, 64 } }, 1);

    assert(__TestAccessBeginCpu(cpu, true, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(otherCpu, false, NULL) == OS_EBUSY);
    assert(SHMDeviceBeginUse(otherReader, NULL) == OS_EBUSY);
    assert(SHMDeviceSyncForDevice(stream, 0, 63, NULL) == OS_EBUSY);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);
    assert(SHMDeviceSyncForDevice(stream, 0, 63, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(otherCpu) == OS_EINVALPARAMS);
    assert(SHMDeviceBeginUse(reader, NULL) == OS_EBUSY);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    memset(g_bounceMemory, 0x7a, 63);
    assert(SHMDeviceSyncForCpu(stream, 63) == OS_EOK);
    assert(g_sourceMemory[1] == 0x7a && g_sourceMemory[63] == 0x7a);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(g_allocationCalls == allocations && g_mapCalls == views);

    assert(__TestAccessBeginCpu(otherCpu, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(otherCpu) == OS_EOK);
    assert(__TestAccessEndCpu(otherCpu) == OS_EINVALPARAMS);
    assert(SHMDeviceUnmap(&stream) == OS_EOK);

    // CPU sessions cover their registered range after a partial transfer.
    assert(SHMDeviceMap(context, 0x20, 0, 256, DMAFromDevice, SHMDeviceMappingStreamingDirect, NULL, &stream) == OS_EOK);
    assert(SHMAccessCreate(0x20, 0, 256, &streamAccess) == OS_EOK);
    assert(SHMAccessCreate(0x20, 128, 64, &tail) == OS_EOK);
    assert(SHMDeviceSyncForDevice(stream, 0, 64, NULL) == OS_EOK);
    assert(SHMDeviceSyncForCpu(stream, 64) == OS_EOK);
    assert(__TestAccessBeginCpu(tail, true, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(streamAccess, false, NULL) == OS_EBUSY);
    assert(__TestAccessEndCpu(tail) == OS_EOK);

    // Failed partial DMA admission must not change the CPU session's range.
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    assert(SHMDeviceSyncForDevice(stream, 0, 64, NULL) == OS_EBUSY);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    assert(__TestAccessBeginCpu(tail, true, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(streamAccess, false, NULL) == OS_EBUSY);
    assert(__TestAccessEndCpu(tail) == OS_EOK);
    assert(SHMAccessDestroy(&tail) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]) {
        { __CacheCleanInvalidate, 0x700000, 64 },
        { __CacheInvalidate, 0x700000, 64 },
        { __CacheCleanInvalidate, 0x700000, 64 },
        { __CacheInvalidate, 0x700000, 64 }
    }, 4);
    assert(SHMDeviceUnmap(&stream) == OS_EOK);

    // Unmapping or abandoning idle DMA mappings leaves CPU access active.
    assert(__TestAccessBeginCpu(streamAccess, true, NULL) == OS_EOK);
    assert(SHMDeviceUnmap(&writer) == OS_EOK);
    assert(SHMDeviceAbandon(&otherReader) == OS_EOK);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    assert(__TestAccessEndCpu(streamAccess) == OS_EOK);
    assert(SHMAccessDestroy(&streamAccess) == OS_EOK);
    assert(SHMAccessDestroy(&cpu) == OS_EOK);
    assert(SHMAccessDestroy(&otherCpu) == OS_EOK);
    assert(SHMDeviceUnmap(&writer) == OS_EOK);
    assert(SHMDeviceUnmap(&otherReader) == OS_EOK);
    assert(SHMDeviceUnmap(&reader) == OS_EOK);
    assert(g_buffer->Access.First == NULL && g_buffer->Access.Last == NULL);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(&otherContext);
}

struct __AccessRace {
    struct SHMDeviceMapping* Mapping;
    atomic_uint*             Attempted;
    oserr_t                  Status;
};

static void*
__RaceBufferAccess(
    _In_ void* argument)
{
    struct __AccessRace* race = argument;

    race->Status = SHMDeviceBeginUse(race->Mapping, NULL);
    atomic_fetch_add(race->Attempted, 1);
    while (atomic_load(race->Attempted) != 2) {
        sched_yield();
    }
    if (race->Status == OS_EOK) {
        assert(SHMDeviceEndUse(race->Mapping) == OS_EOK);
    }
    return NULL;
}

static void
__TestConcurrentBufferAccess(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct __AccessRace races[2] = { 0 };
    atomic_uint attempted;
    pthread_t threads[2];
    unsigned int i;
    unsigned int iteration;

    atomic_init(&attempted, 0);
    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    for (i = 0; i < 2; i++) {
        assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &races[i].Mapping) == OS_EOK);
        races[i].Attempted = &attempted;
    }
    for (iteration = 0; iteration < 100; iteration++) {
        atomic_store(&attempted, 0);
        for (i = 0; i < 2; i++) {
            assert(pthread_create(&threads[i], NULL, __RaceBufferAccess, &races[i]) == 0);
        }
        for (i = 0; i < 2; i++) {
            assert(pthread_join(threads[i], NULL) == 0);
        }
        assert((races[0].Status == OS_EOK && races[1].Status == OS_EBUSY) ||
               (races[1].Status == OS_EOK && races[0].Status == OS_EBUSY));
        assert(g_buffer->Access.First == NULL && g_buffer->Access.Last == NULL);
    }
    for (i = 0; i < 2; i++) {
        assert(SHMDeviceUnmap(&races[i].Mapping) == OS_EOK);
    }
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}
