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

//#define __TRACE

#include "shm_device_support.inc"

static void
__TestCreationFailures(void)
{
    struct SHMDeviceRange range = { 0, 0, 1 };
    SHMDeviceContext_t* context = NULL;

    // Context allocation fails before attachment. Address-space allocation
    // fails after attachment and must undo that association as well.
    g_failAllocation = true;
    assert(__PrepareTestContext(&range, 1, SHMDeviceCacheCoherent,
        UINT64_MAX, &context) == OS_EOOM);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 0);
    assert(atomic_load(&g_testBindings) == 0);
    g_failAllocation = false;
    g_failAtCall = g_allocationCalls + 2;
    assert(__PrepareTestContext(&range, 1, SHMDeviceCacheCoherent,
        UINT64_MAX, &context) == OS_EOOM);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 0);
    assert(atomic_load(&g_testBindings) == 0);
    g_failAtCall = 0;
    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    SHMDeviceContextRelease(&context);
}

static void*
__ReferenceWorker(void* argument)
{
    SHMDeviceContext_t* context = argument;
    SHMDeviceContext_t* temporary;
    unsigned int i;

    // Every worker starts with a reference. The binding and copied address
    // rules must remain available after the creating thread leaves.
    for (i = 0; i < 1000; i++) {
        assert(SHMDeviceContextAcquire(context) == OS_EOK);
        temporary = context;
        assert(temporary->CachePolicy == SHMDeviceCacheCoherent);
        assert(temporary->Binding != NULL && temporary->AddressSpace != NULL);
        SHMDeviceContextRelease(&temporary);
    }
    SHMDeviceContextRelease(&context);
    return NULL;
}

static void
__TestConcurrentReferences(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x2000 };
    SHMDeviceContext_t* context;
    pthread_t workers[4];
    unsigned int i;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceContextAcquire(NULL) == OS_EINVALPARAMS);
    for (i = 0; i < 4; i++) {
        assert(SHMDeviceContextAcquire(context) == OS_EOK);
        assert(pthread_create(&workers[i], NULL, __ReferenceWorker, context) == 0);
    }
    SHMDeviceContextRelease(&context);
    for (i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(atomic_load(&g_liveAllocations) == 0);
    assert(atomic_load(&g_testBindings) == 0);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(NULL);
}

