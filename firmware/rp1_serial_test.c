/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Host test for the RP1 UART setup in deviced (bus/rp1/serial.c), on a
 * bundled Raspberry Pi 5 tree. RP1 UART0 (serial@30000), its clock
 * controller and its GPIO block become children of a modelled RP1 bus.
 * Their register ranges are plain memory, and every write is logged.
 *
 * The cases check the clock and pin registers after DevicePrepare, the
 * order of the clock writes, the restore on DeviceStop and after a failed
 * write at every step, the clock rate answer, the process checks, the
 * interrupt source rule, and the refusal of firmware descriptions the setup
 * does not support. Nothing is written for a refused description.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <threads.h>
static int __TestSleep(const struct timespec*, struct timespec*);
#define thrd_sleep __TestSleep
#include "../../services/deviced/firmware/tree/decode-log.c"
#include "../../services/deviced/firmware/tree/reader.c"
#include "../../services/deviced/firmware/tree/tree.c"
#include "../../services/deviced/firmware/tree/resources.c"
#include "../../services/deviced/firmware/tree/shared-tree.c"
#include "../../services/deviced/firmware/bindings/ethernet.c"
#include "../../services/deviced/firmware/bindings/board-dma.c"
#include "../../services/deviced/firmware/bindings/dependencies.c"
#include "../../services/deviced/firmware/bindings/controllers.c"
#include "../../services/deviced/firmware/bindings/gpio.c"
#include "../../services/deviced/core/io.c"
#include "../../services/deviced/bus/rp1/ethernet.c"
#include "../../services/deviced/bus/rp1/serial.c"
// Rp1RuntimeChildHoldsSource is the real one, so the test reaches the source
// owner check through the same code as the service.
#include "../../services/deviced/bus/rp1/runtime-source.c"
#include "../../services/deviced/bus/rp1/child-resources.c"

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// Register ranges by DeviceIo_t ID: 1 for the clock block, 2 for the GPIO
// control range and 3 for the pad range.
#define CLOCKS 0
#define GPIO   1
#define PADS   2
#define RANGE_WORDS (0x10038 / 4)

// Process IDs of the driver and of another process.
#define DRIVER 1
#define OTHER  2

// RP1 child register addresses as the device tree gives them (bits 31:0 of
// the child address). They identify the blocks in the error messages only.
#define WRITE_LOG_SIZE 64

static uint32_t g_registers[3][RANGE_WORDS];
static uint32_t g_original[3][RANGE_WORDS];
static unsigned int g_ios;
static unsigned int g_writes;
static unsigned int g_failWrite;
static unsigned int g_failRelease;
// The write with this number reports success but does not land, so its
// readback differs.
static unsigned int g_dropWrite;
static struct {
    uint32_t Range;
    uint32_t Offset;
    uint32_t Value;
} g_writeLog[WRITE_LOG_SIZE];

// Set RP1_SERIAL_TEST_LOG in the environment to see deviced's log lines.
void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    va_list arguments;

    (void)level;
    if (getenv("RP1_SERIAL_TEST_LOG") == NULL) {
        return;
    }
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    printf("\n");
}

oserr_t
DeviceIoCreateMemory(
    _Out_ DeviceIo_t* io,
    _In_  uintptr_t   base,
    _In_  size_t      length)
{
    CHECK(base >= 1 && base <= 3);
    *io = (DeviceIo_t) { .Id = base, .Type = DeviceIoMemoryBased };
    io->Access.Memory.Length = length;
    g_ios++;
    return OS_EOK;
}

oserr_t
DeviceIoAcquire(
    _InOut_ DeviceIo_t* io)
{
    io->Access.Memory.VirtualBase = 1;
    return OS_EOK;
}

oserr_t
DeviceIoRelease(
    _InOut_ DeviceIo_t* io)
{
    if (g_failRelease) {
        g_failRelease--;
        return OS_EBUSY;
    }
    io->Access.Memory.VirtualBase = 0;
    return OS_EOK;
}

oserr_t
DeviceIoDestroy(
    _InOut_ DeviceIo_t* io)
{
    (void)io;
    g_ios--;
    return OS_EOK;
}

