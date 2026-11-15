/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Linker wrappers inject address-release failures and capture their diagnostics.
#include "../kernel/memory/shm/device.c"
#define main __ExistingShmDeviceTests
#include "shm_device_test.c"
#undef main
#include <stdarg.h>

static struct DMAAddressMapping* g_failedAddress;
static bool                     g_failAllReleases;
static unsigned int             g_completedReleases;
static char                     g_lastError[256];

int
__wrap_fprintf(
    _In_ FILE*       stream,
    _In_ const char* format,
    ...)
{
    va_list arguments;
    int     length;

    va_start(arguments, format);
    if (stream == stderr) {
        length = vsnprintf(g_lastError, sizeof(g_lastError), format, arguments);
    } else {
        length = vfprintf(stream, format, arguments);
    }
    va_end(arguments);
    return length;
}

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
__TestIdleAbandonReleaseFailure(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x1000 };
    SHMDeviceContext_t*   context;
    SHMDeviceMapping_t*   mapping = NULL;
    SHMDeviceMapping_t*   saved;
    paddr_t              page = 0x700000;
    unsigned int         live;
    char                 expectedError[256];

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    assert(SHMDeviceMap(
        NULL,
        context,
        0x20,
        0,
        64,
        DMAFromDevice,
        SHMDeviceMappingStreamingDirect,
        NULL,
        &mapping
    ) == OS_EOK);
    saved = mapping;
    live = atomic_load(&g_liveAllocations);
    g_failedAddress = mapping->AddressMapping;
    g_lastError[0] = '\0';
    assert(snprintf(
        expectedError,
        sizeof(expectedError),
        "SHM: releasing abandoned mapping %p failed: %u",
        (void*)mapping,
        OS_EDEVFAULT
    ) > 0);

    // Abandonment succeeds by leaving the failed release for context cleanup.
    // Report the mapping and failure before the caller loses its reference.
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    assert(strcmp(g_lastError, expectedError) == 0);
    assert(context->Mappings == saved && saved->Lifetime.Abandoned);
    assert(atomic_load(&g_liveAllocations) == live && g_bufferKernelReferences == 1);

    g_failedAddress = NULL;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
    assert(context->Mappings == NULL && g_bufferKernelReferences == 0);
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
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
    assert(SHMDeviceMap(NULL, context, 0x20, 0, 64, DMAFromDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    saved = mapping;
    live = atomic_load(&g_liveAllocations);
    g_failedAddress = mapping->AddressMapping;

    assert(SHMDeviceUnmap(NULL, &mapping) == OS_EDEVFAULT && mapping == saved);
    assert(atomic_load(&g_liveAllocations) == live && g_bufferKernelReferences == 1);
    assert(SHMDeviceMappingSegments(NULL, mapping, &count) == NULL && count == 0);
    assert(SHMDeviceSyncForDevice(NULL, mapping, 0, 64, NULL) == OS_EBUSY);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceUnmap(NULL, &mapping) == OS_EDEVFAULT && mapping == saved);

    // With no process left to retry, the context must keep this idle mapping.
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    assert(context->Mappings == saved && g_bufferKernelReferences == 1);
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    assert(context->Mappings == saved && atomic_load(&g_liveAllocations) == live);
    g_failedAddress = NULL;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
    assert(context->Mappings == NULL && g_bufferKernelReferences == 0);
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
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
    assert(SHMDeviceMap(NULL, context, 0x20, 128, 64, DMAFromDevice,
        SHMDeviceMappingPersistent, NULL, &successful) == OS_EOK);
    assert(SHMDeviceMap(NULL, context, 0x20, 0, 64, DMAFromDevice,
        SHMDeviceMappingPersistent, NULL, &failed) == OS_EOK);
    assert(SHMDeviceBeginUse(NULL, failed, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(NULL, failed, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(NULL, successful, NULL) == OS_EOK);
    saved = failed;
    g_failedAddress = failed->AddressMapping;
    assert(SHMDeviceAbandon(&successful) == OS_EOK && successful == NULL);
    assert(SHMDeviceAbandon(&failed) == OS_EOK && failed == NULL);
    assert(g_bufferKernelReferences == 2);

    completed = g_completedReleases;
    g_cacheCallCount = 0;
    // The newest mapping fails first. Cleanup must still free the next entry.
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    assert(g_completedReleases == completed + 1 && g_bufferKernelReferences == 1);
    assert(context->Mappings == saved && saved->Lifetime.Next == NULL);
    assert(saved->Persistent.Uses == 0 && saved->Access.Phase == SHMAccessQuarantined);
    assert(SHMDeviceBeginUse(NULL, saved, NULL) == OS_EBUSY);
    cacheCalls = g_cacheCallCount;
    assert(cacheCalls != 0);

    // Stop handling runs once. Repeated address-release failures must neither
    // invalidate caches again nor decrement the completed persistent uses.
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    assert(g_cacheCallCount == cacheCalls && saved->Persistent.Uses == 0);
    g_failedAddress = NULL;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
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
    assert(SHMDeviceMap(NULL, context, 0x20, 16, 64, DMAFromDevice,
        SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceSyncForDevice(NULL, mapping, 0, 64, NULL) == OS_EOK);
    memset(g_bounceMemory, 0xab, 64);
    saved = mapping;
    live = atomic_load(&g_liveAllocations);
    g_failedAddress = mapping->AddressMapping;
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);

    // A reset supplies no completed prefix. Keep the bounce allocation and
    // both kernel views until address release succeeds, without copying data.
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    assert(context->Mappings == saved && g_bufferKernelReferences == 1);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 2);
    assert(g_bounceMemory != NULL && !__PoolAllFree());
    assert(g_sourceMemory[16] == 0x11 && g_sourceMemory[79] == 0x11);
    g_failedAddress = NULL;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
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
    assert(SHMDeviceAllocate(NULL, context, &requirements, &buffer, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && buffer.ID == 0xdead);
    assert(context->Mappings != NULL && g_bufferKernelReferences == 1);
    assert(g_buffer != NULL && g_bufferReferences == 1);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    g_failAllReleases = false;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
    assert(g_buffer == NULL && g_bufferKernelReferences == 0);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestLiveMappingAfterStop(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x1000 };
    SHMDeviceContext_t*   context;
    SHMDeviceMapping_t*   mapping = NULL;
    SHMDeviceMapping_t*   temporary;
    paddr_t              page = 0x700000;
    uint32_t             count = 99;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    assert(SHMDeviceMap(NULL, context, 0x20, 0, 256, DMAFromDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(context->Mappings == mapping);

    // A traversal reference excludes unmap until its resource inspection ends.
    assert(SHMDeviceMappingAcquire(mapping) == OS_EOK);
    temporary = mapping;
    assert(SHMDeviceUnmap(NULL, &mapping) == OS_EBUSY);
    assert(g_bufferKernelReferences == 1);
    SHMDeviceMappingRelease(&temporary);

    // The cancellation API must report a partial walk, not infer completion.
    atomic_store(&mapping->Lifetime.References, UINT_MAX);
    assert(SHMDeviceContextRevoke(context) == OS_EOVERFLOW);
    atomic_store(&mapping->Lifetime.References, 1);
    mapping->Lifetime.Cleaning = true;
    assert(SHMDeviceContextRevoke(context) == OS_EBUSY);
    mapping->Lifetime.Cleaning = false;

    assert(SHMDeviceSyncForDevice(NULL, mapping, 64, 64, NULL) == OS_EOK);
    g_cacheCallCount = 0;
    assert(SHMDeviceContextRevoke(context) == OS_EOK);
    assert(mapping->Access.First == 0 && mapping->Access.Last == 255);

    // Supply the native helper's exclusive, confirmed-stop precondition. The
    // unbound context entry selects abandoned mappings. Bound recovery uses
    // this same resource helper for live records.
    MutexLock(&context->MappingLock);
    mapping->Lifetime.Cleaning = true;
    MutexUnlock(&context->MappingLock);
    assert(SHMDeviceMappingCleanup(mapping, true) == OS_EOK);
    MutexLock(&context->MappingLock);
    mapping->Lifetime.Cleaning = false;
    MutexUnlock(&context->MappingLock);
    __ExpectCacheCalls((struct __CacheCall[]) {
        { __CacheInvalidate, 0x700000, 256 }
    }, 1);
    assert(mapping->State == SHMDeviceMappingStopped);
    assert(context->Mappings == mapping && g_bufferKernelReferences == 0);
    assert(SHMDeviceMappingSegments(NULL, mapping, &count) == NULL && count == 0);
    assert(SHMDeviceSyncForDevice(NULL, mapping, 0, 64, NULL) == OS_ECANCELLED);
    assert(SHMDeviceSyncForCpu(NULL, mapping, 64) == OS_ECANCELLED);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceContextRevoke(context) == OS_EOK);
    assert(SHMDeviceMappingCleanup(mapping, true) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);

    // A stopped record can close while a temporary object reference remains.
    assert(SHMDeviceMappingAcquire(mapping) == OS_EOK);
    temporary = mapping;
    assert(SHMDeviceUnmap(NULL, &mapping) == OS_EOK && mapping == NULL);
    assert(context->Mappings == temporary);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    SHMDeviceMappingRelease(&temporary);
    assert(context->Mappings == NULL);
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestIdleMappingStop(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x1000 };
    SHMDeviceContext_t*   context;
    SHMDeviceMapping_t*   mapping = NULL;
    SHMDeviceMapping_t*   saved;
    paddr_t              page = 0x700000;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(&page, 1, 0, 4096, 0);
    assert(SHMDeviceMap(NULL, context, 0x20, 0, 256, DMAFromDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    saved = mapping;
    g_failedAddress = mapping->AddressMapping;
    assert(SHMDeviceContextRevoke(context) == OS_EOK);
    assert(SHMDeviceAbandon(&mapping) == OS_EOK && mapping == NULL);
    g_cacheCallCount = 0;
    assert(SHMDeviceContextCompleteStop(context) == OS_EDEVFAULT);
    assert(saved->Stop.CacheCompleted && saved->Access.Phase == SHMAccessQuarantined);
    assert(g_buffer->Access.Faults == 1 && g_bufferKernelReferences == 1);
    __ExpectCacheCalls((struct __CacheCall[]) {
        { __CacheInvalidate, 0x700000, 256 }
    }, 1);
    g_failedAddress = NULL;
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(g_buffer->Access.Faults == 0 && context->Mappings == NULL);
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
}

int
main(void)
{
    __ExistingShmDeviceTests();
    __TestIdleAbandonReleaseFailure();
    __TestAddressReleaseRetry();
    __TestQuarantinePartialProgress();
    __TestBounceReleaseRetry();
    __TestAllocationRollbackRelease();
    __TestLiveMappingAfterStop();
    __TestIdleMappingStop();
    assert(g_liveViews == 0 && __PoolAllFree());
    puts("DMA release: retry-only state, exit, quarantine, bounce and allocation rollback passed");
    return 0;
}