static void
__TestMapping(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x701000, 0x705000 };
    const struct DMASegment expected[] = { { 0x1300, 0x1d00 }, { 0x6000, 0xd00 } };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t count = 0;

    // The buffer starts 0x100 into its first page, and the range 0x200 later.
    // The first two pages continue each other physically; the third does not.
    __CreateBuffer(pages, 3, 0x100, 0x2e00, SHM_COMMIT);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    assert(__TestMap(context, 0x20, 0x200, 0x2a00, DMABidirectional, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(g_bufferReferences == 2 && !g_bufferLocked);

    segments = __TestMappingSegments(mapping, &count);
    assert(count == 2 && memcmp(segments, expected, sizeof(expected)) == 0);

    // Both the process and the context creator may leave; the mapping still
    // keeps the pages and the address rules (context, mapping, physical blocks).
    assert(DestroyHandle(0x20) == OS_EINCOMPLETE);
    SHMDeviceContextRelease(&context);
    assert(g_buffer != NULL && atomic_load(&g_liveAllocations) == 5);

    __TestUnmap(&mapping);
    assert(mapping == NULL && g_buffer == NULL);
    assert(atomic_load(&g_liveAllocations) == 0);
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(__TestUnmap(NULL) == OS_EOK);
    assert(__TestMappingSegments(NULL, &count) == NULL);
}

static void
__TestSync(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x701000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;

    __CreateBuffer(pages, 2, 0, 0x2000, SHM_COMMIT);
    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    assert(__TestMap(context, 0x20, 0x1000, 0x1000, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);

    // Ranges count from the mapping, which starts 0x1000 into the buffer.
    assert(__TestSyncForDevice(mapping, 0, 0x1001, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForDevice(mapping, 0x1001, 0, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForDevice(mapping, 0x800, SIZE_MAX, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForDevice(mapping, 0, 0, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForCpu(mapping, 0) == OS_EINVALPARAMS);

    // While the device owns the mapping it can be neither handed over again
    // nor released, because the device may still be using its pages.
    assert(__TestSyncForDevice(mapping, 0, 0x1000, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheCleanInvalidate, 0x701000, 0x1000 } }, 1);
    assert(__TestSyncForDevice(mapping, 0, 0x1000, NULL) == OS_EBUSY);
    saved = mapping;
    assert(__TestUnmap(&mapping) == OS_EBUSY);
    assert(mapping == saved && g_bufferReferences == 2);

    // A short transfer hands back only what was written; a bad range does not
    // change the owner. After that, the cycle can repeat.
    assert(__TestSyncForCpu(mapping, 0x1001) == OS_EINVALPARAMS);
    assert(__TestSyncForCpu(mapping, 0x200) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheInvalidate, 0x701000, 0x200 } }, 1);
    assert(__TestSyncForCpu(mapping, 0x200) == OS_EINVALPARAMS);
    assert(__TestSyncForDevice(mapping, 0x800, 0x800, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheCleanInvalidate, 0x701800, 0x800 } }, 1);

    // A stopped transfer that wrote nothing still gives the mapping back.
    assert(__TestSyncForCpu(mapping, SIZE_MAX) == OS_EINVALPARAMS);
    assert(__TestSyncForCpu(mapping, 0) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(__TestUnmap(&mapping) == OS_EOK && mapping == NULL);

    assert(__TestSyncForDevice(NULL, 0, 1, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForCpu(NULL, 0) == OS_EINVALPARAMS);
    assert(__TestMap(context, 0x20, 0, 0x1000, (enum DMADirection)99, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EINVALPARAMS);
    assert(mapping == NULL && g_bufferReferences == 1);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}

static void
__TestContextClose(void)
{
    struct SHMDeviceRange    range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t            pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* saved;
    struct SHMDeviceMapping* mapping = NULL;

    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    saved = context;
    assert(__TestMap(context, 0x20, 0, 0x1000, DMAToDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY && context == saved);
    assert(g_bufferKernelReferences == 1);
    // Removing the buffer's process reference must leave mapping storage intact.
    assert(DestroyHandle(0x20) == OS_EINCOMPLETE);
    assert(__TestUnmap(&mapping) == OS_EOK && g_buffer == NULL);
    assert(g_bufferKernelReferences == 0);
    assert(SHMDeviceContextClose(&context) == OS_EOK && context == NULL);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestIndependentStreams(void)
{
    struct SHMDeviceRange    range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t           pages[] = { 0x700000, 0x703000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* first = NULL;
    struct SHMDeviceMapping* second = NULL;
    const struct DMASegment* segments;
    uint32_t                 count;
    unsigned int             allocations;
    int                      views;
    int                      iteration;

    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(__TestMap(context, 0x20, 0, 0x1000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &first) == OS_EOK);
    assert(__TestMap(context, 0x20, 0x1000, 0x1000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &second) == OS_EOK);
    segments = __TestMappingSegments(first, &count);
    assert(count == 1 && segments[0].Address == 0x1000 && segments[0].Length == 0x1000);

    // Both mappings must remain usable after the original references are gone.
    SHMDeviceContextRelease(&context);
    assert(DestroyHandle(0x20) == OS_EINCOMPLETE && g_bufferReferences == 2);
    allocations = g_allocationCalls;
    views = g_mapCalls;
    assert(__TestSyncForDevice(second, 0, 0x1000, NULL) == OS_EOK);
    for (iteration = 0; iteration < 16; iteration++) {
        assert(__TestSyncForDevice(first, 0x40, 0x80, NULL) == OS_EOK);
        assert(__TestUnmap(&first) == OS_EBUSY);
        assert(__TestSyncForCpu(first, 0x81) == OS_EINVALPARAMS);
        assert(__TestSyncForDevice(first, 0x40, 0x80, NULL) == OS_EBUSY);
        assert(__TestSyncForCpu(first, iteration % 2 ? 0x20 : 0) == OS_EOK);
        assert(__TestMappingSegments(first, &count) == segments);
        assert(count == 1 && segments[0].Address == 0x1000 && segments[0].Length == 0x1000);
        assert(__TestUnmap(&second) == OS_EBUSY);
    }
    assert(g_allocationCalls == allocations && g_mapCalls == views);
    __ExpectCacheCalls(NULL, 0);

    assert(__TestUnmap(&first) == OS_EOK && g_bufferReferences == 1);
    assert(__TestSyncForCpu(second, 0x80) == OS_EOK);
    assert(__TestUnmap(&second) == OS_EOK && g_buffer == NULL);
    assert(atomic_load(&g_liveAllocations) == 0);
}

static void
__TestCacheMaintenance(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceContext* coherent;
    struct SHMDeviceMapping* mapping = NULL;

    // An ordinary cached buffer whose two pages are apart in physical memory,
    // so cache work for one range must be split across both pages.
    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    nonCoherent = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    coherent = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);

    // A device that only reads gets cached data written back, nothing more,
    // and its range need not sit on cache line boundaries.
    assert(__TestMap(nonCoherent, 0x20, 0xf10, 0x1f0, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(__TestSyncForDevice(mapping, 0, 0x1f0, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){
        { __CacheClean, 0x700f10, 0xf0 },
        { __CacheClean, 0x702000, 0x100 } }, 2);
    assert(__TestSyncForCpu(mapping, 0x1f0) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(__TestUnmap(&mapping) == OS_EOK);

    // A device that writes gets lines written back and thrown away before,
    // and only the completed bytes thrown away after.
    assert(__TestMap(nonCoherent, 0x20, 0xf00, 0x200, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(__TestSyncForDevice(mapping, 0x80, 0x100, NULL) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){
        { __CacheCleanInvalidate, 0x700f80, 0x80 },
        { __CacheCleanInvalidate, 0x702000, 0x80 } }, 2);
    assert(__TestSyncForCpu(mapping, 0x10) == OS_EOK);
    __ExpectCacheCalls((struct __CacheCall[]){ { __CacheInvalidate, 0x700f80, 0x10 } }, 1);
    assert(__TestUnmap(&mapping) == OS_EOK);

    // A coherent device sees the CPU cache, so no cache work happens at all.
    assert(__TestMap(coherent, 0x20, 0, 0x2000, DMABidirectional, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(__TestSyncForDevice(mapping, 0, 0x2000, NULL) == OS_EOK);
    assert(__TestSyncForCpu(mapping, 0x2000) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(__TestUnmap(&mapping) == OS_EOK);

    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&nonCoherent);
    SHMDeviceContextRelease(&coherent);
}

static void
__ExpectMapFailure(struct SHMDeviceContext*, uuid_t, size_t, size_t, oserr_t);

static void
__TestBounce(void)
{
    struct SHMDeviceRange wide = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceRange narrow = { 0x700000, 0x1000, 0x2000 };
    const paddr_t pages[] = { 0x700000, 0x7f0000 };
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceContext* coherent;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t count;
    struct SystemMemoryAllocation full = { 0 };
    unsigned int allocations;
    int views;

    // The second page lies outside both contexts' ranges.
    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    nonCoherent = __CreateContext(&wide, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    coherent = __CreateContext(&narrow, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    memset(g_sourceMemory, 0x11, sizeof(g_sourceMemory));

    // A device that writes, off cache line boundaries: it gets zeroed bounce
    // pages. The pool is uncached, so only ordering and copies are needed.
    assert(__TestMap(nonCoherent, 0x20, 0x10, 0x1e0, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    segments = __TestMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x9000 && segments[0].Length == 0x1e0);
    assert(g_liveViews == 2 && g_bounceMemory[0] == 0);
    assert(__TestSyncForDevice(mapping, 0, 0x1e0, NULL) == OS_EOK);
    assert(g_bounceMemory[0] == 0);
    __ExpectCacheCalls(NULL, 0);

    // The device fills the range but completes only 0x20 bytes: only those
    // reach the buffer, and its neighbouring bytes stay untouched.
    memset(g_bounceMemory, 0xab, 0x1e0);
    assert(__TestSyncForCpu(mapping, 0x20) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(g_sourceMemory[0xf] == 0x11 && g_sourceMemory[0x10] == 0xab);
    assert(g_sourceMemory[0x2f] == 0xab && g_sourceMemory[0x30] == 0x11);

    // Reuse the same bounce pages after cancellation, then complete a short
    // receive at a different offset. Neither cycle rebuilds the address list.
    allocations = g_allocationCalls;
    views = g_mapCalls;
    assert(__TestSyncForDevice(mapping, 0x40, 0x80, NULL) == OS_EOK);
    memset(g_bounceMemory + 0x40, 0xcd, 0x80);
    assert(__TestSyncForCpu(mapping, 0) == OS_EOK);
    assert(g_sourceMemory[0x50] == 0x11 && g_sourceMemory[0xcf] == 0x11);
    assert(__TestSyncForDevice(mapping, 0x40, 0x80, NULL) == OS_EOK);
    assert(__TestSyncForCpu(mapping, 0x10) == OS_EOK);
    assert(g_sourceMemory[0x4f] == 0x11 && g_sourceMemory[0x50] == 0xcd);
    assert(g_sourceMemory[0x5f] == 0xcd && g_sourceMemory[0x60] == 0x11);
    assert(__TestMappingSegments(mapping, &count) == segments);
    assert(count == 1 && segments[0].Address == 0x9000 && segments[0].Length == 0x1e0);
    assert(g_allocationCalls == allocations && g_mapCalls == views);
    __ExpectCacheCalls(NULL, 0);
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(g_liveViews == 0 && g_bounceMemory == NULL && __PoolAllFree());

    // Part of the range is out of reach: what the device reads is copied to
    // reachable bounce pages, across the buffer's page boundary. This pool
    // is cached, as on x86.
    __UsePool(0x701000, true);
    for (size_t i = 0; i < sizeof(g_sourceMemory); i++) {
        g_sourceMemory[i] = (uint8_t)(i * 7);
    }
    assert(__TestMap(coherent, 0x20, 0x800, 0x1000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    segments = __TestMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x2000 && segments[0].Length == 0x1000);
    assert(__TestSyncForDevice(mapping, 0, 0x1000, NULL) == OS_EOK);
    assert(memcmp(g_bounceMemory, &g_sourceMemory[0x800], 0x1000) == 0);
    __ExpectCacheCalls(NULL, 0);

    // A device that only reads never changes the buffer.
    memset(g_bounceMemory, 0xcd, 0x1000);
    assert(__TestSyncForCpu(mapping, 0x1000) == OS_EOK);
    assert(g_sourceMemory[0x801] == (uint8_t)(0x801 * 7));
    assert(__TestUnmap(&mapping) == OS_EOK);

    // Bounce pages out of reach as well: the mapping fails cleanly.
    __UsePool(0x900000, true);
    __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_ENOENT);
    __UsePool(0x701000, true);

    // Failure at either kernel view or any allocation must also give back
    // bounce pool pages. The final allocation builds the address mapping.
    for (int i = 1; i <= 2; i++) {
        g_failMapAt = g_mapCalls + i;
        __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_EOOM);
        g_failMapAt = 0;
    }
    for (unsigned int i = 1; i <= 4; i++) {
        g_failAtCall = g_allocationCalls + i;
        __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_EOOM);
        g_failAtCall = 0;
    }

    // A full pool fails the mapping instead of falling back to other memory,
    // and without any pool nothing can be bounced at all.
    assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 8 * 0x1000, 0, 0, &full) == OS_EOK);
    __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_EOOM);
    assert(SystemMemoryReleaseAllocation(&full) == OS_EOK);
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, 0x1000, 0);
    __ExpectMapFailure(coherent, 0x20, 0x800, 0x1000, OS_ENOTSUPPORTED);

    __UsePool(0x708000, false);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&nonCoherent);
    SHMDeviceContextRelease(&coherent);
}

static void
__TestBackingCachePolicy(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceRange bounceRange = { 0x708000, 0x9000, 0x8000 };
    const paddr_t pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* coherent;
    oserr_t status;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    struct __CacheCall expected;
    enum DMADirection direction;
    uint32_t count;
    size_t offset;
    bool cached;
    int backing;
    int sourceCached;
    int bounceCached;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    coherent = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    for (backing = 0; backing < 3; backing++) {
        // Only uncached pool memory guarantees that every alias is uncached.
        // Ordinary SHM and cached pool memory require cache maintenance.
        __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
        if (backing != 0) {
            g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
            g_buffer->Allocation.Cached = backing == 2;
        }
        status = __TestMap(context,
            0x20,
            0,
            64,
            DMABidirectional,
            SHMDeviceMappingShared,
            NULL,
            &mapping
        );
        assert(status == (backing == 1 ? OS_EOK : OS_ENOTSUPPORTED));
        assert(__TestUnmap(&mapping) == OS_EOK);
        status = __TestMap(coherent,
            0x20,
            0,
            64,
            DMABidirectional,
            SHMDeviceMappingShared,
            NULL,
            &mapping
        );
        assert(status == (backing != 0 ? OS_EOK : OS_ENOTSUPPORTED));
        assert(__TestUnmap(&mapping) == OS_EOK);
        cached = backing != 1;
        offset = cached ? 0x40 : 0x11;
        for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
            assert(__TestMap(context, 0x20, offset, 0x80, direction, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
            segments = __TestMappingSegments(mapping, &count);
            assert(count == 1 && segments[0].Address == 0x1000 + offset);
            assert(g_liveViews == 0 && g_bounceMemory == NULL);
            assert(__TestSyncForDevice(mapping, 0, 0x80, NULL) == OS_EOK);
            expected.Operation = direction == DMAToDevice ? __CacheClean : __CacheCleanInvalidate;
            expected.Address = pages[0] + offset;
            expected.Length = 0x80;
            __ExpectCacheCalls(&expected, cached ? 1 : 0);
            assert(__TestUnmap(&mapping) == OS_EBUSY);
            assert(__TestSyncForDevice(mapping, 0, 0x80, NULL) == OS_EBUSY);
            assert(__TestSyncForCpu(mapping, 0x10) == OS_EOK);
            expected.Operation = __CacheInvalidate;
            expected.Length = 0x10;
            __ExpectCacheCalls(&expected, cached && direction != DMAToDevice ? 1 : 0);
            assert(__TestUnmap(&mapping) == OS_EOK);
        }
        if (backing == 0) {
            // Ordinary SHM needs cache-line isolation for receives.
            assert(__TestMap(context, 0x20, 0x11, 0x80, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
            assert(g_liveViews == 2 && g_bounceMemory != NULL);
            assert(__TestUnmap(&mapping) == OS_EOK);
            assert(g_liveViews == 0 && __PoolAllFree());
        }
        assert(DestroyHandle(0x20) == OS_EOK);
    }
    SHMDeviceContextRelease(&coherent);
    SHMDeviceContextRelease(&context);

    context = __CreateContext(&bounceRange, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    for (sourceCached = 0; sourceCached < 2; sourceCached++) {
        for (bounceCached = 0; bounceCached < 2; bounceCached++) {
            __UsePool(0x708000, bounceCached != 0);
            __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
            g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
            g_buffer->Allocation.Cached = sourceCached != 0;
            for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
                // Unreachable source pages force bouncing regardless of cache
                // policy. Cache work must follow the destination pool alone.
                memset(g_sourceMemory, 0x11, sizeof(g_sourceMemory));
                assert(__TestMap(context, 0x20, 0x20, 0x80, direction, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
                assert(g_liveViews == 2 && g_bounceMemory != NULL);
                assert(__TestSyncForDevice(mapping, 0, 0x80, NULL) == OS_EOK);
                if (direction != DMAFromDevice) {
                    assert(memcmp(g_bounceMemory, &g_sourceMemory[0x20], 0x80) == 0);
                } else {
                    assert(g_bounceMemory[0] == 0);
                }
                expected.Operation = direction == DMAToDevice ? __CacheClean : __CacheCleanInvalidate;
                expected.Address = 0x708000;
                expected.Length = 0x80;
                __ExpectCacheCalls(&expected, bounceCached);
                assert(__TestUnmap(&mapping) == OS_EBUSY);
                memset(g_bounceMemory, 0xab, 0x80);
                assert(__TestSyncForCpu(mapping, 0x10) == OS_EOK);
                expected.Operation = __CacheInvalidate;
                expected.Length = 0x10;
                __ExpectCacheCalls(&expected, bounceCached && direction != DMAToDevice ? 1 : 0);
                assert(g_sourceMemory[0x20] == (direction == DMAToDevice ? 0x11 : 0xab));
                assert(g_sourceMemory[0x1f] == 0x11 && g_sourceMemory[0x30] == 0x11);
                assert(__TestUnmap(&mapping) == OS_EOK);
                assert(g_liveViews == 0 && g_bounceMemory == NULL && __PoolAllFree());
            }
            assert(DestroyHandle(0x20) == OS_EOK);
        }
    }
    SHMDeviceContextRelease(&context);
    __UsePool(0x708000, false);
    puts("DMA cache policy: direct and bounce backing, all directions, short copies and ownership passed");
}

static void
__ExpectAllocateFailure(
    _In_ struct SHMDeviceContext*            context,
    _In_ const struct DMARequirements* requirements,
    _In_ oserr_t                             expected)
{
    struct SHMDeviceMapping* mapping = NULL;
    SHMHandle_t              buffer = { 0 };
    unsigned int             live = atomic_load(&g_liveAllocations);

    // A failed allocation must leave no buffer, mapping, view or bounce behind.
    assert(__TestAllocate(context, requirements, &buffer, &mapping) == expected);
    assert(mapping == NULL && buffer.ID == 0 && g_buffer == NULL);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0 && g_bounceMemory == NULL);
}

static void
__TestAllocations(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    const struct DMASegment* segments;
    SHMHandle_t buffer;
    uint32_t count;
    int calls;

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);

    // Two neighbouring pages meet every requirement; 0x1800 bytes round up to
    // two whole pages, all of which the single segment covers.
    g_createPages[0] = 0x700000;
    g_createPages[1] = 0x701000;
    assert(__TestAllocate(context, &(struct DMARequirements){ 0x1800, 0x1000, 0x10000, true },
                             &buffer, &mapping) == OS_EOK);
    segments = __TestMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x1000 && segments[0].Length == 0x2000);
    assert(buffer.ID == 0x20 && buffer.Buffer != NULL && buffer.Length == 0x2000);
    assert(g_bufferReferences == 2 && g_bounceMemory == NULL);
    assert(g_createAlignment == 0x1000 && g_createBoundary == 0x10000);

    // Allocation mappings are continuously shared. The kernel must reject
    // streaming handoffs even when a caller bypasses the typed DDK interface.
    assert(__TestSyncForDevice(mapping, 0, 0x1000, NULL) == OS_EINVALPARAMS);
    assert(__TestSyncForCpu(mapping, 0) == OS_EINVALPARAMS);
    __ExpectCacheCalls(NULL, 0);

    // An occupied slot is refused before anything is created.
    saved = mapping;
    calls = g_createCalls;
    assert(__TestAllocate(context, &(struct DMARequirements){ 0x1000, 0, 0, false },
                             &buffer, &mapping) == OS_EBUSY);
    assert(mapping == saved && g_createCalls == calls);

    // Release the mapping, then the buffer, as a caller would.
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK && g_buffer == NULL);

    // Pages apart in memory are two segments: fine unless one is required.
    g_createPages[1] = 0x703000;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x2000, 0, 0, true }, OS_ENOTSUPPORTED);
    assert(__TestAllocate(context, &(struct DMARequirements){ 0x2000, 0, 0, false },
                             &buffer, &mapping) == OS_EOK);
    (void)__TestMappingSegments(mapping, &count);
    assert(count == 2);
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK);

    // The memory obtained misses the alignment, or crosses the boundary.
    g_createPages[1] = 0x701000;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 0x2000, 0, false }, OS_ENOTSUPPORTED);
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x2000, 0, 0x1000, false }, OS_ENOTSUPPORTED);

    // Unreachable memory is reported, and never bounced.
    g_createPages[0] = 0x900000;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 0, 0, false }, OS_ENOENT);
    g_createPages[0] = 0x700000;

    // Requirements no memory could meet are refused without creating anything.
    calls = g_createCalls;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 3, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 0, 0x6000, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x2000, 0, 0x1000, true }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, &(struct DMARequirements){ SIZE_MAX, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(NULL, &(struct DMARequirements){ 0x1000, 0, 0, false }, OS_EINVALPARAMS);
    __ExpectAllocateFailure(context, NULL, OS_EINVALPARAMS);
    assert(g_createCalls == calls);

    // Failing to create the buffer, or to map it, leaves nothing behind.
    g_createStatus = OS_EOOM;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 0, 0, false }, OS_EOOM);
    g_createStatus = OS_EOK;
    g_failAtCall = g_allocationCalls + 2;
    __ExpectAllocateFailure(context, &(struct DMARequirements){ 0x1000, 0, 0, false }, OS_EOOM);
    g_failAtCall = 0;
    SHMDeviceContextRelease(&context);
}

