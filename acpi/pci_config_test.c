#include "../../kernel/acpi/pci-config.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t g_registers[1024];
static uint64_t g_physical;
static unsigned int g_maps;
static unsigned int g_unmaps;
static unsigned int g_legacy;
static int g_failMap;
static struct { ACPI_TABLE_MCFG Table; ACPI_MCFG_ALLOCATION Entry; } g_table;
MemorySpace_t* GetDomainMemorySpace(void) { return (void*)1; }
oserr_t MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* out)
{
    if (g_failMap) return OS_EOOM;
    assert(options->Length == 4096);
    assert((options->Flags & (MAPPING_DEVICE | MAPPING_NOCACHE)) == (MAPPING_DEVICE | MAPPING_NOCACHE));
    g_physical = options->PhysicalStart; g_maps++; *out = (uintptr_t)g_registers; return OS_EOK;
}
oserr_t MemorySpaceUnmap(MemorySpace_t* space, vaddr_t mapping, size_t length)
{
    assert(mapping == (uintptr_t)g_registers && length == 4096); g_unmaps++; return OS_EOK;
}
oserr_t ReadDirectPci(unsigned int bus, unsigned int slot, unsigned int function, unsigned int reg, size_t width, size_t* value)
{
    g_legacy++; *value = 0xabcd; return OS_EOK;
}
oserr_t WriteDirectPci(unsigned int bus, unsigned int slot, unsigned int function, unsigned int reg, size_t width, size_t value)
{
    g_legacy++; return OS_EOK;
}
int main(void)
{
    ACPI_PCI_ID id = { .Segment = 7, .Bus = 32, .Device = 3, .Function = 2 };
    UINT64 value = 0;
    g_table.Table.Header.Length = sizeof(g_table);
    g_table.Entry = (ACPI_MCFG_ALLOCATION) { .Address = 0x80000000, .PciSegment = 7, .StartBusNumber = 32, .EndBusNumber = 63 };
    g_pciMcfg = &g_table.Table.Header; g_registers[4] = 0x12345678;
    assert(AcpiPciConfig(&id,16,&value,32,0) == AE_OK && value == 0x12345678);
    assert(g_physical == 0x8201a000 && g_maps == 1 && g_unmaps == 1 && !g_legacy);
    value = 0xaabb; assert(AcpiPciConfig(&id,18,&value,16,1) == AE_OK && g_registers[4] == 0xaabb5678);
    assert(AcpiPciConfig(&id,17,&value,8,0) == AE_OK && value == 0x56);
    assert(AcpiPciConfig(&id,17,&value,16,0) == AE_BAD_PARAMETER);
    assert(AcpiPciConfig(&id,4095,&value,32,0) == AE_BAD_PARAMETER);
    assert(AcpiPciConfig(&id,4096,&value,8,0) == AE_BAD_PARAMETER);
    assert(AcpiPciConfig(&id,0,&value,64,0) == AE_BAD_PARAMETER);
    id.Device = 32; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_BAD_PARAMETER); id.Device = 3;
    id.Function = 8; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_BAD_PARAMETER); id.Function = 2;
    id.Bus = 64; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_NOT_EXIST); id.Bus = 32;
    g_failMap = 1; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_NO_MEMORY); g_failMap = 0;
    id.Segment = 0; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_OK && g_legacy == 1);
    assert(AcpiPciConfig(&id,256,&value,32,0) == AE_NOT_EXIST && g_legacy == 1);
    g_pciMcfgInvalid = 1; assert(AcpiPciConfig(&id,0,&value,32,0) == AE_BAD_DATA && g_legacy == 1);
    assert(g_maps == g_unmaps); puts("AML PCI_Config segment, width, bounds and mapping lifecycle: PASS"); return 0;
}
