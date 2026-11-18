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

// Included by dma_iommu_bound_test.c. Translated streaming bounce through the
// real grant, SHM, binding and syscall paths, with the simulated IOMMU.
//
// Physical RAM is modeled by one host file. g_ram maps all of it, and is what
// the simulated device reads and writes after the IOMMU translates an IOVA.
// Every kernel view of RAM pages maps the same file pages again, page by page,
// so a kernel view and the device see the same bytes, and a view of units that
// are far apart is still one contiguous kernel range. Device access goes
// through the IOMMU tables and never through the published segment, so a test
// can probe the padding after the payload.
#include <sys/mman.h>

#define __RAM_BASE        0x10000000ULL
#define __RAM_LENGTH      0x400000U
#define __RAM_POOL        0x10100000ULL
#define __RAM_POOL_LENGTH 0x100000U
#define __RAM_HIGH        0x10300000ULL
#define __RAM_LOW_LIMIT   0x102fffffULL
#define __RAM_PAGE        0x1000U
#define __RAM_VIEWS       16
#define __RAM_STALE       0xa5

// One kernel view of RAM pages. Bounce is set for a view of pool pages.
struct __RamView {
    uint8_t* Address;
    size_t   Length;
    bool     Bounce;
};

static int              g_ramFile = -1;
static uint8_t*         g_ram;
static struct __RamView g_ramViews[__RAM_VIEWS];
static uintptr_t        g_ramPoolStorage[512];

// The page list of the most recent bounce view, copied after its length was
// checked against the allocation that holds it.
static paddr_t g_lastBouncePages[128];
static size_t  g_lastBouncePageCount;

// How many device address releases had happened when a bounce view was last
// removed, so a test can check the order of the two.
static unsigned int g_bounceUnmapAddressReleases;

static bool
__RamContains(uint64_t physical, uint64_t length)
{
    return physical >= __RAM_BASE && length <= __RAM_LENGTH &&
        physical - __RAM_BASE <= __RAM_LENGTH - length;
}

static uint8_t*
__RamPointer(uint64_t physical, uint64_t length)
{
    assert(g_ram != NULL && __RamContains(physical, length));
    return g_ram + (physical - __RAM_BASE);
}

static bool
__InPool(paddr_t physical)
{
    return physical >= g_pool.Base && physical - g_pool.Base < g_pool.Length;
}

// Construct the device pool inside RAM. Pool pages keep whatever bytes RAM
// holds, so storage that is not zeroed shows the stale pattern.
static void
__RamUsePool(paddr_t base, size_t length, bool cached)
{
    assert(__RamContains(base, length));
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, __RAM_PAGE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, base, length, cached,
        g_ramPoolStorage, sizeof(g_ramPoolStorage)) == OS_EOK);
}

static size_t
__PoolFreePages(void)
{
    return MemoryStackFreeBlockCount(&g_pool.Stack);
}

static void
__RamStart(void)
{
    char path[] = "/tmp/vali-ram-XXXXXX";

    assert(g_ram == NULL);
    g_ramFile = mkstemp(path);
    assert(g_ramFile >= 0 && unlink(path) == 0);
    assert(ftruncate(g_ramFile, __RAM_LENGTH) == 0);
    g_ram = mmap(NULL, __RAM_LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, g_ramFile, 0);
    assert(g_ram != MAP_FAILED);
    memset(g_ram, __RAM_STALE, __RAM_LENGTH);
    __RamUsePool(__RAM_POOL, __RAM_POOL_LENGTH, false);
}

static void
__RamStop(void)
{
    assert(g_ram != NULL);
    for (unsigned int i = 0; i < __RAM_VIEWS; i++) {
        assert(g_ramViews[i].Address == NULL);
    }
    assert(munmap(g_ram, __RAM_LENGTH) == 0 && close(g_ramFile) == 0);
    g_ram = NULL;
    g_ramFile = -1;
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, __RAM_PAGE, 0);
}

// Check the page list a view is created from before reading it. Bounce page
// lists are heap allocations and must hold one entry per page of the view.
// Source page lists point into the buffer's own page array.
static void
__CheckViewPageList(const struct MemorySpaceMapOptions* options, size_t pageCount, bool bounce)
{
    if (bounce) {
        assert(__RecentAllocationSize(options->Pages) >= pageCount * sizeof(paddr_t));
        assert(pageCount <= sizeof(g_lastBouncePages) / sizeof(g_lastBouncePages[0]));
        memcpy(g_lastBouncePages, options->Pages, pageCount * sizeof(paddr_t));
        g_lastBouncePageCount = pageCount;
        assert(((options->Flags & MAPPING_NOCACHE) != 0) == !g_pool.Cached);
        return;
    }
    assert(g_buffer != NULL && options->Pages >= g_buffer->Pages &&
        options->Pages + pageCount <= g_buffer->Pages + g_buffer->PageCount);
    assert(((options->Flags & MAPPING_NOCACHE) != 0) == SHMBufferUncached(g_buffer));
}

static bool
__RamMapView(struct MemorySpaceMapOptions* options, vaddr_t* address, oserr_t* status)
{
    struct __RamView* view = NULL;
    size_t            pageCount;
    bool              bounce;

    if (g_ram == NULL || !__RamContains(options->Pages[0], __RAM_PAGE)) {
        return false;
    }

    if (++g_mapCalls == g_failMapAt) {
        *status = OS_EOOM;
        return true;
    }

    assert(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_GLOBAL));
    assert((options->Flags & MAPPING_COMMIT) && (options->Flags & MAPPING_PERSISTENT));
    assert(options->Length != 0 && options->Length % __RAM_PAGE == 0);
    pageCount = options->Length / __RAM_PAGE;
    bounce = __InPool(options->Pages[0]);
    __CheckViewPageList(options, pageCount, bounce);
    for (unsigned int i = 0; i < __RAM_VIEWS && view == NULL; i++) {
        if (g_ramViews[i].Address == NULL) {
            view = &g_ramViews[i];
        }
    }
    assert(view != NULL);

    // Reserve the kernel range, then put each page of RAM in place.
    view->Address = mmap(NULL, options->Length, PROT_NONE, MAP_SHARED, g_ramFile, 0);
    assert(view->Address != MAP_FAILED);
    for (size_t i = 0; i < pageCount; i++) {
        paddr_t page = options->Pages[i];

        assert(page % __RAM_PAGE == 0 && __RamContains(page, __RAM_PAGE));
        assert(__InPool(page) == bounce);
        assert(mmap(view->Address + i * __RAM_PAGE, __RAM_PAGE, PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_FIXED, g_ramFile, (off_t)(page - __RAM_BASE)) == view->Address + i * __RAM_PAGE);
    }
    if (options->Flags & MAPPING_CLEAN) {
        memset(view->Address, 0, options->Length);
    }
    view->Length = options->Length;
    view->Bounce = bounce;
    g_liveViews++;
    *address = (vaddr_t)view->Address;
    *status = OS_EOK;
    return true;
}

static bool
__RamUnmapView(vaddr_t address, size_t length, oserr_t* status)
{
    for (unsigned int i = 0; i < __RAM_VIEWS; i++) {
        struct __RamView* view = &g_ramViews[i];

        if (view->Address == NULL || (vaddr_t)view->Address != address) {
            continue;
        }
        assert(length == view->Length && g_liveViews > 0);
        if (__FailViewUnmap(view->Bounce)) {
            *status = OS_EUNKNOWN;
            return true;
        }
        if (view->Bounce) {
            g_bounceUnmapAddressReleases = g_addressReleaseCalls;
        }
        assert(munmap(view->Address, view->Length) == 0);
        *view = (struct __RamView){ 0 };
        g_liveViews--;
        *status = OS_EOK;
        return true;
    }
    return false;
}

static int
__RamBounceViews(void)
{
    int views = 0;

    for (unsigned int i = 0; i < __RAM_VIEWS; i++) {
        views += g_ramViews[i].Address != NULL && g_ramViews[i].Bounce ? 1 : 0;
    }
    return views;
}

// Access bytes as the device with hardware ID 0 does: translated by the
// simulated IOMMU, then read from or written to RAM. DMAToDevice reads RAM
// into bytes, DMAFromDevice writes bytes to RAM. Returns the IOMMU result
// (OS_ENOENT without a translation, OS_EPERMISSIONS for a denied direction).
static oserr_t
__DeviceAccess(struct __TranslatedFixture* fixture, uint64_t address, size_t length,
               enum DMADirection access, uint8_t* bytes)
{
    SHMSG_t  spans[80];
    uint32_t count;
    size_t   done = 0;
    oserr_t  status;

    status = DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, address, length, access, spans, 80, &count);
    if (status != OS_EOK) {
        return status;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* ram = __RamPointer(spans[i].Address, spans[i].Length);

        if (access == DMAToDevice) {
            memcpy(bytes + done, ram, spans[i].Length);
        } else {
            memcpy(ram, bytes + done, spans[i].Length);
        }
        done += spans[i].Length;
    }
    assert(done == length);
    return OS_EOK;
}