size_t
DeviceIoRead(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    CHECK(width == 4 && offset < io->Access.Memory.Length && io->Access.Memory.VirtualBase == 1);
    return g_registers[io->Id - 1][offset / 4];
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      width)
{
    CHECK(width == 4 && offset < io->Access.Memory.Length && io->Access.Memory.VirtualBase == 1);
    if (g_writes < WRITE_LOG_SIZE) {
        g_writeLog[g_writes].Range = (uint32_t)io->Id - 1U;
        g_writeLog[g_writes].Offset = (uint32_t)offset;
        g_writeLog[g_writes].Value = (uint32_t)value;
    }
    g_writes++;
    if (g_writes != g_dropWrite) {
        g_registers[io->Id - 1][offset / 4] = (uint32_t)value;
    }
    return g_writes == g_failWrite ? OS_EUNKNOWN : OS_EOK;
}

static int
__TestSleep(
    _In_  const struct timespec* interval,
    _Out_ struct timespec*       remaining)
{
    (void)interval;
    (void)remaining;
    return 0;
}

/** @brief Inventory nodes of the UART setup in the bundled tree. */
struct __SerialNodes {
    struct FdtTree*            Tree;
    const struct FdtResources* Uart;
    const struct FdtResources* SecondUart;
    const struct FdtResources* Clock;
    const struct FdtResources* Gpio;
    const struct FdtResources* Pins;
    const struct FdtResources* FlowPins;
};

static void
__Find(
    _In_    const struct FdtResources* nodes,
    _In_    int                        depth,
    _InOut_ void*                      context)
{
    struct __SerialNodes*      found = context;
    const struct FdtResources* node = FdtTreeNode(found->Tree, nodes[depth].NodeOffset);

    if (FdtCompatible(&node->View, "arm,pl011-axi") && !strcmp(node->Name, "serial@30000")) {
        found->Uart = node;
    }
    if (FdtCompatible(&node->View, "arm,pl011-axi") && !strcmp(node->Name, "serial@34000")) {
        found->SecondUart = node;
    }
    if (FdtCompatible(&node->View, "raspberrypi,rp1-clocks")) {
        found->Clock = node;
    }
    if (FdtCompatible(&node->View, "raspberrypi,rp1-gpio")) {
        found->Gpio = node;
    }
    if (!strcmp(node->Name, "rp1_uart0_14_15")) {
        found->Pins = node;
    }
    if (!strcmp(node->Name, "rp1_uart0_ctsrts_16_17")) {
        found->FlowPins = node;
    }
}

static void
__SetProperty(
    _InOut_ struct FdtResources* node,
    _InOut_ struct FdtProperty*  properties,
    _In_    const char*          name,
    _In_    const void*          value,
    _In_    uint32_t             length)
{
    size_t i;

    for (i = 0; i < node->View.PropertyCount; i++) {
        if (!strcmp(properties[i].Name, name)) {
            break;
        }
    }
    CHECK(i < 32);
    properties[i] = (struct FdtProperty) { name, value, length };
    if (i == node->View.PropertyCount) {
        node->View.PropertyCount++;
    }
    node->View.Properties = properties;
}

// A modelled RP1 with the three children the UART setup uses.
struct __Bus {
    struct FdtPciHost Host;
    PciNativeHeader_t Header;
    struct PciDevice  Parent;
    struct Rp1Bus     Bus;
    struct Rp1Child   Uart;
    struct Rp1Child   Clock;
    struct Rp1Child   Gpio;
    struct Rp1Runtime Runtime;
};

