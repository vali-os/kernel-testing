// Exercise direct address lifetime independently of SHM storage and cache state.
#include <dma/address.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int g_liveAllocations;
static bool         g_failAllocation;

void*
kmalloc(
    _In_ size_t size)
{
    void* allocation;

    if (g_failAllocation) {
        return NULL;
    }
    allocation = malloc(size);
    assert(allocation != NULL);
    g_liveAllocations++;
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    assert(allocation != NULL && g_liveAllocations != 0);
    g_liveAllocations--;
    free(allocation);
}

static void
__TestIndependentMappings(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x8000, 0x1000, 0x1000 },
        { 0x10000, 0x2000, 0x1000 },
        { 0x8000, 0x100000000ULL, 0x1000 }
    };
    SHMSG_t extents[] = {
        { .Address = 0x8200, .Length = 0xe00 },
        { .Address = 0x10000, .Length = 0x200 }
    };
    struct DMAAddressSpace* first = NULL;
    struct DMAAddressSpace* second = NULL;
    struct DMAAddressMapping* firstMap = NULL;
    struct DMAAddressMapping* secondMap = NULL;
    const struct DMASegment* firstSegments;
    const struct DMASegment* secondSegments;
    uint32_t count;

    assert(DMAAddressSpaceCreateDirect(ranges, 3, &first) == OS_EOK);
    ranges[0].DeviceBase = 0x5000;
    ranges[1].DeviceBase = 0x6000;
    assert(DMAAddressSpaceCreateDirect(ranges, 3, &second) == OS_EOK);
    memset(ranges, 0, sizeof(ranges));

    // Disjoint physical pages join when the device sees consecutive addresses.
    // The low aliases fit the caller's 32-bit limit; the high alias does not.
    assert(DMAAddressMappingCreate(first, extents, 2, UINT32_MAX, NULL, &firstMap) == OS_EOK);
    assert(DMAAddressMappingCreate(second, extents, 2, UINT32_MAX, NULL, &secondMap) == OS_EOK);
    firstSegments = DMAAddressMappingSegments(firstMap, &count);
    assert(count == 1 && firstSegments[0].Address == 0x1200 && firstSegments[0].Length == 0x1000);
    secondSegments = DMAAddressMappingSegments(secondMap, &count);
    assert(count == 1 && secondSegments[0].Address == 0x5200 && secondSegments[0].Length == 0x1000);

    // Mapping results no longer depend on caller scratch or a creating context.
    memset(extents, 0, sizeof(extents));
    DMAAddressSpaceRelease(&first);
    DMAAddressSpaceRelease(&second);
    assert(first == NULL && second == NULL);
    assert(DMAAddressMappingSegments(firstMap, &count) == firstSegments);
    assert(firstSegments[0].Address == 0x1200 && secondSegments[0].Address == 0x5200);
    assert(DMAAddressMappingRelease(&firstMap) == OS_EOK && firstMap == NULL);
    assert(secondSegments[0].Length == 0x1000);
    assert(DMAAddressMappingRelease(&secondMap) == OS_EOK && secondMap == NULL);
    assert(DMAAddressMappingRelease(&secondMap) == OS_EOK);
    assert(g_liveAllocations == 0);
}

static void
__TestFailuresAndLimits(void)
{
    struct SHMDeviceRange range = { 0x8000, 0, 0x1000 };
    SHMSG_t extent = { .Address = 0x8000, .Length = 0x1000 };
    struct DMAMapRequirements requirements = { .AddressAlignment = 3 };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressMapping* mapping = NULL;
    struct DMAAddressMapping* saved;
    const struct DMASegment* segments;
    uint32_t count;

    g_failAllocation = true;
    assert(DMAAddressSpaceCreateDirect(&range, 1, &space) == OS_EOOM);
    assert(space == NULL && g_liveAllocations == 0);
    g_failAllocation = false;
    assert(DMAAddressSpaceCreateDirect(&range, 1, &space) == OS_EOK);

    assert(DMAAddressMappingCreate(space, &extent, 1, 0xffe, NULL, &mapping) == OS_ENOENT);
    assert(DMAAddressMappingCreate(space, &extent, 1, 0xfff, &requirements, &mapping) == OS_EINVALPARAMS);
    requirements.AddressAlignment = 0;
    requirements.LengthAlignment = 0x2000;
    assert(DMAAddressMappingCreate(space, &extent, 1, 0xfff, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(DMAAddressMappingCreate(space, &extent, 0, UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    extent.Length = 0;
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    extent.Length = 0x1000;
    assert(mapping == NULL && g_liveAllocations == 1);

    g_failAllocation = true;
    assert(DMAAddressMappingCreate(space, &extent, 1, 0xfff, NULL, &mapping) == OS_EOOM);
    assert(mapping == NULL && g_liveAllocations == 1);
    g_failAllocation = false;

    // Address zero and the inclusive final address are both valid.
    assert(DMAAddressMappingCreate(space, &extent, 1, 0xfff, NULL, &mapping) == OS_EOK);
    segments = DMAAddressMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0 && segments[0].Length == 0x1000);
    saved = mapping;
    assert(DMAAddressMappingCreate(space, &extent, 1, 0xfff, NULL, &mapping) == OS_EBUSY);
    assert(mapping == saved);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    DMAAddressSpaceRelease(&space);
    assert(g_liveAllocations == 0);
}

int
main(void)
{
    __TestIndependentMappings();
    __TestFailuresAndLimits();
    puts("DMA addresses: copied rules, independent mappings, limits and failure cleanup passed");
    return 0;
}