// Read length bytes as the device and check they are all value.
static void
__ExpectDeviceBytes(struct __TranslatedFixture* fixture, uint64_t address, size_t length, uint8_t value)
{
    uint8_t* bytes = malloc(length);

    assert(bytes != NULL);
    assert(__DeviceAccess(fixture, address, length, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < length; i++) {
        assert(bytes[i] == value);
    }
    free(bytes);
}

// The source pattern. Each byte of the source page run gets a value derived
// from its position, so a shifted, repeated or missing byte is detected.
static uint8_t
__SourceByte(size_t index)
{
    return (uint8_t)(index * 7 + 3);
}

// Create the client buffer on contiguous RAM pages, bufferOffset bytes into
// its first page, and fill the whole page run with the source pattern.
static void
__CreateRamBuffer(paddr_t physical, size_t pageCount, size_t bufferOffset, size_t length)
{
    paddr_t  pages[80];
    uint8_t* ram = __RamPointer(physical, pageCount * __RAM_PAGE);

    assert(pageCount <= 80);
    for (size_t i = 0; i < pageCount; i++) {
        pages[i] = physical + i * __RAM_PAGE;
    }
    for (size_t i = 0; i < pageCount * __RAM_PAGE; i++) {
        ram[i] = __SourceByte(i);
    }
    __CreateBuffer(pages, (int)pageCount, bufferOffset, length, SHM_COMMIT);
    __RegisterTestBuffer();
}

static SHMDeviceMapping_t*
__LookupMapping(uuid_t handle)
{
    SHMRecord_t* record = LookupHandle(handle, HandleTypeSHMAccess);

    assert(record != NULL && record->Object != NULL);
    return record->Object;
}

static struct __TranslatedFixture
__PrepareBounceFixture(MSContextSecurity_t* service, MSContextSecurity_t* driver, uint64_t granule,
                       uint64_t physicalLimit, enum SHMDeviceCachePolicy cachePolicy)
{
    const struct __TranslatedOptions options = { .PhysicalLimit = physicalLimit, .CachePolicy = cachePolicy };

    return __PrepareTranslatedFixtureWith(service, driver, granule, 1, &options);
}

static oserr_t
__MapBounce(struct __TranslatedFixture* fixture, uuid_t source, size_t offset, size_t length,
            enum DMADirection direction, unsigned int flags, struct OSDmaMappingResult* result)
{
    struct OSDmaMapParameters parameters = { .Offset = offset, .Length = length,
        .Direction = direction, .Flags = flags };

    return ScDmaMapShm(fixture->ContextHandles[0], source, &parameters, result);
}

static oserr_t
__BeginTransfer(uuid_t mapping, size_t offset, size_t length)
{
    struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer, .Offset = offset, .Length = length };

    return ScShmAccessBegin(NULL, mapping, &begin);
}

// The inline result, the count query and the full query must all describe the
// same single segment of exactly the requested length.
static void
__CheckPublishedSegment(const struct OSDmaMappingResult* result, size_t length)
{
    struct DMASegment segments[2] = { 0 };
    uint32_t          count = 0;

    assert(result->SegmentCount == 1 && result->InlineSegment.Length == length);
    assert(ScDmaGetSegments(result->Handle, NULL, 0, &count) == OS_EOK && count == 1);
    count = 0;
    assert(ScDmaGetSegments(result->Handle, segments, 2, &count) == OS_EOK && count == 1);
    assert(memcmp(&segments[0], &result->InlineSegment, sizeof(segments[0])) == 0);
}

// Check the layout behind a translated bounce mapping: units, extents, the
// native span and the IOMMU translation of every mapped byte. Nothing before
// or after the mapped span translates, and every padding byte reads as zero
// before any transfer.
static void
__CheckBounceLayout(struct __TranslatedFixture* fixture, const struct OSDmaMappingResult* result,
                    uint64_t granule, size_t length)
{
    SHMDeviceMapping_t*      mapping = __LookupMapping(result->Handle);
    const SHMDeviceBounce_t* bounce = &mapping->Stream.Bounce;
    size_t                   mapped = (length + granule - 1) / granule * granule;
    size_t                   unit = granule > __RAM_PAGE ? granule : __RAM_PAGE;
    const struct DMASegment* span;
    uint32_t                 count;
    SHMSG_t                  probe;
    size_t                   covered = 0;

    assert(bounce->Translated && bounce->MappedLength == mapped);
    assert(bounce->UnitCount == (length + unit - 1) / unit);
    assert(bounce->BounceMappingLength == bounce->UnitCount * unit);
    for (size_t i = 0; i < bounce->UnitCount; i++) {
        assert(bounce->Units[i].Owner != NULL && bounce->Units[i].Length == unit);
        assert(bounce->Units[i].PhysicalBase % unit == 0 && __InPool(bounce->Units[i].PhysicalBase));
    }
    for (int i = 0; i < mapping->ExtentCount; i++) {
        assert(mapping->Extents[i].Address % granule == 0 && mapping->Extents[i].Length % granule == 0);
        assert(__InPool(mapping->Extents[i].Address));
        covered += mapping->Extents[i].Length;
    }
    assert(covered == mapped);
    span = DMAAddressMappingSegments(mapping->AddressMapping, &count);
    assert(count == 1 && span->Address == result->InlineSegment.Address && span->Length == mapped);
    __ExpectDeviceBytes(fixture, span->Address, mapped, 0);
    assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, span->Address + mapped, 1,
        DMAToDevice, &probe, 1, &count) == OS_ENOENT);
    if (span->Address != 0) {
        assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, span->Address - 1, 1,
            DMAToDevice, &probe, 1, &count) == OS_ENOENT);
    }
}

// Every resource a failed or finished mapping could hold is gone.
static void
__ExpectNoBounceResources(struct __TranslatedFixture* fixture, size_t freePages)
{
    DMAIOMMUSimStats_t stats;

    DMAIOMMUSimSnapshot(fixture->Sim, &stats);
    assert(stats.Mappings == 0 && __PoolFreePages() == freePages);
    assert(__RamBounceViews() == 0 && g_liveViews == 0);
    assert(fixture->Contexts[0]->Mappings == NULL);
}

// Small requests at many source positions. Each gets one granule of private
// storage, publishes exactly its own length, and the device reads the right
// source bytes after begin.
static void
__TestBounceLayouts(MSContextSecurity_t* service, MSContextSecurity_t* driver, uint64_t granule)
{
    struct __TranslatedFixture fixture;
    const size_t               lengths[] = { 8, 64, 512 };
    size_t                     freePages;
    uint8_t                    bytes[512];

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, granule, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 4, 0, 4 * __RAM_PAGE);
    freePages = __PoolFreePages();
    for (size_t l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
        size_t length = lengths[l];
        size_t offsets[] = { 0, 1, 128, __RAM_PAGE - 1, __RAM_PAGE - length / 2 };

        for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
            struct OSDmaMappingResult result;

            assert(__MapBounce(&fixture, g_buffer->ID, offsets[o], length, DMAToDevice, 0, &result) == OS_EOK);
            __CheckPublishedSegment(&result, length);
            __CheckBounceLayout(&fixture, &result, granule, length);
            // The device can reach the padding, but a transfer cannot name it.
            assert(__BeginTransfer(result.Handle, 0, length + 1) == OS_EINVALPARAMS);
            assert(__BeginTransfer(result.Handle, length, 1) == OS_EINVALPARAMS);
            assert(__BeginTransfer(result.Handle, 0, length) == OS_EOK);
            assert(__DeviceAccess(&fixture, result.InlineSegment.Address, length, DMAToDevice, bytes) == OS_EOK);
            for (size_t i = 0; i < length; i++) {
                assert(bytes[i] == __SourceByte(offsets[o] + i));
            }
            __ExpectDeviceBytes(&fixture, result.InlineSegment.Address + length, granule - length, 0);
            assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, length) == OS_EOK);
            assert(ScResourceRelease(result.Handle) == OS_EOK);
            __ExpectNoBounceResources(&fixture, freePages);
        }
    }

    // A 64 KiB bounce granule behind a single 4 KiB source page is mapped by
    // sixteen bounce page addresses, all inside the one unit.
    if (granule > __RAM_PAGE) {
        struct OSDmaMappingResult result;
        SHMDeviceMapping_t*       mapping;

        assert(__MapBounce(&fixture, g_buffer->ID, 0, 512, DMAToDevice, 0, &result) == OS_EOK);
        mapping = __LookupMapping(result.Handle);
        assert(g_lastBouncePageCount == granule / __RAM_PAGE);
        for (size_t i = 0; i < g_lastBouncePageCount; i++) {
            assert(g_lastBouncePages[i] == mapping->Stream.Bounce.Units[0].PhysicalBase + i * __RAM_PAGE);
        }
        assert(ScResourceRelease(result.Handle) == OS_EOK);
    }
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// A restricted view inside a buffer that itself starts 128 bytes into its
// first page. Both offsets must be applied exactly once, in both directions,
// and the bytes around the grant must not change, even when the device writes
// all of the padding.
static void
__TestBounceGrantOffsets(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    SHMReference_t             source;
    SHMHandle_t                view;
    const size_t               viewOffset = 1000;
    const size_t               mapOffset = 8;
    const size_t               length = 500;
    const size_t               first = 128 + viewOffset + mapOffset;
    uint8_t                    written[4096];
    uint8_t                    bytes[512];
    uint8_t*                   ram;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 2, 128, 2 * __RAM_PAGE - 128);
    ram = __RamPointer(__RAM_BASE, 2 * __RAM_PAGE);
    assert(SHMResolve(g_buffer->ID, &source) == OS_EOK);
    assert(SHMViewCreate(&source, viewOffset, 512, GrantTokenScopeRead | GrantTokenScopeWrite, driver, &view) == OS_EOK);
    assert(ReleaseGlobalHandle(source.GlobalHandleID) == OS_EINCOMPLETE);

    assert(__MapBounce(&fixture, view.ID, mapOffset, length, DMABidirectional, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, length);
    assert(__BeginTransfer(result.Handle, 0, length) == OS_EOK);
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, length, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < length; i++) {
        assert(bytes[i] == __SourceByte(first + i));
    }

    // The device writes the payload and all of the padding.
    for (size_t i = 0; i < sizeof(written); i++) {
        written[i] = (uint8_t)(0xc0 ^ i);
    }
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 4096, DMAFromDevice, written) == OS_EOK);
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, length) == OS_EOK);
    for (size_t i = 0; i < 2 * __RAM_PAGE; i++) {
        bool inside = i >= first && i < first + length;

        assert(ram[i] == (inside ? written[i - first] : __SourceByte(i)));
    }
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(DestroyHandle(view.ID) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// A request one byte over a granule takes two units and maps two granules.
// Units from a fragmented pool are not adjacent, yet the device sees one IOVA
// span and the copies cross the unit boundaries correctly. Units that do
// follow each other in the pool become one extent.
static void
__TestBounceUnits(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture    fixture;
    struct OSDmaMappingResult     result;
    struct SystemMemoryAllocation held[__RAM_POOL_LENGTH / __RAM_PAGE] = { 0 };
    SHMDeviceMapping_t*           mapping;
    const SHMDeviceBounce_t*      bounce;
    size_t                        pageCount = __RAM_POOL_LENGTH / __RAM_PAGE;
    size_t                        freePages;
    uint8_t*                      bytes = malloc(4 * __RAM_PAGE);

    assert(bytes != NULL);
    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 6, 0, 6 * __RAM_PAGE);
    freePages = __PoolFreePages();

    assert(__MapBounce(&fixture, g_buffer->ID, 100, 4097, DMABidirectional, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, 4097);
    __CheckBounceLayout(&fixture, &result, 4096, 4097);
    mapping = __LookupMapping(result.Handle);
    bounce = &mapping->Stream.Bounce;
    assert(bounce->UnitCount == 2 && bounce->MappedLength == 8192);
    assert(bounce->Units[1].PhysicalBase == bounce->Units[0].PhysicalBase + __RAM_PAGE);
    assert(mapping->ExtentCount == 1 && mapping->Extents[0].Length == 8192);
    assert(__BeginTransfer(result.Handle, 0, 4097) == OS_EOK);
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 4097, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < 4097; i++) {
        assert(bytes[i] == __SourceByte(100 + i));
    }
    __ExpectDeviceBytes(&fixture, result.InlineSegment.Address + 4097, 8192 - 4097, 0);
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 0) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __ExpectNoBounceResources(&fixture, freePages);

    // Hold every second pool page, so no two free pages are adjacent.
    for (size_t i = 0; i < pageCount; i++) {
        assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, __RAM_PAGE, 0, 0, &held[i]) == OS_EOK);
    }
    for (size_t i = 1; i < pageCount; i += 2) {
        assert(SystemMemoryReleaseAllocation(&held[i]) == OS_EOK);
    }
    freePages = __PoolFreePages();
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 4 * __RAM_PAGE, DMAToDevice, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, 4 * __RAM_PAGE);
    __CheckBounceLayout(&fixture, &result, 4096, 4 * __RAM_PAGE);
    mapping = __LookupMapping(result.Handle);
    bounce = &mapping->Stream.Bounce;
    assert(bounce->UnitCount == 4 && mapping->ExtentCount == 4);
    for (size_t i = 1; i < 4; i++) {
        assert(bounce->Units[i].PhysicalBase != bounce->Units[i - 1].PhysicalBase + __RAM_PAGE);
    }
    assert(__BeginTransfer(result.Handle, 0, 4 * __RAM_PAGE) == OS_EOK);
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 4 * __RAM_PAGE, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < 4 * __RAM_PAGE; i++) {
        assert(bytes[i] == __SourceByte(1 + i));
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 4 * __RAM_PAGE) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __ExpectNoBounceResources(&fixture, freePages);
    for (size_t i = 0; i < pageCount; i += 2) {
        assert(SystemMemoryReleaseAllocation(&held[i]) == OS_EOK);
    }
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
    free(bytes);
}