static void
__BuildBus(
    _Out_ struct __Bus*         bus,
    _In_  struct __SerialNodes* nodes)
{
    memset(bus, 0, sizeof(*bus));
    bus->Host.Tree = nodes->Tree;
    bus->Header.Revision = RP1_REVISION_C0;
    bus->Parent.Header = &bus->Header;
    bus->Bus = (struct Rp1Bus) { .Host = &bus->Host, .Parent = &bus->Parent, .State = Rp1BusRunning };
    bus->Runtime.Bus = &bus->Bus;
    bus->Bus.Children = &bus->Uart;
    bus->Uart.Next = &bus->Clock;
    bus->Clock.Next = &bus->Gpio;
    bus->Uart.Bus = bus->Clock.Bus = bus->Gpio.Bus = &bus->Bus;
    bus->Uart.DeviceId = 30;
    bus->Uart.Firmware.NodeOffset = nodes->Uart->NodeOffset;
    bus->Uart.Firmware.Compatible = (const char*)nodes->Uart->Compatible;
    bus->Uart.Firmware.CompatibleLength = nodes->Uart->CompatibleLength;
    bus->Clock.Firmware.NodeOffset = nodes->Clock->NodeOffset;
    bus->Clock.Firmware.Compatible = (const char*)nodes->Clock->Compatible;
    bus->Clock.Firmware.CompatibleLength = nodes->Clock->CompatibleLength;
    bus->Gpio.Firmware.NodeOffset = nodes->Gpio->NodeOffset;
    bus->Clock.Firmware.RegisterCount = 1;
    bus->Clock.Firmware.Registers[0] = (struct FdtRp1Range) { .Base = 1, .Length = sizeof(g_registers[0]) };
    bus->Gpio.Firmware.RegisterCount = 3;
    bus->Gpio.Firmware.Registers[0] = (struct FdtRp1Range) { .Base = 2, .Length = sizeof(g_registers[1]) };
    bus->Gpio.Firmware.Registers[2] = (struct FdtRp1Range) { .Base = 3, .Length = sizeof(g_registers[2]) };
}

// Register values after the firmware reset RP1, with other bits set so the
// test sees that the setup keeps them: clk_uart running from input 1 with a
// divider of 3, and GPIO 14 to 17 without a function, overridden outputs and
// disabled output drivers.
static void
__ResetRegisters(void)
{
    memset(g_registers, 0, sizeof(g_registers));
    g_registers[CLOCKS][RP1_CLK_UART_CTRL / 4] = RP1_CLK_CTRL_ENABLE | (1U << 5) | 0x10000U;
    g_registers[CLOCKS][RP1_CLK_UART_DIV_INT / 4] = 3;
    for (uint32_t pin = RP1_GPIO_UART0_FIRST; pin <= RP1_GPIO_UART0_LAST; pin++) {
        g_registers[GPIO][RP1_GPIO_CTRL(0U, pin) / 4] = 0x1fU | RP1_GPIO_CTRL_OEOVER_ENABLE | 0x30000U;
        g_registers[PADS][RP1_PADS_GPIO(0U, pin) / 4] = RP1_PADS_OUTPUT_DISABLE | RP1_PADS_PULL_DOWN | 0x2U;
    }
    memcpy(g_original, g_registers, sizeof(g_original));
    g_writes = 0;
    g_failWrite = 0;
    g_dropWrite = 0;
}

static oserr_t
__Stage(
    _In_ struct Rp1Child*  child,
    _In_ uuid_t            principal,
    _In_ enum DeviceStage  stage)
{
    return Rp1ChildResourceControl(child, principal, OSIOCTLREQUEST_DEVICE_SET_STAGE, &stage, sizeof(stage));
}

static oserr_t
__ClockRate(
    _In_  struct Rp1Child* child,
    _In_  uuid_t           principal,
    _In_  uint32_t         index,
    _Out_ uint64_t*        rate)
{
    struct DeviceClockRequest request = { .Index = index };
    oserr_t                   status;

    status = Rp1ChildResourceControl(child, principal, OSIOCTLREQUEST_CLOCK_RATE, &request, sizeof(request));
    *rate = request.Rate;
    return status;
}

static void
__CheckUnchanged(
    _In_ struct Rp1Child* uart)
{
    CHECK(uart->Serial == NULL && uart->Resources.State == Rp1ChildNotPrepared && g_ios == 0);
    CHECK(!memcmp(g_original, g_registers, sizeof(g_original)));
}

