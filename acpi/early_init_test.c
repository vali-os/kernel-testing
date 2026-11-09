// Exercise the production table initialization and architecture dispatch. The
// ACPICA boundary is mocked so setup failures and table references are visible.
#include <acpiinterface.h>
#include <arch/acpi.h>
#include <machine.h>
#include <log.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void*
kmalloc(
    size_t size)
{
    return malloc(size);
}

void
kfree(
    void* pointer)
{
    free(pointer);
}

static ACPI_STATUS g_subsystemStatus;
static ACPI_STATUS g_tablesStatus;
static unsigned int g_tableInitializations;
static unsigned int g_tableQueries;
static unsigned int g_tableReleases;
static unsigned int g_hookCalls;
static oserr_t g_hookStatus;
static int g_hasMadt;
static SystemMachine_t g_machine;
static ACPI_TABLE_HEADER g_ecdt;
static ACPI_TABLE_HEADER g_sbst;
static unsigned int g_registeredCores;
static unsigned int g_registeredControllers;
static unsigned int g_registeredOverrides;

// A boot CPU, two secondary CPUs with different APIC encodings, one IO-APIC
// and one source override cover the existing x86 discovery work moved here.
struct __attribute__((packed)) __MadtFixture {
    ACPI_TABLE_MADT Header;
    ACPI_MADT_LOCAL_APIC Boot;
    ACPI_MADT_LOCAL_APIC Secondary;
    ACPI_MADT_LOCAL_X2APIC Extended;
    ACPI_MADT_IO_APIC Controller;
    ACPI_MADT_INTERRUPT_OVERRIDE Override;
};
static struct __MadtFixture g_madt;

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)level;
    (void)format;
}

ACPI_STATUS
AcpiInitializeSubsystem(void)
{
    // Availability must not survive a failed setup attempt. Each test resets
    // the mocked subsystem; production calls this once during machine startup.
    assert(AcpiAvailable() == ACPI_NOT_AVAILABLE);
    return g_subsystemStatus;
}

ACPI_STATUS
AcpiInitializeTables(
    ACPI_TABLE_DESC* initialStorage,
    UINT32          initialCount,
    BOOLEAN         allowResize)
{
    assert(initialStorage == NULL);
    assert(initialCount == ACPI_MAX_INIT_TABLES && allowResize);
    g_tableInitializations++;
    assert(AcpiAvailable() == ACPI_NOT_AVAILABLE);
    return g_tablesStatus;
}

ACPI_STATUS
AcpiGetTable(
    char*               signature,
    UINT32              instance,
    ACPI_TABLE_HEADER** output)
{
    (void)instance;
    assert(g_subsystemStatus == AE_OK && g_tablesStatus == AE_OK);
    assert(AcpiAvailable() == ACPI_AVAILABLE);
    g_tableQueries++;
    if (!memcmp(signature, ACPI_SIG_MADT, 4) && g_hasMadt) {
        *output = &g_madt.Header.Header;
    } else if (!memcmp(signature, ACPI_SIG_ECDT, 4)) {
        *output = &g_ecdt;
    } else if (!memcmp(signature, ACPI_SIG_SBST, 4)) {
        *output = &g_sbst;
    } else {
        return AE_NOT_FOUND;
    }
    return AE_OK;
}

void
AcpiPutTable(
    ACPI_TABLE_HEADER* table)
{
    assert(table == &g_madt.Header.Header || table == &g_ecdt || table == &g_sbst);
    g_tableReleases++;
}

#if defined(TEST_ARM64_HOOK)
#include "../../kernel/arch/aarch64/acpi/tables.h"
struct Arm64Platform g_arm64Platform;
void Arm64AcpiInitializeConsole(void) {}
PlatformCpuCoreBlock_t* CpuCorePlatformBlock(SystemCpuCore_t* core) { (void)core; return NULL; }
SystemCpuCore_t* GetProcessorCore(uuid_t id) { (void)id; return NULL; }
void SetMachineUmaMode(void) {}
#endif

#if defined(TEST_SHARED_HOOK)
oserr_t
ArchAcpiInitialize(void)
{
    assert(g_tableInitializations == 1);
    assert(AcpiAvailable() == ACPI_AVAILABLE);
    g_hookCalls++;
    return g_hookStatus;
}
#endif

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

uuid_t
CpuCoreId(
    _In_ SystemCpuCore_t* core)
{
    (void)core;
    return 0;
}

void
CpuCoreRegister(
    _In_ SystemCpu_t*     cpu,
    _In_ uuid_t           coreId,
    _In_ SystemCpuState_t state,
    _In_ int              external)
{
    assert(cpu == &g_machine.Processor && state == CpuStateShutdown);
    assert(coreId == 1 || coreId == 256);
    assert(!external);
    g_registeredCores++;
}

oserr_t
CreateInterruptController(
    _In_ uuid_t    id,
    _In_ int       lineBase,
    _In_ int       count,
    _In_ uintptr_t address)
{
    assert(id == 2 && lineBase == 0 && count == 24 && address == 0xfec00000);
    g_registeredControllers++;
    return OS_EOK;
}