// Source pages whose layout cannot be mapped as they are. Each case bounces
// when bounce is allowed. Direct-only and persistent mappings keep the error
// they always had, and an exact layout still uses the original pages.
static void
__TestBounceSelection(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    const unsigned int         forbidden[] = { DMA_MAP_REQUIRE_DIRECT, DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT };
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    size_t                     freePages;
    int                        views;

    // Committed 4 KiB pages that do not form one contiguous 64 KiB granule.
    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 65536, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 16, 0, 16 * __RAM_PAGE);
    g_buffer->Pages[3] += 16 * __RAM_PAGE;
    for (unsigned int i = 0; i < 2; i++) {
        assert(__MapBounce(&fixture, g_buffer->ID, 0, 65536, DMAToDevice, forbidden[i], &result) == OS_ENOTSUPPORTED);
    }
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 65536, DMAToDevice, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, 65536);
    __CheckBounceLayout(&fixture, &result, 65536, 65536);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // The same 64 KiB, now physically contiguous and aligned, is mapped from
    // the original pages. No pool page and no kernel view is used.
    g_buffer->Pages[3] -= 16 * __RAM_PAGE;
    freePages = __PoolFreePages();
    views = g_mapCalls;
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 65536, DMAToDevice, 0, &result) == OS_EOK);
    assert(__PoolFreePages() == freePages && g_mapCalls == views);
    assert(!__LookupMapping(result.Handle)->Stream.Bounce.Translated);
    __CheckHardwareID(&fixture, 0, result.InlineSegment.Address, 65536, DMAToDevice, __RAM_BASE);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);

    // Whole granules above the unit's physical limit. Bounce storage comes
    // from the pool below the limit. Other kinds keep the address core error.
    fixture = __PrepareBounceFixture(service, driver, 4096, __RAM_LOW_LIMIT, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_HIGH, 2, 0, 2 * __RAM_PAGE);
    for (unsigned int i = 0; i < 2; i++) {
        assert(__MapBounce(&fixture, g_buffer->ID, 0, 8192, DMAToDevice, forbidden[i], &result) == OS_EINVALPARAMS);
    }
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 8192, DMABidirectional, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, 8192);
    __CheckBounceLayout(&fixture, &result, 4096, 8192);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // A pool above the limit cannot help. The address core rejects the
    // bounce storage, and every unit goes back.
    __RamUsePool(__RAM_HIGH + 4 * __RAM_PAGE, 16 * __RAM_PAGE, false);
    freePages = __PoolFreePages();
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 8192, DMABidirectional, 0, &result) == OS_EINVALPARAMS);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// The size limit is applied to the mapped length, before any allocation. A
// refusal is a normal result, so only the first one of a context is a warning,
// and closing the context reports how many there were.
static void
__TestBounceSizeLimit(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    const char*                notice = "above the bounce limit of 262144";
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct OSDmaMappingResult  unchanged;
    DMAIOVAAllocation_t*       interval = NULL;
    DMAIOVARequest_t           request = { .Length = 4096, .Limit = __IOVA_LAST };
    size_t                     freePages;
    unsigned int               unitReleases;
    int                        views;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 66, 0, 66 * __RAM_PAGE);
    freePages = __PoolFreePages();

    // Exactly at the limit, 128 bytes into a page: 64 units of 4096 bytes.
    assert(__MapBounce(&fixture, g_buffer->ID, 128, SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT, DMAToDevice, 0, &result) == OS_EOK);
    __CheckPublishedSegment(&result, SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT);
    assert(__LookupMapping(result.Handle)->Stream.Bounce.UnitCount == 64);
    assert(__PoolFreePages() == freePages - 64);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // One byte more maps one granule more and is refused before any pool
    // allocation, kernel view or IOVA reservation.
    memset(&unchanged, 0xa5, sizeof(unchanged));
    result = unchanged;
    unitReleases = g_unitReleaseCalls;
    views = g_mapCalls;
    g_boundWarningHistory[0] = '\0';
    g_boundErrorHistory[0] = '\0';
    assert(__MapBounce(&fixture, g_buffer->ID, 128, SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT + 1,
        DMAToDevice, 0, &result) == OS_EBUFFER);
    assert(memcmp(&result, &unchanged, sizeof(result)) == 0);
    assert(g_mapCalls == views && g_unitReleaseCalls == unitReleases);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(strstr(g_boundWarningHistory, "length 262145 maps to 266240 bytes, above the bounce limit of 262144. "
        "Later refusals on this context are only traced") != NULL);

    // The same request again is refused the same way, but only traced. The
    // refusal is never an error in the SHM device layer.
    assert(__MapBounce(&fixture, g_buffer->ID, 128, SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT + 1,
        DMAToDevice, 0, &result) == OS_EBUFFER);
    assert(__CountText(g_boundWarningHistory, notice) == 1);
    assert(strstr(g_boundErrorHistory, "bounce limit") == NULL);
    // The syscall layer only traces the refusal as well.
    assert(strstr(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == NULL);
    assert(fixture.Contexts[0]->TranslatedBounce.LimitRefusals == 2);
    assert(DMAIOVAAllocate(fixture.Domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMAIOVAAllocationRange(interval)->First == __IOVA_FIRST);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    g_boundWarningHistory[0] = '\0';
    __FinishTranslatedFixture(&fixture);
    assert(strstr(g_boundWarningHistory, "had 2 translated bounce requests above the size limit and 0 pool shortages") != NULL);

    // The limit counts mapped bytes. 512 bytes behind a 512 KiB granule
    // would hold twice the limit. A new context gives its own first warning.
    fixture = __PrepareBounceFixture(service, driver, 0x80000, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 1, 0, __RAM_PAGE);
    views = g_mapCalls;
    g_boundWarningHistory[0] = '\0';
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 512, DMAToDevice, 0, &result) == OS_EBUFFER);
    assert(g_mapCalls == views);
    assert(strstr(g_boundWarningHistory, "length 512 maps to 524288 bytes, above the bounce limit") != NULL);
    assert(__CountText(g_boundWarningHistory, notice) == 1);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Grant scope decides the directions, as for any mapping, and a refused or