static void
__TestPrepareAndStop(
    _In_ struct __SerialNodes* nodes)
{
    struct __Bus bus;
    uint64_t     rate = 0;
    uint32_t     ctrl;
    uint32_t     pad;

    __BuildBus(&bus, nodes);
    __ResetRegisters();
    CHECK(Rp1SerialIsDevice(&bus.Uart) && !Rp1SerialIsDevice(&bus.Clock));

    // No clock rate before prepare, and no prepare without a process.
    CHECK(__ClockRate(&bus.Uart, DRIVER, 0, &rate) == OS_EBUSY);
    CHECK(Rp1SerialStatus(&bus.Uart, DRIVER) == OS_EBUSY);
    CHECK(__Stage(&bus.Uart, UUID_INVALID, DevicePrepare) == OS_EPERMISSIONS);
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStart) == OS_EBUSY);
    __CheckUnchanged(&bus.Uart);

    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EOK);
    CHECK(bus.Uart.Resources.State == Rp1ChildPrepared && bus.Uart.Serial != NULL && g_ios == 3);

    // clk_uart: stopped first, then switched to the crystal, then divider 1,
    // then started. Other bits stay.
    CHECK(g_writes == RP1_SERIAL_CLOCK_STEPS + 2U * 2U);
    CHECK(g_writeLog[0].Range == CLOCKS && g_writeLog[0].Offset == RP1_CLK_UART_CTRL);
    CHECK(g_writeLog[0].Value == ((1U << 5) | 0x10000U));
    CHECK(g_writeLog[1].Offset == RP1_CLK_UART_CTRL && g_writeLog[1].Value == (RP1_CLK_UART_AUXSRC_XOSC | 0x10000U));
    CHECK(g_writeLog[2].Offset == RP1_CLK_UART_DIV_INT && g_writeLog[2].Value == 1);
    CHECK(g_writeLog[3].Offset == RP1_CLK_UART_CTRL);
    CHECK(g_writeLog[3].Value == (RP1_CLK_CTRL_ENABLE | RP1_CLK_UART_AUXSRC_XOSC | 0x10000U));

    // GPIO 14 (TX, no bias) and 15 (RX, pull-up): pad first, then function
    // 4 with the overrides back to the function. Other bits stay.
    CHECK(g_writeLog[4].Range == PADS && g_writeLog[4].Offset == RP1_PADS_GPIO(0U, 14U));
    CHECK(g_writeLog[5].Range == GPIO && g_writeLog[5].Offset == RP1_GPIO_CTRL(0U, 14U));
    CHECK(g_writeLog[6].Range == PADS && g_writeLog[6].Offset == RP1_PADS_GPIO(0U, 15U));
    CHECK(g_writeLog[7].Range == GPIO && g_writeLog[7].Offset == RP1_GPIO_CTRL(0U, 15U));
    pad = g_registers[PADS][RP1_PADS_GPIO(0U, 14U) / 4];
    CHECK(pad == (RP1_PADS_INPUT_ENABLE | RP1_PADS_PULL_NONE | 0x2U));
    pad = g_registers[PADS][RP1_PADS_GPIO(0U, 15U) / 4];
    CHECK(pad == (RP1_PADS_INPUT_ENABLE | RP1_PADS_PULL_UP | 0x2U));
    ctrl = g_registers[GPIO][RP1_GPIO_CTRL(0U, 14U) / 4];
    CHECK(ctrl == (RP1_GPIO_CTRL_FUNCSEL_UART0 | 0x30000U));
    CHECK(g_registers[GPIO][RP1_GPIO_CTRL(0U, 16U) / 4] == g_original[GPIO][RP1_GPIO_CTRL(0U, 16U) / 4]);

    // A second prepare is a no-op.
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EOK && g_writes == 8);
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStart) == OS_EOK);

    // The clock rate is the crystal's, for clock 0 only.
    CHECK(__ClockRate(&bus.Uart, DRIVER, 0, &rate) == OS_EOK && rate == RP1_XOSC_RATE_HZ);
    CHECK(__ClockRate(&bus.Uart, DRIVER, 1, &rate) == OS_ENOTSUPPORTED);
    CHECK(__ClockRate(&bus.Uart, OTHER, 0, &rate) == OS_EPERMISSIONS);
    {
        struct DeviceClockRequest bad = { .Index = 0, .Reserved = 1 };
        CHECK(Rp1ChildResourceControl(&bus.Uart, DRIVER, OSIOCTLREQUEST_CLOCK_RATE, &bad, sizeof(bad)) ==
            OS_EINVALPARAMS);
        CHECK(Rp1ChildResourceControl(&bus.Uart, DRIVER, OSIOCTLREQUEST_CLOCK_RATE, &bad, 4) == OS_EINVALPARAMS);
    }
    // Other children have no clock rate here.
    CHECK(__ClockRate(&bus.Clock, DRIVER, 0, &rate) == OS_ENOTSUPPORTED);

    // Interrupt reservations follow the preparing process.
    CHECK(Rp1SerialStatus(&bus.Uart, DRIVER) == OS_EOK);
    CHECK(Rp1SerialStatus(&bus.Uart, UUID_INVALID) == OS_EOK);
    CHECK(Rp1SerialStatus(&bus.Uart, OTHER) == OS_EPERMISSIONS);
    CHECK(Rp1SerialStatus(&bus.Clock, OTHER) == OS_EOK);
    CHECK(__Stage(&bus.Uart, OTHER, DeviceStop) == OS_EPERMISSIONS);

    // While the driver holds the interrupt source, stop is refused and
    // nothing changes.
    bus.Bus.Runtime = &bus.Runtime;
    bus.Runtime.Sources[25].Owner = &bus.Uart;
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EBUSY);
    CHECK(bus.Uart.Resources.State == Rp1ChildPrepared && bus.Uart.Serial != NULL && g_writes == 8);
    bus.Runtime.Sources[25].Owner = NULL;

    // During removal, prepare and start are refused, stop still works.
    bus.Bus.State = Rp1BusRemoving;
    CHECK(Rp1SerialStatus(&bus.Uart, DRIVER) == OS_EBUSY);
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStart) == OS_EBUSY);

    // A failed unmap keeps the state for a retry. The registers are already
    // back then.
    bus.Bus.Runtime = NULL;
    g_failRelease = 1;
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EBUSY);
    CHECK(bus.Uart.Serial != NULL && bus.Uart.Resources.State == Rp1ChildNotPrepared);
    CHECK(!memcmp(g_original, g_registers, sizeof(g_original)));
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EOK);
    __CheckUnchanged(&bus.Uart);

    // Stop restored in reverse order: the pins first, the clock last, and
    // the clock control ends with the firmware's value.
    CHECK(g_writes == 16);
    CHECK(g_writeLog[8].Range == GPIO && g_writeLog[8].Offset == RP1_GPIO_CTRL(0U, 15U));
    CHECK(g_writeLog[15].Range == CLOCKS && g_writeLog[15].Offset == RP1_CLK_UART_CTRL);
    CHECK(g_writeLog[15].Value == g_original[CLOCKS][RP1_CLK_UART_CTRL / 4]);
    bus.Bus.State = Rp1BusRunning;
    printf("RP1 UART prepare and stop: passed\n");
}

