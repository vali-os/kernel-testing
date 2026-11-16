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

// Address conversion has no SHM permissions or device lifecycle. Exercise the
// direct backend here. Bound SHM and recovery behavior live in their own suites.
#include "../kernel/memory/dma/private.h"
#include <assert.h>
#include <limits.h>
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
__ExpectSegments(
    _In_ struct DMAAddressSpace*   space,
    _In_ const SHMSG_t*            extents,
    _In_ int                       count,
    _In_ uint64_t                  limit,
    _In_ const struct DMASegment*  expected,
    _In_ uint32_t                  expectedCount)
{
    struct DMAAddressMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t segmentCount;

    assert(DMAAddressMappingCreate(space, extents, count, limit, NULL, &mapping) == OS_EOK);
    segments = DMAAddressMappingSegments(mapping, &segmentCount);
    assert(segmentCount == expectedCount);
    assert(memcmp(segments, expected, segmentCount * sizeof(*segments)) == 0);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
}

static void
__TestInvalidRanges(void)
{
    const struct SHMDeviceRange invalid[][2] = {
        { { 0, 0, 0 }, { 0, 10, 1 } },
        { { UINT64_MAX, 0, 2 }, { 0, 10, 1 } },
        { { 0, UINT64_MAX, 2 }, { 0, 10, 1 } },
        { { 0, 10, 10 }, { 100, 19, 10 } },
        { { 100, 19, 10 }, { 0, 10, 10 } },
        { { 0, 10, 10 }, { 100, 10, 10 } },
        { { 0, 10, 10 }, { 100, 12, 1 } },
        { { 0, 12, 1 }, { 100, 10, 10 } }
    };
    struct SHMDeviceRange valid = { 0, 0, 1 };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressSpace* saved;
    size_t i;

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(DMAAddressSpaceCreateDirect(invalid[i], 2, &space) == OS_EINVALPARAMS);
        assert(space == NULL);
    }
    assert(DMAAddressSpaceCreateDirect(NULL, 1, &space) == OS_EINVALPARAMS);
    assert(DMAAddressSpaceCreateDirect(&valid, 0, &space) == OS_EINVALPARAMS);
    assert(DMAAddressSpaceCreateDirect(&valid, 1, NULL) == OS_EINVALPARAMS);
    // Count validation must precede reading this single accessible entry.
    assert(DMAAddressSpaceCreateDirect(&valid, DMA_MAX_RANGES + 1, &space) == OS_ENOTSUPPORTED);
    assert(DMAAddressSpaceCreateDirect(&valid, UINT32_MAX, &space) == OS_ENOTSUPPORTED);
    assert(space == NULL && g_liveAllocations == 0);
    g_failAllocation = true;
    assert(DMAAddressSpaceCreateDirect(&valid, 1, &space) == OS_EOOM);
    assert(space == NULL && g_liveAllocations == 0);
    g_failAllocation = false;
    assert(DMAAddressSpaceCreateDirect(&valid, 1, &space) == OS_EOK);
    saved = space;
    assert(DMAAddressSpaceCreateDirect(&valid, 1, &space) == OS_EBUSY);
    assert(space == saved);
    DMAAddressSpaceRelease(&space);
}

static void
__TestMaximumMap(void)
{
    struct SHMDeviceRange ranges[DMA_MAX_RANGES];
    struct DMAAddressSpace* space = NULL;
    unsigned int i;

    // The maximum unsorted description is copied. Overwriting the source
    // cannot change either end of the saved map.
    for (i = 0; i < DMA_MAX_RANGES; i++) {
        ranges[i] = (struct SHMDeviceRange) {
            i * 0x1000, (DMA_MAX_RANGES - i) * 0x1000, 0x1000
        };
    }
    assert(DMAAddressSpaceCreateDirect(ranges, DMA_MAX_RANGES, &space) == OS_EOK);
    memset(ranges, 0, sizeof(ranges));
    __ExpectSegments(space, (SHMSG_t[]) { { 0, 0x1000 } }, 1, UINT64_MAX,
        (struct DMASegment[]) { { DMA_MAX_RANGES * 0x1000, 0x1000 } }, 1);
    __ExpectSegments(space, (SHMSG_t[]) { { (DMA_MAX_RANGES - 1) * 0x1000, 0x1000 } }, 1,
        UINT64_MAX, (struct DMASegment[]) { { 0x1000, 0x1000 } }, 1);
    DMAAddressSpaceRelease(&space);
}

