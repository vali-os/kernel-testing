// Exercise the production readers and publication hook. ACPICA returns bounded
// byte buffers so ASan catches reads beyond a declared table prefix or entry.
#include <arch/acpi.h>
#include <component/cpu.h>
#include <component/ic.h>
#include <machine.h>
#include "../../kernel/components/cpu_private.h"
#include <log.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/arch/aarch64/acpi/tables.h"

static SystemMachine_t g_machine;
struct Arm64Platform g_arm64Platform;
static SystemCpuCore_t g_cores[ARM64_CPU_COUNT];
static ACPI_TABLE_FADT g_fadt;
static ACPI_TABLE_GTDT g_gtdt;
static uint8_t g_madt[32768];
static unsigned int g_registered;
static unsigned int g_controllers;
static unsigned int g_liveTables;
static unsigned int g_consoleCalls;
static unsigned int g_uma;
static oserr_t g_controllerStatus;
static const char* g_missing;
static int g_allocationsBeforeFailure = -1;
static unsigned int g_allocations;

void*
kmalloc(
    size_t size)
{
    void* memory;

    if (g_allocationsBeforeFailure == 0) {
        return NULL;
    }
    if (g_allocationsBeforeFailure > 0) {
        g_allocationsBeforeFailure--;
    }
    memory = malloc(size);
    if (memory) {
        g_allocations++;
    }
    return memory;
}