oserr_t
CreateInterruptOverrides(
    _In_ int count)
{
    assert(count == 1);
    return OS_EOK;
}

oserr_t
RegisterInterruptOverride(
    _In_ int          source,
    _In_ int          destination,
    _In_ unsigned int flags)
{
    assert(source == 0 && destination == 2 && flags == 0);
    g_registeredOverrides++;
    return OS_EOK;
}

oserr_t
CreateNumaDomain(
    _In_  uuid_t           id,
    _In_  int              cores,
    _In_  uintptr_t        start,
    _In_  uintptr_t        length,
    _Out_ SystemDomain_t** domain)
{
    (void)id;
    (void)cores;
    (void)start;
    (void)length;
    (void)domain;
    assert(!"UMA fixture must not create a NUMA domain");
    return OS_EUNKNOWN;
}

static void
__Reset(void)
{
    g_subsystemStatus = AE_OK;
    g_tablesStatus = AE_OK;
    g_tableInitializations = 0;
    g_tableQueries = 0;
    g_tableReleases = 0;
    g_hookCalls = 0;
    g_hookStatus = OS_EOK;
    g_hasMadt = 0;
    g_registeredCores = 0;
    g_registeredControllers = 0;
    g_registeredOverrides = 0;
    memset(&g_machine, 0, sizeof(g_machine));
    g_machine.Processor.NumberOfCores = 1;
}

int
main(void)
{
    __Reset();
    g_subsystemStatus = AE_ERROR;
    assert(AcpiInitializeEarly() != OS_EOK);
    assert(AcpiAvailable() == ACPI_NOT_AVAILABLE);
    assert(!g_tableInitializations && !g_hookCalls && !g_tableQueries);

    __Reset();
    g_tablesStatus = AE_ERROR;
    assert(AcpiInitializeEarly() != OS_EOK);
    assert(AcpiAvailable() == ACPI_NOT_AVAILABLE);
    assert(g_tableInitializations == 1 && !g_hookCalls && !g_tableQueries);

    __Reset();
#if defined(TEST_SHARED_HOOK)
    assert(AcpiInitializeEarly() == OS_EOK);
    assert(g_hookCalls == 1 && g_tableReleases == 2);
    __Reset();
    g_hookStatus = OS_ENOTSUPPORTED;
    assert(AcpiInitializeEarly() == OS_ENOTSUPPORTED);
    assert(g_hookCalls == 1 && g_tableReleases == 2);
#elif defined(TEST_X86_HOOK)
    assert(AcpiInitializeEarly() == OS_EUNKNOWN);
    assert(g_tableReleases == 2 && !g_registeredCores);
    __Reset();
    g_hasMadt = 1;
    g_madt.Header.Header.Length = sizeof(g_madt);
    g_madt.Boot.Header.Type = ACPI_MADT_TYPE_LOCAL_APIC;
    g_madt.Boot.Header.Length = sizeof(g_madt.Boot);
    g_madt.Boot.LapicFlags = 1;
    g_madt.Secondary.Header = g_madt.Boot.Header;
    g_madt.Secondary.Id = 1;
    g_madt.Secondary.LapicFlags = 1;
    g_madt.Extended.Header.Type = ACPI_MADT_TYPE_LOCAL_X2APIC;
    g_madt.Extended.Header.Length = sizeof(g_madt.Extended);
    g_madt.Extended.LocalApicId = 256;
    g_madt.Extended.LapicFlags = 1;
    g_madt.Controller.Header.Type = ACPI_MADT_TYPE_IO_APIC;
    g_madt.Controller.Header.Length = sizeof(g_madt.Controller);
    g_madt.Controller.Id = 2;
    g_madt.Controller.Address = 0xfec00000;
    g_madt.Override.Header.Type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE;
    g_madt.Override.Header.Length = sizeof(g_madt.Override);
    g_madt.Override.GlobalIrq = 2;
    assert(AcpiInitializeEarly() == OS_EOK);
    assert(g_machine.Processor.NumberOfCores == 3 && g_machine.NumberOfCores == 3);
    assert(g_registeredCores == 2 && g_registeredControllers == 1 && g_registeredOverrides == 1);
    assert(g_tableReleases == 3);
#else
    // Missing mandatory ARM tables must leave the platform unpublished.
    assert(AcpiInitializeEarly() == OS_ENOENT);
    assert(!g_registeredCores && !g_registeredControllers && !g_registeredOverrides);
    assert(g_tableReleases == 2);
#endif
    // Platform failure must leave successfully initialized table access usable.
    assert(AcpiAvailable() == ACPI_AVAILABLE);
    __Reset();
    g_subsystemStatus = AE_ERROR;
    assert(AcpiInitializeEarly() != OS_EOK);
    assert(AcpiAvailable() == ACPI_NOT_AVAILABLE);
    puts("ACPI table setup and architecture hook PASS");
    return 0;
}