// invalid request publishes nothing and leaves no bounce storage behind. A
// device-write transfer never reads the source: its bounce bytes stay zero.
static void
__TestBounceAuthorization(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    const struct {
        GrantTokenScope_t Scope;
        enum DMADirection Refused;
        enum DMADirection Allowed;
    } grants[] = {
        { GrantTokenScopeRead, DMAFromDevice, DMAToDevice },
        { GrantTokenScopeWrite, DMAToDevice, DMAFromDevice },
        { GrantTokenScopeRead | GrantTokenScopeWrite, (enum DMADirection)99, DMABidirectional }
    };
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct OSDmaMappingResult  unchanged;
    SHMReference_t             source;
    SHMHandle_t                view;
    SHMDeviceMapping_t*        mapping;
    size_t                     freePages;
    int                        views;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 2, 0, 2 * __RAM_PAGE);
    freePages = __PoolFreePages();
    memset(&unchanged, 0xa5, sizeof(unchanged));
    for (unsigned int i = 0; i < sizeof(grants) / sizeof(grants[0]); i++) {
        assert(SHMResolve(g_buffer->ID, &source) == OS_EOK);
        assert(SHMViewCreate(&source, 64, 1024, grants[i].Scope, driver, &view) == OS_EOK);
        assert(ReleaseGlobalHandle(source.GlobalHandleID) == OS_EINCOMPLETE);
        result = unchanged;
        assert(__MapBounce(&fixture, view.ID, 0, 512, grants[i].Refused, 0, &result) ==
            (grants[i].Refused == DMAFromDevice || grants[i].Refused == DMAToDevice ? OS_EPERMISSIONS : OS_EINVALPARAMS));
        assert(memcmp(&result, &unchanged, sizeof(result)) == 0);
        __ExpectNoBounceResources(&fixture, freePages);
        result = unchanged;
        assert(__MapBounce(&fixture, view.ID, 1000, 25, grants[i].Allowed, 0, &result) == OS_EINVALPARAMS);
        assert(memcmp(&result, &unchanged, sizeof(result)) == 0);

        assert(__MapBounce(&fixture, view.ID, 0, 512, grants[i].Allowed, 0, &result) == OS_EOK);
        mapping = __LookupMapping(result.Handle);
        assert(__BeginTransfer(result.Handle, 0, 512) == OS_EOK);
        if (grants[i].Allowed == DMAFromDevice) {
            const uint8_t* storage = __RamPointer(mapping->Stream.Bounce.Units[0].PhysicalBase, 512);

            for (size_t b = 0; b < 512; b++) {
                assert(storage[b] == 0);
            }
            assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, result.InlineSegment.Address, 1,
                DMAToDevice, &(SHMSG_t){ 0 }, 1, &(uint32_t){ 0 }) == OS_EPERMISSIONS);
        } else {
            assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, result.InlineSegment.Address + 4095, 1,
                DMAToDevice, &(SHMSG_t){ 0 }, 1, &(uint32_t){ 0 }) == OS_EOK);
        }
        if (grants[i].Allowed == DMAToDevice) {
            assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, result.InlineSegment.Address + 4095, 1,
                DMAFromDevice, &(SHMSG_t){ 0 }, 1, &(uint32_t){ 0 }) == OS_EPERMISSIONS);
        }
        assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 0) == OS_EOK);
        assert(ScResourceRelease(result.Handle) == OS_EOK);
        assert(DestroyHandle(view.ID) == OS_EOK);
        __Drain();
        __ExpectNoBounceResources(&fixture, freePages);
    }

    // A missing page and a region buffer are refused before any bounce work.
    views = g_mapCalls;
    g_buffer->Pages[1] = 0;
    result = unchanged;
    assert(__MapBounce(&fixture, g_buffer->ID, 100, 4096, DMAToDevice, 0, &result) == OS_EINCOMPLETE);
    g_buffer->Pages[1] = __RAM_BASE + __RAM_PAGE;
    g_buffer->Flags |= SHM_REGIONS;
    assert(__MapBounce(&fixture, g_buffer->ID, 100, 4096, DMAToDevice, 0, &result) == OS_ENOTSUPPORTED);
    g_buffer->Flags &= ~SHM_REGIONS;
    assert(__MapBounce(&fixture, g_buffer->ID, 8192 - 100, 101, DMAToDevice, 0, &result) == OS_EINVALPARAMS);
    assert(memcmp(&result, &unchanged, sizeof(result)) == 0 && g_mapCalls == views);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Device requirements, IOVA placement and pool shortages. LengthAlignment is
// checked against the request, AddressAlignment against the IOVA only. Every
// failure returns its own error, publishes nothing and gives back every unit.
static void
__TestBouncePlacement(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedOptions    options = { .PhysicalLimit = UINT64_MAX, .CachePolicy = SHMDeviceCacheCoherent };
    struct __TranslatedFixture    fixture;
    struct OSDmaMappingResult     result;
    struct OSDmaMapParameters     parameters = { .Offset = 1, .Length = 512, .Direction = DMAToDevice };
    struct SystemMemoryAllocation held[__RAM_POOL_LENGTH / __RAM_PAGE] = { 0 };
    DMAIOVAAllocation_t*          interval = NULL;
    size_t                        freePages;
    int                           views;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 8, 0, 8 * __RAM_PAGE);
    freePages = __PoolFreePages();

    // 512 bytes are not a multiple of 4096, even though the mapped granule is.
    views = g_mapCalls;
    parameters.Requirements.LengthAlignment = 4096;
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_EINVALPARAMS);
    assert(g_mapCalls == views);
    __ExpectNoBounceResources(&fixture, freePages);

    // A 256 KiB IOVA alignment, from a pool that has no 256 KiB aligned page.
    __RamUsePool(__RAM_POOL + __RAM_PAGE, 32 * __RAM_PAGE, false);
    freePages = __PoolFreePages();
    parameters.Requirements = (struct DMAMapRequirements) { .AddressAlignment = 0x40000, .LengthAlignment = 512 };
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_EOK);
    assert(result.InlineSegment.Address % 0x40000 == 0 && result.InlineSegment.Length == 512);
    assert(__LookupMapping(result.Handle)->Stream.Bounce.Units[0].PhysicalBase % 0x40000 != 0);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // No IOVA left: the allocator's error, no retry, every unit back.
    parameters.Requirements = (struct DMAMapRequirements) { 0 };
    assert(DMAIOVAAllocate(fixture.Domain->AddressSpace->Translated.Allocator, &(DMAIOVARequest_t) {
        .Length = __IOVA_LAST + 1 - __IOVA_FIRST, .Limit = __IOVA_LAST }, &interval) == OS_EOK);
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_ENOENT);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);

    // The pool is full, or runs out after two of four units were taken. A
    // shortage is a normal result: the first one is a warning, the second is
    // only traced, and neither is an error.
    for (size_t i = 0; i < 32; i++) {
        assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, __RAM_PAGE, 0, 0, &held[i]) == OS_EOK);
    }
    g_boundWarningHistory[0] = '\0';
    g_boundErrorHistory[0] = '\0';
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_EOOM);
    __ExpectNoBounceResources(&fixture, 0);
    assert(strstr(g_boundWarningHistory, "has no pool storage for translated bounce unit 0 of 1, length 4096. "
        "Later shortages on this context are only traced") != NULL);
    assert(SystemMemoryReleaseAllocation(&held[3]) == OS_EOK);
    assert(SystemMemoryReleaseAllocation(&held[9]) == OS_EOK);
    parameters.Length = 4 * __RAM_PAGE;
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_EOOM);
    __ExpectNoBounceResources(&fixture, 2);
    assert(__CountText(g_boundWarningHistory, "has no pool storage") == 1);
    assert(strstr(g_boundErrorHistory, "SystemMemoryAllocateDevice") == NULL);
    // The syscall layer only traces the shortage as well.
    assert(strstr(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == NULL);
    assert(fixture.Contexts[0]->TranslatedBounce.PoolShortages == 2);
    for (size_t i = 0; i < 32; i++) {
        assert(SystemMemoryReleaseAllocation(&held[i]) == OS_EOK);
    }

    // Without a pool nothing can bounce. A missing pool is not a shortage and
    // is logged as an error each time.
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, __RAM_PAGE, 0);
    for (int attempt = 0; attempt < 2; attempt++) {
        assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_ENOTSUPPORTED);
        assert(g_liveViews == 0 && fixture.Contexts[0]->Mappings == NULL);
    }
    assert(__CountText(g_boundErrorHistory, "SystemMemoryAllocateDevice unit 0 of 4, length 4096 failed") == 2);
    assert(__CountText(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == 2);
    assert(fixture.Contexts[0]->TranslatedBounce.PoolShortages == 2);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    g_boundWarningHistory[0] = '\0';
    __FinishTranslatedFixture(&fixture);
    assert(strstr(g_boundWarningHistory, "had 0 translated bounce requests above the size limit and 2 pool shortages") != NULL);

    // IOVA zero is a valid device address. A context ceiling that leaves
    // room for 512 bytes but not a whole granule cannot place the span.
    __RamUsePool(__RAM_POOL, __RAM_POOL_LENGTH, false);
    freePages = __PoolFreePages();
    options.IOVAFromZero = true;
    fixture = __PrepareTranslatedFixtureWith(service, driver, 4096, 1, &options);
    __CreateRamBuffer(__RAM_BASE, 1, 0, __RAM_PAGE);
    parameters = (struct OSDmaMapParameters) { .Offset = 1, .Length = 512, .Direction = DMAToDevice };
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_EOK);
    __CheckPublishedSegment(&result, 512);
    assert(result.InlineSegment.Address == 0);
    __ExpectDeviceBytes(&fixture, 0, 4096, 0);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    options.ContextLimit = 511;
    fixture = __PrepareTranslatedFixtureWith(service, driver, 4096, 1, &options);
    __CreateRamBuffer(__RAM_BASE, 1, 0, __RAM_PAGE);
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &result) == OS_ENOENT);
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// The cache calls since the last check must be exactly one per unit, each
// cleaning and invalidating the whole unit: the zeroed storage is written to
// RAM before the device gets a translation to it.
static void
__ExpectUnitCacheCalls(const SHMDeviceMapping_t* mapping)
{
    const SHMDeviceBounce_t* bounce = &mapping->Stream.Bounce;

    assert(g_cacheCallCount == (int)bounce->UnitCount);
    for (size_t i = 0; i < bounce->UnitCount; i++) {
        assert(g_cacheCalls[i].Operation == __CacheCleanInvalidate);
        assert(g_cacheCalls[i].Address == bounce->Units[i].PhysicalBase);
        assert(g_cacheCalls[i].Length == bounce->Units[i].Length);
    }
    g_cacheCallCount = 0;
}

