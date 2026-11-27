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
 * Host test for the BCM2712 SDHCI controller variant
 * (variants/bcm2712.c), linked with the real engine sources and the fakes
 * in sdhci_fake_host.inc. A test slot stands in for the device tree slot: it
 * hands out BAR 0 as the host window (0x260 bytes, as in the bundled trees)
 * and BAR 1 as the config window, a scripted input clock, the board values
 * of the case and a scripted signaling voltage, and records card power.
 *
 * The cases check every write to the config window and to the command
 * queueing engine: the presence decision for "broken-cd", a soldered device,
 * a working detect line and a GPIO detect line, the base clock in the
 * command queueing capability, the switch-off of a command queueing engine
 * left on, the restore after a full reset, the pin selection for each timing
 * and device kind while the card clock is stopped, the capability
 * corrections, and the refusal of a host without the config range or with
 * a host window that is too short.
 */

#include "sdhci_fake_host.inc"
#include "sdhci/variants/bcm2712.h"

#define CONFIG_BAR_ID  (BAR_ID_BASE + 1)
#define CLOCK_200M     200000000U
#define CLOCK_400K     400000U
#define CLOCK_50M      50000000U

// Capabilities of a version 3.00 host as the BCM2712 might report them: a
// 200 MHz base clock, ADMA2 with the 64-bit format, high speed, eight lines
// and 3.3 V bus power. The 1.8 V bus power bit is left clear on purpose: the
// card supply is a fixed 3.3 V regulator, and 1.8 V signaling must work
// without that bit. No register dump from a board exists yet.
#define BOARD_CAPABILITIES (CAPS_BASE(200) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_64BIT_V3 | SDHCI_CAPS_HIGH_SPEED | \
                            SDHCI_CAPS_8BIT | SDHCI_CAPS_VOLTAGE_330)

// Other bits in the config registers, which every write must keep.
#define CTRL_OTHER_BITS     0x00000105U
#define PIN_SEL_OTHER_BITS  0x000000F0U
// Other bits in CQHCI_CFG (task descriptor size), which the switch-off keeps.
#define CQHCI_OTHER_BITS    0x00000100U

static SdhciController_t       g_controller;
static uint32_t                g_slotMarker;
static uint32_t                g_inputClockHz;
static SdhciSlotValues_t       g_slotValues;
static bool                    g_noConfigRange;
static enum SdhciSignalVoltage g_slotSignal;
// Set to keep the command queueing enable bit on, whatever is written.
static bool                    g_queueingStuck;

// ---------------------------------------------------------------------------
// Test slot
// ---------------------------------------------------------------------------

static oserr_t
__SlotSelectRegisters(
    _In_  SdhciController_t*            controller,
    _In_  const struct DeviceResources* resources,
    _Out_ DeviceIo_t*                   registersOut)
{
    assert(controller->SlotContext == &g_slotMarker);
    assert(resources->Count >= 1);
    *registersOut = resources->Registers[0];
    return OS_EOK;
}

static oserr_t
__SlotSelectConfigRegisters(
    _In_  SdhciController_t*            controller,
    _In_  const struct DeviceResources* resources,
    _Out_ DeviceIo_t*                   registersOut)
{
    assert(controller->SlotContext == &g_slotMarker);
    if (g_noConfigRange || resources->Count < 2) {
        return OS_ENOENT;
    }
    *registersOut = resources->Registers[1];
    return OS_EOK;
}

static oserr_t
__SlotGetInputClock(
    _In_  SdhciController_t* controller,
    _Out_ uint32_t*          clockHzOut)
{
    assert(controller->SlotContext == &g_slotMarker);
    *clockHzOut = g_inputClockHz;
    return OS_EOK;
}

static oserr_t
__SlotSelectSignalVoltage(
    _In_  SdhciController_t*       controller,
    _Out_ enum SdhciSignalVoltage* voltageOut)
{
    assert(controller->SlotContext == &g_slotMarker);
    *voltageOut = g_slotSignal;
    return OS_EOK;
}

static oserr_t
__SlotSetCardPower(
    _In_ SdhciController_t* controller,
    _In_ enum SdBusPower    power)
{
    assert(controller->SlotContext == &g_slotMarker);
    __Record(EventCardPower, 0, 0, (uint32_t)power);
    return OS_EOK;
}

