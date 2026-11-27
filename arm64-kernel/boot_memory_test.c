// Execute the production map validator without enabling host page tables.
#include "boot-memory.h"
#include <stdlib.h>
#define CHECK(value) do { if (!(value)) { printf("failed at %d: %s\n", __LINE__, #value); abort(); } } while (0)
#include <stdio.h>

static struct VBootMemoryEntry entries[4];
static struct VBoot boot;
static const uint64_t limit = 1ULL << 39;

static void
__Reset(void)
{
    entries[0] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Available, .PhysicalBase = 0x40000000, .Length = 0x1000000
    };
    entries[1] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Firmware, .PhysicalBase = 0x41000000, .Length = 0x1000
    };
    entries[2] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = 1ULL << 44, .Length = 0x1000
    };
    entries[3] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Firmware, .PhysicalBase = 0x41001000,
        .Length = 0x1000, .Attributes = VBOOT_MEMORY_NO_MAP
    };
    boot = (struct VBoot){ .Magic = VBOOT_MAGIC, .Version = VBOOT_VERSION,
        .Memory = {4, sizeof(entries[0]), (uintptr_t)entries} };
}

// Checks one reported cache range against the expected VBoot field.
static void
__CheckCacheRange(
    const struct Arm64BootCacheRange* range,
    uint64_t                          base,
    uint64_t                          length,
    enum Arm64BootCacheOperation      operation)
{
    CHECK(range->Base == base);
    CHECK(range->Length == length);
    CHECK(range->Operation == operation);
}

// The descriptor and the memory the kernel touched with the cache off are
// invalidated. Resources first read with the cache on are written back first.
static void
__TestCacheRanges(void)
{
    struct Arm64BootCacheRange ranges[ARM64_BOOT_CACHE_RANGE_COUNT];
    unsigned int               count;

    __Reset();
    boot.Kernel = (struct VBootModule){ .Base = 0x48000000, .EntryPoint = 0x48001000, .Length = 0x200000 };
    boot.Stack = (struct VBootStack){ .Base = 0x90000, .Length = 0x10000 };
    boot.DeviceTree = (struct VBootDeviceTree){ .PhysicalBase = 0x2eff0008, .Length = 0x15a3c };
    boot.Phoenix = (struct VBootModule){ .Base = 0x50000000, .EntryPoint = 0x1000, .Length = 0x40000 };
    boot.Ramdisk = (struct VBootRamdisk){ .Data = 0x2f000000, .Length = 0x123456 };

    count = Arm64BootCacheRanges(&boot, ranges);
    CHECK(count == ARM64_BOOT_CACHE_RANGE_COUNT);
    __CheckCacheRange(&ranges[0], (uintptr_t)&boot, sizeof(boot), Arm64BootCacheInvalidate);
    __CheckCacheRange(&ranges[1], (uintptr_t)entries, sizeof(entries), Arm64BootCacheInvalidate);
    __CheckCacheRange(&ranges[2], 0x48000000, 0x200000, Arm64BootCacheInvalidate);
    __CheckCacheRange(&ranges[3], 0x90000, 0x10000, Arm64BootCacheInvalidate);
    __CheckCacheRange(&ranges[4], 0x2eff0008, 0x15a3c, Arm64BootCacheCleanInvalidate);
    __CheckCacheRange(&ranges[5], 0x50000000, 0x40000, Arm64BootCacheCleanInvalidate);
    __CheckCacheRange(&ranges[6], 0x2f000000, 0x123456, Arm64BootCacheCleanInvalidate);

    // A kernel-only boot has no device tree, Phoenix or ramdisk, and the
    // remaining ranges keep their order.
    boot.DeviceTree = (struct VBootDeviceTree){0};
    boot.Phoenix = (struct VBootModule){0};
    boot.Ramdisk = (struct VBootRamdisk){0};
    count = Arm64BootCacheRanges(&boot, ranges);
    CHECK(count == 4);
    __CheckCacheRange(&ranges[3], 0x90000, 0x10000, Arm64BootCacheInvalidate);

    // An empty memory map is skipped without shifting the later ranges wrongly.
    boot.Memory.NumberOfEntries = 0;
    count = Arm64BootCacheRanges(&boot, ranges);
    CHECK(count == 3);
    __CheckCacheRange(&ranges[1], 0x48000000, 0x200000, Arm64BootCacheInvalidate);
    __CheckCacheRange(&ranges[2], 0x90000, 0x10000, Arm64BootCacheInvalidate);
}

int
main(void)
{
    unsigned int type;

    __Reset();
    CHECK(Arm64ValidateBootMemory(&boot, limit) == OS_EOK);
    CHECK(Arm64BootMemoryIsRam(&entries[0]));
    CHECK(Arm64BootMemoryIsRam(&entries[1]));
    CHECK(!Arm64BootMemoryIsRam(&entries[2]));
    CHECK(!Arm64BootMemoryIsRam(&entries[3]));
    for (type = VBootMemoryType_Firmware; type <= VBootMemoryType_DevicePool; type++) {
        entries[1].Type = type;
        CHECK(Arm64ValidateBootMemory(&boot, limit) == OS_EOK);
        CHECK(Arm64BootMemoryIsRam(&entries[1]));
    }
    __Reset(); boot.Version--;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); boot.Memory.EntrySize++;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); boot.Memory.Entries = limit - 8;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[2].Type = 99;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[2].PhysicalBase = UINT64_MAX - 4;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[2].Length = 0;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[2].Type = VBootMemoryType_Firmware;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[2].PhysicalBase = entries[0].PhysicalBase;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[1].Type = VBootMemoryType_DevicePool;
    entries[1].Attributes = VBOOT_MEMORY_NO_MAP;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    __Reset(); entries[0].Attributes = VBOOT_MEMORY_NO_MAP;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);

    // A reserved or no-map byte must not become readable through rounding an
    // adjacent RAM fragment. Equivalent RAM fragments may share the same page.
    __Reset(); entries[1].Length = 0x800;
    entries[2].PhysicalBase = entries[1].PhysicalBase + 0x800;
    entries[2].Length = 0x800;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    entries[2].Type = VBootMemoryType_Firmware;
    CHECK(Arm64ValidateBootMemory(&boot, limit) == OS_EOK);
    entries[2].Attributes = VBOOT_MEMORY_NO_MAP;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);
    entries[2].Attributes = 0;
    entries[2].Type = VBootMemoryType_DevicePool;
    CHECK(Arm64ValidateBootMemory(&boot, limit) != OS_EOK);

    __TestCacheRanges();
    puts("ARM64 boot memory classification, boundaries and cache ranges PASS");
    return 0;
}