// When expected, the cache calls since the last check must cover the whole
// mapped span of bounce storage, one call per extent, and nothing else. A
// transfer of any length gets the same span.
static void
__ExpectSpanCacheCalls(const SHMDeviceMapping_t* mapping, bool expected, enum __CacheOperation operation)
{
    if (!expected) {
        assert(g_cacheCallCount == 0);
        return;
    }
    assert(g_cacheCallCount == mapping->ExtentCount);
    for (int i = 0; i < mapping->ExtentCount; i++) {
        assert(g_cacheCalls[i].Operation == operation);
        assert(g_cacheCalls[i].Address == mapping->Extents[i].Address);
        assert(g_cacheCalls[i].Length == mapping->Extents[i].Length);
    }
    g_cacheCallCount = 0;
}

// Resources that begin and end must never touch.
struct __BounceSnapshot {
    unsigned int       Allocations;
    int                MapCalls;
    size_t             FreePages;
    DMAIOMMUSimStats_t Sim;
    struct DMASegment  Segment;
};

static struct __BounceSnapshot
__TakeBounceSnapshot(struct __TranslatedFixture* fixture, uuid_t mapping)
{
    struct __BounceSnapshot snapshot = { 0 };
    uint32_t                count = 0;

    snapshot.Allocations = atomic_load(&g_recentAllocationIndex);
    snapshot.MapCalls = g_mapCalls;
    snapshot.FreePages = __PoolFreePages();
    DMAIOMMUSimSnapshot(fixture->Sim, &snapshot.Sim);
    assert(ScDmaGetSegments(mapping, &snapshot.Segment, 1, &count) == OS_EOK && count == 1);
    return snapshot;
}

static void
__ExpectSameSnapshot(const struct __BounceSnapshot* before, const struct __BounceSnapshot* after)
{
    assert(after->Allocations == before->Allocations && after->MapCalls == before->MapCalls);
    assert(after->FreePages == before->FreePages);
    assert(after->Sim.Mappings == before->Sim.Mappings);
    assert(after->Sim.SubmittedCommands == before->Sim.SubmittedCommands);
    assert(memcmp(&after->Segment, &before->Segment, sizeof(after->Segment)) == 0);
}

// One direction and one cache setup. The mapping covers 5000 bytes, 300
// bytes into the source, so it holds two units and 3192 bytes of padding.
// A full transfer, a subrange with zero completion and a later transfer at
// another offset run on the same mapping.
static void
__RunBounceTransfers(MSContextSecurity_t* service, MSContextSecurity_t* driver,
                     enum SHMDeviceCachePolicy policy, bool poolCached, enum DMADirection direction)
{
    const size_t               offset = 300;
    const size_t               length = 5000;
    const size_t               mapped = 8192;
    bool                       cacheWork = poolCached && policy == SHMDeviceCacheNonCoherent;
    bool                       deviceReads = direction != DMAFromDevice;
    bool                       deviceWrites = direction != DMAToDevice;
    enum __CacheOperation      beginOperation = deviceWrites ? __CacheCleanInvalidate : __CacheClean;
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct __BounceSnapshot    before;
    struct __BounceSnapshot    after;
    SHMDeviceMapping_t*        mapping;
    uint8_t                    written[8192];
    uint8_t                    bytes[8192];
    uint8_t*                   ram;

    __RamStart();
    __RamUsePool(__RAM_POOL, __RAM_POOL_LENGTH, poolCached);
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, policy);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, 3 * __RAM_PAGE);
    for (size_t i = 0; i < sizeof(written); i++) {
        written[i] = (uint8_t)(0x80 + i * 13);
    }

    g_cacheCallCount = 0;
    assert(__MapBounce(&fixture, g_buffer->ID, offset, length, direction, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);
    assert(mapping->Stream.Bounce.UnitCount == 2 && mapping->Stream.Bounce.MappedLength == mapped);
    if (cacheWork) {
        __ExpectUnitCacheCalls(mapping);
    } else {
        assert(g_cacheCallCount == 0);
    }
    before = __TakeBounceSnapshot(&fixture, result.Handle);

    // A full transfer. The device writes the payload and all of the padding,
    // but only the first 1000 bytes are reported complete.
    assert(__BeginTransfer(result.Handle, 0, length) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork, beginOperation);
    if (deviceReads) {
        assert(__DeviceAccess(&fixture, result.InlineSegment.Address, length, DMAToDevice, bytes) == OS_EOK);
        for (size_t i = 0; i < length; i++) {
            assert(bytes[i] == __SourceByte(offset + i));
        }
    }
    if (deviceWrites) {
        assert(__DeviceAccess(&fixture, result.InlineSegment.Address, mapped, DMAFromDevice, written) == OS_EOK);
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, length + 1) == OS_EINVALPARAMS);
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 1000) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork && deviceWrites, __CacheInvalidate);
    for (size_t i = 0; i < 3 * __RAM_PAGE; i++) {
        bool received = deviceWrites && i >= offset && i < offset + 1000;

        assert(ram[i] == (received ? written[i - offset] : __SourceByte(i)));
    }

    // A subrange after the CPU changed the source. The device reads the new
    // bytes. Zero completion copies nothing back, but still gets the full
    // cache work, because the device may have written anyway.
    memset(ram + offset + 2000, 0x77, 100);
    assert(__BeginTransfer(result.Handle, 2000, 100) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork, beginOperation);
    if (deviceReads) {
        assert(__DeviceAccess(&fixture, result.InlineSegment.Address + 2000, 100, DMAToDevice, bytes) == OS_EOK);
        for (size_t i = 0; i < 100; i++) {
            assert(bytes[i] == 0x77);
        }
    }
    if (deviceWrites) {
        memset(bytes, 0x33, 100);
        assert(__DeviceAccess(&fixture, result.InlineSegment.Address + 2000, 100, DMAFromDevice, bytes) == OS_EOK);
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 0) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork && deviceWrites, __CacheInvalidate);
    for (size_t i = 0; i < 100; i++) {
        assert(ram[offset + 2000 + i] == 0x77);
    }

    // A later transfer at the end of the mapping. Only its prefix comes back.
    assert(__BeginTransfer(result.Handle, 4000, 1000) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork, beginOperation);
    if (deviceWrites) {
        memset(bytes, 0x44, 1000);
        assert(__DeviceAccess(&fixture, result.InlineSegment.Address + 4000, 1000, DMAFromDevice, bytes) == OS_EOK);
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 600) == OS_EOK);
    __ExpectSpanCacheCalls(mapping, cacheWork && deviceWrites, __CacheInvalidate);
    for (size_t i = 4000; i < 5000; i++) {
        bool received = deviceWrites && i < 4600;

        assert(ram[offset + i] == (received ? 0x44 : __SourceByte(offset + i)));
    }
    assert(ram[offset + length] == __SourceByte(offset + length));

    // Begin and end never allocate, map, install or remove anything.
    after = __TakeBounceSnapshot(&fixture, result.Handle);
    __ExpectSameSnapshot(&before, &after);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __ExpectNoBounceResources(&fixture, before.FreePages + 2);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

static void
__TestBounceTransfers(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    const enum DMADirection directions[] = { DMAToDevice, DMAFromDevice, DMABidirectional };

    for (unsigned int setup = 0; setup < 4; setup++) {
        enum SHMDeviceCachePolicy policy = setup < 2 ? SHMDeviceCacheCoherent : SHMDeviceCacheNonCoherent;

        for (unsigned int d = 0; d < 3; d++) {
            __RunBounceTransfers(service, driver, policy, (setup & 1) != 0, directions[d]);
        }
    }
}

