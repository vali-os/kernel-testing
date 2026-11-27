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
 * Host test for the BCM2711 SDHCI controller variant
 * (variants/bcm2711.c), linked with the real engine sources and the fakes
 * in sdhci_fake_host.inc. A test slot stands in for the device tree slot: it
 * hands out resource 0, a scripted input clock, the board values of the case,
 * and records card power.
 *
 * The fake register file runs in word-only mode for every case: any 8 or
 * 16-bit access, by the variant or by the engine through it, fails an
 * assertion. The cases call the hooks directly to check widths, shadows,
 * resets and the write delay, then run the whole engine on top of the
 * variant: setup, reset, bus settings, and requests that complete by polling
 * at 400 kHz and by interrupt at 25 MHz.
 */

#include "sdhci_fake_host.inc"
#include "sdhci/variants/bcm2711.h"

#define EVENT_DESCRIPTOR 9
#define BUFFER_ADDRESS   0x20000000ULL
#define BLOCK_SIZE       512U
#define MAX_SCRIPT       16U
#define MAX_COMMANDS     8U
#define MAX_STALLS       64U
#define MAX_EXPECTED     16U
#define RUN_LIMIT_US     10000000ULL

// Register values read from a Raspberry Pi 4 EMMC2 host: a 100 MHz base
// clock, ADMA2, high speed, 3.3 V and 1.8 V, eight data lines, 2048-byte
// blocks and an embedded slot type. The host version register reports 3.00.
#define BOARD_CAPABILITIES  0x45EE6432U
#define BOARD_CAPABILITIES1 0x0000A525U
#define BOARD_BASE_CLOCK_HZ 100000000U

// Card clocks the delay cases use. 396,825 Hz is not a whole divisor of the
// base clock, so its four periods are not a whole number of microseconds.
#define CLOCK_400K   400000U
#define CLOCK_ODD    396825U
#define CLOCK_200K   200000U
#define CLOCK_25M    25000000U

// Present state of a slot with a card and every line free and high.
#define PRESENT_IDLE (SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_STABLE | \
                      SDHCI_PRESENT_WRITE_ENABLED | SDHCI_PRESENT_DAT_LEVEL_MASK | SDHCI_PRESENT_CMD_LEVEL)

#define CC SDHCI_INT_COMMAND_COMPLETE
#define TC SDHCI_INT_TRANSFER_COMPLETE

// CMD13 with an R1 response: index 13, index check, CRC check, 48 bits.
#define CMD13_BITS 0x0D1AU
// CMD18 with data and an R1 response.
#define CMD18_BITS 0x123AU
// Transfer mode of a multiple-block ADMA2 read with Auto CMD12.
#define READ_MULTIPLE_MODE (SDHCI_TRANSFER_DMA | SDHCI_TRANSFER_BLOCK_COUNT | SDHCI_TRANSFER_AUTO_CMD12 | \
                            SDHCI_TRANSFER_READ | SDHCI_TRANSFER_MULTIPLE_BLOCKS)

struct ExpectedWrite {
    uint32_t Offset;
    uint32_t Value;
};

// A busy wait the variant made, and where in the event list it was.
struct Stall {
    uint32_t EventIndex;
    uint64_t LengthUs;
};

// A status bit the fake host latches at a given time.
struct ScriptEvent {
    uint64_t AtUs;
    uint32_t Status;
    bool     Pending;
};

static SdhciController_t     g_controller;
static uint32_t              g_platformMarker;
static uint32_t              g_inputClockHz;
static uint32_t              g_platformSelects;
static uint32_t              g_platformClockReads;
static size_t                g_windowLength;
// Board values the test slot reports.
static SdhciSlotValues_t     g_slotValues;

static struct Stall          g_stallList[MAX_STALLS];
static uint32_t              g_stallCount;
static uint64_t              g_lastStallUs;

static struct ScriptEvent    g_script[MAX_SCRIPT];
static uint32_t              g_scriptCount;
static uint32_t              g_commandWords[MAX_COMMANDS];
static uint32_t              g_commandCount;
static uint32_t              g_replyStatus[MAX_COMMANDS][2];
static uint32_t              g_replyDelayUs[MAX_COMMANDS][2];
static struct DMASegment     g_segments[1];
static uint32_t              g_completions;
static SdResult_t            g_result;

// ---------------------------------------------------------------------------
// Test slot
// ---------------------------------------------------------------------------

// The variant state the engine allocated for the controller.
static SdhciBcm2711Context_t*
__Context(void)
{
    assert(g_controller.VariantContext != NULL);
    return (SdhciBcm2711Context_t*)g_controller.VariantContext;
}

static oserr_t
__SlotSelectRegisters(
    _In_  SdhciController_t*            controller,
    _In_  const struct DeviceResources* resources,
    _Out_ DeviceIo_t*                   registersOut)
{
    assert(controller->SlotContext == &g_platformMarker);
    assert(resources->Count >= 1);
    g_platformSelects++;
    *registersOut = resources->Registers[0];
    registersOut->Access.Memory.Length = g_windowLength;
    return OS_EOK;
}

static oserr_t
__SlotGetInputClock(
    _In_  SdhciController_t* controller,
    _Out_ uint32_t*          clockHzOut)
{
    assert(controller->SlotContext == &g_platformMarker);
    g_platformClockReads++;
    *clockHzOut = g_inputClockHz;
    return OS_EOK;
}

static oserr_t
__SlotSetCardPower(
    _In_ SdhciController_t* controller,
    _In_ enum SdBusPower    power)
{
    assert(controller->SlotContext == &g_platformMarker);
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
    assert(controller->SlotContext == &g_platformMarker);
    assert(capabilities->MinimumClockHz == SDHCI_BCM2711_MINIMUM_CLOCK_HZ);
    *valuesOut = g_slotValues;
    return OS_EOK;
}

