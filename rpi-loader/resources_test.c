// Exercise the real resource loader with host-addressable physical storage.
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char __rpi_loader_start[4096];

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int
main(int argc, char** argv)
{
    struct RpiBootContext context = {0};
    unsigned char* transport = malloc(65536);
    unsigned char* memory = aligned_alloc(65536, 65536);
    FILE* file;
    size_t length;
    uint64_t pointer;
    enum RpiBootStatus expected;

    CHECK(argc == 3 && transport && memory);
    file = fopen(argv[1], "rb");
    CHECK(file);
    length = fread(transport, 1, 65536, file);
    fclose(file);
    expected = atoi(argv[2]);
    memset(memory, 0xa5, 65536);
    context.Kernel.ImageLength = 4096;
    context.ExternalPayloadBase = (uintptr_t)transport;
    context.ExternalPayloadLength = length;
    context.MemoryMapCount = 1;
    context.MemoryMap[0] = (struct VBootMemoryEntry){
        .Type = VBootMemoryType_Available,
        .PhysicalBase = (uintptr_t)memory,
        .Length = 65536
    };
    CHECK(RpiLoadResources(&context) == expected);
    if (expected == RpiBootOk) {
        CHECK(context.BootInformation.Phoenix.Base == (uintptr_t)memory);
        CHECK(context.BootInformation.Phoenix.EntryPoint == 0x48001000);
        CHECK(context.BootInformation.Phoenix.Length == 0x5000);
        memcpy(&pointer, memory + 0x2000, 8);
        CHECK(pointer == 0x48001000);
        for (unsigned int i = 0x2200; i < 0x3800; i++) {
            CHECK(memory[i] == 0);
        }
        CHECK(context.BootInformation.Ramdisk.Data == (uintptr_t)transport + 4096);
        CHECK(context.BootInformation.Ramdisk.Length == 13);
        CHECK(!memcmp((void*)(uintptr_t)context.BootInformation.Ramdisk.Data,
            "Vali ramdisk!", 13));
        CHECK(context.MemoryMap[0].Type == VBootMemoryType_Reserved);
        CHECK(context.MemoryMap[0].Length == 0x5000);
    } else {
        CHECK(!context.BootInformation.Phoenix.Base);
        CHECK(!context.BootInformation.Ramdisk.Data);
        for (unsigned int i = 0; i < 65536; i++) {
            CHECK(memory[i] == 0xa5);
        }
    }
    free(memory);
    free(transport);
    return 0;
}
