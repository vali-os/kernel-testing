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
 *
 * Exercise boot selection of reserved memory independently of allocation.
 */
#include <component/memory.h>
#include <vboot/vboot.h>
#include <assert.h>
#include <stdio.h>

static void
__ExpectRange(
    _In_ const struct VBootMemoryEntry* entries,
    _In_ unsigned int                   count,
    _In_ paddr_t                        limit,
    _In_ oserr_t                        expectedStatus,
    _In_ paddr_t                        expectedBase,
    _In_ size_t                         expectedLength)
{
    struct VBoot boot = { 0 };
    paddr_t      base = 0xfeed;
    size_t       length = 0xfeed;

    boot.Memory.NumberOfEntries = count;
    boot.Memory.EntrySize = sizeof(struct VBootMemoryEntry);
    boot.Memory.Entries = (unsigned long long)(uintptr_t)entries;

    // 2 MiB pool, 2 MiB alignment, as ARM64 uses. Nothing is written on failure.
    assert(SystemMemoryFitDeviceRegion(&boot, 0x200000, limit, 0x200000, &base, &length) == expectedStatus);
    assert(base == (expectedStatus == OS_EOK ? expectedBase : 0xfeed));
    assert(length == (expectedStatus == OS_EOK ? expectedLength : 0xfeed));
}

static void
__TestChooseRange(void)
{
    struct VBootMemoryEntry map[] = {
        { VBootMemoryType_Available, 0x0,        0, 0x1000000,  0 },
        { VBootMemoryType_Reserved,  0x1000000,  0, 0x1000000,  0 },
        { VBootMemoryType_Available, 0x2000000,  0, 0x3100000,  0 },
        { VBootMemoryType_Available, 0x80000000, 0, 0x40000000, 0 }
    };
    struct VBootMemoryEntry marked[] = {
        { VBootMemoryType_Available,  0x0,      0, 0x1000000, 0 },
        { VBootMemoryType_DevicePool, 0x400000, 0, 0x600000,  0 }
    };
    struct VBoot boot = { 0 };
    paddr_t      base;
    size_t       length;

    // The highest available RAM below the limit wins, aligned down to 2 MiB:
    // 0x2000000 + 0x3100000 ends at 0x5100000, so the pool sits at 0x4e00000.
    // RAM above the limit and reserved ranges are never used.
    __ExpectRange(map, 4, 0x7fffffff, OS_EOK, 0x4e00000, 0x200000);

    // A lower limit cuts a range short; the pool must still end at or below it.
    __ExpectRange(map, 4, 0x2ffffff, OS_EOK, 0x2e00000, 0x200000);
    __ExpectRange(map, 4, 0x1ffffff, OS_EOK, 0xe00000, 0x200000);

    // No range fits below the limit, or nothing is available at all.
    __ExpectRange(map, 4, 0x1fffff - 1, OS_ENOENT, 0, 0);
    __ExpectRange(map + 1, 1, 0x7fffffff, OS_ENOENT, 0, 0);

    // A loader-marked pool wins over the default and keeps its own size, but
    // only when it is aligned well enough to be mapped as given.
    __ExpectRange(marked, 2, 0x7fffffff, OS_EOK, 0x400000, 0x600000);
    marked[1].PhysicalBase = 0x401000;
    __ExpectRange(marked, 2, 0x7fffffff, OS_EOK, 0xe00000, 0x200000);

    // A marked pool cannot bypass the limit, including when only its last
    // byte exceeds it. Invalid marked ranges leave the available fallback.
    marked[1].PhysicalBase = 0x400000;
    __ExpectRange(marked, 2, 0x9fffff, OS_EOK, 0x400000, 0x600000);
    __ExpectRange(marked, 2, 0x9ffffe, OS_EOK, 0x600000, 0x200000);
    marked[1].PhysicalBase = 0x80000000;
    __ExpectRange(marked, 2, 0x7fffffff, OS_EOK, 0xe00000, 0x200000);
    __ExpectRange(marked + 1, 1, 0x7fffffff, OS_ENOENT, 0, 0);

    // An aligned range that wraps must not become a small valid pool.
    marked[1].PhysicalBase = UINT64_MAX - 0x1fffff;
    marked[1].Length = 0x400000;
    __ExpectRange(marked, 2, UINT64_MAX, OS_EOK, 0xe00000, 0x200000);
    marked[1].Length = 0;
    __ExpectRange(marked, 2, UINT64_MAX, OS_EOK, 0xe00000, 0x200000);

    // Bad sizes and alignments are mistakes, not missing room.
    boot.Memory.NumberOfEntries = 4;
    boot.Memory.Entries = (unsigned long long)(uintptr_t)map;
    assert(SystemMemoryFitDeviceRegion(&boot, 0, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x200000, 0x7fffffff, 0x300000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x300000, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(NULL, 0x200000, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x200000, 0x7fffffff, 0x200000, NULL, &length) == OS_EINVALPARAMS);
}

static void
__TestReachableIntervals(void)
{
    struct VBootMemoryEntry map[] = {
        { VBootMemoryType_Available, 0, 0, 0x100000000ULL, 0 },
        { VBootMemoryType_DevicePool, 0x200000, 0, 0x400000, 0 }
    };
    SystemMemoryPhysicalRange_t ranges[] = {
        { 0x20000000, 0x203fffff },
        { 0x50000000, 0x503fffff }
    };
    struct VBoot boot = { 0 };
    paddr_t base = 0xfeed;
    size_t length = 0xfeed;

    boot.Memory.NumberOfEntries = 2;
    boot.Memory.Entries = (uint64_t)(uintptr_t)map;
    assert(SystemMemoryFitDeviceRegionInRanges(&boot, 0x400000, 0x200000,
        ranges, 2, &base, &length) == OS_EOK);
    assert(base == 0x50000000 && length == 0x400000);

    // Adjacent intervals cannot be combined to hold a single pool.
    ranges[0] = (SystemMemoryPhysicalRange_t) { 0x200000, 0x3fffff };
    ranges[1] = (SystemMemoryPhysicalRange_t) { 0x400000, 0x5fffff };
    base = length = 0xfeed;
    assert(SystemMemoryFitDeviceRegionInRanges(&boot, 0x400000, 0x200000,
        ranges, 2, &base, &length) == OS_ENOENT);
    assert(base == 0xfeed && length == 0xfeed);
    assert(SystemMemoryFitDeviceRegionInRanges(&boot, 0x400000, 0x200000,
        NULL, 0, &base, &length) == OS_ENOENT);
    assert(SystemMemoryFitDeviceRegionInRanges(&boot, 0x400000, 0x200000,
        NULL, 1, &base, &length) == OS_EINVALPARAMS);
    ranges[0].Base = ranges[0].Limit + 1;
    assert(SystemMemoryFitDeviceRegionInRanges(&boot, 0x400000, 0x200000,
        ranges, 2, &base, &length) == OS_EINVALPARAMS);
    assert(base == 0xfeed && length == 0xfeed);
}

int
main(void)
{
    __TestChooseRange();
    __TestReachableIntervals();
    puts("Reserved memory: loader and fallback placement passed");
    return 0;
}
