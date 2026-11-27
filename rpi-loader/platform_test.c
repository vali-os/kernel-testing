// Exercise the real platform stage on host-addressable DTBs. Absolute linker
// symbols model the wrapper's physical span without executing ARM entry code.
//
// Usage: test <dtb> <board> <expected status> <arena base> <shape>
// The arena is 1 MiB of host memory mapped at the given address. It must
// cover the top of the highest RAM bank, where the loader places the copied
// tree. Shape "fixture" adds checks that only hold for the Python fixture.
// Shape "firmware" skips them for bundled Raspberry Pi trees. On success the
// harness prints each dynamic reservation and the memory map, and writes the
// copied tree to "<dtb>.copy" so Python can decode the inserted reg.
#include "loader.h"
#include <fdt/reader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// Host Linux mmap ABI; Vali's public mmap flags describe a different ABI.
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);

// Linker wrapping observes the shared walker without introducing production
// test hooks. Successful preparation must perform exactly one structure walk.
static unsigned int structureWalks;

oserr_t __real_FdtParseStructure(const void*, uint32_t, const char*, uint32_t, struct FdtParser*);

oserr_t
__wrap_FdtParseStructure(const void* block, uint32_t size, const char* strings,
    uint32_t stringsSize, struct FdtParser* parser)
{
    structureWalks++;
    return __real_FdtParseStructure(block, size, strings, stringsSize, parser);
}

static unsigned char blob[RPI_DTB_MAX_SIZE] __attribute__((aligned(8)));
static struct RpiBootContext context;
static struct RpiBootContext copiedContext;
static char copyPath[4096];

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

