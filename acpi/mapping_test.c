// Link the production OSL and architecture policy against a page-map backend.
// The fixture distinguishes cached RAM, DMA RAM, holes and device mappings.
#include <acpi.h>
#include <arch/acpi.h>
#include <arch/mmu.h>
#include <machine.h>
#include <memoryspace.h>
#include <log.h>
#include <assert.h>
#include <stdio.h>

static SystemMachine_t g_machine;
static unsigned int g_pages[4];
static unsigned int g_maps;
static unsigned int g_unmaps;
static int g_failMap;
static int g_failLookup;
static struct MemorySpaceMapOptions g_options;

SystemMachine_t* GetMachine(void) { return &g_machine; }
MemorySpace_t* GetDomainMemorySpace(void) { return &g_machine.SystemSpace; }
size_t GetMemorySpacePageSize(void) { return 4096; }
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }

oserr_t
ArchMmuGetPageAttributes(MemorySpace_t* space, vaddr_t address, int count,
                        unsigned int* attributes, int* retrieved)
{
    assert(space == &g_machine.SystemSpace && count == 1);
    assert(!(address & 4095));
    *retrieved = 1;
    *attributes = address >= 0x10000 && address < 0x14000 ? g_pages[(address - 0x10000) / 4096] : 0;
    return g_failLookup ? OS_EUNKNOWN : OS_EOK;
}

oserr_t
MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* output)
{
    assert(space == &g_machine.SystemSpace);
    g_maps++;
    g_options = *options;
    *output = 0x40000000;
    return g_failMap ? OS_EOOM : OS_EOK;
}

oserr_t
MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    assert(space == &g_machine.SystemSpace);
    assert(address == 0x40000000 && length == g_options.Length);
    assert(g_options.Flags & MAPPING_PERSISTENT);
    g_unmaps++;
    return OS_EOK;
}

int
main(void)
{
    void* view;
    unsigned int before;

    g_pages[0] = g_pages[1] = MAPPING_COMMIT;
    g_pages[2] = MAPPING_COMMIT | MAPPING_NOCACHE;
    g_pages[3] = 0;
    view = AcpiOsMapMemory(0x10ff8, 36);
    assert(view == (void*)0x40000ff8);
    assert(g_options.PhysicalStart == 0x10000 && g_options.Length == 8192);
    assert(g_options.PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_GLOBAL));
#ifdef TEST_ARM64
    assert(!(g_options.Flags & (MAPPING_NOCACHE | MAPPING_DEVICE)));
#else
    assert(g_options.Flags & MAPPING_NOCACHE);
#endif
    AcpiOsUnmapMemory(view, 36);
    assert(g_unmaps == 1);
    assert(AcpiOsMapMemory(0x12008, 8) == (void*)0x40000008);
    assert(g_options.Flags & MAPPING_NOCACHE);
    AcpiOsUnmapMemory((void*)0x40000008, 8);

    before = g_maps;
    assert(!AcpiOsMapMemory(0, 0));
    assert(!AcpiOsMapMemory(UINT64_MAX - 8, 16));
    assert(!AcpiOsMapMemory(1, SIZE_MAX));
    assert(g_maps == before);
#ifdef TEST_ARM64
    assert(!AcpiOsMapMemory(0x11ff8, 16)); // Cached/uncached boundary.
    assert(!AcpiOsMapMemory(0x12ff8, 16)); // RAM/hole boundary.
    assert(!AcpiOsMapMemory(0x9000000, 4)); // MMIO is not table RAM.
    assert(!AcpiOsMapMemory(0x8000000000ULL, 4)); // Outside identity window.
    assert(!AcpiOsMapMemory(0x7fffffffffULL, 2));
    g_pages[3] = MAPPING_COMMIT | MAPPING_DEVICE;
    assert(!AcpiOsMapMemory(0x13000, 4));
    g_failLookup = 1;
    assert(!AcpiOsMapMemory(0x10000, 4));
    g_failLookup = 0;
    assert(g_maps == before);
#else
    // Preserve the PC BIOS search range and x86 MMIO behavior.
    assert(AcpiOsMapMemory(0xe0000, 20));
#endif
    g_failMap = 1;
    assert(!AcpiOsMapMemory(0x10000, 8));
    puts("ACPICA mapping: PASS");
    return 0;
}