static void
__TestSegmentsAndAliases(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x1000, 0x2000 },
        { 0x702000, 0x3800, 0x1000 },
        { 0x703000, 0x4800, 0x1000 },
        { 0x710000, 0x100000000ULL, 0x2000 },
        { 0x710000, 0x9000, 0x1000 }
    };
    struct SHMDeviceRange reversed[5];
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressSpace* other = NULL;
    unsigned int i;

    for (i = 0; i < 5; i++) {
        reversed[i] = ranges[4 - i];
    }
    assert(DMAAddressSpaceCreateDirect(ranges, 5, &space) == OS_EOK);
    assert(DMAAddressSpaceCreateDirect(reversed, 5, &other) == OS_EOK);
    // Touching physical ranges split at a device-address gap, then join where
    // device addresses continue. Input order must survive conversion.
    __ExpectSegments(space, (SHMSG_t[]) { { 0x701800, 0x2000 } }, 1, UINT64_MAX,
        (struct DMASegment[]) { { 0x2800, 0x800 }, { 0x3800, 0x1800 } }, 2);
    __ExpectSegments(space, (SHMSG_t[]) { { 0x702000, 0x1000 }, { 0x703000, 0x100 } }, 2,
        UINT64_MAX, (struct DMASegment[]) { { 0x3800, 0x1100 } }, 1);
    __ExpectSegments(space, (SHMSG_t[]) { { 0x701000, 0x1000 }, { 0x700000, 0x1000 } }, 2,
        UINT64_MAX, (struct DMASegment[]) { { 0x2000, 0x1000 }, { 0x1000, 0x1000 } }, 2);
    // The longest usable alias wins regardless of declaration order. A 32-bit
    // device must use the low alias, which reaches only the first page.
    __ExpectSegments(space, (SHMSG_t[]) { { 0x710000, 0x2000 } }, 1, UINT64_MAX,
        (struct DMASegment[]) { { 0x100000000ULL, 0x2000 } }, 1);
    __ExpectSegments(other, (SHMSG_t[]) { { 0x710000, 0x2000 } }, 1, UINT64_MAX,
        (struct DMASegment[]) { { 0x100000000ULL, 0x2000 } }, 1);
    __ExpectSegments(space, (SHMSG_t[]) { { 0x710000, 0x1000 } }, 1, UINT32_MAX,
        (struct DMASegment[]) { { 0x9000, 0x1000 } }, 1);
    __ExpectSegments(other, (SHMSG_t[]) { { 0x710000, 0x1000 } }, 1, UINT32_MAX,
        (struct DMASegment[]) { { 0x9000, 0x1000 } }, 1);
    DMAAddressSpaceRelease(&space);
    DMAAddressSpaceRelease(&other);
}