static oserr_t
__SlotDescribe(
    _In_  SdhciController_t*         controller,
    _In_  const SdhciCapabilities_t* capabilities,
    _Out_ SdhciSlotValues_t*         valuesOut)
{
    // The board values are applied after the variant's corrections.
    assert(controller->SlotContext == &g_slotMarker);
    assert(capabilities->DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    *valuesOut = g_slotValues;
    return OS_EOK;
}

// A command queueing engine that ignores the switch-off.
static void
__KeepQueueingOn(
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    (void)width;
    if (g_queueingStuck && offset == SDHCI_BCM2712_CQHCI_CFG) {
        __StoreRegister(SDHCI_BCM2712_CQHCI_CFG, 4, value | SDHCI_BCM2712_CQHCI_ENABLE);
    }
}

// The Compute Module 5 slot: eight lines, removable as far as the tree says,
// "broken-cd", the fixed 200 MHz clock and 3.3 V signaling unless a case
// changes it. Firmware left test level, test enable and other bits in
// SDIO_CFG_CTRL, other bits in the pin selection, and the command queueing
// engine on.
static void
__ConfigureHost(
    _In_ uint32_t capabilities)
{
    __Reset();
    __AddBar(0, 0x260, true);
    __AddBar(1, 0x200, true);
    g_configWindowId = CONFIG_BAR_ID;
    __SetHost(SDHCI_SPEC_300, capabilities);
    __StoreConfig(SDHCI_BCM2712_CFG_CTRL, CTRL_OTHER_BITS | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_LEV);
    __StoreConfig(SDHCI_BCM2712_CFG_PIN_SEL, PIN_SEL_OTHER_BITS | SDHCI_BCM2712_CFG_PIN_SEL_MMC);
    __StoreRegister(SDHCI_BCM2712_CQHCI_CFG, 4, CQHCI_OTHER_BITS | SDHCI_BCM2712_CQHCI_ENABLE);

    g_variant = g_sdhciBcm2712Variant;
    g_slot = (SdhciSlot_t) {
        .Name = "test",
        .SelectRegisters = __SlotSelectRegisters,
        .SelectConfigRegisters = __SlotSelectConfigRegisters,
        .GetInputClock = __SlotGetInputClock,
        .SelectSignalVoltage = __SlotSelectSignalVoltage,
        .SetCardPower = __SlotSetCardPower,
        .DescribeSlot = __SlotDescribe
    };
    g_slotValues = (SdhciSlotValues_t) {
        .WiredBusWidth = SdBusWidth8,
        .Removable = 1,
        .MaximumClockHz = 0,
        .Flags = SDHCI_SLOT_BROKEN_CARD_DETECT
    };
    g_inputClockHz = CLOCK_200M;
    g_noConfigRange = false;
    g_slotSignal = SdhciSignalVoltage330;
    g_queueingStuck = false;
    g_onWrite = __KeepQueueingOn;

    memset(&g_controller, 0, sizeof(g_controller));
    g_controller.SlotContext = &g_slotMarker;
}

static oserr_t
__PrepareBoard(void)
{
    return SdhciControllerPrepare(&g_controller, &g_device.Base, &g_variant, &g_slot);
}

static void
__SetupBoard(void)
{
    __ConfigureHost(BOARD_CAPABILITIES);
    assert(__PrepareBoard() == OS_EOK);
}

static uint32_t
__Count(
    _In_ enum EventKind kind,
    _In_ uint32_t       start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind) {
            count++;
        }
    }
    return count;
}

// The shared __Find compares offsets only for host window accesses.
static int
__FindConfigWrite(
    _In_ uint32_t offset,
    _In_ uint32_t start)
{
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventConfigWrite && g_events[i].Offset == offset) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t
__CountConfigWrites(
    _In_ uint32_t offset,
    _In_ uint32_t start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventConfigWrite && g_events[i].Offset == offset) {
            count++;
        }
    }
    return count;
}

static uint32_t
__QueueCapability(
    _In_ uint32_t frequencyMhz)
{
    return (SDHCI_BCM2712_CQ_UNIT_1MHZ << SDHCI_BCM2712_CQ_UNIT_SHIFT) | frequencyMhz;
}

// ---------------------------------------------------------------------------
// Setup: capabilities, access and the restore
// ---------------------------------------------------------------------------