static void
__OnStall(void)
{
    assert(g_stallCount < MAX_STALLS);
    g_stallList[g_stallCount].EventIndex = g_eventCount;
    g_stallList[g_stallCount].LengthUs = g_nowUs - g_lastStallUs;
    g_stallCount++;
    g_lastStallUs = g_nowUs;
}

static void
__ConfigureHost(
    _In_ uint32_t capabilities)
{
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, capabilities);
    __StoreRegister(SDHCI_CAPABILITIES1, 4, BOARD_CAPABILITIES1);
    g_wordAccessOnly = true;
    // Line resets finish on the first read, so the reset cases read zero.
    g_lineResetBusyReads = 0;

    // The Raspberry Pi 4 slot: four lines, removable, no clock limit, and no
    // card detect, because the board describes the detect line as broken.
    g_variant = g_sdhciBcm2711Variant;
    g_slot = (SdhciSlot_t) {
        .Name = "test",
        .SelectRegisters = __SlotSelectRegisters,
        .GetInputClock = __SlotGetInputClock,
        .SetCardPower = __SlotSetCardPower,
        .DescribeSlot = __SlotDescribe
    };
    g_slotValues = (SdhciSlotValues_t) { .WiredBusWidth = SdBusWidth4, .Removable = 1 };
    g_inputClockHz = 0;
    g_platformSelects = 0;
    g_platformClockReads = 0;
    g_windowLength = 0x100;

    g_stallCount = 0;
    g_lastStallUs = g_nowUs;
    g_onStall = __OnStall;

    memset(g_script, 0, sizeof(g_script));
    g_scriptCount = 0;
    memset(g_commandWords, 0, sizeof(g_commandWords));
    g_commandCount = 0;
    memset(g_replyStatus, 0, sizeof(g_replyStatus));
    memset(g_replyDelayUs, 0, sizeof(g_replyDelayUs));
    g_completions = 0;
    memset(&g_result, 0, sizeof(g_result));

    memset(&g_controller, 0, sizeof(g_controller));
    g_controller.SlotContext = &g_platformMarker;
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
    __StoreRegister(SDHCI_PRESENT_STATE, 4, PRESENT_IDLE);
}

// Starts a fresh stall list from now on.
static uint32_t
__Mark(void)
{
    g_stallCount = 0;
    g_lastStallUs = g_nowUs;
    return g_eventCount;
}

static uint32_t
__WritesSince(
    _In_ uint32_t start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventWrite) {
            count++;
        }
    }
    return count;
}

static uint32_t
__ReadsSince(
    _In_ uint32_t start,
    _In_ uint32_t offset)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventRead && g_events[i].Offset == offset) {
            count++;
        }
    }
    return count;
}

// Exactly these 32-bit writes since start, in this order. Each one is
// followed directly by a wait of delayUs, or by no wait at all when delayUs
// is zero.
static void
__ExpectWrites(
    _In_ uint32_t                    start,
    _In_ const struct ExpectedWrite* expected,
    _In_ uint32_t                    count,
    _In_ uint32_t                    delayUs)
{
    uint32_t found = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind != EventWrite) {
            continue;
        }
        assert(found < count);
        assert(g_events[i].Width == 4);
        assert(g_events[i].Offset == expected[found].Offset);
        assert(g_events[i].Value == expected[found].Value);
        if (delayUs != 0) {
            assert(found < g_stallCount);
            assert(g_stallList[found].EventIndex == i + 1);
            assert(g_stallList[found].LengthUs == delayUs);
        }
        found++;
    }
    assert(found == count);
    assert(g_stallCount == (delayUs != 0 ? count : 0));
}

static void
__ExpectWordAccessesOnly(void)
{
    uint32_t i;

    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventRead || g_events[i].Kind == EventWrite ||
            g_events[i].Kind == EventFastRead || g_events[i].Kind == EventFastWrite) {
            assert(g_events[i].Width == 4);
            assert(g_events[i].Offset % 4 == 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Write delay
// ---------------------------------------------------------------------------

static void
__TestWriteDelay(void)
{
    const struct { uint32_t ClockHz; uint32_t DelayUs; } cases[] = {
        { CLOCK_400K, 10 },
        { CLOCK_ODD, 11 },
        { CLOCK_200K, 20 },
        { 0, 10 },
        { CLOCK_400K + 1, 0 },
        { CLOCK_25M, 0 }
    };
    uint32_t mark;
    size_t   i;

    __SetupBoard();
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(SdhciBcm2711WriteDelayUs(cases[i].ClockHz) == cases[i].DelayUs);
        assert(g_variant.WriteDelayUs(&g_controller, cases[i].ClockHz) == cases[i].DelayUs);

        // Every write path waits the same time after the write: a plain
        // 32-bit write, a read-modify-write, and the block flush and command
        // write of the shadows.
        g_controller.Bus.CardClockHz = cases[i].ClockHz;
        mark = __Mark();
        assert(g_variant.Write32(&g_controller, SDHCI_ARGUMENT, 0x11223344) == OS_EOK);
        assert(g_variant.Write8(&g_controller, SDHCI_HOST_CONTROL1, SDHCI_HOST_CONTROL1_4BIT) == OS_EOK);
        assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_SIZE, BLOCK_SIZE) == OS_EOK);
        assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_COUNT, 2) == OS_EOK);
        assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, READ_MULTIPLE_MODE) == OS_EOK);
        assert(g_variant.Write16(&g_controller, SDHCI_COMMAND, CMD18_BITS) == OS_EOK);
        {
            const struct ExpectedWrite expected[] = {
                { SDHCI_ARGUMENT, 0x11223344 },
                { SDHCI_HOST_CONTROL1, SDHCI_HOST_CONTROL1_4BIT },
                { SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (2U << 16) },
                { SDHCI_TRANSFER_MODE_COMMAND, READ_MULTIPLE_MODE | (CMD18_BITS << 16) }
            };
            __ExpectWrites(mark, expected, 4, cases[i].DelayUs);
        }
    }
    g_controller.Bus.CardClockHz = 0;
    __DestroyAll(&g_controller);
    printf("BCM2711 write delay passed\n");
}

