// Host RAM lets the real loader and reservation code run under sanitizers.
// The input remains a file-layout PE; the destination starts dirty so missing
// zero-fill cannot accidentally pass because the host supplied fresh pages.
#include "loader.h"
#include <os/pe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char __rpi_loader_start[2 * 1024 * 1024];
static struct RpiBootContext context;

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int
main(int argc, char** argv)
{
    FILE* file;
    size_t length;
    unsigned char* memory;
    unsigned char* source = __rpi_loader_start + 4096;
    uint64_t base;
    uint64_t value;
    int expected;
    int status;
    PeOptionalHeader64_t* optional;
    PeSectionHeader_t* sections;
    unsigned char* snapshot;

    CHECK(argc == 4);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL);
    length = fread(source, 1, sizeof(__rpi_loader_start) - 4096, file);
    CHECK(!ferror(file));
    fclose(file);
    memory = aligned_alloc(65536, 0x40000);
    CHECK(memory != NULL);
    memset(memory, 0xa5, 0x40000);
    base = (uintptr_t)memory + 0x10000;
    CHECK(base > (uintptr_t)__rpi_loader_start + sizeof(__rpi_loader_start));
    context.Kernel.Offset = 4096;
    context.Kernel.Length = length;
    context.Kernel.ImageLength = 4096 + length;
    context.MemoryMapCount = 1;
    context.MemoryMap[0] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Available, .PhysicalBase = base, .Length = 0x20000
    };
    context.BootInformation.Memory.Entries = (uintptr_t)context.MemoryMap;
    context.BootInformation.Memory.EntrySize = sizeof(struct VBootMemoryEntry);
    context.BootInformation.Memory.NumberOfEntries = 1;
    // A stale success descriptor must not survive a failed attempt.
    memset(&context.BootInformation.Kernel, 0xff, sizeof(struct VBootModule));
    expected = atoi(argv[2]);
    optional = (PeOptionalHeader64_t*)(source + 88);
    sections = (PeSectionHeader_t*)(source + 328);

    if (!strcmp(argv[3], "preferred")) {
        optional->BaseAddress = base;
        value = base + 0x1000;
        memcpy(source + 1024, &value, 8);
    } else if (!strcmp(argv[3], "lower")) {
        optional->BaseAddress = base + 0x100000;
        value = optional->BaseAddress + 0x1000;
        memcpy(source + 1024, &value, 8);
    } else if (!strcmp(argv[3], "full")) {
        // Splitting the final available range needs a slot that does not exist.
        context.MemoryMapCount = RPI_MEMORY_MAP_CAPACITY;
        for (unsigned int i = 0; i < RPI_MEMORY_MAP_CAPACITY - 1; i++) {
            context.MemoryMap[i] = (struct VBootMemoryEntry){
                .Type = VBootMemoryType_Reserved, .PhysicalBase = i * 8192, .Length = 4096
            };
        }
        context.MemoryMap[RPI_MEMORY_MAP_CAPACITY - 1] = (struct VBootMemoryEntry){
            .Type = VBootMemoryType_Available, .PhysicalBase = base, .Length = 0x20000
        };
    } else if (!strcmp(argv[3], "reserved")) {
        context.MemoryMap[0].Type = VBootMemoryType_Reserved;
    } else if (!strcmp(argv[3], "small")) {
        context.MemoryMap[0].Length = 4096;
    } else if (!strcmp(argv[3], "source")) {
        context.MemoryMap[0].PhysicalBase = (uintptr_t)__rpi_loader_start;
        context.MemoryMap[0].Length = context.Kernel.ImageLength;
    } else if (!strcmp(argv[3], "fragmented")) {
        context.MemoryMap[0].Length = 0x2000;
        context.MemoryMap[1] = (struct VBootMemoryEntry){
            .Type = VBootMemoryType_Reserved, .PhysicalBase = base + 0x2000, .Length = 0xe000
        };
        context.MemoryMap[2] = (struct VBootMemoryEntry){
            .Type = VBootMemoryType_Available, .PhysicalBase = base + 0x10000, .Length = 0x10000
        };
        context.MemoryMapCount = 3;
        base += 0x10000;
    }
    snapshot = malloc(length);
    CHECK(snapshot != NULL);
    memcpy(snapshot, source, length);
    status = RpiLoadKernel(&context);
    if (status != expected) {
        printf("status %d, expected %d\n", status, expected);
        return 1;
    }
    CHECK(!memcmp(snapshot, source, length));
    CHECK(context.BootInformation.Memory.NumberOfEntries == context.MemoryMapCount);
    if (expected) {
        CHECK(!context.BootInformation.Kernel.Base && !context.BootInformation.Kernel.Length &&
            !context.BootInformation.Kernel.EntryPoint);
        for (unsigned int i = 0; i < 0x40000; i++) {
            CHECK(memory[i] == 0xa5);
        }
        if (!strcmp(argv[3], "full")) {
            CHECK(context.MemoryMapCount == 0);
        }
    } else {
        unsigned char* loaded = (unsigned char*)(uintptr_t)base;
        CHECK(context.BootInformation.Kernel.Base == base);
        if (!strcmp(argv[3], "linked")) {
            // The linked fixture deliberately keeps a function pointer as its
            // first .data object and a large zero-initialized tail beside it.
            MzHeader_t* mz = (MzHeader_t*)source;
            PeHeader_t* pe = (PeHeader_t*)(source + mz->PeHeaderAddress);
            optional = (PeOptionalHeader64_t*)(pe + 1);
            sections = (PeSectionHeader_t*)(optional + 1);
        }
        CHECK(context.BootInformation.Kernel.EntryPoint == base + optional->Base.EntryPointRVA);
        CHECK(context.BootInformation.Kernel.Length == optional->SizeOfImage);
        CHECK(!memcmp(loaded, source, optional->SizeOfHeaders));
        CHECK(!memcmp(loaded + sections[0].VirtualAddress, source + sections[0].RawAddress,
            sections[0].RawSize));
        memcpy(&value, loaded + 0x2000, 8);
        CHECK(value == base + optional->Base.EntryPointRVA);
        for (unsigned int i = 0x2200; i < 0x3800; i++) {
            CHECK(loaded[i] == 0);
        }
        for (unsigned int i = 0; i < 0x10000; i++) {
            CHECK(memory[i] == 0xa5 && memory[0x30000 + i] == 0xa5);
        }
        for (uint32_t i = 0; i < context.MemoryMapCount; i++) {
            struct VBootMemoryEntry* entry = &context.MemoryMap[i];
            CHECK(entry->Type != VBootMemoryType_Available || entry->PhysicalBase >= base + optional->SizeOfImage ||
                entry->PhysicalBase + entry->Length <= base);
        }
    }
    free(snapshot);
    free(memory);
    return 0;
}