static void
__TestSetup(void)
{
    int reset;
    int control;
    int capability;
    int queueing;
    int masks;

    __SetupBoard();

    // The 64-bit format is dropped, the base clock is the capability value
    // and matches the input clock, and there is no write delay.
    assert(g_controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    assert(g_controller.Dma.Format == SdhciAdmaFormat32 && g_openLimit == UINT32_MAX);
    assert((g_sdhciBcm2712Variant.Flags & SDHCI_VARIANT_ADMA2_64) == 0);
    assert(g_controller.Capabilities.BaseClockHz == CLOCK_200M);
    assert(strstr(g_log, "base clock 200000000 Hz in the capabilities, platform input clock 200000000 Hz") != NULL);
    assert(strstr(g_log, "differ") == NULL);
    assert(g_controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4 | SD_HOST_BUS_WIDTH_8));
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_AUTO_CMD12_REQUIRED) == 0);
    assert(g_variant.WriteDelayUs(&g_controller, 0) == 0 && g_variant.WriteDelayUs(&g_controller, CLOCK_400K) == 0);

    // Registers are accessed at their own width: the 16-bit host version.
    assert(g_events[__Find(EventRead, SDHCI_HOST_VERSION, ANY_VALUE, 0)].Width == 2);

    // After the reset: presence forced, the base clock in MHz with the 1 MHz
    // unit, and the command queueing engine switched off. Then the
    // interrupt enables.
    reset = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, 0);
    control = __FindConfigWrite(SDHCI_BCM2712_CFG_CTRL, (uint32_t)reset);
    capability = __FindConfigWrite(SDHCI_BCM2712_CFG_CQ_CAPABILITY, (uint32_t)reset);
    queueing = __Find(EventWrite, SDHCI_BCM2712_CQHCI_CFG, ANY_VALUE, (uint32_t)reset);
    masks = __Find(EventWrite, SDHCI_INTERRUPT_ENABLE, ANY_VALUE, (uint32_t)reset);
    assert(reset >= 0 && control > reset && capability > control && queueing > capability && masks > queueing);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CTRL) == (CTRL_OTHER_BITS | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_EN));
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY) == __QueueCapability(200));
    assert(g_events[queueing].Width == 4 && g_events[queueing].Value == CQHCI_OTHER_BITS);
    assert(__LoadRegister(SDHCI_BCM2712_CQHCI_CFG, 4) == CQHCI_OTHER_BITS);
    assert(strstr(g_log, "command queueing engine was left on (CQHCI_CFG 0x00000101), switching it off") != NULL);
    // The pin selection is set by the bus settings restored after the reset.
    assert(__LoadConfig(SDHCI_BCM2712_CFG_PIN_SEL) == (PIN_SEL_OTHER_BITS | SDHCI_BCM2712_CFG_PIN_SEL_SD));
    // SDIO_CFG_MAX_50MHZ_MODE is never touched.
    assert(__FindConfigWrite(0x1AC, 0) < 0);

    // A full reset writes everything again, here over values changed since,
    // and switches a command queueing engine off that came on again.
    __StoreConfig(SDHCI_BCM2712_CFG_CTRL, CTRL_OTHER_BITS);
    __StoreConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY, 0);
    __StoreRegister(SDHCI_BCM2712_CQHCI_CFG, 4, CQHCI_OTHER_BITS | SDHCI_BCM2712_CQHCI_ENABLE);
    reset = (int)g_eventCount;
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(__CountConfigWrites(SDHCI_BCM2712_CFG_CTRL, (uint32_t)reset) == 1);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CTRL) == (CTRL_OTHER_BITS | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_EN));
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY) == __QueueCapability(200));
    assert(__LoadRegister(SDHCI_BCM2712_CQHCI_CFG, 4) == CQHCI_OTHER_BITS);

    // An engine that is already off is only read.
    reset = (int)g_eventCount;
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(__CountWrites(SDHCI_BCM2712_CQHCI_CFG, (uint32_t)reset) == 0);
    assert(__Find(EventRead, SDHCI_BCM2712_CQHCI_CFG, ANY_VALUE, (uint32_t)reset) >= 0);
    __DestroyAll(&g_controller);

    // A command queueing engine that stays on fails setup.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_queueingStuck = true;
    assert(__PrepareBoard() == OS_EDEVFAULT);
    assert(strstr(g_log, "command queueing engine stays on") != NULL);
    __DestroyAll(&g_controller);
    printf("BCM2712 setup and restore passed\n");
}