// ---------------------------------------------------------------------------
// Register widths
// ---------------------------------------------------------------------------

static void
__TestReads(void)
{
    uint32_t mark;
    uint32_t i;

    __SetupBoard();

    // Every byte and both halves of a word come from one 32-bit read of the
    // word that holds them.
    __StoreRegister(SDHCI_ARGUMENT, 4, 0xA1B2C3D4U);
    for (i = 0; i < 4; i++) {
        mark = __Mark();
        assert(g_variant.Read8(&g_controller, SDHCI_ARGUMENT + i) == ((0xA1B2C3D4U >> (8 * i)) & 0xFFU));
        assert(__ReadsSince(mark, SDHCI_ARGUMENT) == 1 && g_eventCount == mark + 1);
    }
    mark = __Mark();
    assert(g_variant.Read16(&g_controller, SDHCI_ARGUMENT) == 0xC3D4U);
    assert(g_variant.Read16(&g_controller, SDHCI_ARGUMENT + 2) == 0xA1B2U);
    assert(g_variant.Read32(&g_controller, SDHCI_ARGUMENT) == 0xA1B2C3D4U);
    assert(__ReadsSince(mark, SDHCI_ARGUMENT) == 3 && g_eventCount == mark + 3);

    // The registers the engine reads narrow: host version (upper half of
    // 0xFC), Auto CMD status (lower half of 0x3C), ADMA error status (byte
    // at 0x54) and power control (second byte of 0x28).
    __StoreRegister(SDHCI_SLOT_INTERRUPT_STATUS, 2, 0x0001);
    assert(g_variant.Read16(&g_controller, SDHCI_HOST_VERSION) == SDHCI_SPEC_300);
    __StoreRegister(SDHCI_AUTO_CMD_STATUS, 4, 0xBEEF0012U);
    assert(g_variant.Read16(&g_controller, SDHCI_AUTO_CMD_STATUS) == 0x0012U);
    assert(g_variant.Read16(&g_controller, SDHCI_HOST_CONTROL2) == 0xBEEFU);
    __StoreRegister(SDHCI_ADMA_ERROR_STATUS, 4, 0x77665505U);
    assert(g_variant.Read8(&g_controller, SDHCI_ADMA_ERROR_STATUS) == 0x05U);
    __StoreRegister(SDHCI_HOST_CONTROL1, 4, 0x00000F12U);
    assert(g_variant.Read8(&g_controller, SDHCI_POWER_CONTROL) == 0x0FU);
    __ExpectWordAccessesOnly();
    __DestroyAll(&g_controller);
    printf("BCM2711 reads passed\n");
}

static void
__TestWrites(void)
{
    uint32_t mark;
    uint32_t i;

    __SetupBoard();

    // A byte write replaces its byte and keeps the other three.
    for (i = 0; i < 4; i++) {
        __StoreRegister(SDHCI_HOST_CONTROL1, 4, 0x44332211U);
        mark = __Mark();
        assert(g_variant.Write8(&g_controller, SDHCI_HOST_CONTROL1 + i, 0xAA) == OS_EOK);
        {
            const struct ExpectedWrite expected[] = {
                { SDHCI_HOST_CONTROL1, (0x44332211U & ~(0xFFU << (8 * i))) | (0xAAU << (8 * i)) }
            };
            __ExpectWrites(mark, expected, 1, SdhciBcm2711WriteDelayUs(0));
        }
        assert(__ReadsSince(mark, SDHCI_HOST_CONTROL1) == 1);
    }

    // A half write replaces its half and keeps the other one.
    __StoreRegister(SDHCI_INTERRUPT_ENABLE, 4, 0x12345678U);
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_ERROR_INTERRUPT_ENABLE, 0xBEEF) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_NORMAL_INTERRUPT_ENABLE, 0xCAFE) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_INTERRUPT_ENABLE, 0xBEEF5678U },
            { SDHCI_INTERRUPT_ENABLE, 0xBEEFCAFEU }
        };
        __ExpectWrites(mark, expected, 2, SdhciBcm2711WriteDelayUs(0));
    }

    // Host control 2 shares its word with the read-only Auto CMD status.
    __StoreRegister(SDHCI_AUTO_CMD_STATUS, 4, 0x00000055U);
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_HOST_CONTROL2, 0x1234) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = { { SDHCI_AUTO_CMD_STATUS, 0x12340055U } };
        __ExpectWrites(mark, expected, 1, SdhciBcm2711WriteDelayUs(0));
    }
    assert(__LoadRegister(SDHCI_HOST_CONTROL2, 2) == 0x1234U);

    // Interrupt status is write-one-to-clear: it is never read for a narrow
    // write, and the other bytes are written as zero.
    __StoreRegister(SDHCI_INTERRUPT_STATUS, 4, 0xFFFFFFFFU);
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_ERROR_INTERRUPT_STATUS, 0x0001) == OS_EOK);
    assert(g_variant.Write8(&g_controller, SDHCI_NORMAL_INTERRUPT_STATUS, 0x02) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_INTERRUPT_STATUS, 0x00010000U },
            { SDHCI_INTERRUPT_STATUS, 0x00000002U }
        };
        __ExpectWrites(mark, expected, 2, SdhciBcm2711WriteDelayUs(0));
    }
    assert(__ReadsSince(mark, SDHCI_INTERRUPT_STATUS) == 0);
    assert(__LoadRegister(SDHCI_INTERRUPT_STATUS, 4) == 0xFFFEFFFDU);

    // Widths and offsets outside the register contract are refused before
    // any access.
    mark = __Mark();
    assert(g_variant.Write8(&g_controller, SDHCI_BLOCK_COUNT, 1) == OS_EINVALPARAMS);
    assert(g_variant.Write8(&g_controller, SDHCI_COMMAND + 1, 1) == OS_EINVALPARAMS);
    assert(g_variant.Write16(&g_controller, SDHCI_POWER_CONTROL, 1) == OS_EINVALPARAMS);
    assert(g_variant.Write32(&g_controller, SDHCI_TIMEOUT_CONTROL, 1) == OS_EINVALPARAMS);
    assert(g_eventCount == mark);
    __ExpectWordAccessesOnly();
    __DestroyAll(&g_controller);
    printf("BCM2711 writes passed\n");
}