static void
__ExpectMapFailure(
    _In_ struct SHMDeviceContext* context,
    _In_ uuid_t                   id,
    _In_ size_t                   offset,
    _In_ size_t                   length,
    _In_ oserr_t                  expected)
{
    struct SHMDeviceMapping* mapping = NULL;
    unsigned int live = atomic_load(&g_liveAllocations);
    size_t freePages = MemoryStackFreeBlockCount(&g_pool.Stack);

    // A failure must give back every reference, allocation, kernel view and
    // pool page it took.
    assert(__TestMap(context, id, offset, length, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == expected);
    assert(mapping == NULL && g_bufferReferences == 1 && !g_bufferLocked);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0);
    assert(MemoryStackFreeBlockCount(&g_pool.Stack) == freePages && g_poolLockHeld == 0);
}

static void
__TestMappingFailures(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0, 0x702000 };
    struct SHMDeviceContext* coherent;
    struct SHMDeviceContext* limited;
    struct SHMDeviceContext* nonCoherent;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMDeviceMapping* saved;
    unsigned int calls;

    coherent = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    limited = __CreateContext(&range, 1, SHMDeviceCacheCoherent, 0x2fff);
    nonCoherent = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);

    // The middle page was never allocated, so only ranges avoiding it work.
    __CreateBuffer(pages, 3, 0, 0x3000, 0);
    __ExpectMapFailure(coherent, 0x21, 0, 0x1000, OS_ENOENT);
    __ExpectMapFailure(coherent, 0x20, 0, 0x2000, OS_EINCOMPLETE);
    __ExpectMapFailure(coherent, 0x20, 0, 0, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x3001, 1, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x2000, 0x1001, OS_EINVALPARAMS);
    __ExpectMapFailure(coherent, 0x20, 0x2000, SIZE_MAX, OS_EINVALPARAMS);
    __ExpectMapFailure(limited, 0x20, 0x2000, 0x1000, OS_ENOENT);

    calls = g_allocationCalls;
    assert(__TestMap(coherent,
        0x20,
        0,
        64,
        DMAToDevice,
        (enum SHMDeviceMappingKind)99,
        NULL,
        &mapping
    ) == OS_EINVALPARAMS);
    assert(mapping == NULL && g_bufferReferences == 1);
    assert(g_allocationCalls == calls);

    // Shared mode must not bypass handoffs for ordinary cached SHM.
    assert(__TestMap(nonCoherent,
        0x20,
        0,
        64,
        DMABidirectional,
        SHMDeviceMappingShared,
        NULL,
        &mapping
    ) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_bufferReferences == 1);
    assert(g_allocationCalls == calls);

    // A device that writes on a non-coherent path, off cache line boundaries,
    // is given bounce pages; a device that only reads, or a coherent one, uses
    // the buffer itself.
    assert(__TestMap(nonCoherent, 0x20, 0x20, 0x40, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(g_bounceMemory != NULL);
    __TestUnmap(&mapping);
    assert(__TestMap(nonCoherent, 0x20, 0, 0x30, DMABidirectional, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(g_bounceMemory != NULL);
    __TestUnmap(&mapping);
    assert(__TestMap(nonCoherent, 0x20, 0x20, 0x30, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(g_bounceMemory == NULL);
    __TestUnmap(&mapping);
    assert(__TestMap(coherent, 0x20, 0x20, 0x30, DMAFromDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    assert(g_bounceMemory == NULL);
    __TestUnmap(&mapping);
    assert(g_liveViews == 0);

    assert(__TestMap(coherent, 0x20, 0x2000, 0x1000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
    saved = mapping;
    assert(__TestMap(coherent, 0x20, 0, 0x1000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EBUSY);
    assert(mapping == saved && g_bufferReferences == 2);
    __TestUnmap(&mapping);

    // Fail the extents, SHM mapping and address mapping allocations in turn.
    for (unsigned int i = 1; i <= 3; i++) {
        calls = g_allocationCalls;
        g_failAtCall = calls + i;
        __ExpectMapFailure(coherent, 0x20, 0x2000, 0x1000, OS_EOOM);
        g_failAtCall = 0;
    }
    assert(DestroyHandle(0x20) == OS_EOK);

    assert(__TestMap(NULL, 0x20, 0, 1, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EINVALPARAMS);
    assert(__TestMap(coherent, 0x20, 0, 1, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, NULL) == OS_EINVALPARAMS);
    SHMDeviceContextRelease(&coherent);
    SHMDeviceContextRelease(&limited);
    SHMDeviceContextRelease(&nonCoherent);
}

#include "shm_device_alignment_test.inc"
#include "shm_device_direct_test.inc"
#include "shm_device_persistent_test.inc"
#include "shm_device_access_test.inc"
#include "shm_range_access_test.inc"

static void
__TestRestrictedAccess(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMAccess* restricted = NULL;
    struct SHMAccess* ordinary = NULL;
    struct SHMView view;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    view = (struct SHMView) {
        .Buffer = g_buffer,
        .Offset = 64,
        .Length = 128,
        .Scope = GrantTokenScopeRead
    };
    g_restricted = &view;
    assert(__TestMap(context, 0x21, 0, 64, DMAToDevice,
                        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(mapping != NULL && g_restrictedReferences == 1);
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(g_restrictedReferences == 0);
    assert(SHMAccessCreate(0x21, 128, 1, &restricted) == OS_EINVALPARAMS);
    assert(g_restrictedReferences == 0);
    assert(SHMAccessCreate(0x21, 0, 64, &restricted) == OS_EOK);
    assert(SHMAccessCreate(0x20, 64, 64, &ordinary) == OS_EOK);
    assert(__TestAccessBeginCpu(restricted, true, NULL) == OS_EPERMISSIONS);
    assert(__TestAccessBeginCpu(ordinary, true, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(restricted, false, NULL) == OS_EBUSY);
    assert(__TestAccessEndCpu(ordinary) == OS_EOK);
    assert(__TestAccessBeginCpu(restricted, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(restricted) == OS_EOK);
    assert(SHMAccessDestroy(&restricted) == OS_EOK);
    assert(SHMAccessDestroy(&ordinary) == OS_EOK);
    view.Scope = GrantTokenScopeWrite;
    assert(SHMAccessCreate(0x21, 0, 64, &restricted) == OS_EOK);
    assert(__TestAccessBeginCpu(restricted, false, NULL) == OS_EPERMISSIONS);
    assert(__TestAccessBeginCpu(restricted, true, NULL) == OS_EPERMISSIONS);
    assert(SHMAccessDestroy(&restricted) == OS_EOK);
    assert(g_restrictedReferences == 0);
    g_restricted = NULL;
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}

static void
__TestRestrictedDma(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x701000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;
    struct SHMAccess* cpu = NULL;
    const struct DMASegment* segments;
    struct SHMView view;
    unsigned int scope;
    unsigned int needed;
    enum DMADirection direction;
    enum SHMDeviceMappingKind kind;
    uint32_t count;
    unsigned int allocations;
    int calls;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    view = (struct SHMView){ .Buffer = g_buffer, .Offset = 4096 + 64, .Length = 128 };
    g_restricted = &view;
    for (scope = 1; scope <= 3; scope++) {
        view.Scope = (GrantTokenScope_t)scope;
        for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
            needed = direction == DMAToDevice ? GrantTokenScopeRead : GrantTokenScopeWrite;
            if (direction == DMABidirectional) {
                needed |= GrantTokenScopeRead;
            }
            for (kind = SHMDeviceMappingStreamingBounce; kind <= SHMDeviceMappingPersistent; kind++) {
                if ((scope & needed) != needed) {
                    assert(__TestMap(context, 0x21, 64, 64, direction, kind,
                                        NULL, &mapping) == OS_EPERMISSIONS);
                    assert(mapping == NULL && g_restrictedReferences == 0);
                    continue;
                }
                assert(__TestMap(context, 0x21, 128, 1, direction, kind,
                                    NULL, &mapping) == OS_EINVALPARAMS);
                assert(__TestMap(context, 0x21, 1, SIZE_MAX, direction, kind,
                                    NULL, &mapping) == OS_EINVALPARAMS);
                assert(g_restrictedReferences == 0);
                assert(__TestMap(context, 0x21, 64, 64, direction, kind,
                                    NULL, &mapping) == OS_EOK);
                assert(g_restrictedReferences == 1 && g_bounceMemory == NULL);
                segments = __TestMappingSegments(mapping, &count);
                assert(count == 1 && segments[0].Address == 0x2080 && segments[0].Length == 64);
                assert(SHMAccessCreate(0x20, 4096 + 128, 64, &cpu) == OS_EOK);
                assert(__TestAccessBeginCpu(cpu, true, NULL) == OS_EOK);
                if (kind == SHMDeviceMappingPersistent) {
                    assert(__TestBeginUse(mapping, NULL) == OS_EBUSY);
                } else {
                    assert(__TestSyncForDevice(mapping, 0, 64, NULL) == OS_EBUSY);
                }
                assert(__TestAccessEndCpu(cpu) == OS_EOK);
                assert(SHMAccessDestroy(&cpu) == OS_EOK);
                allocations = g_allocationCalls;
                calls = g_mapCalls;
                if (kind == SHMDeviceMappingPersistent) {
                    assert(__TestBeginUse(mapping, NULL) == OS_EOK);
                    assert(__TestBeginUse(mapping, NULL) == OS_EOK);
                    assert(__TestEndUse(mapping) == OS_EOK);
                    assert(__TestUnmap(&mapping) == OS_EBUSY);
                    assert(__TestEndUse(mapping) == OS_EOK);
                } else {
                    assert(__TestSyncForDevice(mapping, 0, 64, NULL) == OS_EOK);
                    assert(__TestSyncForCpu(mapping, 64) == OS_EOK);
                }
                assert(g_allocationCalls == allocations && g_mapCalls == calls);
                assert(__TestUnmap(&mapping) == OS_EOK && g_restrictedReferences == 0);
            }
        }
    }
    g_restricted = NULL;
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
}

int
main(void)
{
    // Bounce pages come from an uncached pool unless a test moves it.
    __UsePool(0x708000, false);

    // Keep the tests ordered so each case must finish with no live contexts.
    __TestAlignmentCopies();
    __TestAlignmentFailures();
    __TestTranslatedAlignment();
    __TestLargeMappingAlignment();
    __TestCreationFailures();
    __TestConcurrentReferences();
    __TestMapping();
    __TestSync();
    __TestContextClose();
    __TestIndependentStreams();
    __TestCacheMaintenance();
    __TestBounce();
    __TestBackingCachePolicy();
    __TestAllocations();
    __TestMappingFailures();
    __TestDirectMappings();
    __TestPersistentMappings();
    __TestBufferAccess();
    __TestConcurrentBufferAccess();
    __TestRangeAccess();
    __TestRangeWakeSelection();
    __TestAccessStates();
    __TestAccessCancellation();
    __TestRestrictedAccess();
    __TestRestrictedDma();
    assert(atomic_load(&g_liveAllocations) == 0 && g_buffer == NULL && g_liveViews == 0);
    assert(__PoolAllFree() && g_poolLockHeld == 0);
    assert(atomic_load(&g_testBindings) == 0);
    puts("SHM device context: bound mappings, pool use, validation and lifetime passed");
    return 0;
}