static void
__TestFailedWrites(
    _In_ struct __SerialNodes* nodes)
{
    struct __Bus bus;
    unsigned int failure;

    // A failed write at any step undoes every step before it, including the
    // failed one, and leaves no mapping behind.
    for (failure = 1; failure <= RP1_SERIAL_CLOCK_STEPS + 4U; failure++) {
        __BuildBus(&bus, nodes);
        __ResetRegisters();
        g_failWrite = failure;
        CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EUNKNOWN);
        __CheckUnchanged(&bus.Uart);
    }

    // A write whose readback differs counts as failed too. The clock
    // divider write (step 3) is the one that does not land.
    __BuildBus(&bus, nodes);
    __ResetRegisters();
    g_dropWrite = 3;
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EUNKNOWN);
    __CheckUnchanged(&bus.Uart);

    // An RP1 that is not C0 is refused before anything is mapped.
    __BuildBus(&bus, nodes);
    __ResetRegisters();
    bus.Header.Revision = RP1_REVISION_C0 + 1;
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);
    __CheckUnchanged(&bus.Uart);
    printf("RP1 UART failed writes: passed\n");
}

static void
__TestFirmwareVariants(
    _In_ struct __SerialNodes* nodes)
{
    struct FdtResources* uart = (void*)nodes->Uart;
    struct FdtResources* entry;
    struct FdtResources  savedUart = *uart;
    struct FdtResources  savedEntry;
    struct FdtProperty   uartProperties[32];
    struct FdtProperty   entryProperties[32];
    struct FdtResources  rxEntry;
    struct __Bus         bus;
    uint8_t              groups[8];
    uint32_t             length;
    const char           names[] = "apb_pclk\0uartclk";

    // Locate the RX entry of the TX/RX group to change its properties.
    memset(&rxEntry, 0, sizeof(rxEntry));
    entry = NULL;
    for (uint32_t offset = nodes->Pins->NodeOffset + 1; entry == NULL; offset++) {
        const struct FdtResources* candidate = FdtTreeNode(nodes->Tree, offset);
        if (candidate != NULL && !strcmp(candidate->Name, "pin_rxd")) {
            entry = (void*)candidate;
        }
        CHECK(offset < nodes->Pins->NodeOffset + 512U);
    }
    savedEntry = *entry;

    // Clock names in another order.
    __BuildBus(&bus, nodes);
    __ResetRegisters();
    memcpy(uartProperties, uart->View.Properties, uart->View.PropertyCount * sizeof(*uartProperties));
    __SetProperty(uart, uartProperties, "clock-names", names, sizeof(names));
    uart->ClockNames = (const uint8_t*)names;
    uart->ClockNamesLength = sizeof(names);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);
    __CheckUnchanged(&bus.Uart);
    *uart = savedUart;

    // Another function on the RX line.
    memcpy(entryProperties, entry->View.Properties, entry->View.PropertyCount * sizeof(*entryProperties));
    __SetProperty(entry, entryProperties, "function", "uart1", 6);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);
    __CheckUnchanged(&bus.Uart);
    *entry = savedEntry;

    // A line outside UART0, and a name that is not gpio<number>.
    memcpy(entryProperties, entry->View.Properties, entry->View.PropertyCount * sizeof(*entryProperties));
    __SetProperty(entry, entryProperties, "pins", "gpio20", 7);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);
    __SetProperty(entry, entryProperties, "pins", "gpio1x", 7);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);
    __SetProperty(entry, entryProperties, "pins", "gpio015", 8);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOTSUPPORTED);

    // TX named twice, and two biases on one entry.
    __SetProperty(entry, entryProperties, "pins", "gpio14", 7);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EINVALPARAMS);
    __SetProperty(entry, entryProperties, "pins", "gpio15", 7);
    __SetProperty(entry, entryProperties, "bias-disable", "", 0);
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EINVALPARAMS);
    __CheckUnchanged(&bus.Uart);
    *entry = savedEntry;

    // Two groups, as the overlay names with its ctsrts option: all four
    // lines get the UART function, CTS with a pull-up.
    CHECK(nodes->FlowPins != NULL);
    memcpy(uartProperties, uart->View.Properties, uart->View.PropertyCount * sizeof(*uartProperties));
    memcpy(groups, FdtProperty(&uart->View, "pinctrl-0", &length), 4);
    CHECK(length == 4);
    groups[4] = (uint8_t)(nodes->FlowPins->Phandle >> 24);
    groups[5] = (uint8_t)(nodes->FlowPins->Phandle >> 16);
    groups[6] = (uint8_t)(nodes->FlowPins->Phandle >> 8);
    groups[7] = (uint8_t)nodes->FlowPins->Phandle;
    __SetProperty(uart, uartProperties, "pinctrl-0", groups, sizeof(groups));
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EOK);
    CHECK(g_writes == RP1_SERIAL_CLOCK_STEPS + 2U * 4U);
    for (uint32_t pin = RP1_GPIO_UART0_FIRST; pin <= RP1_GPIO_UART0_LAST; pin++) {
        CHECK((g_registers[GPIO][RP1_GPIO_CTRL(0U, pin) / 4] & RP1_GPIO_CTRL_FUNCSEL_MASK) ==
            RP1_GPIO_CTRL_FUNCSEL_UART0);
    }
    CHECK((g_registers[PADS][RP1_PADS_GPIO(0U, 16U) / 4] & RP1_PADS_PULL_MASK) == RP1_PADS_PULL_UP);
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EOK);
    __CheckUnchanged(&bus.Uart);

    // The same group twice names every line twice.
    memcpy(&groups[4], groups, 4);
    __SetProperty(uart, uartProperties, "pinctrl-0", groups, sizeof(groups));
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EINVALPARAMS);
    __CheckUnchanged(&bus.Uart);

    // Without a pin state only the clock is set up.
    uart->View.PropertyCount = 0;
    for (uint32_t i = 0; i < savedUart.View.PropertyCount; i++) {
        if (strncmp(savedUart.View.Properties[i].Name, "pinctrl-", 8) != 0) {
            uartProperties[uart->View.PropertyCount++] = savedUart.View.Properties[i];
        }
    }
    uart->View.Properties = uartProperties;
    __ResetRegisters();
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EOK);
    CHECK(g_writes == RP1_SERIAL_CLOCK_STEPS && g_ios == 1);
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EOK);
    __CheckUnchanged(&bus.Uart);
    *uart = savedUart;

    // A GPIO block whose ranges are too short is refused after the clock
    // block was mapped, and that mapping is released again.
    __BuildBus(&bus, nodes);
    __ResetRegisters();
    bus.Gpio.Firmware.Registers[2].Length = RP1_GPIO_UART0_PAD_MINIMUM_LENGTH - 4U;
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EINVALPARAMS);
    __CheckUnchanged(&bus.Uart);
    printf("RP1 UART firmware variants: passed\n");
}