static void
__TestSoftwareReset(void)
{
    uint32_t clockWord;
    uint32_t mark;

    __SetupBoard();

    // A line reset is a 32-bit write to 0x2C that carries the clock control
    // and timeout bytes it just read (the stable bit reads back set and is
    // read only).
    __StoreRegister(SDHCI_CLOCK_CONTROL, 2, 0x4005U);
    __StoreRegister(SDHCI_TIMEOUT_CONTROL, 1, SDHCI_TIMEOUT_VALUE_MAXIMUM);
    clockWord = 0x4007U | ((uint32_t)SDHCI_TIMEOUT_VALUE_MAXIMUM << 16);
    mark = __Mark();
    assert(g_variant.Write8(&g_controller, SDHCI_SOFTWARE_RESET, SDHCI_RESET_COMMAND | SDHCI_RESET_DATA) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_CLOCK_CONTROL, clockWord | ((uint32_t)(SDHCI_RESET_COMMAND | SDHCI_RESET_DATA) << 24) }
        };
        __ExpectWrites(mark, expected, 1, SdhciBcm2711WriteDelayUs(0));
    }
    assert(__LoadRegister(SDHCI_CLOCK_CONTROL, 2) == 0x4005U);
    assert(__LoadRegister(SDHCI_TIMEOUT_CONTROL, 1) == SDHCI_TIMEOUT_VALUE_MAXIMUM);
    assert(g_variant.Read8(&g_controller, SDHCI_SOFTWARE_RESET) == 0);

    // While a reset bit still reads set, a write of a neighbour in the same
    // word writes the reset byte as zero, so no second reset starts.
    g_registers[SDHCI_SOFTWARE_RESET] = SDHCI_RESET_ALL;
    g_resetBusyLeft = FOREVER;
    mark = __Mark();
    assert(g_variant.Write8(&g_controller, SDHCI_TIMEOUT_CONTROL, 0x0A) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = { { SDHCI_CLOCK_CONTROL, 0x4007U | (0x0AU << 16) } };
        __ExpectWrites(mark, expected, 1, SdhciBcm2711WriteDelayUs(0));
    }
    g_resetBusyLeft = 0;

    // A full reset through the engine: one 32-bit write at 0x2C with the
    // reset bit in the top byte and the clock and timeout bytes as read.
    __StoreRegister(SDHCI_TIMEOUT_CONTROL, 1, SDHCI_TIMEOUT_VALUE_MAXIMUM);
    mark = __Mark();
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(g_events[mark].Kind == EventRead && g_events[mark].Offset == SDHCI_CLOCK_CONTROL);
    assert(g_events[mark + 1].Kind == EventWrite && g_events[mark + 1].Offset == SDHCI_CLOCK_CONTROL);
    assert(g_events[mark + 1].Width == 4);
    assert(g_events[mark + 1].Value == (clockWord | ((uint32_t)SDHCI_RESET_ALL << 24)));
    __ExpectWordAccessesOnly();
    __DestroyAll(&g_controller);
    printf("BCM2711 software reset passed\n");
}

// ---------------------------------------------------------------------------
// Shadows
// ---------------------------------------------------------------------------