void
kfree(
    void* pointer)
{
    if (pointer) {
        assert(g_allocations);
        g_allocations--;
    }
    free(pointer);
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}
SystemCpuCore_t*
GetProcessorCore(
    _In_ uuid_t id)
{
    assert(id < ARM64_CPU_COUNT);
    return &g_cores[id];
}
PlatformCpuCoreBlock_t*
CpuCorePlatformBlock(
    _In_ SystemCpuCore_t* core)
{
    return &core->PlatformData;
}
void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}
void
Arm64AcpiInitializeConsole(void)
{
    g_consoleCalls++;
}
void
SetMachineUmaMode(void)
{
    g_uma++;
}
void
CpuCoreRegister(
    _In_ SystemCpu_t*     cpu,
    _In_ uuid_t           id,
    _In_ SystemCpuState_t state,
    _In_ int              external)
{
    assert(cpu == &g_machine.Processor && cpu->NumberOfCores > 1);
    assert(id == ++g_registered && state == CpuStateShutdown && !external);
    assert(!g_liveTables && g_controllers == 1);
}
oserr_t
CreateInterruptController(
    _In_ uuid_t    id,
    _In_ int       base,
    _In_ int       count,
    _In_ uintptr_t address)
{
    assert(id == 42 && base == 0 && count == INTERRUPT_PHYSICAL_END && address == 0x8000000);
    assert(!g_registered && !g_liveTables);
    g_controllers++;
    return g_controllerStatus;
}
ACPI_STATUS
AcpiGetTable(
    _In_  char*               signature,
    _In_  UINT32              instance,
    _Out_ ACPI_TABLE_HEADER** result)
{
    ACPI_TABLE_HEADER* source = NULL;
    size_t             size;

    assert(instance == 1);
    if (g_missing && !memcmp(signature, g_missing, 4)) {
        return AE_NOT_FOUND;
    }
    if (!memcmp(signature, ACPI_SIG_FADT, 4)) {
        source = &g_fadt.Header;
    } else if (!memcmp(signature, ACPI_SIG_GTDT, 4)) {
        source = &g_gtdt.Header;
    } else if (!memcmp(signature, ACPI_SIG_MADT, 4)) {
        source = (void*)g_madt;
    } else {
        assert(!"unexpected table lookup");
    }
    // Keep only the header when Length is malformed below sizeof(header).
    // Otherwise give the reader exactly the bytes ACPICA would have mapped.
    size = source->Length < sizeof(*source) ? sizeof(*source) : source->Length;
    *result = malloc(size);
    assert(*result);
    memcpy(*result, source, size);
    g_liveTables++;
    return AE_OK;
}
void
AcpiPutTable(
    _In_ ACPI_TABLE_HEADER* table)
{
    assert(g_liveTables);
    g_liveTables--;
    free(table);
}
static void
__Checksum(
    _InOut_ ACPI_TABLE_HEADER* header)
{
    uint8_t sum = 0;
    uint32_t i;
    header->Checksum = 0;
    for (i = 0; i < header->Length; i++) {
        sum = (uint8_t)(sum + ((uint8_t*)header)[i]);
    }
    header->Checksum = (uint8_t)-sum;
}
static ACPI_MADT_GENERIC_INTERRUPT*
__Cpu(
    _In_ unsigned int index)
{
    return (void*)(g_madt + sizeof(ACPI_TABLE_MADT) + index * sizeof(ACPI_MADT_GENERIC_INTERRUPT));
}
static ACPI_MADT_GENERIC_DISTRIBUTOR*
__Gic(void)
{
    return (void*)__Cpu(3);
}
static void
__Reset(void)
{
    ACPI_TABLE_MADT* madt = (void*)g_madt;
    unsigned int i;
    assert(!g_liveTables);
    memset(&g_machine, 0, sizeof(g_machine));
    memset(g_cores, 0, sizeof(g_cores));
    kfree(g_arm64Platform.Gic.Regions);
    assert(!g_allocations);
    g_allocationsBeforeFailure = -1;
    memset(&g_arm64Platform, 0, sizeof(g_arm64Platform));
    memset(&g_fadt, 0, sizeof(g_fadt));
    memset(&g_gtdt, 0, sizeof(g_gtdt));
    memset(g_madt, 0, sizeof(g_madt));
    g_registered = g_controllers = g_consoleCalls = g_uma = 0;
    g_controllerStatus = OS_EOK;
    g_missing = NULL;
    g_machine.Processor.Cores = &g_cores[0];
    g_machine.Processor.NumberOfCores = 1;
    g_cores[0].PlatformData.Affinity = 0x100;
    g_arm64Platform.Uart = 0x9000000;
    g_arm64Platform.UartLength = 0x1c;
    memcpy(g_fadt.Header.Signature, ACPI_SIG_FADT, 4);
    g_fadt.Header.Length = sizeof(g_fadt);
    g_fadt.Header.Revision = 6;
    g_fadt.Flags = ACPI_FADT_HW_REDUCED;
    g_fadt.ArmBootFlags = ACPI_FADT_PSCI_COMPLIANT;
    __Checksum(&g_fadt.Header);
    memcpy(g_gtdt.Header.Signature, ACPI_SIG_GTDT, 4);
    g_gtdt.Header.Length = sizeof(g_gtdt);
    g_gtdt.Header.Revision = 2;
    g_gtdt.NonSecureEl1Interrupt = 30;
    __Checksum(&g_gtdt.Header);
    memcpy(madt->Header.Signature, ACPI_SIG_MADT, 4);
    madt->Header.Length = sizeof(*madt) + 3 * sizeof(*__Cpu(0)) + sizeof(*__Gic());
    madt->Header.Revision = 5;
    for (i = 0; i < 3; i++) {
        __Cpu(i)->Header.Type = ACPI_MADT_TYPE_GENERIC_INTERRUPT;
        __Cpu(i)->Header.Length = sizeof(*__Cpu(i));
        __Cpu(i)->Flags = ACPI_MADT_ENABLED;
        __Cpu(i)->CpuInterfaceNumber = i;
        __Cpu(i)->Uid = 50 + i;
        __Cpu(i)->ArmMpidr = 0x101 - i; // Boot CPU is second; affinities are not logical IDs.
        __Cpu(i)->BaseAddress = 0x8010000;
    }
    __Gic()->Header.Type = ACPI_MADT_TYPE_GENERIC_DISTRIBUTOR;
    __Gic()->Header.Length = sizeof(*__Gic());
    __Gic()->GicId = 42;
    __Gic()->Version = ACPI_MADT_GIC_VERSION_V2;
    __Gic()->BaseAddress = 0x8000000;
    __Checksum(&madt->Header);
}
// Change the number of enabled records without changing the controller. This
// exercises the driver's eight-CPU limit independently of duplicate detection.
static void
__SetCpuCount(
    _In_ unsigned int count)
{
    ACPI_TABLE_HEADER*            header = (void*)g_madt;
    ACPI_MADT_GENERIC_DISTRIBUTOR distributor = *__Gic();
    unsigned int                  i;

    for (i = 0; i < count; i++) {
        memset(__Cpu(i), 0, sizeof(*__Cpu(i)));
        __Cpu(i)->Header.Type = ACPI_MADT_TYPE_GENERIC_INTERRUPT;
        __Cpu(i)->Header.Length = sizeof(*__Cpu(i));
        __Cpu(i)->Flags = ACPI_MADT_ENABLED;
        __Cpu(i)->CpuInterfaceNumber = i;
        __Cpu(i)->Uid = 50 + i;
        __Cpu(i)->ArmMpidr = 0x100 + i;
        __Cpu(i)->BaseAddress = 0x8010000;
    }
    memcpy(__Cpu(count), &distributor, sizeof(distributor));
    header->Length = sizeof(ACPI_TABLE_MADT) + count * sizeof(*__Cpu(0)) + sizeof(distributor);
    __Checksum(header);
}

