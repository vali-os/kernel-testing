#include <os/osdefs.h>
#include <ds/list.h>
// The host-only libds initializer includes a trailing semicolon; use its value
// here so the real kernel file's static list initializer is valid C.
#undef SYNC_INIT
#define SYNC_INIT { 0 }
#include "../kernel/deviceio.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static DeviceIOEntry_t g_entry;
static struct MemorySpaceMapOptions g_options;
static unsigned int g_maps;
static int g_mapFailure;

void*
list_find_value(list_t* list, void* key)
{
    CHECK(list == &g_ioSpaces);
    return (uintptr_t)key == 77 ? &g_entry : NULL;
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return NULL;
}

size_t
GetMemorySpacePageSize(void)
{
    return 4096;
}

oserr_t
MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* mapping)
{
    CHECK(space == NULL);
    g_maps++;
    g_options = *options;
    if (g_mapFailure) {
        return OS_EOOM;
    }
    *mapping = 0x8000;
    return OS_EOK;
}

int
main(void)
{
    uintptr_t mapping = 0;
    unsigned int maps;

    g_entry.Io.Type = DeviceIoMemoryBased;
    g_entry.Io.Access.Memory.PhysicalBase = 0x40000008;
    g_entry.Io.Access.Memory.Length = 0x2000;
    g_entry.Owner = 123;
    g_entry.MappedAddress = 0x9000;
    CHECK(MapSystemDeviceIo(77, 0x120, 64, &mapping) == OS_EOK);
    CHECK(mapping == 0x8128 && g_options.PhysicalStart == 0x40000000);
    CHECK(g_options.Length == 0x168);
    CHECK(g_options.Flags == (MAPPING_COMMIT | MAPPING_USERSPACE | MAPPING_NOCACHE |
        MAPPING_DEVICE | MAPPING_PERSISTENT));
    CHECK(g_options.PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_PROCESS));
    CHECK(g_entry.Owner == 123 && g_entry.MappedAddress == 0x9000);
    CHECK(MapSystemDeviceIo(77, 0x1fff, 1, &mapping) == OS_EOK);
    CHECK(g_options.PhysicalStart == 0x40002000 && g_options.Length == 8);
    CHECK(mapping == 0x8007);

    maps = g_maps;
    CHECK(MapSystemDeviceIo(78, 0, 1, &mapping) == OS_ENOENT);
    CHECK(MapSystemDeviceIo(77, 0, 0, &mapping) == OS_EINVALPARAMS);
    CHECK(MapSystemDeviceIo(77, 0x2000, 1, &mapping) == OS_EINVALPARAMS);
    CHECK(MapSystemDeviceIo(77, 0x1fff, 2, &mapping) == OS_EINVALPARAMS);
    CHECK(MapSystemDeviceIo(77, 1, SIZE_MAX, &mapping) == OS_EINVALPARAMS);
    g_entry.Io.Type = DeviceIoPortBased;
    CHECK(MapSystemDeviceIo(77, 0, 1, &mapping) == OS_ENOTSUPPORTED);
    g_entry.Io.Type = DeviceIoMemoryBased;
    g_entry.Io.Access.Memory.PhysicalBase = UINTPTR_MAX - 7;
    CHECK(MapSystemDeviceIo(77, 8, 1, &mapping) == OS_EOVERFLOW);
    CHECK(MapSystemDeviceIo(77, 0, 9, &mapping) == OS_EOVERFLOW);
    g_entry.Io.Access.Memory.PhysicalBase = 0;
    g_entry.Io.Access.Memory.Length = SIZE_MAX;
    CHECK(MapSystemDeviceIo(77, 0, SIZE_MAX, &mapping) == OS_EOVERFLOW);
    if (SIZE_MAX / 4096 > INT_MAX) {
        CHECK(MapSystemDeviceIo(77, 0, (size_t)INT_MAX * 4096 + 1, &mapping) == OS_EOVERFLOW);
    }
    CHECK(g_maps == maps);

    g_mapFailure = 1;
    mapping = 1234;
    CHECK(MapSystemDeviceIo(77, 0, 64, &mapping) == OS_EOOM);
    CHECK(mapping == 1234 && g_entry.Owner == 123 && g_entry.MappedAddress == 0x9000);
    g_mapFailure = 0;
    g_entry.Owner = UUID_INVALID;
    CHECK(MapSystemDeviceIo(77, 0, 64, &mapping) == OS_EOK);
    CHECK(g_entry.Owner == UUID_INVALID && g_entry.MappedAddress == 0x9000);
    puts("MMIO mapping: bounds, overflow, page offset, map failure and exclusive owner preservation passed");
    return 0;
}