static void
__TestShadows(void)
{
    uint32_t mark;

    __SetupBoard();
    g_controller.Bus.CardClockHz = CLOCK_400K;
    __StoreRegister(SDHCI_BLOCK_SIZE_COUNT, 4, 0x00050040U);
    __StoreRegister(SDHCI_TRANSFER_MODE_COMMAND, 4, 0x0D1A0000U);

    // Block size, block count, transfer mode and command give one write to
    // 0x04 and then one to 0x0C. The halves read back from the shadows.
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_SIZE, BLOCK_SIZE) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_COUNT, 8) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, READ_MULTIPLE_MODE) == OS_EOK);
    assert(__WritesSince(mark) == 0);
    assert(g_variant.Read16(&g_controller, SDHCI_BLOCK_SIZE) == BLOCK_SIZE);
    assert(g_variant.Read16(&g_controller, SDHCI_BLOCK_COUNT) == 8);
    assert(g_variant.Read8(&g_controller, SDHCI_BLOCK_SIZE + 1) == (BLOCK_SIZE >> 8));
    assert(g_variant.Read16(&g_controller, SDHCI_TRANSFER_MODE) == READ_MULTIPLE_MODE);
    assert(__ReadsSince(mark, SDHCI_BLOCK_SIZE_COUNT) == 1);
    assert(__ReadsSince(mark, SDHCI_TRANSFER_MODE_COMMAND) == 1);
    // A 32-bit read is always the hardware.
    assert(g_variant.Read32(&g_controller, SDHCI_BLOCK_SIZE_COUNT) == 0x00050040U);
    assert(g_variant.Write16(&g_controller, SDHCI_COMMAND, CMD18_BITS) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (8U << 16) },
            { SDHCI_TRANSFER_MODE_COMMAND, READ_MULTIPLE_MODE | (CMD18_BITS << 16) }
        };
        __ExpectWrites(mark, expected, 2, 10);
    }
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);

    // Transfer mode without a command writes nothing. A command with no
    // block values collected writes only the transfer word.
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, 0) == OS_EOK);
    assert(__WritesSince(mark) == 0 && __Context()->Transfer.Pending);
    assert(g_variant.Write16(&g_controller, SDHCI_COMMAND, CMD13_BITS) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = { { SDHCI_TRANSFER_MODE_COMMAND, CMD13_BITS << 16 } };
        __ExpectWrites(mark, expected, 1, 10);
    }

    // A direct 32-bit write of the block word replaces the shadow, so the
    // next command does not write the stale value.
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_SIZE, 64) == OS_EOK);
    assert(g_variant.Write32(&g_controller, SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (1U << 16)) == OS_EOK);
    assert(!__Context()->Block.Pending);
    assert(g_variant.Read16(&g_controller, SDHCI_BLOCK_SIZE) == BLOCK_SIZE);
    assert(g_variant.Write16(&g_controller, SDHCI_COMMAND, CMD13_BITS) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (1U << 16) },
            { SDHCI_TRANSFER_MODE_COMMAND, CMD13_BITS << 16 }
        };
        __ExpectWrites(mark, expected, 2, 10);
    }

    // A direct 32-bit write of transfer mode and command starts a command:
    // collected block values go first, the transfer shadow is replaced.
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_SIZE, BLOCK_SIZE) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_COUNT, 3) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, SDHCI_TRANSFER_READ) == OS_EOK);
    assert(g_variant.Write32(&g_controller, SDHCI_TRANSFER_MODE_COMMAND,
        READ_MULTIPLE_MODE | (CMD18_BITS << 16)) == OS_EOK);
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);
    {
        const struct ExpectedWrite expected[] = {
            { SDHCI_BLOCK_SIZE_COUNT, BLOCK_SIZE | (3U << 16) },
            { SDHCI_TRANSFER_MODE_COMMAND, READ_MULTIPLE_MODE | (CMD18_BITS << 16) }
        };
        __ExpectWrites(mark, expected, 2, 10);
    }
    g_controller.Bus.CardClockHz = 0;

    // A CMD and DAT line reset drops both shadows, and so does a full reset.
    // The command after it writes only the transfer word, built from the
    // hardware.
    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_SIZE, 64) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, SDHCI_TRANSFER_DMA) == OS_EOK);
    assert(SdhciRecoveryResetLines(&g_controller) == OS_EOK);
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);
    assert(strstr(g_log, "line reset dropped") != NULL);
    __StoreRegister(SDHCI_TRANSFER_MODE_COMMAND, 4, 0);
    mark = __Mark();
    assert(g_variant.Write16(&g_controller, SDHCI_COMMAND, CMD13_BITS) == OS_EOK);
    {
        const struct ExpectedWrite expected[] = { { SDHCI_TRANSFER_MODE_COMMAND, CMD13_BITS << 16 } };
        __ExpectWrites(mark, expected, 1, 10);
    }

    assert(g_variant.Write16(&g_controller, SDHCI_BLOCK_COUNT, 4) == OS_EOK);
    assert(g_variant.Write16(&g_controller, SDHCI_TRANSFER_MODE, SDHCI_TRANSFER_DMA) == OS_EOK);
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);

    // RestoreConfiguration drops them as well, for a reset that did not go
    // through the 8-bit write hook.
    __Context()->Block.Pending = true;
    __Context()->Transfer.Pending = true;
    assert(g_variant.RestoreConfiguration(&g_controller) == OS_EOK);
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);
    __ExpectWordAccessesOnly();
    __DestroyAll(&g_controller);
    printf("BCM2711 shadows passed\n");
}

// ---------------------------------------------------------------------------
// Capabilities
// ---------------------------------------------------------------------------

static void
__TestCapabilities(void)
{
    uint32_t state;

    // The real board vector with the default wiring of four lines.
    __SetupBoard();
    assert(g_controller.RawCapabilities.Version == SDHCI_SPEC_300);
    assert(g_controller.RawCapabilities.Capabilities1 == BOARD_CAPABILITIES1);
    assert(g_controller.Capabilities.BaseClockHz == BOARD_BASE_CLOCK_HZ);
    assert(g_controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    assert(g_controller.Capabilities.MinimumClockHz == SDHCI_BCM2711_MINIMUM_CLOCK_HZ);
    assert(g_controller.Dma.Format == SdhciAdmaFormat32);
    assert(g_openLimit == UINT32_MAX);
    assert(g_controller.Host.Limits.MinimumClockHz == SDHCI_BCM2711_MINIMUM_CLOCK_HZ);
    assert(g_controller.Host.Limits.MaximumClockHz == BOARD_BASE_CLOCK_HZ);
    assert(g_controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4));
    assert(g_controller.Host.Limits.MaximumBlockSize == 2048);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_AUTO_CMD12_REQUIRED) != 0);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_HIGH_SPEED) != 0);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_WRITE_PROTECT) == 0);
    assert((g_sdhciBcm2711Variant.Flags & SDHCI_VARIANT_ADMA2_64) == 0);
    assert((g_controller.SlotValues.Flags & SDHCI_SLOT_CARD_DETECT) == 0);
    assert(SdhciControllerGetCardState(&g_controller, &state) == OS_ENOTSUPPORTED);
    // The input clock is read once for the log line, even though the
    // capabilities carry a base clock. A platform rate of zero is shown and
    // not compared.
    assert(g_platformSelects == 1 && g_platformClockReads == 1);
    assert(strstr(g_log, "base clock 100000000 Hz in the capabilities, platform input clock 0 Hz") != NULL);
    assert(strstr(g_log, "differ") == NULL);
    // The slot type reads embedded, but the board says removable.
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_REMOVABLE) != 0);
    __ExpectWordAccessesOnly();
    __DestroyAll(&g_controller);

    // A board that says eight lines gets them, because the host has them.
    // One wired line leaves only one.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotValues.WiredBusWidth = SdBusWidth8;
    assert(__PrepareBoard() == OS_EOK);
    assert(g_controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4 | SD_HOST_BUS_WIDTH_8));
    __DestroyAll(&g_controller);
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotValues.WiredBusWidth = SdBusWidth1;
    assert(__PrepareBoard() == OS_EOK);
    assert(g_controller.Host.Limits.BusWidths == SD_HOST_BUS_WIDTH_1);
    __DestroyAll(&g_controller);
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotValues.WiredBusWidth = (enum SdBusWidth)2;
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "wired bus width 2") != NULL);
    __DestroyAll(&g_controller);

    // A host that reports 64-bit ADMA2 still uses the 32-bit format.
    __ConfigureHost(BOARD_CAPABILITIES | SDHCI_CAPS_64BIT_V3);
    assert(__PrepareBoard() == OS_EOK);
    assert(g_controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    assert(g_controller.Dma.Format == SdhciAdmaFormat32);
    __DestroyAll(&g_controller);
    printf("BCM2711 capabilities passed\n");
}

