/** Verify writable registers and per-CPU mode setup during APIC initialization. */
#ifndef APIC_INIT_SOURCE
#define APIC_INIT_SOURCE "../kernel/arch/x86/interrupts/apic/apicinit.c"
#endif
#include APIC_INIT_SOURCE
#include <stdio.h>
#include <stdlib.h>

static int          g_x2Selected;
static int          g_localX2Enabled;
static unsigned int g_enableCalls;
static unsigned int g_ldrWrites;
static unsigned int g_dfrWrites;
static unsigned int g_priorityWrites;
static unsigned int g_timerStarts;

static void
__Check(
    _In_ int condition,
    _In_ int line)
{
    if (!condition) {
        printf("APIC initialization check failed at line %d\n", line);
        exit(1);
    }
}

#define CHECK(condition) __Check((condition), __LINE__)

static void
__CheckLocalAccess(void)
{
    // Selecting MSR access globally does not enable it on a secondary CPU.
    CHECK(!g_x2Selected || g_localX2Enabled);
}

int
ApicIsX2Apic(void)
{
    return g_x2Selected;
}

int
ApicEnableX2Apic(void)
{
    CHECK(g_x2Selected);
    g_localX2Enabled = 1;
    g_enableCalls++;
    return 1;
}

uint32_t
ApicReadLocal(
    _In_ size_t reg)
{
    __CheckLocalAccess();
    return 0;
}

void
ApicWriteLocal(
    _In_ size_t   reg,
    _In_ uint32_t value)
{
    __CheckLocalAccess();
    if (reg == APIC_LOGICAL_DEST) {
        CHECK(!g_x2Selected);
        CHECK(value == APIC_DESTINATION(3));
        g_ldrWrites++;
    }
    if (reg == APIC_DEST_FORMAT) {
        CHECK(!g_x2Selected);
        CHECK(value == 0xffffffffU);
        g_dfrWrites++;
    }
}

int
ApicGetMaxLvt(void)
{
    __CheckLocalAccess();
    return 6;
}

int
ApicIsIntegrated(void)
{
    return 1;
}

uint32_t
ApicComputeLogicalDestination(
    _In_ uuid_t coreId)
{
    CHECK(!g_x2Selected);
    return 3;
}

void
ApicSetTaskPriority(
    _In_ uint32_t priority)
{
    CHECK(priority == 0);
    g_priorityWrites++;
}

void
ApicSendEoi(
    _In_ int      gsi,
    _In_ uint32_t vector)
{
}

void
ApicTimerStart(
    _In_ size_t quantumValue)
{
    CHECK(quantumValue == 0);
    g_timerStarts++;
}

uuid_t
ArchGetProcessorCoreId(void)
{
    __CheckLocalAccess();
    return 1;
}

uuid_t
CpuCoreId(
    _In_ SystemCpuCore_t* core)
{
    return 0;
}

SystemMachine_t*
GetMachine(void)
{
    static SystemMachine_t machine;
    return &machine;
}

ACPI_STATUS
AcpiGetTable(
    _In_  char*               signature,
    _In_  UINT32              instance,
    _Out_ ACPI_TABLE_HEADER** table)
{
    return AE_NOT_FOUND;
}

int
AcpiAvailable(void)
{
    return ACPI_AVAILABLE;
}

void
AcpiPutTable(
    _In_ ACPI_TABLE_HEADER* table)
{
}

int
AcpiGetPolarityMode(
    _In_ uint16_t flags,
    _In_ int      defaultValue)
{
    return defaultValue;
}

int
AcpiGetTriggerMode(
    _In_ uint16_t flags,
    _In_ int      defaultValue)
{
    return defaultValue;
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
}

static void
__Reset(
    _In_ int x2Selected,
    _In_ int localEnabled)
{
    g_x2Selected = x2Selected;
    g_localX2Enabled = localEnabled;
    g_enableCalls = 0;
    g_ldrWrites = 0;
    g_dfrWrites = 0;
    g_priorityWrites = 0;
    g_timerStarts = 0;
}

int
main(void)
{
    int x2Selected;

    for (x2Selected = 0; x2Selected <= 1; x2Selected++) {
        __Reset(x2Selected, x2Selected);
        __PrepareApic(0);
        CHECK(g_ldrWrites == !x2Selected && g_dfrWrites == !x2Selected);
        CHECK(g_priorityWrites == 1 && g_enableCalls == 0);

        __Reset(x2Selected, 0);
        ApicInitializeForApplicationCore();
        CHECK(g_enableCalls == x2Selected);
        CHECK(g_ldrWrites == !x2Selected && g_dfrWrites == !x2Selected);
        CHECK(g_priorityWrites == 1 && g_timerStarts == 1);
    }
    puts("APIC initialization: xAPIC register setup and x2APIC per-CPU enable passed");
    return 0;
}