// ---------------------------------------------------------------------------
// Card presence
// ---------------------------------------------------------------------------

static void
__TestPresence(void)
{
    const uint32_t forced = CTRL_OTHER_BITS | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_EN;

    // "broken-cd" was the setup case. A soldered device ("non-removable")
    // forces presence as well.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotValues.Removable = 0;
    g_slotValues.Flags = 0;
    assert(__PrepareBoard() == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CTRL) == forced);
    __DestroyAll(&g_controller);

    // A detect line to the host: the test bits are cleared, so the host's
    // own pin decides, even when firmware had forced presence.
    __ConfigureHost(BOARD_CAPABILITIES);
    __StoreConfig(SDHCI_BCM2712_CFG_CTRL, forced | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_LEV);
    g_slotValues.Flags = SDHCI_SLOT_CARD_DETECT | SDHCI_SLOT_WRITE_PROTECT;
    assert(__PrepareBoard() == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CTRL) == CTRL_OTHER_BITS);
    __DestroyAll(&g_controller);

    // A GPIO detect line ("cd-gpios"): the register keeps what firmware set
    // and is never written, as in Linux.
    __ConfigureHost(BOARD_CAPABILITIES);
    __StoreConfig(SDHCI_BCM2712_CFG_CTRL, forced);
    g_slotValues.Flags = 0;
    assert(__PrepareBoard() == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CTRL) == forced);
    assert(__CountConfigWrites(SDHCI_BCM2712_CFG_CTRL, 0) == 0);
    __DestroyAll(&g_controller);
    printf("BCM2712 card presence passed\n");
}

// ---------------------------------------------------------------------------
// Command queueing capability
// ---------------------------------------------------------------------------

static void
__TestQueueCapability(void)
{
    // A host without a base clock in its capabilities uses the input clock,
    // and that rate goes into the capability.
    __ConfigureHost(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_inputClockHz = CLOCK_50M;
    assert(__PrepareBoard() == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY) == __QueueCapability(50));

    // Below 1 MHz the value is 1, as in Linux. Above the 10-bit field it is
    // capped, with a warning.
    g_controller.Capabilities.BaseClockHz = 500000;
    assert(g_variant.RestoreConfiguration(&g_controller) == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY) == __QueueCapability(1));
    g_controller.Capabilities.BaseClockHz = 2000000000U;
    assert(g_variant.RestoreConfiguration(&g_controller) == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_CQ_CAPABILITY) == __QueueCapability(SDHCI_BCM2712_CQ_FREQUENCY_MASK));
    assert(strstr(g_log, "base clock of 2000 MHz does not fit the command queueing capability") != NULL);
    __DestroyAll(&g_controller);
    printf("BCM2712 command queueing capability passed\n");
}

// ---------------------------------------------------------------------------
// Pin selection
// ---------------------------------------------------------------------------

static void
__ExpectPins(
    _In_ uint32_t         clockHz,
    _In_ enum SdBusTiming timing,
    _In_ enum SdBusDevice device,
    _In_ uint32_t         pins)
{
    SdBusSettings_t settings = __Settings(clockHz, SdBusWidth8, timing, SdBusPowerOn);
    uint32_t        actualHz;
    uint32_t        start = g_eventCount;
    int             stop;
    int             select;
    int             enable;

    settings.Device = device;
    assert(SdhciControllerSetBus(&g_controller, &settings, &actualHz) == OS_EOK);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_PIN_SEL) == (PIN_SEL_OTHER_BITS | pins));

    // The selection is written once, after the card clock stopped and before
    // the new clock is enabled.
    stop = __Find(EventWrite, SDHCI_CLOCK_CONTROL, 0, start);
    select = __FindConfigWrite(SDHCI_BCM2712_CFG_PIN_SEL, start);
    enable = __Find(EventWrite, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)stop + 1);
    assert(stop >= 0 && select > stop);
    assert(__CountConfigWrites(SDHCI_BCM2712_CFG_PIN_SEL, start) == 1);
    if (clockHz != 0) {
        assert(enable > select);
    } else {
        assert(enable < 0);
    }
}

