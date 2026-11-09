#include <acpi.h>
#include <arch/output.h>
#include <log.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/arch/aarch64/common/private.h"
#include "../../kernel/arch/aarch64/acpi/spcr.h"

struct Arm64Platform g_arm64Platform;
static struct {
    ACPI_TABLE_SPCR Table;
    uint8_t Extension[16];
} g_fixture;
static unsigned int g_queries;
static unsigned int g_releases;
static unsigned int g_serialCalls;
static unsigned int g_renders;
static ACPI_STATUS g_tableStatus;
static oserr_t g_serialStatus;

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
void LogSetRenderMode(int enabled) { assert(enabled); g_renders++; }
oserr_t SerialPortInitialize(void)
{
    assert(g_arm64Platform.Uart == g_fixture.Table.SerialPort.Address);
    assert(g_arm64Platform.UartLength == ARM64_UART_REGION_MINIMUM);
    assert(g_releases == 1); // No dangling table reference during device setup.
    g_serialCalls++;
    return g_serialStatus;
}
ACPI_STATUS AcpiGetTable(char* signature, UINT32 instance, ACPI_TABLE_HEADER** output)
{
    assert(!memcmp(signature, "SPCR", 4) && instance == 1);
    g_queries++;
    *output = &g_fixture.Table.Header;
    return g_tableStatus;
}
void AcpiPutTable(ACPI_TABLE_HEADER* header)
{
    assert(header == &g_fixture.Table.Header);
    g_releases++;
}
static void __Checksum(void)
{
    uint8_t sum = 0;
    size_t i;
    g_fixture.Table.Header.Checksum = 0;
    for (i = 0; i < g_fixture.Table.Header.Length; i++) {
        sum += ((uint8_t*)&g_fixture)[i];
    }
    g_fixture.Table.Header.Checksum = (uint8_t)-sum;
}
static void __Reset(void)
{
    memset(&g_fixture, 0, sizeof(g_fixture));
    memset(&g_arm64Platform, 0, sizeof(g_arm64Platform));
    g_queries = g_releases = g_serialCalls = g_renders = 0;
    g_tableStatus = AE_OK;
    g_serialStatus = OS_EOK;
    memcpy(g_fixture.Table.Header.Signature, "SPCR", 4);
    g_fixture.Table.Header.Length = sizeof(ACPI_TABLE_SPCR);
    g_fixture.Table.Header.Revision = 2;
    g_fixture.Table.InterfaceType = ACPI_DBG2_ARM_PL011;
    g_fixture.Table.SerialPort.SpaceId = ACPI_ADR_SPACE_SYSTEM_MEMORY;
    g_fixture.Table.SerialPort.BitWidth = 32;
    g_fixture.Table.SerialPort.AccessWidth = 3;
    g_fixture.Table.SerialPort.Address = 0x9000000;
    g_fixture.Table.FlowControl = 2;
    __Checksum();
}
static void __Rejected(void)
{
    Arm64AcpiInitializeConsole();
    assert(g_queries == 1 && g_releases == 1);
    assert(!g_serialCalls && !g_renders && !g_arm64Platform.Uart);
}
int main(void)
{
    __Reset();
    Arm64AcpiInitializeConsole();
    assert(g_serialCalls == 1 && g_renders == 1 && g_releases == 1);
    __Reset();
    g_fixture.Table.Header.Revision = 4;
    g_fixture.Table.Header.Length = sizeof(g_fixture);
    g_fixture.Table.SerialPort.AccessWidth = 0;
    g_fixture.Table.SerialPort.BitWidth = 0;
    __Checksum();
    Arm64AcpiInitializeConsole();
    assert(g_serialCalls == 1 && g_renders == 1);
    __Reset(); g_tableStatus = AE_NOT_FOUND;
    Arm64AcpiInitializeConsole(); assert(!g_releases && !g_serialCalls);
    __Reset(); g_fixture.Table.Header.Length = sizeof(ACPI_TABLE_HEADER); __Rejected();
    __Reset(); g_fixture.Table.Header.Revision = 1; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.Header.Checksum++; __Rejected();
    __Reset(); g_fixture.Table.SerialPort.Address = 0; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.InterfaceType = 0; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.SerialPort.SpaceId = ACPI_ADR_SPACE_SYSTEM_IO; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.SerialPort.BitOffset = 1; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.SerialPort.AccessWidth = 1; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.SerialPort.BitWidth = 8; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.SerialPort.Address++; __Checksum(); __Rejected();
    __Reset(); g_fixture.Table.FlowControl = 4; __Checksum(); __Rejected();
    __Reset(); g_serialStatus = OS_EOOM;
    Arm64AcpiInitializeConsole();
    assert(g_serialCalls == 1 && !g_renders && !g_arm64Platform.Uart && !g_arm64Platform.UartLength);
    puts("SPCR validation and console publication: PASS");
    return 0;
}