static void
__TestSharedClock(
    _In_ struct __SerialNodes* nodes)
{
    struct __Bus    bus;
    struct Rp1Child second;

    // UART1 joins the bus. Both share clk_uart, so it is refused while UART0
    // is prepared, before anything is read, mapped or written.
    __BuildBus(&bus, nodes);
    __ResetRegisters();
    memset(&second, 0, sizeof(second));
    second.Bus = &bus.Bus;
    second.DeviceId = 34;
    second.Firmware.NodeOffset = nodes->SecondUart->NodeOffset;
    second.Firmware.Compatible = (const char*)nodes->SecondUart->Compatible;
    second.Firmware.CompatibleLength = nodes->SecondUart->CompatibleLength;
    bus.Gpio.Next = &second;
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_EOK);
    CHECK(__Stage(&second, DRIVER, DevicePrepare) == OS_EBUSY);
    CHECK(second.Serial == NULL && g_writes == RP1_SERIAL_CLOCK_STEPS + 4U && g_ios == 3);

    // Once UART0 has stopped, UART1 is no longer refused for the clock. Its
    // own description decides then: the bundled trees keep it disabled.
    CHECK(__Stage(&bus.Uart, DRIVER, DeviceStop) == OS_EOK);
    CHECK(__Stage(&second, DRIVER, DevicePrepare) == OS_ENOENT);
    __CheckUnchanged(&bus.Uart);
    CHECK(second.Serial == NULL);
    printf("RP1 UART shared clock: passed\n");
}

