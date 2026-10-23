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
static int g_allocationFailure;
static int g_unmapFailure;
static unsigned int g_allocations;
static uintptr_t g_nextMapping = 0x8000;
static uintptr_t g_unmapped;

void*
kmalloc(size_t length)
{
    if (g_allocationFailure) {
        return NULL;
    }
    g_allocations++;
    return malloc(length);
}

void
kfree(void* allocation)
{
    CHECK(g_allocations != 0);
    g_allocations--;
    free(allocation);
}

oserr_t
MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    CHECK(space == NULL && length == 0x2008);
    if (g_unmapFailure) {
        return OS_EUNKNOWN;
    }
    g_unmapped = address;
    return OS_EOK;
}

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
    *mapping = g_nextMapping;
    return OS_EOK;
}

int
main(void)
{
    uintptr_t mapping = 0;
    unsigned int maps;
    DeviceIo_t* first;
    DeviceIo_t* second;

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

    g_entry.Io.Id = 77;
    g_entry.Io.Access.Memory.PhysicalBase = 0x40000008;
    g_entry.Io.Access.Memory.Length = 0x2000;
    g_entry.Io.Access.Memory.VirtualBase = 0x123000;
    g_allocationFailure = 1;
    CHECK(CreateKernelSystemDeviceIo(&g_entry.Io, &first) == OS_EOOM && first == NULL);
    g_allocationFailure = 0;
    g_mapFailure = 1;
    CHECK(CreateKernelSystemDeviceIo(&g_entry.Io, &first) == OS_EOOM && first == NULL);
    CHECK(g_allocations == 0);
    g_mapFailure = 0;
    CHECK(CreateKernelSystemDeviceIo(&g_entry.Io, &first) == OS_EOK);
    CHECK(first != &g_entry.Io && first->Access.Memory.VirtualBase == 0x8008);
    CHECK(g_options.PhysicalStart == 0x40000000 && g_options.Length == 0x2008);
    CHECK(g_options.PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_GLOBAL));
    CHECK(g_options.Flags == (MAPPING_COMMIT | MAPPING_NOCACHE | MAPPING_DEVICE | MAPPING_PERSISTENT));
    g_nextMapping = 0x10000;
    CHECK(CreateKernelSystemDeviceIo(&g_entry.Io, &second) == OS_EOK);
    CHECK(second != first && second != &g_entry.Io && second->Access.Memory.VirtualBase == 0x10008);
    CHECK(g_entry.Io.Access.Memory.VirtualBase == 0x123000);
    CHECK(ReleaseKernelSystemDeviceIo(first) == OS_EOK && g_unmapped == 0x8000);
    CHECK(second->Access.Memory.VirtualBase == 0x10008 && g_allocations == 1);
    g_unmapFailure = 1;
    CHECK(ReleaseKernelSystemDeviceIo(second) == OS_EUNKNOWN);
    CHECK(second->Access.Memory.VirtualBase == 0x10008 && g_allocations == 1);
    g_unmapFailure = 0;
    CHECK(ReleaseKernelSystemDeviceIo(second) == OS_EOK && g_unmapped == 0x10000);
    CHECK(g_allocations == 0 && g_entry.Io.Access.Memory.VirtualBase == 0x123000);
    g_entry.Io.Type = DeviceIoPortBased;
    CHECK(CreateKernelSystemDeviceIo(&g_entry.Io, &first) == OS_EOK);
    CHECK(first->Type == DeviceIoPortBased && first != &g_entry.Io);
    CHECK(ReleaseKernelSystemDeviceIo(first) == OS_EOK && g_allocations == 0);
    puts("MMIO mapping: bounds, overflow, page offset, map failure and exclusive owner preservation passed");
    puts("Interrupt IO: independent binding mappings, allocation/map failures and retryable unmap passed");
    return 0;
}