// ---------------------------------------------------------------------------
// Slot hooks
// ---------------------------------------------------------------------------

static void
__TestSlotHooks(void)
{
    // A slot without an input clock: a host that reports its base clock runs
    // with it, and one that reports none is refused.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slot.GetInputClock = NULL;
    assert(__PrepareBoard() == OS_EOK);
    assert(g_platformClockReads == 0 && g_controller.Capabilities.BaseClockHz == BOARD_BASE_CLOCK_HZ);
    __DestroyAll(&g_controller);
    __ConfigureHost(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_slot.GetInputClock = NULL;
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "capabilities report no base clock and the test slot has no other source") != NULL);
    __DestroyAll(&g_controller);

    // A slot or variant with a missing required hook is refused before
    // anything is called, and so is a controller that already has state.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slot.DescribeSlot = NULL;
    assert(__PrepareBoard() == OS_EINVALPARAMS);
    assert(g_platformSelects == 0 && g_eventCount == 0);
    __ConfigureHost(BOARD_CAPABILITIES);
    g_variant.RestoreConfiguration = NULL;
    assert(__PrepareBoard() == OS_EINVALPARAMS);
    __ConfigureHost(BOARD_CAPABILITIES);
    g_controller.VariantContext = &g_platformMarker;
    assert(__PrepareBoard() == OS_EBUSY);
    g_controller.VariantContext = NULL;

    // A window smaller than the register map is refused by the engine.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_windowLength = 0x80;
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "test register window is not a memory window of at least 256 bytes") != NULL);
    __DestroyAll(&g_controller);

    // A host without a base clock uses the slot's input clock, read once.
    // The variant state is zeroed and freed by teardown.
    __ConfigureHost(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_inputClockHz = 50000000U;
    assert(__PrepareBoard() == OS_EOK);
    assert(g_platformSelects == 1 && g_platformClockReads == 1);
    assert(g_controller.Capabilities.BaseClockHz == 50000000U);
    assert(!__Context()->Block.Pending && !__Context()->Transfer.Pending);
    assert(g_slot.SetCardPower(&g_controller, SdBusPowerOn) == OS_EOK);
    assert(__Find(EventCardPower, 0, SdBusPowerOn, 0) >= 0);
    __DestroyAll(&g_controller);
    assert(g_controller.VariantContext == NULL);

    // A slot that reports no rate is refused.
    __ConfigureHost(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_inputClockHz = 0;
    assert(__PrepareBoard() == OS_ENOTSUPPORTED);
    __DestroyAll(&g_controller);

    // The board values: a soldered device, a clock limit, and a slot whose
    // present state register reports card detect and write protect.
    __ConfigureHost(BOARD_CAPABILITIES);
    g_slotValues.Removable = 0;
    g_slotValues.MaximumClockHz = 50000000U;
    assert(__PrepareBoard() == OS_EOK);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_REMOVABLE) == 0);
    assert(g_controller.Host.Limits.MaximumClockHz == 50000000U);
    __DestroyAll(&g_controller);
    printf("BCM2711 slot hooks passed\n");
}

// ---------------------------------------------------------------------------
// Whole engine: scripted host, fast handler and worker driving
// ---------------------------------------------------------------------------

static void
__AddStatus(
    _In_ uint64_t atUs,
    _In_ uint32_t status)
{
    assert(g_scriptCount < MAX_SCRIPT);
    g_script[g_scriptCount++] = (struct ScriptEvent) { atUs, status, true };
}

static void
__LatchDue(void)
{
    uint32_t enabled = __LoadRegister(SDHCI_INTERRUPT_ENABLE, 4);
    uint32_t i;

    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Pending && g_script[i].AtUs <= g_nowUs) {
            g_script[i].Pending = false;
            __StoreRegister(SDHCI_INTERRUPT_STATUS, 4,
                __LoadRegister(SDHCI_INTERRUPT_STATUS, 4) | (g_script[i].Status & enabled));
        }
    }
}

static uint64_t
__NextScriptUs(void)
{
    uint64_t next = SDHCI_WAKE_NEVER;
    uint32_t i;

    for (i = 0; i < g_scriptCount; i++) {
        if (g_script[i].Pending && g_script[i].AtUs < next) {
            next = g_script[i].AtUs;
        }
    }
    return next;
}

static void
__OnRead(
    _In_ uint32_t offset)
{
    (void)offset;
    __LatchDue();
}