static void
__Reject(void)
{
    assert(ArchAcpiInitialize() != OS_EOK);
    assert(g_consoleCalls == 1 && !g_liveTables);
    assert(!g_registered && !g_controllers && !g_uma && !g_allocations);
    assert(g_machine.Processor.NumberOfCores == 1);
    assert(g_cores[0].PlatformData.Affinity == 0x100);
    assert(!g_arm64Platform.Gic.Distributor && !g_arm64Platform.TimerInterrupt);
    assert(g_arm64Platform.Uart == 0x9000000);
}
int
main(void)
{
    ACPI_TABLE_HEADER* madt = (void*)g_madt;
    const char* required[] = {ACPI_SIG_FADT, ACPI_SIG_MADT, ACPI_SIG_GTDT};
    unsigned int i;

    __Reset();
    assert(ArchAcpiInitialize() == OS_EOK);
    assert(g_registered == 2 && g_uma == 1 && !g_liveTables);
    assert(g_cores[0].PlatformData.Affinity == 0x100);
    assert(g_cores[1].PlatformData.Affinity == 0x101);
    assert(g_cores[2].PlatformData.Affinity == 0xff);
    assert(g_cores[2].PlatformData.EnableMethod == ARM64_CPU_ENABLE_PSCI);
    assert(g_arm64Platform.PsciConduit == VBOOT_PSCI_SMC);
    assert(g_arm64Platform.Uart == 0x9000000);
    assert(g_arm64Platform.TimerInterrupt == 30);
    assert(g_arm64Platform.TimerFlags == (INTERRUPT_ACPICONFORM_PRESENT | INTERRUPT_ACPICONFORM_TRIGGERMODE));

    __Reset();
    g_fadt.ArmBootFlags |= ACPI_FADT_PSCI_USE_HVC;
    g_gtdt.NonSecureEl1Flags = ACPI_GTDT_INTERRUPT_POLARITY | ACPI_GTDT_ALWAYS_ON;
    __Cpu(0)->Flags = 8; // Online-capable without Enabled: do not start it.
    __Checksum(&g_fadt.Header);
    __Checksum(&g_gtdt.Header);
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK && g_registered == 1);
    assert(g_arm64Platform.PsciConduit == VBOOT_PSCI_HVC);
    assert(g_arm64Platform.TimerFlags & INTERRUPT_ACPICONFORM_POLARITY);
    for (i = 0; i < 3; i++) {

        __Reset();
        g_missing = required[i];
        __Reject();
    }

    __Reset();
    g_fadt.Header.Length = offsetof(ACPI_TABLE_FADT, ArmBootFlags);
    __Reject();

    __Reset();
    g_fadt.Header.Checksum++;
    __Reject();

    __Reset();
    g_fadt.Header.Revision = 5;
    g_fadt.MinorRevision = 0;
    __Checksum(&g_fadt.Header);
    __Reject();

    __Reset();
    g_fadt.Flags = 0;
    __Checksum(&g_fadt.Header);
    __Reject();

    __Reset();
    g_fadt.ArmBootFlags = 0;
    __Checksum(&g_fadt.Header);
    __Reject();

    __Reset();
    g_gtdt.Header.Length = offsetof(ACPI_TABLE_GTDT, NonSecureEl1Flags);
    __Reject();

    __Reset();
    g_gtdt.Header.Checksum++;
    __Reject();

    __Reset();
    g_gtdt.NonSecureEl1Interrupt = 15;
    __Checksum(&g_gtdt.Header);
    __Reject();

    __Reset();
    g_gtdt.NonSecureEl1Interrupt = 32;
    __Checksum(&g_gtdt.Header);
    __Reject();

    __Reset();
    g_gtdt.NonSecureEl1Flags = ACPI_GTDT_INTERRUPT_MODE;
    __Checksum(&g_gtdt.Header);
    __Reject();

    __Reset();
    madt->Length = sizeof(ACPI_TABLE_MADT) - 1;
    __Reject();

    __Reset();
    madt->Checksum++;
    __Reject();

    __Reset();
    __Cpu(0)->Header.Length = 0;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Cpu(0)->Header.Length = 1;
    __Checksum(madt);
    __Reject();

    __Reset();
    madt->Length = sizeof(ACPI_TABLE_MADT) + sizeof(*__Cpu(0)) - 1;
    __Checksum(madt);
    __Reject();

    __Reset();
    madt->Length++;
    __Checksum(madt);
    __Reject();
    // Truncated subtable header.

    __Reset();
    __Cpu(0)->ArmMpidr = __Cpu(1)->ArmMpidr;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Cpu(0)->Uid = __Cpu(1)->Uid;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Cpu(0)->CpuInterfaceNumber = 8;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Cpu(0)->ArmMpidr |= 1ULL << 63;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Cpu(1)->Flags = 0;
    __Checksum(madt);
    __Reject();
    // Boot CPU missing.

    __Reset();
    __Cpu(0)->BaseAddress += 4096;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->Version = 3;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->Version = 0;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->BaseAddress++;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->GlobalIrqBase = 32;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->BaseAddress = 0x8010000;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->Header.Type = 0x7f;
    __Checksum(madt);
    __Reject();

    __Reset();
    g_controllerStatus = OS_EOOM;
    assert(ArchAcpiInitialize() == OS_EOOM);
    assert(g_controllers == 1 && !g_registered && !g_uma && !g_arm64Platform.Gic.Distributor);
    assert(!g_liveTables);
    // Maximum supported topology succeeds; one more CPU is rejected without
    // registering a controller or publishing even the earlier valid records.
    __Reset();
    __SetCpuCount(8);
    assert(ArchAcpiInitialize() == OS_EOK && g_registered == 7);
    __Reset();
    __SetCpuCount(9);
    __Reject();

    __Reset();
    __Cpu(0)->CpuInterfaceNumber = __Cpu(1)->CpuInterfaceNumber;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->Version = 4;
    __Checksum(madt);
    __Reject();

    __Reset();
    memcpy(g_madt + madt->Length, __Gic(), sizeof(*__Gic()));
    madt->Length += sizeof(*__Gic());
    __Checksum(madt);
    __Reject();

    // Unknown records and a longer GICC are allowed when their framing is
    // complete. Known fields retain the same offsets in later extensions.
    __Reset();
    g_madt[madt->Length] = 0x7f;
    g_madt[madt->Length + 1] = 4;
    madt->Length += 4;
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK);
    __Reset();
    memmove((uint8_t*)__Cpu(1) + 4, __Cpu(1), madt->Length - sizeof(ACPI_TABLE_MADT) - sizeof(*__Cpu(0)));
    memset(__Cpu(1), 0, 4);
    __Cpu(0)->Header.Length += 4;
    madt->Length += 4;
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK);
    __Reset();
    g_allocationsBeforeFailure = 0;
    __Reject(); // Failure allocating CPU records publishes nothing.

    // GICv3 does not use legacy GICC interface numbers or BaseAddress. A
    // per-CPU redistributor description must match a unique complete frame.
    __Reset();
    __Gic()->Version = 3;
    for (i = 0; i < 3; i++) {
        __Cpu(i)->CpuInterfaceNumber = 0;
        __Cpu(i)->BaseAddress = 0;
        __Cpu(i)->GicrBaseAddress = 0x80a0000 + i * 0x20000;
    }
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK);
    assert(g_arm64Platform.Gic.Version == 3 && g_registered == 2);
    assert(g_cores[0].PlatformData.Gic.RedistributorPhysical == 0x80c0000);

    // Multiple GICR records override even invalid per-CPU fallback addresses.
    // This also checks that regions can follow all the CPU records.
    __Reset();
    __Gic()->Version = 3;
    for (i = 0; i < 2; i++) {
        ACPI_MADT_GENERIC_REDISTRIBUTOR region = {0};
        region.Header.Type = ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR;
        region.Header.Length = sizeof(region);
        region.BaseAddress = 0x80a0000 + i * 0x40000;
        region.Length = 0x40000;
        memcpy(g_madt + madt->Length, &region, sizeof(region));
        madt->Length += sizeof(region);
    }
    __Cpu(0)->GicrBaseAddress = 1;
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK);
    assert(g_arm64Platform.Gic.RegionCount == 2);
    assert(g_arm64Platform.Gic.Regions[1].Base == 0x80e0000);
    assert(!g_cores[1].PlatformData.Gic.RedistributorPhysical);

    // A failed GICR allocation releases the temporary CPU array as well.
    __Reset();
    __Gic()->Version = 3;
    {
        ACPI_MADT_GENERIC_REDISTRIBUTOR region = {0};
        region.Header.Type = ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR;
        region.Header.Length = sizeof(region);
        region.BaseAddress = 0x80a0000;
        region.Length = 0x60000;
        memcpy(g_madt + madt->Length, &region, sizeof(region));
        madt->Length += sizeof(region);
    }
    __Checksum(madt);
    g_allocationsBeforeFailure = 1;
    __Reject();

    // A controller beyond GICv2's target-mask capacity uses dynamic CPU records.
    __Reset();
    __SetCpuCount(16);
    ((ACPI_MADT_GENERIC_DISTRIBUTOR*)__Cpu(16))->Version = 3;
    for (i = 0; i < 16; i++) {
        __Cpu(i)->CpuInterfaceNumber = 0;
        __Cpu(i)->BaseAddress = 0;
        __Cpu(i)->GicrBaseAddress = 0x80a0000 + i * 0x20000;
    }
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK && g_registered == 15);

    __Reset();
    __SetCpuCount(ARM64_CPU_COUNT);
    ((ACPI_MADT_GENERIC_DISTRIBUTOR*)__Cpu(ARM64_CPU_COUNT))->Version = 3;
    for (i = 0; i < ARM64_CPU_COUNT; i++) {
        __Cpu(i)->CpuInterfaceNumber = 0;
        __Cpu(i)->BaseAddress = 0;
        __Cpu(i)->GicrBaseAddress = 0x80a0000 + i * 0x20000;
    }
    __Checksum(madt);
    assert(ArchAcpiInitialize() == OS_EOK && g_registered == ARM64_CPU_COUNT - 1);

    __Reset();
    __SetCpuCount(ARM64_CPU_COUNT + 1);
    ((ACPI_MADT_GENERIC_DISTRIBUTOR*)__Cpu(ARM64_CPU_COUNT + 1))->Version = 3;
    __Checksum(madt);
    __Reject();

    __Reset();
    __Gic()->Version = 3;
    for (i = 0; i < 3; i++) {
        __Cpu(i)->GicrBaseAddress = 0x80a0000;
    }
    __Checksum(madt);
    __Reject(); // Duplicate physical fallback frames.

    __Reset();
    __Gic()->Version = 3;
    for (i = 0; i < 3; i++) {
        __Cpu(i)->GicrBaseAddress = (1ULL << 48) - 0x10000;
    }
    __Checksum(madt);
    __Reject(); // Frame reaches beyond the supported physical width.

    __Reset();
    __Gic()->Version = 3;
    {
        ACPI_MADT_GENERIC_REDISTRIBUTOR region = {0};
        region.Header.Type = ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR;
        region.Header.Length = sizeof(region);
        region.BaseAddress = 0x8000000;
        region.Length = 0x20000;
        memcpy(g_madt + madt->Length, &region, sizeof(region));
        madt->Length += sizeof(region);
    }
    __Checksum(madt);
    __Reject(); // Redistributor overlaps distributor.

    __Reset();
    assert(!g_liveTables);
    puts("ARM64 MADT/GTDT/FADT validation and publication: PASS");
    return 0;
}
