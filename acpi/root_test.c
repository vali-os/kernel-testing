#include <acpi.h>
#include <machine.h>
#include <vboot/uefi.h>
#include <log.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SystemMachine_t g_machine;
static __EFI_CONFIGURATION_TABLE g_tables[2];
static uint8_t g_signature[8];
static unsigned int g_maps;
static unsigned int g_unmaps;
static unsigned int g_scans;
static int g_fail;
SystemMachine_t* GetMachine(void) { return &g_machine; }
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
void* AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS address, ACPI_SIZE length)
{
    assert(address == 0x12345000 || address == 0x56789000);
    assert(length == sizeof(g_signature));
    g_maps++;
    return g_fail ? NULL : g_signature;
}
void AcpiOsUnmapMemory(void* address, ACPI_SIZE length)
{
    assert(address == g_signature && length == sizeof(g_signature));
    g_unmaps++;
}
ACPI_STATUS AcpiFindRootPointer(ACPI_PHYSICAL_ADDRESS* address)
{
    g_scans++;
    *address = 0xeeee0;
    return AE_OK;
}
int main(void)
{
    g_tables[0].VendorGuid = (__EFI_GUID)ACPI_TABLE_GUID;
    g_tables[0].VendorTable = (void*)0x12345000;
    g_tables[1].VendorGuid = (__EFI_GUID)EFI_ACPI_20_TABLE_GUID;
    g_tables[1].VendorTable = (void*)0x56789000;
    g_machine.BootInformation.ConfigurationTable = (uintptr_t)g_tables;
    g_machine.BootInformation.ConfigurationTableCount = 2;
    memcpy(g_signature, ACPI_SIG_RSDP, sizeof(g_signature));
    assert(AcpiOsGetRootPointer() == 0x56789000); // ACPI 2 preferred.
    assert(g_maps == 1 && g_unmaps == 1 && !g_scans);
    g_machine.BootInformation.ConfigurationTableCount = 1;
    assert(AcpiOsGetRootPointer() == 0x12345000);
    g_signature[0] = 0;
#ifdef TEST_ARM64
    assert(!AcpiOsGetRootPointer() && !g_scans);
    g_fail = 1;
    assert(!AcpiOsGetRootPointer() && !g_scans);
    g_machine.BootInformation.ConfigurationTable = 0;
    assert(!AcpiOsGetRootPointer() && !g_scans);
#else
    assert(AcpiOsGetRootPointer() == 0xeeee0 && g_scans == 1);
#endif
    puts("UEFI RSDP selection and architecture fallback: PASS");
    return 0;
}
