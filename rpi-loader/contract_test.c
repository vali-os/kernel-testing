// Host-addressable context storage exercises lifetime checks without executing
// the ARM stack transition. Absolute linker symbols model the reserved wrapper.
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int
main(int argc, char** argv)
{
    struct RpiBootContext* context = calloc(1, sizeof(*context));
    struct VBootMemoryEntry snapshot[RPI_MEMORY_MAP_CAPACITY];
    struct VBoot* boot;
    enum RpiBootStatus expected = RpiBootInvalidPlatform;
    int scenario;

    CHECK(context && argc == 2);
    CHECK((uintptr_t)context > 0x500000);
    scenario = atoi(argv[1]);
    boot = &context->BootInformation;
    context->Kernel.ImageLength = 0x21000;
    context->DtbPhysical = 0x300000;
    context->ExternalPayloadBase = 0x400000;
    context->ExternalPayloadLength = 0x1000;
    context->MemoryMapCount = 5;
    context->MemoryMap[0] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = 0, .Length = 0xa1000
    };
    context->MemoryMap[1] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = 0x200000, .Length = 0x6000
    };
    context->MemoryMap[2] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = 0x300000, .Length = 0x1000
    };
    context->MemoryMap[3] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = 0x400000, .Length = 0x1000
    };
    context->MemoryMap[4] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Reserved, .PhysicalBase = (uintptr_t)context, .Length = sizeof(*context)
    };
    boot->Memory = (struct VBootMemory){5, sizeof(struct VBootMemoryEntry), (uintptr_t)context->MemoryMap};
    boot->Kernel = (struct VBootModule){0x200000, 0x201000, 0x6000};
    boot->DeviceTree = (struct VBootDeviceTree){0x300000, 100};
    boot->Magic = VBOOT_MAGIC;
    boot->Version = VBOOT_VERSION;
    boot->Firmware = VBootFirmware_UEFI;
    boot->ConfigurationTable = 0xdead;
    boot->ConfigurationEntrySize = 24;
    boot->ConfigurationTableCount = 1;
    boot->Video.FrameBuffer = 0xdead;

    switch (scenario) {
        case 0: expected = RpiBootOk; break;
        case 1: context->MemoryMapCount = 0; break;
        case 2: boot->Memory.EntrySize--; break;
        case 3: boot->Memory.Entries++; break;
        case 4: boot->Memory.NumberOfEntries--; break;
        case 5:
            context->MemoryMap[1].Type = VBootMemoryType_Available;
            expected = RpiBootInvalidPayload;
            break;
        case 6:
            context->MemoryMap[1].Length = 0x1000;
            expected = RpiBootInvalidPayload;
            break;
        case 7: boot->Kernel.EntryPoint = 0x206000; expected = RpiBootInvalidPayload; break;
        case 8: boot->Kernel.EntryPoint++; expected = RpiBootInvalidPayload; break;
        case 9: boot->DeviceTree.Length = 0; break;
        case 10: boot->DeviceTree.PhysicalBase += 8; break;
        case 11: context->MemoryMap[2].Type = VBootMemoryType_Available; break;
        case 12: context->MemoryMap[4].Length--; break;
        case 13: context->MemoryMap[4].Length = UINT64_MAX; break;
        case 14: context->MemoryMap[1].PhysicalBase = 0x90000; break;
        case 15: context->MemoryMap[1].Type = (enum VBootMemoryType)99; break;
        case 16:
            context->MemoryMap[1].Type = VBootMemoryType_Firmware;
            expected = RpiBootInvalidPayload;
            break;
        case 17: context->ExternalPayloadLength = 0; expected = RpiBootInvalidPayload; break;
        case 18:
            context->MemoryMap[3].Type = VBootMemoryType_Available;
            expected = RpiBootInvalidPayload;
            break;
        case 19: context->DtbPhysical = boot->DeviceTree.PhysicalBase = 0x201000; break;
        case 20: boot->Phoenix.Length = 0x1000; expected = RpiBootInvalidPayload; break;
        case 21: boot->Ramdisk.Data = 0x400000; expected = RpiBootInvalidPayload; break;
        case 22: context->MemoryMap[0].Length = 0x80000; break;
        case 23: context->ExternalPayloadBase = 0x201000; expected = RpiBootInvalidPayload; break;
        case 24:
            context->ExternalPayloadBase = context->ExternalPayloadLength = 0;
            expected = RpiBootOk;
            break;
        case 25:
            // Adjacent reservations with different provenance still cover the
            // allocation. A gap between them must not pass the same check.
            memmove(&context->MemoryMap[2], &context->MemoryMap[1], 4 * sizeof(struct VBootMemoryEntry));
            context->MemoryMap[0].Length = 0x90000;
            context->MemoryMap[1] = (struct VBootMemoryEntry){
                .Type = VBootMemoryType_Reserved, .PhysicalBase = 0x90000, .Length = 0x11000
            };
            context->MemoryMapCount = boot->Memory.NumberOfEntries = 6;
            expected = RpiBootOk;
            break;
        case 26: context->MemoryMapCount = RPI_MEMORY_MAP_CAPACITY + 1; break;
        case 27: context->Kernel.ImageLength = UINT64_MAX; break;
        case 28: boot->Kernel.Base = UINT64_MAX - 4095; expected = RpiBootInvalidPayload; break;
        default: CHECK(0);
    }
    memcpy(snapshot, context->MemoryMap, sizeof(snapshot));
    CHECK(RpiBuildContract(NULL) == RpiBootInvalidPlatform);
    CHECK(RpiBuildContract(context) == expected);
    CHECK(!memcmp(snapshot, context->MemoryMap, sizeof(snapshot)));
    if (expected != RpiBootOk) {
        CHECK(boot->Magic == 0 && boot->Version == 0);
    } else {
        CHECK(boot->Magic == VBOOT_MAGIC && boot->Version == VBOOT_VERSION);
        CHECK(boot->Firmware == VBootFirmware_Native);
        CHECK(!boot->ConfigurationTable && !boot->ConfigurationEntrySize && !boot->ConfigurationTableCount);
        CHECK(boot->Stack.Base == 0x88000 && boot->Stack.Length == 0x10000);
        CHECK(boot->Kernel.Base == 0x200000 && boot->Kernel.EntryPoint == 0x201000 && boot->Kernel.Length == 0x6000);
        CHECK(boot->DeviceTree.PhysicalBase == 0x300000 && boot->DeviceTree.Length == 100);
        CHECK(!boot->Video.FrameBuffer && !boot->Phoenix.Length && !boot->Ramdisk.Length);
        CHECK(RpiBuildContract(context) == RpiBootOk);
    }
    free(context);
    return 0;
}
