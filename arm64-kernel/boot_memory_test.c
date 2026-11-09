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
    puts("ARM64 boot memory classification and boundaries PASS");
    return 0;
}