static void
__TestFailuresAndLifetime(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x2000 };
    SHMSG_t extent = { 0x700300, 0x100 };
    SHMSG_t hole[] = { { 0x700000, 0x100 }, { 0x702000, 1 } };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressMapping* mapping = NULL;
    struct DMAAddressMapping* saved;
    const struct DMASegment* segments;
    uint32_t count;

    assert(DMAAddressSpaceCreateDirect(&range, 1, &space) == OS_EOK);
    range.DeviceBase = 0;
    // Source changes cannot alter the stored rules. The limit is inclusive.
    __ExpectSegments(space, &extent, 1, 0x13ff, (struct DMASegment[]) { { 0x1300, 0x100 } }, 1);
    assert(DMAAddressMappingCreate(space, &extent, 1, 0x13fe, NULL, &mapping) == OS_ENOENT);
    assert(DMAAddressMappingCreate(space, hole, 2, UINT64_MAX, NULL, &mapping) == OS_ENOENT);
    assert(DMAAddressMappingCreate(NULL, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    assert(DMAAddressMappingCreate(space, NULL, 1, UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    assert(DMAAddressMappingCreate(space, &extent, 0, UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, NULL) == OS_EINVALPARAMS);
    assert(DMAAddressMappingCreate(space, (SHMSG_t[]) { { 0x700000, 0 } }, 1,
        UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    assert(DMAAddressMappingCreate(space, (SHMSG_t[]) { { UINTPTR_MAX, 2 } }, 1,
        UINT64_MAX, NULL, &mapping) == OS_EINVALPARAMS);
    assert(mapping == NULL && g_liveAllocations == 1);
    g_failAllocation = true;
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EOOM);
    g_failAllocation = false;
    atomic_store(&space->References, UINT_MAX);
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EOVERFLOW);
    assert(mapping == NULL && g_liveAllocations == 1);
    atomic_store(&space->References, 1);
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EOK);
    saved = mapping;
    assert(DMAAddressMappingCreate(space, &extent, 1, UINT64_MAX, NULL, &mapping) == OS_EBUSY);
    assert(mapping == saved);
    DMAAddressSpaceRelease(&space);
    assert(space == NULL && g_liveAllocations == 2);
    segments = DMAAddressMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Address == 0x1300 && segments[0].Length == 0x100);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(DMAAddressMappingRelease(NULL) == OS_EOK);
    DMAAddressSpaceRelease(&space);
    DMAAddressSpaceRelease(NULL);
}

static void
__TestAddressEdges(void)
{
    struct SHMDeviceRange ranges[] = {
        { UINTPTR_MAX - 0xfff, 0, 0x1000 }, { 0, 0x1000, 0x1000 }
    };
    struct SHMDeviceRange top = { UINT64_MAX - 15, UINT64_MAX - 15, 16 };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressMapping* mapping = NULL;

    // A final physical page can join a separate extent at zero when their
    // device addresses continue, without overflowing a physical endpoint.
    assert(DMAAddressSpaceCreateDirect(ranges, 2, &space) == OS_EOK);
    __ExpectSegments(space, (SHMSG_t[]) { { UINTPTR_MAX - 0xfff, 0x1000 }, { 0, 0x1000 } }, 2,
        UINT64_MAX, (struct DMASegment[]) { { 0, 0x2000 } }, 1);
    __ExpectSegments(space, (SHMSG_t[]) { { UINTPTR_MAX - 0xfff, 1 } }, 1, 0,
        (struct DMASegment[]) { { 0, 1 } }, 1);
    DMAAddressSpaceRelease(&space);
    assert(DMAAddressSpaceCreateDirect(&top, 1, &space) == OS_EOK);
    __ExpectSegments(space, (SHMSG_t[]) { { UINTPTR_MAX - 15, 16 } }, 1, UINT64_MAX,
        (struct DMASegment[]) { { UINT64_MAX - 15, 16 } }, 1);
    assert(DMAAddressMappingCreate(space, (SHMSG_t[]) { { UINTPTR_MAX, 1 } }, 1,
        UINT64_MAX - 1, NULL, &mapping) == OS_ENOENT);
    assert(mapping == NULL);
    DMAAddressSpaceRelease(&space);
}

static void
__TestRangeValidation(void)
{
    struct SHMDeviceRange ranges[2] = {
        { UINT64_MAX, UINT64_MAX, 1 },
        { UINT64_MAX, 0, 1 }
    };

    // The last possible byte is valid, including aliases at disjoint device addresses.
    g_failAllocation = true;
    assert(DMAAddressRangesValidate(ranges, 2) == OS_EOK);
    assert(DMAAddressRangesValidate(NULL, 1) == OS_EINVALPARAMS);
    assert(DMAAddressRangesValidate(ranges, 0) == OS_EINVALPARAMS);
    assert(DMAAddressRangesValidate(ranges, DMA_MAX_RANGES + 1) == OS_ENOTSUPPORTED);
    ranges[1].DeviceBase = UINT64_MAX;
    assert(DMAAddressRangesValidate(ranges, 2) == OS_EINVALPARAMS);
    ranges[0].Length = 2;
    assert(DMAAddressRangesValidate(ranges, 1) == OS_EINVALPARAMS);
    ranges[0] = (struct SHMDeviceRange) { 0, UINT64_MAX, 2 };
    assert(DMAAddressRangesValidate(ranges, 1) == OS_EINVALPARAMS);
    ranges[0].Length = 0;
    assert(DMAAddressRangesValidate(ranges, 1) == OS_EINVALPARAMS);
    assert(g_liveAllocations == 0);
    g_failAllocation = false;
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
    __TestRangeValidation();
    __TestIndependentMappings();
    __TestFailuresAndLimits();
    __TestInvalidRanges();
    __TestMaximumMap();
    __TestSegmentsAndAliases();
    __TestFailuresAndLifetime();
    __TestAddressEdges();
    assert(g_liveAllocations == 0);
    puts("DMA address conversion: ranges, aliases, segments, limits and rollback passed");
    return 0;
}
