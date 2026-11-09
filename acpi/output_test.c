#include <arch/output.h>
#include <assert.h>
#include <stdio.h>
#include "../../kernel/arch/aarch64/common/private.h"
#include "../../kernel/arch/aarch64/common/device.h"

// Replace only the CPU yield/barrier instructions in this host build. The
// production polling, register offsets, publication and failure logic run
// unchanged against an array standing in for volatile PL011 registers.
#define __asm__
#define volatile(...)
#include "../../kernel/arch/aarch64/common/output.c"
#undef volatile
#undef __asm__

struct Arm64Platform g_arm64Platform;
static uint32_t g_registers[ARM64_UART_REGION_MINIMUM / 4];
static unsigned int g_maps;
static int g_fail;
uintptr_t Arm64MapDevice(uint64_t physical, uint64_t length)
{
    assert(physical == 0x9000000 && length == ARM64_UART_REGION_MINIMUM);
    g_maps++;
    return g_fail ? 0 : (uintptr_t)g_registers;
}
int main(void)
{
    assert(SerialPortInitialize() == OS_ENOENT && !g_maps);
    g_arm64Platform.Uart = 0x9000000;
    g_arm64Platform.UartLength = ARM64_UART_REGION_MINIMUM;
    g_fail = 1;
    assert(SerialPortInitialize() == OS_EOOM);
    g_fail = 0;
    assert(SerialPortInitialize() == OS_EOK);
    assert(VideoGetTerminal()->AvailableOutputs & VIDEO_UART);
    SerialPutCharacter('A');
    assert(g_registers[0] == 'A');
    g_registers[ARM64_UART_FLAGS / 4] = ARM64_UART_TX_FULL;
    SerialPutCharacter('\n');
    assert(g_uartStalled && g_registers[0] == 'A');
    // Clearing TXFF after a failure must not make later log bytes pay another
    // polling delay or unexpectedly restart a console disabled for this boot.
    g_registers[ARM64_UART_FLAGS / 4] = 0;
    SerialPutCharacter('B');
    assert(g_registers[0] == 'A');
    puts("PL011 bounded polling: PASS");
    return 0;
}