// The context counts held, peak and prepared translated bounce storage. Only
// its first bounce mapping is a warning, and closing it reports the totals.
// An exact layout changes no counter.
static void
__TestBounceCounters(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    const char*                notice = "through translated bounce storage";
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  exact;
    struct OSDmaMappingResult  first;
    struct OSDmaMappingResult  second;
    struct SHMDeviceBounceUse* use;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, __RAM_LOW_LIMIT, SHMDeviceCacheCoherent);
    use = &fixture.Contexts[0]->TranslatedBounce;
    __CreateRamBuffer(__RAM_BASE, 4, 0, 4 * __RAM_PAGE);
    g_boundWarningHistory[0] = '\0';
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 8192, DMAToDevice, 0, &exact) == OS_EOK);
    assert(use->HeldBytes == 0 && use->PeakBytes == 0 && use->Mappings == 0);
    assert(__CountText(g_boundWarningHistory, notice) == 0);

    assert(__MapBounce(&fixture, g_buffer->ID, 1, 512, DMAToDevice, 0, &first) == OS_EOK);
    assert(use->HeldBytes == 4096 && use->PeakBytes == 4096 && use->Mappings == 1);
    assert(__CountText(g_boundWarningHistory, notice) == 1);
    assert(strstr(g_boundWarningHistory, "range that is not whole granules through translated bounce storage: "
        "length 512, granule 4096, mapped 4096") != NULL);
    assert(__MapBounce(&fixture, g_buffer->ID, 100, 4097, DMAFromDevice, 0, &second) == OS_EOK);
    assert(use->HeldBytes == 3 * 4096 && use->PeakBytes == 3 * 4096 && use->Mappings == 2);
    assert(__CountText(g_boundWarningHistory, notice) == 1);
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(use->HeldBytes == 2 * 4096 && use->PeakBytes == 3 * 4096);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(ScResourceRelease(exact.Handle) == OS_EOK);
    assert(use->HeldBytes == 0 && use->PeakBytes == 3 * 4096 && use->Mappings == 2);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();

    // A second context gives its own first notice, here for the physical limit.
    __CreateRamBuffer(__RAM_HIGH, 1, 0, __RAM_PAGE);
    g_boundWarningHistory[0] = '\0';
    g_boundErrorHistory[0] = '\0';
    __FinishTranslatedFixture(&fixture);
    assert(strstr(g_boundWarningHistory, "prepared 2 translated bounce mappings, holding at most 12288 bytes") != NULL);
    assert(strstr(g_boundWarningHistory, "pool shortages") == NULL);
    assert(strstr(g_boundErrorHistory, "is freed with") == NULL);
    fixture = __PrepareBounceFixture(service, driver, 4096, __RAM_LOW_LIMIT, SHMDeviceCacheCoherent);
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 4096, DMAToDevice, 0, &first) == OS_EOK);
    assert(strstr(g_boundWarningHistory, "range that lies above the physical limit through translated bounce storage") != NULL);
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Every allocation and kernel view of a translated bounce map can fail. Each
// failure publishes nothing and gives back everything taken so far.
static void
__TestBounceSetupFailures(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct OSDmaMappingResult  unchanged;
    size_t                     freePages;
    unsigned int               releases;
    oserr_t                    status;
    int                        failures = 0;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    freePages = __PoolFreePages();
    memset(&unchanged, 0xa5, sizeof(unchanged));
    for (int allowed = 0;; allowed++) {
        result = unchanged;
        g_failAllocation = allowed;
        status = __MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result);
        g_failAllocation = -1;
        __Drain();
        if (status == OS_EOK) {
            break;
        }
        failures++;
        assert(status == OS_EOOM && memcmp(&result, &unchanged, sizeof(result)) == 0);
        __ExpectNoBounceResources(&fixture, freePages);
        assert(fixture.Contexts[0]->TranslatedBounce.HeldBytes == 0);
    }
    assert(failures >= 6);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    for (int view = 1; view <= 2; view++) {
        g_failMapAt = g_mapCalls + view;
        result = unchanged;
        assert(__MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result) == OS_EOOM);
        g_failMapAt = 0;
        assert(memcmp(&result, &unchanged, sizeof(result)) == 0);
        __ExpectNoBounceResources(&fixture, freePages);
    }

    // A prepared address mapping is released before the bounce storage. Here
    // the published segment check fails after the IOVA span was prepared.
    releases = g_addressReleaseCalls;
    g_segmentCountOverride = 2;
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result) == OS_ENOTSUPPORTED);
    assert(g_addressReleaseCalls == releases + 1 && g_bounceUnmapAddressReleases == releases + 1);
    __ExpectNoBounceResources(&fixture, freePages);

    // A source view that cannot be removed is logged and left. The units
    // still go back to the pool.
    g_boundErrorHistory[0] = '\0';
    g_failSourceUnmaps = 1;
    g_segmentCountOverride = 2;
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result) == OS_ENOTSUPPORTED);
    assert(strstr(g_boundErrorHistory, "MemorySpaceUnmap source view") != NULL);
    assert(strstr(g_boundErrorHistory, "0 bytes left allocated") != NULL);
    assert(__PoolFreePages() == freePages && __RamBounceViews() == 0 && g_liveViews == 1);
    assert(fixture.Contexts[0]->Binding->Group->Access.State == DMARecoveryLive);
    for (unsigned int i = 0; i < __RAM_VIEWS; i++) {
        if (g_ramViews[i].Address != NULL) {
            assert(munmap(g_ramViews[i].Address, g_ramViews[i].Length) == 0);
            g_ramViews[i] = (struct __RamView){ 0 };
            g_liveViews--;
        }
    }
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Storage that cannot be freed before installation is quarantined: the map
// keeps its original error, the units stay out of the pool and keep counting,
// the binding stays live, the next map on the context works, and closing the
// context logs the lost bytes as an error. Runs in a child because the
// quarantined records are lost on purpose.
static void
__TestBounceQuarantineTranslated(MSContextSecurity_t* service, MSContextSecurity_t* driver, int unitFailure)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    size_t                     freePages;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    freePages = __PoolFreePages();
    g_boundErrorHistory[0] = '\0';
    if (unitFailure) {
        g_failUnitReleases = 1;
    } else {
        g_failBounceUnmaps = 1;
    }
    g_segmentCountOverride = 2;
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result) == OS_ENOTSUPPORTED);
    assert(fixture.Contexts[0]->Mappings == NULL);
    assert(__PoolFreePages() == freePages - (unitFailure ? 1 : 2));
    assert(fixture.Contexts[0]->TranslatedBounce.HeldBytes == (unitFailure ? 4096U : 8192U));
    assert(strstr(g_boundErrorHistory, unitFailure ? "4096 bytes left allocated" : "8192 bytes left allocated") != NULL);
    assert(strstr(g_boundErrorHistory, "(original status 8)") != NULL);
    assert(fixture.Contexts[0]->Binding->Group->Access.State == DMARecoveryLive);
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 5000, DMAToDevice, 0, &result) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // The quarantined bytes are still counted when the context is freed, and
    // the closing report names them as lost. RAM is not stopped, because a
    // quarantined bounce view stays mapped for good.
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    g_boundErrorHistory[0] = '\0';
    __FinishTranslatedFixture(&fixture);
    assert(strstr(g_boundErrorHistory, unitFailure ?
        "is freed with 4096 bytes of translated bounce storage that never went back to the pool" :
        "is freed with 8192 bytes of translated bounce storage that never went back to the pool") != NULL);
}

