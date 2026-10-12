/** Exercise boot selection of reserved memory independently of allocation. */
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

    // Bad sizes and alignments are mistakes, not missing room.
    boot.Memory.NumberOfEntries = 4;
    boot.Memory.Entries = (unsigned long long)(uintptr_t)map;
    assert(SystemMemoryFitDeviceRegion(&boot, 0, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x200000, 0x7fffffff, 0x300000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x300000, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(NULL, 0x200000, 0x7fffffff, 0x200000, &base, &length) == OS_EINVALPARAMS);
    assert(SystemMemoryFitDeviceRegion(&boot, 0x200000, 0x7fffffff, 0x200000, NULL, &length) == OS_EINVALPARAMS);
}

int
main(void)
{
    __TestChooseRange();
    puts("Reserved memory: loader and fallback placement passed");
    return 0;
}