static int
excluded(uint64_t base, uint64_t length)
{
    for (uint32_t i = 0; i < context.MemoryMapCount; i++) {
        struct VBootMemoryEntry* entry = &context.MemoryMap[i];
        if (entry->Type == VBootMemoryType_Available &&
            entry->PhysicalBase < base + length && base < entry->PhysicalBase + entry->Length) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char** argv)
{
    CHECK(argc == 6);
    // Dynamic DTB copies use the last pages of the highest RAM bank, which
    // must have real host backing. The fixture's RAM ends at 1 GiB.
    void* arenaBase = (void*)(uintptr_t)strtoull(argv[4], NULL, 0);
    int fixtureShape = strcmp(argv[5], "fixture") == 0;
    void* arena = mmap(arenaBase, 0x100000, 3,
        0x100022, -1, 0);
    CHECK(arena == arenaBase);
    FILE* file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    size_t length = fread(blob, 1, sizeof(blob), file);
    CHECK(!ferror(file) && length < sizeof(blob));
    fclose(file);
    context.DtbPhysical = (uintptr_t)blob;
    context.Board = (unsigned int)atoi(argv[2]);
    context.Kernel.ImageLength = 0x21000;
    int expected = atoi(argv[3]);
    CHECK((int)RpiPlatformPrepare(&context) == expected);
    if (expected) {
        CHECK(context.MemoryMapCount == 0 && context.BootInformation.Memory.NumberOfEntries == 0);
        CHECK(!context.BootInformation.Memory.Entries && !context.BootInformation.DeviceTree.Length);
        return 0;
    }
    CHECK(structureWalks == 1);
    CHECK(context.MemoryMapCount > 0);
    CHECK(context.BootInformation.Memory.Entries == (uintptr_t)context.MemoryMap);
    CHECK(context.BootInformation.Memory.EntrySize == sizeof(struct VBootMemoryEntry));
    if (context.ReservationCount) {
        struct FDTHeader header;
        const void* tree = (void*)(uintptr_t)context.BootInformation.DeviceTree.PhysicalBase;

        CHECK(context.BootInformation.DeviceTree.Length > length);
        CHECK(FdtParseHeader(tree, context.BootInformation.DeviceTree.Length, &header) == OS_EOK);
        CHECK(context.Reservations[0].PhysicalBase % context.Reservations[0].Alignment == 0);
        CHECK(excluded(context.Reservations[0].PhysicalBase, context.Reservations[0].Length));
        CHECK(excluded((uintptr_t)tree, context.BootInformation.DeviceTree.Length));
        copiedContext.Board = context.Board;
        CHECK(DeviceTreeParseEarlyPlatform(tree, context.BootInformation.DeviceTree.Length,
            &copiedContext) == OS_EOK);
        CHECK(copiedContext.ReservationCount == 0);
        for (uint32_t i = 0; i < context.ReservationCount; i++) {
            printf("reservation 0x%llx 0x%llx 0x%llx\n",
                (unsigned long long)context.Reservations[i].PhysicalBase,
                (unsigned long long)context.Reservations[i].Length,
                (unsigned long long)context.Reservations[i].Alignment);
        }
        CHECK(snprintf(copyPath, sizeof(copyPath), "%s.copy", argv[1]) < (int)sizeof(copyPath));
        file = fopen(copyPath, "wb");
        CHECK(file != NULL);
        CHECK(fwrite(tree, 1, context.BootInformation.DeviceTree.Length, file) ==
            context.BootInformation.DeviceTree.Length);
        CHECK(fclose(file) == 0);
    } else {
        CHECK(context.BootInformation.DeviceTree.PhysicalBase == (uintptr_t)blob);
        CHECK(context.BootInformation.DeviceTree.Length == length);
    }
    if (context.ExternalPayloadLength) {
        CHECK(!fixtureShape ||
            (context.ExternalPayloadBase == 0x4000000 && context.ExternalPayloadLength == 0x12345));
        CHECK(excluded(context.ExternalPayloadBase, context.ExternalPayloadLength));
    } else {
        CHECK(context.ExternalPayloadBase == 0);
    }
    CHECK(excluded(0, 0xa1000));
    CHECK(excluded((uintptr_t)blob, length));
    if (fixtureShape) {
        if (!context.ReservationCount) {
            CHECK(excluded(0x2000000, 0x100));
        }
        CHECK(excluded(0x300123, 4));
        CHECK(excluded(0x1100088, 8));
    }
    for (uint32_t i = 0; i < context.MemoryMapCount; i++) {
        struct VBootMemoryEntry* entry = &context.MemoryMap[i];
        CHECK(entry->Length > 0);
        if (entry->Attributes & VBOOT_MEMORY_NO_MAP) {
            CHECK(entry->Type == VBootMemoryType_Firmware);
            CHECK(!(entry->PhysicalBase & 4095) && !(entry->Length & 4095));
        }
        if (i) {
            CHECK(context.MemoryMap[i - 1].PhysicalBase + context.MemoryMap[i - 1].Length <= entry->PhysicalBase);
        }
        if (entry->Type == VBootMemoryType_Available) {
            CHECK(!(entry->PhysicalBase & 4095) && !(entry->Length & 4095));
        }
        printf("memory %u 0x%llx 0x%llx 0x%llx\n", (unsigned)entry->Type,
            (unsigned long long)entry->Attributes, (unsigned long long)entry->PhysicalBase,
            (unsigned long long)entry->Length);
    }
    // A reservation beyond every memory node does not establish RAM. In
    // contrast, even a sub-page reservation inside a bank is Firmware RAM.
    CHECK(DeviceTreeReserveMemory(&context, 1ULL << 44, 0x1000) == OS_EOK);
    CHECK(context.MemoryMap[context.MemoryMapCount - 1].Type == VBootMemoryType_Reserved);
    for (uint32_t i = 0; fixtureShape && i < context.MemoryMapCount; i++) {
        struct VBootMemoryEntry* entry = &context.MemoryMap[i];
        if (entry->PhysicalBase <= 0x300123 && 0x300123 < entry->PhysicalBase + entry->Length) {
            CHECK(entry->Type == VBootMemoryType_Firmware);
        }
    }
    // Header/truncation failures must not leave the previous call's count live.
    CHECK(DeviceTreeParseEarlyPlatform(blob, 39, &context) != OS_EOK);
    CHECK(context.MemoryMapCount == 0 && context.ExternalPayloadLength == 0);
    CHECK(DeviceTreeParseEarlyPlatform(blob, (uint32_t)length - 1, &context) != OS_EOK);
    CHECK(context.MemoryMapCount == 0);
    CHECK(DeviceTreeParseEarlyPlatform(&context, sizeof(context), &context) != OS_EOK);
    CHECK(context.MemoryMapCount == 0);
    return 0;
}