// An installed mapping whose storage release fails keeps its handle, its
// source reference and every unit still allocated. A retry resumes at the
// storage: the translation is removed once, a unit that failed in the middle
// is the only one returned again, and the counters follow each unit.
static void
__TestBounceInstalledRelease(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool stop)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    OSDmaStopTicket_t          ticket;
    SHMDeviceMapping_t*        mapping;
    SHMDeviceBounce_t*         bounce;
    size_t                     freePages;
    unsigned int               addressReleases;
    unsigned int               unitReleases;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 4, 0, 4 * __RAM_PAGE);
    freePages = __PoolFreePages();
    assert(__MapBounce(&fixture, g_buffer->ID, 1, 3 * __RAM_PAGE, DMAFromDevice, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);
    bounce = &mapping->Stream.Bounce;
    assert(bounce->UnitCount == 3);
    addressReleases = g_addressReleaseCalls;
    unitReleases = g_unitReleaseCalls;
    if (stop) {
        g_current = fixture.Service;
        assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    }

    // The middle unit is refused. The first and last go back. Stop passes
    // run as the service, and the mapping handle belongs to the driver.
    g_failUnitBase = bounce->Units[1].PhysicalBase;
    g_failUnitReleases = 1;
    assert((stop ? DMAManagerCompleteStop(fixture.Manager, &ticket) : ScResourceRelease(result.Handle)) == OS_EUNKNOWN);
    g_current = fixture.Driver;
    assert(g_unitReleaseCalls == unitReleases + 3 && g_addressReleaseCalls == addressReleases + 1);
    assert(bounce->Units[0].Owner == NULL && bounce->Units[1].Owner != NULL && bounce->Units[2].Owner == NULL);
    assert(bounce->BounceMapping == 0 && mapping->AddressMapping == NULL && mapping->Access.Buffer == g_buffer);
    assert(__PoolFreePages() == freePages - 1 && fixture.Contexts[0]->TranslatedBounce.HeldBytes == 4096);
    assert(LookupHandle(result.Handle, HandleTypeSHMAccess) != NULL);

    // The retry returns only the unit that is still out.
    g_current = stop ? fixture.Service : fixture.Driver;
    assert((stop ? DMAManagerCompleteStop(fixture.Manager, &ticket) : ScResourceRelease(result.Handle)) == OS_EOK);
    assert(g_unitReleaseCalls == unitReleases + 4 && g_addressReleaseCalls == addressReleases + 1);
    assert(__PoolFreePages() == freePages && fixture.Contexts[0]->TranslatedBounce.HeldBytes == 0);
    g_current = fixture.Driver;
    if (stop) {
        assert(ScResourceRelease(result.Handle) == OS_EOK);
    }
    g_failUnitBase = 0;
    __ExpectNoBounceResources(&fixture, freePages);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

static SHMDeviceMapping_t* g_stopMapping;

// Called from the cache log. The cache work of a stop runs while every unit is
// still out of the pool.
static void
__ExpectUnitsHeld(void)
{
    for (size_t i = 0; i < g_stopMapping->Stream.Bounce.UnitCount; i++) {
        assert(g_stopMapping->Stream.Bounce.Units[i].Owner != NULL);
    }
}

// A confirmed stop of an active device write has no completed count. It
// copies nothing back, syncs the whole cached span before any unit is
// returned, and then frees the storage.
static void
__TestBounceStop(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    OSDmaStopTicket_t          ticket;
    uint8_t                    written[8192];
    SHMSG_t                    span;
    size_t                     freePages;
    uint8_t*                   ram;

    __RamStart();
    __RamUsePool(__RAM_POOL, __RAM_POOL_LENGTH, true);
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheNonCoherent);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, 3 * __RAM_PAGE);
    freePages = __PoolFreePages();
    assert(__MapBounce(&fixture, g_buffer->ID, 300, 5000, DMAFromDevice, 0, &result) == OS_EOK);
    g_stopMapping = __LookupMapping(result.Handle);
    assert(g_stopMapping->ExtentCount == 1);
    span = g_stopMapping->Extents[0];
    assert(__BeginTransfer(result.Handle, 0, 5000) == OS_EOK);
    memset(written, 0x5c, sizeof(written));
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 8192, DMAFromDevice, written) == OS_EOK);
    g_current = fixture.Service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    g_cacheCallCount = 0;
    g_cacheHook = __ExpectUnitsHeld;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    // The extents are freed with the storage, so compare with the copy.
    assert(g_cacheHook == NULL && g_cacheCallCount == 1 && span.Length == 8192);
    assert(g_cacheCalls[0].Operation == __CacheInvalidate);
    assert(g_cacheCalls[0].Address == span.Address && g_cacheCalls[0].Length == span.Length);
    g_cacheCallCount = 0;
    for (size_t i = 0; i < 3 * __RAM_PAGE; i++) {
        assert(ram[i] == __SourceByte(i));
    }
    assert(__PoolFreePages() == freePages && fixture.Contexts[0]->TranslatedBounce.HeldBytes == 0);
    g_current = fixture.Driver;
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    g_stopMapping = NULL;
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// While the IOMMU has not confirmed the removal, the IOVA and every unit stay
// reserved. The retry after completion frees them. The source handle can be
// dropped meanwhile, because the mapping keeps its own reference.
static void
__TestBounceDelayedRemoval(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    DMAIOMMUSimControl_t       control = { .HoldCompletions = true };
    SHMDeviceMapping_t*        mapping;
    size_t                     freePages;
    uint8_t*                   ram;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 2, 0, 2 * __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, 2 * __RAM_PAGE);
    freePages = __PoolFreePages();
    assert(__MapBounce(&fixture, g_buffer->ID, 10, 512, DMAFromDevice, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);

    // The client drops its handle during the transfer. The completed prefix
    // still lands in the right source bytes.
    assert(__BeginTransfer(result.Handle, 0, 512) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(g_buffer != NULL && ScResourceRelease(result.Handle) == OS_EBUSY);
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 4, DMAFromDevice, (uint8_t[]){ 1, 2, 3, 4 }) == OS_EOK);
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 4) == OS_EOK);
    assert(ram[10] == 1 && ram[13] == 4 && ram[14] == __SourceByte(14));

    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_ETIMEOUT);
    assert(mapping->AddressMapping != NULL && mapping->Stream.Bounce.Units[0].Owner != NULL);
    assert(__PoolFreePages() == freePages - 1 && g_buffer != NULL);
    control.HoldCompletions = false;
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(DMAIOMMUSimAdvance(fixture.Sim, 100) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __Drain();
    assert(g_buffer == NULL);
    __ExpectNoBounceResources(&fixture, freePages);
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Hooks for the failures after linking. g_bounceFixture is the fixture the
// hook stops. The stop ticket is kept for the cleanup pass that follows.
static struct __TranslatedFixture* g_bounceFixture;
static OSDmaStopTicket_t           g_bounceTicket;
static bool                        g_bounceStopPrepared;

static void
__StopBounceFixture(void)
{
    MSContextSecurity_t* caller = g_current;

    g_current = g_bounceFixture->Service;
    assert(DMAManagerPrepareStop(g_bounceFixture->Manager, g_bounceFixture->Attachments[0], &g_bounceTicket) == OS_EOK);
    g_bounceStopPrepared = true;
    g_current = caller;
}

// Install the mapping for real, then cancel the binding before the final
// check that runs ahead of publication.
static oserr_t
__InstallThenStop(struct DMAAddressMapping* addressMapping)
{
    oserr_t status = __real_DMAAddressMappingInstall(addressMapping);

    assert(status == OS_EOK);
    __StopBounceFixture();
    return OS_EOK;
}

enum __LinkedFailure {
    // The binding is cancelled right after the mapping is linked.
    __LinkedCancel,
    // The IOMMU installs the first granule, then fails on the second.
    __LinkedPartialInstall,
    // The final check after a complete installation fails.
    __LinkedFinalCheck,
    // Copying the result to the caller fails after a complete installation.
    __LinkedCopyOut
};

// A translated bounce mapping that fails after it was linked is abandoned to
// recovery. Nothing is published, and its units and both kernel views stay
// allocated while the device may reach them. The stop pass then removes the
// translation once and returns everything. Runs in a child: the binding is
// stopped and the fixture is not torn down.
static void
__TestBounceLinkedFailure(MSContextSecurity_t* service, MSContextSecurity_t* driver, int failure)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct OSDmaMappingResult  unchanged;
    DMAIOMMUSimControl_t       control = { .FailurePoint = DMAIOMMUSimFailureInstallEntry,
        .AfterSteps = 1, .FailureStatus = OS_EDEVFAULT };
    DMAIOMMUSimStats_t         stats;
    SHMDeviceMapping_t*        mapping;
    size_t                     freePages;
    unsigned int               addressReleases;
    uint8_t                    byte;
    oserr_t                    status;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    freePages = __PoolFreePages();
    addressReleases = g_addressReleaseCalls;
    g_bounceFixture = &fixture;
    g_bounceStopPrepared = false;
    memset(&unchanged, 0xa5, sizeof(unchanged));
    result = unchanged;
    if (failure == __LinkedCancel) {
        g_afterLinkHook = __StopBounceFixture;
    } else if (failure == __LinkedPartialInstall) {
        assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    } else if (failure == __LinkedFinalCheck) {
        g_installOverride = __InstallThenStop;
    } else {
        g_failCopy = 1;
    }
    status = __MapBounce(&fixture, g_buffer->ID, 300, 5000, DMABidirectional, 0, &result);
    g_installOverride = NULL;
    g_failCopy = -1;
    assert(status == (failure == __LinkedPartialInstall ? OS_EDEVFAULT :
        failure == __LinkedCopyOut ? OS_EINVALPARAMS : OS_ECANCELLED));
    if (failure != __LinkedCopyOut) {
        assert(memcmp(&result, &unchanged, sizeof(result)) == 0);
    }
    assert(failure != __LinkedCopyOut || LookupHandle(result.Handle, HandleTypeSHMAccess) == NULL);
    __Drain();

    // Abandoned to recovery, with all of its storage.
    mapping = fixture.Contexts[0]->Mappings;
    assert(mapping != NULL && mapping->Lifetime.Abandoned && mapping->AddressMapping != NULL);
    assert(mapping->Stream.Bounce.UnitCount == 2);
    assert(mapping->Stream.Bounce.Units[0].Owner != NULL && mapping->Stream.Bounce.Units[1].Owner != NULL);
    assert(__PoolFreePages() == freePages - 2 && __RamBounceViews() == 1 && g_liveViews == 2);
    assert(fixture.Contexts[0]->TranslatedBounce.HeldBytes == 8192 && g_addressReleaseCalls == addressReleases);
    if (failure != __LinkedCancel) {
        // The first granule is installed in every case that reached the IOMMU.
        assert(__DeviceAccess(&fixture, mapping->Stream.PayloadSegment.Address, 1, DMAToDevice, &byte) == OS_EOK);
    }

    g_current = service;
    if (!g_bounceStopPrepared) {
        assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &g_bounceTicket) == OS_EOK);
    }
    assert(DMAManagerCompleteStop(fixture.Manager, &g_bounceTicket) == OS_EOK);
    assert(fixture.Contexts[0]->Mappings == NULL && g_addressReleaseCalls == addressReleases + 1);
    assert(__PoolFreePages() == freePages && __RamBounceViews() == 0 && g_liveViews == 0);
    assert(fixture.Contexts[0]->TranslatedBounce.HeldBytes == 0);
    DMAIOMMUSimSnapshot(fixture.Sim, &stats);
    assert(stats.Mappings == 0);
}

struct __BounceWaiter {
    uuid_t      Mapping;
    atomic_bool Done;
    oserr_t     Status;
};

static void*
__BeginWhenAvailable(void* argument)
{
    struct __BounceWaiter*          waiter = argument;
    struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer, .Length = 512,
        .Mode = SHMWaitUntilAvailable, .Deadline = *OS_DEADLINE_INFINITE };

    waiter->Status = ScShmAccessBegin(NULL, waiter->Mapping, &begin);
    atomic_store(&waiter->Done, true);
    return NULL;
}

// A CPU session on the source range holds a transfer back. A nonblocking begin
// is refused and copies nothing. A blocking begin copies only after admission,
// so the device reads what the CPU wrote while it waited. A CPU writer just
// outside the range, in the same page, keeps its bytes across a device write.
static void
__TestBounceAdmission(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct __BounceWaiter      waiter = { 0 };
    struct SHMAccess           cpu;
    struct SHMAccess           neighbor;
    SHMDeviceMapping_t*        mapping;
    const uint8_t*             storage;
    uint8_t                    bytes[4096];
    pthread_t                  thread;
    bool                       waiting;
    uint8_t*                   ram;

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 1, 0, __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, __RAM_PAGE);
    assert(__MapBounce(&fixture, g_buffer->ID, 300, 512, DMABidirectional, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);
    storage = __RamPointer(mapping->Stream.Bounce.Units[0].PhysicalBase, 512);

    SHMAccessInitialize(&cpu, g_buffer, 300, 512);
    assert(SHMAccessBegin(&cpu, 0, 512, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    assert(__BeginTransfer(result.Handle, 0, 512) == OS_EBUSY);
    for (size_t i = 0; i < 512; i++) {
        assert(storage[i] == 0);
    }

    waiter.Mapping = result.Handle;
    assert(pthread_create(&thread, NULL, __BeginWhenAvailable, &waiter) == 0);
    do {
        MutexLock(&g_buffer->Mutex);
        waiting = g_buffer->Access.Waiting == 1;
        MutexUnlock(&g_buffer->Mutex);
        sched_yield();
    } while (!waiting);
    assert(!atomic_load(&waiter.Done));
    memset(ram + 300, 0x6d, 512);
    SHMAccessEnd(&cpu);
    assert(pthread_join(thread, NULL) == 0 && waiter.Status == OS_EOK);
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 512, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < 512; i++) {
        assert(bytes[i] == 0x6d);
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 0) == OS_EOK);

    // The mapping covers bytes 300 to 811, cache lines 256 to 831. The CPU
    // writes bytes 900 to 963 of the same page while the device writes the
    // whole granule.
    SHMAccessInitialize(&neighbor, g_buffer, 900, 64);
    assert(SHMAccessBegin(&neighbor, 0, 64, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    memset(ram + 900, 0x2e, 64);
    assert(__BeginTransfer(result.Handle, 0, 512) == OS_EOK);
    memset(bytes, 0x91, sizeof(bytes));
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 4096, DMAFromDevice, bytes) == OS_EOK);
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 512) == OS_EOK);
    SHMAccessEnd(&neighbor);
    for (size_t i = 0; i < __RAM_PAGE; i++) {
        uint8_t expected = i >= 300 && i < 812 ? 0x91 : i >= 900 && i < 964 ? 0x2e : __SourceByte(i);

        assert(ram[i] == expected);
    }
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// Every mapped granule of one mapping translates to one of its own units.
static void
__ExpectOwnTranslations(struct __TranslatedFixture* fixture, SHMDeviceMapping_t* mapping, uint64_t address)
{
    const SHMDeviceBounce_t* bounce = &mapping->Stream.Bounce;
    SHMSG_t                  span;
    uint32_t                 count;

    for (size_t g = 0; g < bounce->MappedLength / __RAM_PAGE; g++) {
        assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, address + g * __RAM_PAGE, __RAM_PAGE,
            DMABidirectional, &span, 1, &count) == OS_EOK);
        assert(count == 1 && span.Address == bounce->Units[g].PhysicalBase);
    }
    assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, address - 1, 1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, 0, address + bounce->MappedLength, 1,
        DMAToDevice, &span, 1, &count) == OS_ENOENT);
}

