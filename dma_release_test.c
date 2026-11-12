// Linker wrapping injects address-release failures without production hooks.
#include "../kernel/memory/shm/device.c"
#define main __ExistingShmDeviceTests
#include "shm_device_test.c"
#undef main

static struct DMAAddressMapping* g_failedAddress;
static bool                     g_failAllReleases;
static unsigned int             g_completedReleases;

oserr_t
__real_DMAAddressMappingRelease(
    _InOut_ struct DMAAddressMapping** mapping);

oserr_t
__wrap_DMAAddressMappingRelease(
    _InOut_ struct DMAAddressMapping** mapping)
{
    if (g_failAllReleases || *mapping == g_failedAddress) {
        return OS_EDEVFAULT;
    }
    g_completedReleases++;
    return __real_DMAAddressMappingRelease(mapping);
}

static void
__TestAddressReleaseRetry(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x1000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    paddr_t page = 0x700000;
    unsigned int live;
    uint32_t count = 99;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    saved = mapping;
    live = atomic_load(&g_liveAllocations);
    g_failedAddress = mapping->AddressMapping;

    assert(SHMDeviceUnmap(&mapping) == OS_EDEVFAULT && mapping == saved);
    assert(atomic_load(&g_liveAllocations) == live && g_bufferKernelReferences == 1);
    assert(SHMDeviceMappingSegments(mapping, &count) == NULL && count == 0);
    assert(SHMDeviceSyncForDevice(mapping, 0, 64, NULL) == OS_EBUSY);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceUnmap(&mapping) == OS_EDEVFAULT && mapping == saved);

    // With no process left to retry, the context must keep this idle mapping.
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    assert(context->Quarantine == saved && g_bufferKernelReferences == 1);
    assert(SHMDeviceReleaseQuarantine(context) == OS_EDEVFAULT);
    assert(context->Quarantine == saved && atomic_load(&g_liveAllocations) == live);
    g_failedAddress = NULL;
    assert(SHMDeviceReleaseQuarantine(context) == OS_EOK);
    assert(context->Quarantine == NULL && g_bufferKernelReferences == 0);
    assert(SHMDeviceReleaseQuarantine(context) == OS_EOK);
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EOK && context == NULL);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestQuarantinePartialProgress(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x1000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* failed = NULL;
    struct SHMDeviceMapping* successful = NULL;
    struct SHMDeviceMapping* saved;
    paddr_t page = 0x700000;
    unsigned int completed;
    int cacheCalls;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    g_cacheCallCount = 0;
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice,
        SHMDeviceMappingPersistent, NULL, &failed) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 128, 64, DMAFromDevice,
        SHMDeviceMappingPersistent, NULL, &successful) == OS_EOK);
    assert(SHMDeviceBeginUse(failed, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(failed, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(successful, NULL) == OS_EOK);
    saved = failed;
    g_failedAddress = failed->AddressMapping;
    assert(SHMDeviceAbandon(&successful) == OS_EOK && successful == NULL);
    assert(SHMDeviceAbandon(&failed) == OS_EOK && failed == NULL);
    assert(g_bufferKernelReferences == 2);

    completed = g_completedReleases;
    g_cacheCallCount = 0;
    // Quarantine is a stack: the first entry fails, but the next must be freed.
    assert(SHMDeviceReleaseQuarantine(context) == OS_EDEVFAULT);
    assert(g_completedReleases == completed + 1 && g_bufferKernelReferences == 1);
    assert(context->Quarantine == saved && saved->QuarantineNext == NULL);
    assert(saved->Persistent.Uses == 0 && saved->Access.Phase == SHMAccessIdle);
    assert(SHMDeviceBeginUse(saved, NULL) == OS_EBUSY);
    cacheCalls = g_cacheCallCount;
    assert(cacheCalls != 0);

    // Stop handling runs once. Repeated address-release failures must neither
    // invalidate caches again nor decrement the completed persistent uses.
    assert(SHMDeviceReleaseQuarantine(context) == OS_EDEVFAULT);
    assert(g_cacheCallCount == cacheCalls && saved->Persistent.Uses == 0);
    g_failedAddress = NULL;
    assert(SHMDeviceReleaseQuarantine(context) == OS_EOK);
    assert(g_cacheCallCount == cacheCalls && g_bufferKernelReferences == 0);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
    assert(atomic_load(&g_liveAllocations) == 0);
    g_cacheCallCount = 0;
}

static void
__TestBounceReleaseRetry(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    paddr_t page = 0x700000;
    unsigned int live;

    __UsePool(0x708000, false);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    memset(g_sourceMemory, 0x11, sizeof(g_sourceMemory));
    assert(SHMDeviceMap(context, 0x20, 16, 64, DMAFromDevice,
        SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(mapping, 0, 64, NULL) == OS_EOK);
    memset(g_bounceMemory, 0xab, 64);
    saved = mapping;
    live = atomic_load(&g_liveAllocations);
    g_failedAddress = mapping->AddressMapping;
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);

    // A reset supplies no completed prefix. Keep the bounce allocation and
    // both kernel views until address release succeeds, without copying data.
    assert(SHMDeviceReleaseQuarantine(context) == OS_EDEVFAULT);
    assert(context->Quarantine == saved && g_bufferKernelReferences == 1);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 2);
    assert(g_bounceMemory != NULL && !__PoolAllFree());
    assert(g_sourceMemory[16] == 0x11 && g_sourceMemory[79] == 0x11);
    g_failedAddress = NULL;
    assert(SHMDeviceReleaseQuarantine(context) == OS_EOK);
    assert(g_bounceMemory == NULL && g_liveViews == 0 && __PoolAllFree());
    assert(g_sourceMemory[16] == 0x11 && g_sourceMemory[79] == 0x11);
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestAllocationRollbackRelease(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1800, 0x1000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct DMARequirements requirements = { .Length = 4096, .Alignment = 4096 };
    SHMHandle_t buffer = { .ID = 0xdead };

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    g_createStatus = OS_EOK;
    g_createPages[0] = 0x700000;
    g_failAllReleases = true;

    // Physical alignment is sufficient but the translated device address is
    // misaligned. Failure to undo that mapping must keep its storage alive.
    assert(SHMDeviceAllocate(context, &requirements, &buffer, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && buffer.ID == 0xdead);
    assert(context->Quarantine != NULL && g_bufferKernelReferences == 1);
    assert(g_buffer != NULL && g_bufferReferences == 1);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceReleaseQuarantine(context) == OS_EDEVFAULT);
    g_failAllReleases = false;
    assert(SHMDeviceReleaseQuarantine(context) == OS_EOK);
    assert(g_buffer == NULL && g_bufferKernelReferences == 0);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
}

int
main(void)
{
    __ExistingShmDeviceTests();
    __TestAddressReleaseRetry();
    __TestQuarantinePartialProgress();
    __TestBounceReleaseRetry();
    __TestAllocationRollbackRelease();
    assert(g_liveViews == 0 && __PoolAllFree());
    puts("DMA release: retry-only state, exit, quarantine, bounce and allocation rollback passed");
    return 0;
}