static void
__OnWrite(
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    uint32_t i;

    // Only the 32-bit write at 0x0C starts a command on this host.
    if (offset != SDHCI_TRANSFER_MODE_COMMAND) {
        return;
    }
    assert(width == 4);
    assert(g_commandCount < MAX_COMMANDS);
    g_commandWords[g_commandCount] = value;
    for (i = 0; i < 2; i++) {
        if (g_replyStatus[g_commandCount][i] != 0) {
            __AddStatus(g_nowUs + g_replyDelayUs[g_commandCount][i], g_replyStatus[g_commandCount][i]);
        }
    }
    g_commandCount++;
}

static void
__Plan(
    _In_ uint32_t command,
    _In_ uint32_t index,
    _In_ uint32_t delayUs,
    _In_ uint32_t status)
{
    g_replyDelayUs[command][index] = delayUs;
    g_replyStatus[command][index] = status;
}

static size_t
__FastRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    uint32_t value;

    assert(ioSpace == &g_controller.Registers);
    assert(length == 4);
    value = __LoadRegister((uint32_t)offset, 4);
    __Record(EventFastRead, (uint32_t)offset, 4, value);
    return value;
}

static oserr_t
__FastWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(ioSpace == &g_controller.Registers);
    assert(length == 4);
    __Record(EventFastWrite, (uint32_t)offset, 4, (uint32_t)value);
    if (!__StoreStatusWrite((uint32_t)offset, 4, (uint32_t)value)) {
        __StoreRegister((uint32_t)offset, 4, (uint32_t)value);
    }
    return OS_EOK;
}

static oserr_t
__FastSignal(
    _In_ uuid_t handle)
{
    assert(handle == EVENT_DESCRIPTOR);
    __Record(EventSignal, 0, 0, 0);
    g_signals++;
    return OS_EOK;
}

static void
__FastTrace(
    _In_ const char* format, ...)
{
    (void)format;
}

static InterruptFunctionTable_t g_functionTable = {
    .ReadIoSpace = __FastRead,
    .WriteIoSpace = __FastWrite,
    .EventSignal = __FastSignal,
    .Trace = __FastTrace
};

static void
__DeliverInterrupt(void)
{
    uint32_t asserted;

    // The line is up while a status bit that may signal is set.
    asserted = __LoadRegister(SDHCI_INTERRUPT_STATUS, 4) & __LoadRegister(SDHCI_INTERRUPT_SIGNAL, 4);
    if (!g_interruptEnabled || asserted == 0) {
        return;
    }
    assert(g_handlerResources.Handler == SdhciFastInterrupt);
    if (g_handlerResources.Handler(&g_functionTable, &g_handlerResources) == IRQSTATUS_HANDLED) {
        SdhciInterruptDeferred(&g_controller);
    }
}

static void
__Run(void)
{
    uint64_t    limit = g_nowUs + RUN_LIMIT_US;
    uint64_t    next;
    SdhciWake_t wake;

    // Like the worker loop: step, then wait for the wake or for the next
    // thing the host does, whichever comes first.
    for (;;) {
        __LatchDue();
        __DeliverInterrupt();
        SdhciWorkerRunOnce(&g_controller, &wake);
        if (g_controller.Request.Active == NULL) {
            return;
        }
        next = wake.AtUs;
        if (__NextScriptUs() < next) {
            next = __NextScriptUs();
        }
        assert(next != SDHCI_WAKE_NEVER);
        if (next > g_nowUs) {
            g_nowUs = next;
        }
        assert(g_nowUs < limit);
    }
}

static void
__Complete(
    _In_ SdRequest_t* request)
{
    g_completions++;
    g_result = request->Result;
}

static SdRequest_t
__Command13(void)
{
    SdRequest_t request;

    memset(&request, 0, sizeof(request));
    request.Opcode = 13;
    request.Argument = 0x12340000;
    request.ResponseType = SdResponseR1;
    request.CommandTimeoutUs = 5000;
    request.Complete = __Complete;
    return request;
}

static SdRequest_t
__ReadMultiple(
    _In_ uint32_t blocks)
{
    SdRequest_t request = __Command13();

    request.Opcode = 18;
    request.Argument = 0x1000;
    request.Direction = SdDataRead;
    request.StopPolicy = SdStopAutoCmd12;
    request.BlockSize = BLOCK_SIZE;
    request.BlockCount = blocks;
    request.DataTimeoutUs = 100000;
    g_segments[0] = (struct DMASegment) { BUFFER_ADDRESS, (uint64_t)BLOCK_SIZE * blocks };
    request.Data.Segments = g_segments;
    request.Data.SegmentCount = 1;
    request.Data.Length = (uint64_t)BLOCK_SIZE * blocks;
    return request;
}

static int
__FindWrite(
    _In_ uint32_t start,
    _In_ uint32_t offset)
{
    return __Find(EventWrite, offset, ANY_VALUE, start);
}

static void
__ExpectNoSignals(
    _In_ uint32_t start)
{
    uint32_t i;

    // While a write delay applies the signal enable register is never set,
    // and the fast handler never writes.
    for (i = start; i < g_eventCount; i++) {
        assert(g_events[i].Kind != EventFastWrite);
        if (g_events[i].Kind == EventWrite && g_events[i].Offset == SDHCI_INTERRUPT_SIGNAL) {
            assert(g_events[i].Value == 0);
        }
    }
}