static void
__TestDisabledNode(
    _In_ struct __SerialNodes* nodes)
{
    struct __Bus bus;

    __BuildBus(&bus, nodes);
    __ResetRegisters();
    CHECK(__Stage(&bus.Uart, DRIVER, DevicePrepare) == OS_ENOENT);
    __CheckUnchanged(&bus.Uart);
    printf("RP1 UART disabled in the tree: refused\n");
}

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    struct __SerialNodes nodes = { 0 };
    FILE*                file;
    long                 length;
    void*                blob;

    CHECK(argc == 2);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    blob = malloc((size_t)length);
    CHECK(blob != NULL && fread(blob, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    CHECK(FdtTreeCreate(blob, (size_t)length, &nodes.Tree) == OS_EOK);
    CHECK(FdtWalkResources(nodes.Tree, __Find, &nodes) == OS_EOK);
    CHECK(nodes.Uart != NULL && nodes.Clock != NULL && nodes.Gpio != NULL && nodes.Pins != NULL);
    CHECK(nodes.SecondUart != NULL && nodes.SecondUart->Disabled);

    // The bundled trees mark the port as disabled. The firmware enables it
    // when config.txt loads the uart0-pi5 overlay. Until then deviced
    // refuses to decode it, which is the reason the overlay is needed.
    CHECK(nodes.Uart->Disabled);
    __TestDisabledNode(&nodes);
    ((struct FdtResources*)nodes.Uart)->Disabled = 0;

    __TestPrepareAndStop(&nodes);
    __TestFailedWrites(&nodes);
    __TestFirmwareVariants(&nodes);
    __TestSharedClock(&nodes);

    FdtTreeRelease(nodes.Tree);
    free(blob);
    printf("RP1 UART setup: %s passed\n", argv[1]);
    return 0;
}