static void
__TestPins(void)
{
    uint32_t start;

    __SetupBoard();
    __ExpectPins(CLOCK_400K, SdBusTimingDefault, SdBusDeviceSd, SDHCI_BCM2712_CFG_PIN_SEL_SD);
    __ExpectPins(CLOCK_50M, SdBusTimingHighSpeed, SdBusDeviceSd, SDHCI_BCM2712_CFG_PIN_SEL_SD);
    __ExpectPins(CLOCK_400K, SdBusTimingDefault, SdBusDeviceMmc, SDHCI_BCM2712_CFG_PIN_SEL_SD);
    __ExpectPins(CLOCK_50M, SdBusTimingHighSpeed, SdBusDeviceMmc, SDHCI_BCM2712_CFG_PIN_SEL_MMC);
    __ExpectPins(0, SdBusTimingHighSpeed, SdBusDeviceMmc, SDHCI_BCM2712_CFG_PIN_SEL_MMC);
    __ExpectPins(0, SdBusTimingDefault, SdBusDeviceMmc, SDHCI_BCM2712_CFG_PIN_SEL_SD);

    // A full reset writes the selection of the kept settings again.
    __ExpectPins(CLOCK_50M, SdBusTimingHighSpeed, SdBusDeviceMmc, SDHCI_BCM2712_CFG_PIN_SEL_MMC);
    __StoreConfig(SDHCI_BCM2712_CFG_PIN_SEL, PIN_SEL_OTHER_BITS | SDHCI_BCM2712_CFG_PIN_SEL_SD);
    start = g_eventCount;
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(__CountConfigWrites(SDHCI_BCM2712_CFG_PIN_SEL, start) == 1);
    assert(__LoadConfig(SDHCI_BCM2712_CFG_PIN_SEL) == (PIN_SEL_OTHER_BITS | SDHCI_BCM2712_CFG_PIN_SEL_MMC));
    __DestroyAll(&g_controller);
    printf("BCM2712 pin selection passed\n");
}

// ---------------------------------------------------------------------------
// 1.8 V signaling and refusals
// ---------------------------------------------------------------------------

static void
__TestSignaling(void)
{
    SdBusSettings_t settings = __Settings(CLOCK_400K, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    uint32_t        clockHz;

    // The Compute Module 5 slot signals at 1.8 V: the bit is set before the
    // card supply, and kept across a reset.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotSignal = SdhciSignalVoltage180;
    assert(__PrepareBoard() == OS_EOK);
    assert(SdhciControllerSetBus(&g_controller, &settings, &clockHz) == OS_EOK);
    assert(__Find(EventWrite, SDHCI_HOST_CONTROL2, SDHCI_HOST_CONTROL2_1V8_ENABLE, 0) <
        __Find(EventCardPower, 0, SdBusPowerOn, 0));
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(__LoadRegister(SDHCI_HOST_CONTROL2, 2) == SDHCI_HOST_CONTROL2_1V8_ENABLE);
    __DestroyAll(&g_controller);
    printf("BCM2712 signaling passed\n");
}

static void
__TestRefusals(void)
{
    // No config range on the board.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_noConfigRange = true;
    assert(__PrepareBoard() == OS_ENOENT);
    assert(strstr(g_log, "the bcm2712 variant needs a config register range") != NULL);
    assert(__Count(EventRead, 0) == 0);
    __DestroyAll(&g_controller);

    // A slot that cannot give one at all.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slot.SelectConfigRegisters = NULL;
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "the bcm2712 variant needs a config register range and the test slot has none") != NULL);
    __DestroyAll(&g_controller);

    // A host window that ends before the command queueing registers.
    __ConfigureHost(BOARD_CAPABILITIES);
    __AddBar(0, 0x100, true);
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "test register window is not a memory window of at least 524 bytes") != NULL);
    __DestroyAll(&g_controller);

    // A config window that ends before the command queueing capability.
    __ConfigureHost(BOARD_CAPABILITIES);
    __AddBar(1, 0x40, true);
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "test config register window is not a memory window of at least 80 bytes") != NULL);
    __DestroyAll(&g_controller);
    printf("BCM2712 refusals passed\n");
}

int
main(void)
{
    __TestSetup();
    __TestPresence();
    __TestQueueCapability();
    __TestPins();
    __TestSignaling();
    __TestRefusals();
    printf("BCM2712 variant tests passed\n");
    return 0;
}