// Two translated bounce mappings in one domain at once. A 64 KiB IOVA
// alignment leaves unmapped space around each span. Releasing one leaves the
// other intact and usable.
static void
__TestBounceSharedDomain(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  first;
    struct OSDmaMappingResult  second;
    struct OSDmaMapParameters  parameters = { .Offset = 300, .Length = 5000, .Direction = DMABidirectional,
        .Requirements = { .AddressAlignment = 0x10000 } };
    SHMDeviceMapping_t*        mappings[2];
    uint8_t                    bytes[5000];

    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 4, 0, 4 * __RAM_PAGE);
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &first) == OS_EOK);
    parameters.Offset = 8000;
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &second) == OS_EOK);
    mappings[0] = __LookupMapping(first.Handle);
    mappings[1] = __LookupMapping(second.Handle);
    for (size_t i = 0; i < 2; i++) {
        for (size_t u = 0; u < 2; u++) {
            assert(mappings[1 - i]->Stream.Bounce.Units[0].PhysicalBase != mappings[i]->Stream.Bounce.Units[u].PhysicalBase);
            assert(mappings[1 - i]->Stream.Bounce.Units[1].PhysicalBase != mappings[i]->Stream.Bounce.Units[u].PhysicalBase);
        }
    }
    __ExpectOwnTranslations(&fixture, mappings[0], first.InlineSegment.Address);
    __ExpectOwnTranslations(&fixture, mappings[1], second.InlineSegment.Address);

    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, first.InlineSegment.Address, 1, DMAToDevice,
        &(SHMSG_t){ 0 }, 1, &(uint32_t){ 0 }) == OS_ENOENT);
    __ExpectOwnTranslations(&fixture, mappings[1], second.InlineSegment.Address);
    assert(__BeginTransfer(second.Handle, 0, 5000) == OS_EOK);
    assert(__DeviceAccess(&fixture, second.InlineSegment.Address, 5000, DMAToDevice, bytes) == OS_EOK);
    for (size_t i = 0; i < 5000; i++) {
        assert(bytes[i] == __SourceByte(8000 + i));
    }
    assert(ScShmAccessEnd(second.Handle, DMAAccessTransfer, 0) == OS_EOK);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

// A terminal IOMMU unit failure during a transfer. Admission closes, nothing
// is copied back, and the mapping keeps every unit and view, because the
// device may still reach them through the failed unit. Runs in a child: the
// storage stays allocated for good.
static void
__TestBounceUnitFailure(MSContextSecurity_t* service, MSContextSecurity_t* driver, int unused)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    struct OSDmaMappingResult  other;
    OSDmaStopTicket_t          ticket;
    SHMDeviceMapping_t*        mapping;
    uint8_t                    written[8192];
    size_t                     freePages;
    uint8_t*                   ram;

    (void)unused;
    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    g_faultUnit = DMAIOMMUSimUnit(fixture.Sim);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, 3 * __RAM_PAGE);
    assert(__MapBounce(&fixture, g_buffer->ID, 300, 5000, DMABidirectional, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);
    freePages = __PoolFreePages();
    assert(__BeginTransfer(result.Handle, 0, 5000) == OS_EOK);
    memset(written, 0x3c, sizeof(written));
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 8192, DMAFromDevice, written) == OS_EOK);
    __LatchFailure();

    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 5000) == OS_ECANCELLED);
    assert(__MapBounce(&fixture, g_buffer->ID, 0, 512, DMAToDevice, 0, &other) == OS_ECANCELLED);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    assert(mapping->AddressMapping != NULL && mapping->Stream.Bounce.BounceMapping != 0);
    assert(mapping->Stream.Bounce.Units[0].Owner != NULL && mapping->Stream.Bounce.Units[1].Owner != NULL);
    assert(__PoolFreePages() == freePages && __RamBounceViews() == 1);
    for (size_t i = 0; i < 3 * __RAM_PAGE; i++) {
        assert(ram[i] == __SourceByte(i));
    }
}

// The mapping handle and the source handle are destroyed while a transfer is
// active, as on process exit. The mapping's own source reference and kernel
// source view keep working until the stop pass, which then releases
// everything once and copies nothing back. Runs in a child: the binding is
// stopped and the fixture is not torn down.
static void
__TestBounceHandleExit(MSContextSecurity_t* service, MSContextSecurity_t* driver, int unused)
{
    struct __TranslatedFixture fixture;
    struct OSDmaMappingResult  result;
    OSDmaStopTicket_t          ticket;
    SHMDeviceMapping_t*        mapping;
    uint8_t                    written[8192];
    size_t                     freePages;
    unsigned int               addressReleases;
    uint8_t*                   ram;

    (void)unused;
    __RamStart();
    fixture = __PrepareBounceFixture(service, driver, 4096, UINT64_MAX, SHMDeviceCacheCoherent);
    __CreateRamBuffer(__RAM_BASE, 3, 0, 3 * __RAM_PAGE);
    ram = __RamPointer(__RAM_BASE, 3 * __RAM_PAGE);
    freePages = __PoolFreePages();
    addressReleases = g_addressReleaseCalls;
    assert(__MapBounce(&fixture, g_buffer->ID, 300, 5000, DMABidirectional, 0, &result) == OS_EOK);
    mapping = __LookupMapping(result.Handle);
    assert(__BeginTransfer(result.Handle, 0, 5000) == OS_EOK);
    assert(DestroyHandle(result.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(g_buffer != NULL && fixture.Contexts[0]->Mappings == mapping && mapping->Lifetime.Abandoned);
    assert(mapping->Stream.Bounce.SourceMapping != 0 && g_liveViews == 2);
    assert(__PoolFreePages() == freePages - 2);

    // The device keeps writing, and the CPU source view still reaches the pages.
    memset(written, 0x4b, sizeof(written));
    assert(__DeviceAccess(&fixture, result.InlineSegment.Address, 8192, DMAFromDevice, written) == OS_EOK);
    assert(((const uint8_t*)mapping->Stream.Bounce.SourceMapping)[mapping->Stream.Bounce.SourceOffset] == __SourceByte(300));

    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(fixture.Contexts[0]->Mappings == NULL && g_addressReleaseCalls == addressReleases + 1);
    assert(__PoolFreePages() == freePages && g_liveViews == 0);
    assert(fixture.Contexts[0]->TranslatedBounce.HeldBytes == 0);
    for (size_t i = 0; i < 3 * __RAM_PAGE; i++) {
        assert(ram[i] == __SourceByte(i));
    }
    __Drain();
    assert(g_buffer == NULL);
}

static void
__TestTranslatedBounce(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    // The 64 KiB granule runs first: its sixteen bounce pages behind one
    // source page are where a short page list is caught.
    __TestBounceLayouts(service, driver, 65536);
    __TestBounceLayouts(service, driver, 4096);
    __TestBounceGrantOffsets(service, driver);
    __TestBounceUnits(service, driver);
    __TestBounceSelection(service, driver);
    __TestBounceSizeLimit(service, driver);
    __TestBounceAuthorization(service, driver);
    __TestBouncePlacement(service, driver);
    __TestBounceTransfers(service, driver);
    __TestBounceCounters(service, driver);
    __TestBounceSetupFailures(service, driver);
    __TestBounceInstalledRelease(service, driver, false);
    __TestBounceInstalledRelease(service, driver, true);
    __TestBounceStop(service, driver);
    __TestBounceDelayedRemoval(service, driver);
    __RunBoundChild(__TestBounceQuarantineTranslated, service, driver, 0);
    __RunBoundChild(__TestBounceQuarantineTranslated, service, driver, 1);
    for (int failure = __LinkedCancel; failure <= __LinkedCopyOut; failure++) {
        __RunBoundChild(__TestBounceLinkedFailure, service, driver, failure);
    }
    __TestBounceAdmission(service, driver);
    __TestBounceSharedDomain(service, driver);
    __RunBoundChild(__TestBounceUnitFailure, service, driver, 0);
    __RunBoundChild(__TestBounceHandleExit, service, driver, 0);
    puts("Translated bounce: layouts, grant offsets, units, selection, size limit, grants, placement, transfers, counters, failures, recovery and admission passed");
}