static void
__TestEngine(void)
{
    SdBusSettings_t settings;
    SdRequest_t     request;
    uint32_t        clockHz;
    uint32_t        table;
    uint32_t        mark;
    uint32_t        i;
    int             timeout;
    int             blocks;
    int             argument;
    int             command;

    // Setup, interrupt and start, then a full reset on demand.
    __SetupBoard();
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    assert(SdhciControllerStart(&g_controller) == OS_EOK);
    assert(SdhciControllerReset(&g_controller) == OS_EOK);
    g_onRead = __OnRead;
    g_onWrite = __OnWrite;
    table = (uint32_t)g_controller.Dma.Table.DeviceAddress;

    // Card setup clock: 100 MHz / 250 is exactly 400 kHz, which needs the
    // 10 microsecond write delay, so requests are polled.
    settings = (SdBusSettings_t) { CLOCK_400K, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn, SdBusDeviceSd };
    assert(SdhciControllerSetBus(&g_controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == CLOCK_400K && g_controller.Bus.CardClockHz == CLOCK_400K);
    assert(__Find(EventCardPower, 0, SdBusPowerOn, 0) >= 0);
    assert(SdhciInterruptUsePolling(&g_controller));

    // A command without data, polled.
    mark = __Mark();
    request = __Command13();
    __Plan(0, 0, 150, CC);
    assert(g_controller.Host.Operations->Submit(&g_controller.Host, &request) == OS_EOK);
    __Run();
    assert(g_completions == 1 && g_result.Status == OS_EOK);
    assert(g_commandWords[0] == (CMD13_BITS << 16));
    argument = __FindWrite(mark, SDHCI_ARGUMENT);
    command = __FindWrite(mark, SDHCI_TRANSFER_MODE_COMMAND);
    assert(argument > 0 && command > argument);
    assert(__FindWrite(mark, SDHCI_BLOCK_SIZE_COUNT) < 0);
    __ExpectNoSignals(mark);
    // The worker read the status itself and acknowledged command complete.
    assert(__Find(EventWrite, SDHCI_INTERRUPT_STATUS, CC, (uint32_t)command) > command);
    assert(g_stallCount > 0);

    // A multiple-block read with Auto CMD12, polled. Programming order:
    // descriptor table address, timeout (a 32-bit write at 0x2C that keeps
    // the running clock), block word, argument, then transfer mode and
    // command in one write.
    mark = __Mark();
    request = __ReadMultiple(4);
    __Plan(1, 0, 150, CC);
    __Plan(1, 1, 900, TC);
    assert(g_controller.Host.Operations->Submit(&g_controller.Host, &request) == OS_EOK);
    __Run();
    assert(g_completions == 2 && g_result.Status == OS_EOK);
    assert(g_result.ConfirmedBytes == 4 * BLOCK_SIZE);
    assert(g_commandWords[1] == (READ_MULTIPLE_MODE | (CMD18_BITS << 16)));
    timeout = __FindWrite(mark, SDHCI_CLOCK_CONTROL);
    blocks = __FindWrite(mark, SDHCI_BLOCK_SIZE_COUNT);
    argument = __FindWrite(mark, SDHCI_ARGUMENT);
    command = __FindWrite(mark, SDHCI_TRANSFER_MODE_COMMAND);
    assert(__Find(EventWrite, SDHCI_ADMA_ADDRESS, table, mark) > 0);
    assert(__FindWrite(mark, SDHCI_ADMA_ADDRESS) < timeout);
    assert(timeout < blocks && blocks < argument && argument < command);
    // The clock half is written back as read. The read-only stable bit may
    // read either way, so it is left out of the comparison.
    assert((g_events[timeout].Value & ~SDHCI_CLOCK_INTERNAL_STABLE) ==
        (__LoadRegister(SDHCI_CLOCK_CONTROL, 2) | ((uint32_t)SDHCI_TIMEOUT_VALUE_MAXIMUM << 16)));
    assert((g_events[timeout].Value & SDHCI_CLOCK_CARD_ENABLE) != 0);
    assert(g_events[blocks].Value == (BLOCK_SIZE | (4U << 16)));
    __ExpectNoSignals(mark);

    // Default speed at 25 MHz and four lines: no delay, so the same read
    // completes by interrupt through the fast handler.
    settings = (SdBusSettings_t) { CLOCK_25M, SdBusWidth4, SdBusTimingDefault, SdBusPowerOn, SdBusDeviceSd };
    assert(SdhciControllerSetBus(&g_controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == CLOCK_25M && !SdhciInterruptUsePolling(&g_controller));
    mark = __Mark();
    request = __ReadMultiple(8);
    __Plan(2, 0, 100, CC);
    __Plan(2, 1, 400, TC);
    assert(g_controller.Host.Operations->Submit(&g_controller.Host, &request) == OS_EOK);
    __Run();
    assert(g_completions == 3 && g_result.Status == OS_EOK);
    assert(g_result.ConfirmedBytes == 8 * BLOCK_SIZE);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_SIGNAL, SDHCI_INT_HANDLED, mark) > 0);
    assert(__Find(EventFastWrite, SDHCI_INTERRUPT_STATUS, ANY_VALUE, mark) > 0);
    assert(g_signals > 0);
    assert(g_stallCount == 0);

    // No card detect on this slot: no card detect bit was ever enabled, in
    // either mask, and the stored masks hold only the request bits.
    assert(g_controller.CardDetect.State == SdhciCardDetectOff);
    assert(g_controller.Interrupts.StatusEnable == SDHCI_INT_HANDLED);
    assert(g_controller.Interrupts.SignalEnable == 0);
    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventWrite &&
            (g_events[i].Offset == SDHCI_INTERRUPT_ENABLE || g_events[i].Offset == SDHCI_INTERRUPT_SIGNAL)) {
            assert((g_events[i].Value & SDHCI_INT_CARD_DETECT) == 0);
        }
    }
    assert(g_controller.Host.Operations->SetEventHandler(&g_controller.Host, NULL, NULL) == OS_EOK);

    __ExpectWordAccessesOnly();
    g_onRead = NULL;
    g_onWrite = NULL;
    __DestroyAll(&g_controller);
    printf("BCM2711 engine requests passed\n");
}

int
main(void)
{
    __TestWriteDelay();
    __TestReads();
    __TestWrites();
    __TestSoftwareReset();
    __TestShadows();
    __TestCapabilities();
    __TestSlotHooks();
    __TestEngine();
    printf("BCM2711 variant tests passed\n");
    return 0;
}
