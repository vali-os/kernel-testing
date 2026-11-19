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
 * Host test for the SDHCI engine setup (controller.c) and the standard PCI
 * integration (integrations/standard.c). The fakes of every outside call are
 * in sdhci_fake_host.inc. The cases check results and the order of the
 * recorded calls and register accesses.
 */

#include "sdhci_fake_host.inc"

static uint32_t
__Count(
    _In_ enum EventKind kind)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind) {
            count++;
        }
    }
    return count;
}

static int
__FirstRegisterAccess(void)
{
    uint32_t i;

    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventRead || g_events[i].Kind == EventWrite) {
            return (int)i;
        }
    }
    return -1;
}

static void
__PrepareAndStart(
    _Out_ SdhciController_t* controller)
{
    __Prepare(controller);
    assert(SdhciControllerStart(controller) == OS_EOK);
}

// ---------------------------------------------------------------------------
// Setup order and failures
// ---------------------------------------------------------------------------

static void
__TestSetupOrder(void)
{
    SdhciController_t controller;
    int               prepare;
    int               firstAccess;
    int               resetWrite;
    int               open;
    int               table;
    int               control;
    int               start;
    int               quiesce;
    int               firstFree;
    int               secondFree;
    int               close;
    int               release;

    __DefaultHost();
    __Prepare(&controller);

    prepare = __Find(EventPrepare, 0, ANY_VALUE, 0);
    firstAccess = __FirstRegisterAccess();
    resetWrite = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, 0);
    open = __Find(EventDmaOpen, 0, ANY_VALUE, 0);
    table = __Find(EventDmaAllocate, 0, ANY_VALUE, 0);
    control = __Find(EventDmaAllocate, 0, ANY_VALUE, (uint32_t)table + 1);
    assert(prepare == 0 && firstAccess > prepare);
    assert(resetWrite > firstAccess && open > resetWrite);
    assert(table > open && control > table);
    assert(__Count(EventStart) == 0);
    assert(!g_started);

    // Both allocations are continuous, 8-byte aligned and sized as designed.
    assert(g_allocationCount == 2);
    assert(g_allocations[0].Length == SDHCI_ADMA_TABLE_REQUEST_SIZE);
    assert(g_allocations[1].Length >= 512);
    assert(g_allocations[0].Contiguous && g_allocations[1].Contiguous);
    assert(g_allocations[0].Alignment == 8 && g_allocations[1].Alignment == 8);
    assert(g_openLimit == UINT32_MAX);

    // The published host routes the operations back to this controller.
    assert(controller.Host.Context == &controller);
    assert(controller.Host.DMAContext == controller.Dma.Context);
    assert(controller.Host.ControlBuffer == controller.Dma.ControlBuffer);
    assert(controller.Host.Operations->SetBus != NULL);
    assert(controller.Host.Operations->Reset != NULL);
    assert(controller.Host.Operations->GetCardState != NULL);
    assert(controller.Host.Operations->Submit != NULL && controller.Host.Operations->Stop != NULL);
    assert(controller.Host.Operations->Schedule != NULL && controller.Host.Operations->CancelWork != NULL);

    assert(SdhciControllerStart(&controller) == OS_EOK);
    start = __Find(EventStart, 0, ANY_VALUE, 0);
    assert(start > control);
    assert(SdhciControllerStart(&controller) == OS_EINVALPARAMS);

    // Teardown: reset the started host, then free, close, release, stop.
    __DestroyAll(&controller);
    quiesce = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, (uint32_t)start);
    firstFree = __Find(EventDmaFree, 0, ANY_VALUE, 0);
    secondFree = __Find(EventDmaFree, 0, ANY_VALUE, (uint32_t)firstFree + 1);
    close = __Find(EventDmaClose, 0, ANY_VALUE, 0);
    release = __Find(EventRelease, 0, ANY_VALUE, 0);
    assert(quiesce > start && firstFree > quiesce && secondFree > firstFree);
    assert(close > secondFree && release > close);
    assert(__Find(EventStop, 0, ANY_VALUE, 0) > release);
    assert(__Count(EventDmaFree) == 2 && __Count(EventDmaClose) == 1);
    assert(__Count(EventRelease) == 1 && __Count(EventStop) == 1);

    // A second teardown has nothing left to do.
    assert(SdhciControllerDestroy(&controller) == OS_EOK);
    assert(__Count(EventStop) == 1);
    printf("SDHCI setup order passed\n");
}

// Rank of each event kind in a successful setup. A failed step must not be
// followed by any event of a higher rank.
static int
__Rank(
    _In_ enum EventKind kind)
{
    switch (kind) {
        case EventPrepare: return 0;
        case EventResources: return 1;
        case EventIoctl: return 2;
        case EventAcquire: return 3;
        case EventRead:
        case EventWrite: return 4;
        case EventDmaOpen: return 5;
        case EventDmaAllocate: return 6;
        default: return 100;
    }
}

static int
__FailRank(
    _In_ enum FailPoint point)
{
    switch (point) {
        case FailPrepare: return 0;
        case FailResources: return 1;
        case FailIoctl: return 2;
        case FailAcquire: return 3;
        case FailResetTimeout: return 4;
        case FailDmaOpen:
        case FailDmaOpenCleanup: return 5;
        default: return 6;
    }
}

static void
__TestSetupFailures(void)
{
    SdhciController_t controller;
    enum FailPoint    point;
    uint32_t          i;
    int               maximumRank;
    bool              prepared;
    bool              opened;

    for (point = FailPrepare; point < FailPointCount; point++) {
        __DefaultHost();
        g_failPoint = point;
        if (point == FailResetTimeout) {
            g_resetBusyReads = FOREVER;
        }

        memset(&controller, 0, sizeof(controller));
        assert(SdhciControllerPrepare(&controller, &g_device.Base, &g_integration) != OS_EOK);

        // Nothing happened after the failed step.
        maximumRank = 0;
        for (i = 0; i < g_eventCount; i++) {
            if (__Rank(g_events[i].Kind) > maximumRank) {
                maximumRank = __Rank(g_events[i].Kind);
            }
        }
        assert(maximumRank == __FailRank(point));
        assert(__Count(EventStart) == 0);
        if (point == FailAllocateTable || point == FailTableSegments) {
            assert(__Count(EventDmaAllocate) == 1);
        }
        assert(controller.Failed == (point == FailResetTimeout));
        assert(SdhciControllerStart(&controller) != OS_EOK);
        assert(__Count(EventStart) == 0);

        // Teardown releases exactly what was set up, the cleanup-only
        // context of a failed DMAOpen included.
        prepared = point != FailPrepare;
        opened = point >= FailDmaOpenCleanup;
        __DestroyAll(&controller);
        assert(__Count(EventStop) == (prepared ? 1U : 0U));
        assert(__Count(EventRelease) == (point > FailAcquire ? 1U : 0U));
        assert(__Count(EventDmaClose) == (opened ? 1U : 0U));
        assert(__Count(EventDmaFree) == __Count(EventDmaAllocate) -
            ((point == FailAllocateTable || point == FailAllocateControl) ? 1U : 0U));
    }
    printf("SDHCI setup failures passed\n");
}

static void
__TestTeardownRetry(void)
{
    SdhciController_t controller;

    // A failing free keeps its buffer and everything after it. The other,
    // independent buffer is still freed.
    __DefaultHost();
    __PrepareAndStart(&controller);
    g_failFrees = 1;
    assert(SdhciControllerDestroy(&controller) == OS_EDEVFAULT);
    assert(g_liveBuffers == 1 && controller.Dma.ControlBuffer != NULL);
    assert(controller.Dma.TableBuffer == NULL);
    assert(g_liveContexts == 1 && g_acquired && g_prepared);
    assert(__Count(EventDmaClose) == 0 && __Count(EventRelease) == 0 && __Count(EventStop) == 0);
    __DestroyAll(&controller);
    assert(__Count(EventDmaFree) == 3);
    assert(__CountWrites(SDHCI_SOFTWARE_RESET, (uint32_t)__Find(EventStart, 0, ANY_VALUE, 0)) == 1);

    // A failing close keeps the context and the register window.
    __DefaultHost();
    __PrepareAndStart(&controller);
    g_failCloses = 1;
    assert(SdhciControllerDestroy(&controller) == OS_EDEVFAULT);
    assert(g_liveBuffers == 0 && g_liveContexts == 1 && g_acquired);
    __DestroyAll(&controller);
    assert(__Count(EventDmaFree) == 2 && __Count(EventDmaClose) == 2);

    // A failing release keeps the window and does not repeat the close.
    __DefaultHost();
    __PrepareAndStart(&controller);
    g_failReleases = 1;
    assert(SdhciControllerDestroy(&controller) == OS_EDEVFAULT);
    assert(g_acquired && g_prepared && g_liveContexts == 0);
    __DestroyAll(&controller);
    assert(__Count(EventDmaClose) == 1 && __Count(EventRelease) == 2 && __Count(EventStop) == 1);

    // A failing DeviceStop is retried alone.
    __DefaultHost();
    __PrepareAndStart(&controller);
    g_failStops = 1;
    assert(SdhciControllerDestroy(&controller) == OS_EDEVFAULT);
    assert(!g_acquired && g_prepared);
    __DestroyAll(&controller);
    assert(__Count(EventRelease) == 1 && __Count(EventStop) == 2);
    assert(__CountWrites(SDHCI_SOFTWARE_RESET, (uint32_t)__Find(EventStart, 0, ANY_VALUE, 0)) == 1);

    // A failed DeviceStart leaves the controller for an ordinary teardown,
    // without a quiesce reset because the device never started.
    __DefaultHost();
    __Prepare(&controller);
    g_failStarts = 1;
    assert(SdhciControllerStart(&controller) == OS_EDEVFAULT);
    __DestroyAll(&controller);
    assert(__CountWrites(SDHCI_SOFTWARE_RESET, (uint32_t)__Find(EventStart, 0, ANY_VALUE, 0)) == 0);
    printf("SDHCI teardown retry passed\n");
}

// ---------------------------------------------------------------------------
// Capabilities and limits
// ---------------------------------------------------------------------------

static oserr_t
__PrepareHost(
    _In_  uint16_t           version,
    _In_  uint32_t           capabilities,
    _Out_ SdhciController_t* controller)
{
    oserr_t status;

    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(version, capabilities);
    memset(controller, 0, sizeof(*controller));
    status = SdhciControllerPrepare(controller, &g_device.Base, &g_integration);
    if (status != OS_EOK) {
        // A rejected host never gets as far as DMA.
        assert(__Count(EventDmaOpen) == 0);
    }
    __DestroyAll(controller);
    return status;
}

static oserr_t
__RaiseMinimumClock(
    _In_  SdhciController_t*            controller,
    _In_  const SdhciRawCapabilities_t* raw,
    _Out_ SdhciCapabilities_t*          capabilitiesOut)
{
    oserr_t status;

    status = SdhciStandardDecodeCapabilities(controller, raw, capabilitiesOut);
    capabilitiesOut->MinimumClockHz = 500000;
    return status;
}

static void
__TestCapabilities(void)
{
    SdhciController_t controller;
    uint32_t          baseCaps = SDHCI_CAPS_ADMA2 | SDHCI_CAPS_VOLTAGE_330;

    // Version 2 uses a 6-bit base clock field.
    assert(__PrepareHost(SDHCI_SPEC_200, CAPS_BASE(63) | baseCaps, &controller) == OS_EOK);
    assert(controller.Capabilities.BaseClockHz == 63000000U);
    assert(controller.Host.Limits.MinimumClockHz == (63000000U + 255U) / 256U);
    assert(controller.Host.Limits.MaximumClockHz == SDHCI_DEFAULT_SPEED_CLOCK_HZ);
    // Bits 15:14 are reserved before version 3.
    assert(__PrepareHost(SDHCI_SPEC_200, CAPS_BASE(0xC8) | baseCaps, &controller) == OS_ENOTSUPPORTED);

    // Version 3 uses the 8-bit field.
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(0xC8) | baseCaps | SDHCI_CAPS_HIGH_SPEED, &controller) == OS_EOK);
    assert(controller.Capabilities.BaseClockHz == 200000000U);
    assert(controller.Host.Limits.MinimumClockHz == (200000000U + 2045U) / 2046U);
    assert(controller.Host.Limits.MaximumClockHz == 200000000U);
    assert(controller.Host.Limits.Flags ==
        (SD_HOST_FLAG_HIGH_SPEED | SD_HOST_FLAG_REMOVABLE | SD_HOST_FLAG_WRITE_PROTECT));
    assert(controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4));
    assert(controller.Host.Limits.SignalVoltages == SD_HOST_SIGNAL_330);
    assert(controller.Host.Limits.SupplyVoltages == ((1U << 20) | (1U << 21)));
    assert(controller.Host.Limits.MaximumBlockSize == 512);
    assert(controller.Host.Limits.MaximumBlockCount == 65535);

    // No ADMA2 means no host: PIO and SDMA are excluded. The standard decoder
    // reports an SDMA-only host as an unknown engine, and the engine rejects
    // every engine type that is not ADMA2.
    g_log[0] = '\0';
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(200) | SDHCI_CAPS_SDMA | SDHCI_CAPS_VOLTAGE_330,
        &controller) == OS_ENOTSUPPORTED);
    assert(controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_UNKNOWN);
    assert(strstr(g_log, "no ADMA2") != NULL);

    // The 64-bit capability selects the 12-byte format on version 3.
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(200) | baseCaps | SDHCI_CAPS_64BIT_V3, &controller) == OS_EOK);
    assert(controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2_64);
    assert(controller.Dma.Format == SdhciAdmaFormat64);
    assert(g_openLimit == UINT64_MAX);
    assert(__Find(EventWrite, SDHCI_HOST_CONTROL1, SDHCI_HOST_CONTROL1_DMA_ADMA2_64, 0) >= 0);
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, CAPS_BASE(200) | baseCaps | SDHCI_CAPS_64BIT_V3);
    __Prepare(&controller);
    assert(controller.Dma.Table.Capacity == 341 && controller.Dma.Table.Format == SdhciAdmaFormat64);
    __DestroyAll(&controller);
    __DefaultHost();
    __Prepare(&controller);
    assert(controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    assert(controller.Dma.Table.Capacity == 512 && controller.Dma.Table.Format == SdhciAdmaFormat32);
    assert(__Find(EventWrite, SDHCI_HOST_CONTROL1, SDHCI_HOST_CONTROL1_DMA_ADMA2_32, 0) >= 0);
    __DestroyAll(&controller);

    // The same bit means something else before version 3, and an integration
    // without verified 64-bit support keeps the 32-bit format.
    assert(__PrepareHost(SDHCI_SPEC_200, CAPS_BASE(50) | baseCaps | SDHCI_CAPS_64BIT_V3, &controller) == OS_EOK);
    assert(controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2);
    assert(controller.Dma.Format == SdhciAdmaFormat32 && g_openLimit == UINT32_MAX);
    g_integration.Flags &= ~SDHCI_INTEGRATION_ADMA2_64;
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, CAPS_BASE(200) | baseCaps | SDHCI_CAPS_64BIT_V3);
    memset(&controller, 0, sizeof(controller));
    assert(SdhciControllerPrepare(&controller, &g_device.Base, &g_integration) == OS_EOK);
    assert(controller.Capabilities.DmaEngine == SDHCI_DMA_ENGINE_ADMA2_64);
    assert(controller.Dma.Format == SdhciAdmaFormat32);
    __DestroyAll(&controller);

    // Version and capability combinations the driver cannot run.
    assert(__PrepareHost(SDHCI_SPEC_100, CAPS_BASE(50) | baseCaps, &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(SDHCI_SPEC_420 + 1, CAPS_BASE(50) | baseCaps, &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(0xFFFF, 0xFFFFFFFFU, &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(50) | baseCaps | (3U << SDHCI_CAPS_MAX_BLOCK_SHIFT),
        &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(50) | baseCaps | (2U << SDHCI_CAPS_SLOT_TYPE_SHIFT),
        &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(50) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_VOLTAGE_180,
        &controller) == OS_ENOTSUPPORTED);
    assert(__PrepareHost(SDHCI_SPEC_300, baseCaps, &controller) == OS_ENOTSUPPORTED);

    // Embedded slot with eight lines, 2048-byte blocks and no high speed.
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(100) | baseCaps | SDHCI_CAPS_8BIT |
        (2U << SDHCI_CAPS_MAX_BLOCK_SHIFT) | (1U << SDHCI_CAPS_SLOT_TYPE_SHIFT), &controller) == OS_EOK);
    assert(controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4 | SD_HOST_BUS_WIDTH_8));
    assert(controller.Host.Limits.MaximumBlockSize == 2048);
    assert(controller.Host.Limits.Flags == 0);
    assert(controller.Host.Limits.MaximumClockHz == SDHCI_DEFAULT_SPEED_CLOCK_HZ);
    // The same host in a removable slot is wired for four lines.
    assert(__PrepareHost(SDHCI_SPEC_300, CAPS_BASE(100) | baseCaps | SDHCI_CAPS_8BIT, &controller) == OS_EOK);
    assert(controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4));
    // The 8-bit bit means nothing before version 3.
    assert(__PrepareHost(SDHCI_SPEC_200, CAPS_BASE(50) | baseCaps | SDHCI_CAPS_8BIT, &controller) == OS_EOK);
    assert(controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4));

    // An integration minimum above 400 kHz rules out card initialization.
    __DefaultHost();
    g_integration.InterpretCapabilities = __RaiseMinimumClock;
    memset(&controller, 0, sizeof(controller));
    assert(SdhciControllerPrepare(&controller, &g_device.Base, &g_integration) == OS_ENOTSUPPORTED);
    __DestroyAll(&controller);
    printf("SDHCI capabilities passed\n");
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

static void
__TestReset(void)
{
    SdhciController_t controller;
    SdBusSettings_t   settings;
    uint32_t          clockHz;
    uint64_t          started;
    int               reset;
    int               voltage;
    int               busPower;
    int               index;

    // Firmware left version 4 mode and CMD23 on. The prepare reset writes the
    // reset bit as one byte and clears host control 2 explicitly.
    __DefaultHost();
    __StoreRegister(SDHCI_HOST_CONTROL2, 2, SDHCI_HOST_CONTROL2_V4_MODE | SDHCI_HOST_CONTROL2_CMD23_ENABLE);
    __PrepareAndStart(&controller);
    reset = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, 0);
    assert(reset >= 0 && g_events[reset].Width == 1);
    index = __Find(EventWrite, SDHCI_HOST_CONTROL2, 0, (uint32_t)reset);
    assert(index > reset && g_events[index].Width == 2);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_SIGNAL, 0, (uint32_t)reset) > reset);
    assert(__Find(EventWrite, SDHCI_INTERRUPT_ENABLE, 0, (uint32_t)reset) > reset);

    // Power, width, timing and clock come back after a reset.
    settings = __Settings(400000, SdBusWidth4, SdBusTimingHighSpeed, SdBusPowerOn);
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    index = (int)g_eventCount;
    assert(controller.Host.Operations->Reset(&controller.Host) == OS_EOK);
    reset = __Find(EventWrite, SDHCI_SOFTWARE_RESET, SDHCI_RESET_ALL, (uint32_t)index);
    voltage = __Find(EventWrite, SDHCI_POWER_CONTROL, SDHCI_POWER_VOLTAGE_330, (uint32_t)reset);
    busPower = __Find(EventWrite, SDHCI_POWER_CONTROL, SDHCI_POWER_VOLTAGE_330 | SDHCI_POWER_BUS_ON, (uint32_t)reset);
    assert(reset >= 0 && voltage > reset && busPower > voltage);
    assert(__Find(EventCardPower, 0, ANY_VALUE, (uint32_t)reset) < 0);
    assert(g_registers[SDHCI_POWER_CONTROL] == (SDHCI_POWER_VOLTAGE_330 | SDHCI_POWER_BUS_ON));
    assert(g_registers[SDHCI_HOST_CONTROL1] == (SDHCI_HOST_CONTROL1_DMA_ADMA2_32 |
        SDHCI_HOST_CONTROL1_4BIT | SDHCI_HOST_CONTROL1_HIGH_SPEED));
    assert((__LoadRegister(SDHCI_CLOCK_CONTROL, 2) & SDHCI_CLOCK_CARD_ENABLE) != 0);
    assert(controller.Bus.CardClockHz == clockHz && !controller.Failed);

    // A reset bit that never clears fails after the deadline, marks the
    // controller failed and keeps all DMA memory.
    g_resetBusyReads = FOREVER;
    started = g_nowUs;
    index = (int)g_eventCount;
    assert(SdhciControllerReset(&controller) == OS_ETIMEOUT);
    assert(g_nowUs - started >= SDHCI_RESET_TIMEOUT_US);
    assert(g_nowUs - started <= SDHCI_RESET_TIMEOUT_US + 2 * SDHCI_REGISTER_POLL_US);
    assert(controller.Failed);
    assert(g_liveBuffers == 2 && g_liveContexts == 1);
    assert(__Find(EventDmaFree, 0, ANY_VALUE, (uint32_t)index) < 0);
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EDEVFAULT);

    // Teardown of a host that cannot be reset keeps the memory too.
    assert(SdhciControllerDestroy(&controller) == OS_ETIMEOUT);
    assert(g_liveBuffers == 2 && g_liveContexts == 1 && g_acquired && g_prepared);
    assert(__Count(EventDmaFree) == 0);

    // Once the reset works again, teardown finishes.
    g_resetBusyReads = 1;
    __DestroyAll(&controller);

    // A later successful reset clears the failed mark.
    __DefaultHost();
    __Prepare(&controller);
    g_resetBusyReads = FOREVER;
    assert(SdhciControllerReset(&controller) == OS_ETIMEOUT && controller.Failed);
    g_resetBusyReads = 0;
    assert(SdhciControllerReset(&controller) == OS_EOK && !controller.Failed);
    __DestroyAll(&controller);

    // Version 2 has no host control 2.
    assert(__PrepareHost(SDHCI_SPEC_200, CAPS_BASE(50) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_VOLTAGE_330,
        &controller) == OS_EOK);
    assert(__CountWrites(SDHCI_HOST_CONTROL2, 0) == 0);
    printf("SDHCI reset passed\n");
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

static uint32_t
__DecodeDivisor(
    _In_ uint8_t  specVersion,
    _In_ uint16_t bits)
{
    uint32_t divider;

    if (specVersion >= SDHCI_SPEC_300) {
        divider = ((bits >> SDHCI_CLOCK_DIVIDER_SHIFT) & 0xFFU) |
            (((bits >> SDHCI_CLOCK_DIVIDER_HIGH_SHIFT) & 0x3U) << 8);
        return divider == 0 ? 1U : divider * 2U;
    }
    divider = (bits >> SDHCI_CLOCK_DIVIDER_SHIFT) & 0xFFU;
    return divider == 0 ? 1U : divider * 2U;
}

// The fastest valid divisor whose clock does not exceed the request, found by
// trying every candidate in increasing order. Zero when none exists.
static uint32_t
__ExpectedDivisor(
    _In_ uint8_t  specVersion,
    _In_ uint32_t baseClockHz,
    _In_ uint32_t requestedHz)
{
    uint32_t divisor;

    if (baseClockHz <= requestedHz) {
        return 1;
    }
    if (specVersion >= SDHCI_SPEC_300) {
        for (divisor = 2; divisor <= 2046; divisor += 2) {
            if ((uint64_t)requestedHz * divisor >= baseClockHz) {
                return divisor;
            }
        }
        return 0;
    }
    for (divisor = 2; divisor <= 256; divisor *= 2) {
        if ((uint64_t)requestedHz * divisor >= baseClockHz) {
            return divisor;
        }
    }
    return 0;
}

static void
__TestClockTable(void)
{
    static const uint32_t bases[] = {
        200000000U, 100000000U, 63000000U, 52000000U, 50000000U, 41666666U, 25000000U, 1000000U
    };
    static const uint32_t requests[] = {
        1U, 97656U, 123457U, 200000U, 300000U, 400000U, 25000000U, 26000000U, 50000000U,
        52000000U, 49999999U, 100000000U, 200000000U, UINT32_MAX
    };
    static const uint8_t versions[] = { SDHCI_SPEC_200, SDHCI_SPEC_300, SDHCI_SPEC_420 };
    SdhciClockSetting_t   setting;
    uint32_t              expected;
    uint32_t              v;
    uint32_t              b;
    uint32_t              r;
    oserr_t               status;

    for (v = 0; v < sizeof(versions); v++) {
        for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
            for (r = 0; r < sizeof(requests) / sizeof(requests[0]); r++) {
                expected = __ExpectedDivisor(versions[v], bases[b], requests[r]);
                status = SdhciClockSelect(versions[v], bases[b], requests[r], &setting);
                if (expected == 0) {
                    assert(status == OS_ENOTSUPPORTED);
                    continue;
                }
                assert(status == OS_EOK);
                assert(setting.Divisor == expected);
                assert(__DecodeDivisor(versions[v], setting.ControlBits) == expected);
                assert((uint64_t)requests[r] * setting.Divisor >= bases[b]);
                assert(setting.ClockHz == bases[b] / expected);
                assert(setting.ClockHz <= requests[r]);
                // Programmable clock mode is never selected.
                assert((setting.ControlBits & SDHCI_CLOCK_PROGRAMMABLE) == 0);
                if (versions[v] < SDHCI_SPEC_300) {
                    assert((setting.ControlBits & SDHCI_CLOCK_DIVIDER_HIGH_MASK) == 0);
                }
            }
        }
    }
    assert(SdhciClockSelect(SDHCI_SPEC_300, 0, 400000, &setting) == OS_EINVALPARAMS);
    assert(SdhciClockSelect(SDHCI_SPEC_300, 50000000, 0, &setting) == OS_EINVALPARAMS);
}

static void
__TestClockSequence(void)
{
    SdhciController_t controller;
    SdBusSettings_t   settings;
    uint32_t          clockHz = 1;
    uint64_t          started;
    int               start;
    int               stop;
    int               internal;
    int               stable;
    int               card;

    __DefaultHost();
    __PrepareAndStart(&controller);
    settings = __Settings(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == 200000000U / 500U && controller.Bus.CardClockHz == clockHz);

    // The card clock is off before the divider changes, the internal clock is
    // stable before the card clock starts.
    stop = __Find(EventWrite, SDHCI_CLOCK_CONTROL, 0, (uint32_t)start);
    internal = __Find(EventWrite, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)stop + 1);
    stable = __Find(EventRead, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)internal);
    while (stable >= 0 && (g_events[stable].Value & SDHCI_CLOCK_INTERNAL_STABLE) == 0) {
        stable = __Find(EventRead, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)stable + 1);
    }
    card = __Find(EventWrite, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)internal + 1);
    assert(stop >= 0 && internal > stop && stable > internal && card > stable);
    assert(g_events[stop].Width == 2);
    assert(g_events[internal].Value == ((250U << SDHCI_CLOCK_DIVIDER_SHIFT) | SDHCI_CLOCK_INTERNAL_ENABLE));
    assert(g_events[card].Value == (g_events[internal].Value | SDHCI_CLOCK_CARD_ENABLE));
    assert(__Find(EventWrite, SDHCI_CLOCK_CONTROL, ANY_VALUE, (uint32_t)card + 1) < 0);

    // Zero stops the clock and writes nothing else to clock control.
    settings.ClockHz = 0;
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == 0 && controller.Bus.CardClockHz == 0);
    assert(__CountWrites(SDHCI_CLOCK_CONTROL, (uint32_t)start) == 1);
    assert(__LoadRegister(SDHCI_CLOCK_CONTROL, 2) == 0);

    // A rate below the minimum is rejected before any register write.
    settings.ClockHz = controller.Host.Limits.MinimumClockHz - 1;
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ENOTSUPPORTED);
    assert(g_eventCount == (uint32_t)start);
    settings.ClockHz = controller.Host.Limits.MinimumClockHz;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz <= settings.ClockHz && clockHz > 0);

    // An internal clock that never becomes stable times out after the
    // deadline and never enables the card clock.
    g_clockStableReads = FOREVER;
    settings.ClockHz = 25000000U;
    start = (int)g_eventCount;
    started = g_nowUs;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ETIMEOUT);
    assert(g_nowUs - started >= SDHCI_CLOCK_STABLE_TIMEOUT_US);
    assert(controller.Bus.CardClockHz == 0);
    for (card = start; card < (int)g_eventCount; card++) {
        if (g_events[card].Kind == EventWrite && g_events[card].Offset == SDHCI_CLOCK_CONTROL) {
            assert((g_events[card].Value & SDHCI_CLOCK_CARD_ENABLE) == 0);
        }
    }
    // The last applied settings stay those of the last successful call.
    assert(controller.Bus.Settings.ClockHz == controller.Host.Limits.MinimumClockHz);
    __DestroyAll(&controller);

    // Version 2: 8-bit power of two divider.
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_200, CAPS_BASE(50) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_VOLTAGE_330);
    __Prepare(&controller);
    settings = __Settings(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == 50000000U / 128U);
    assert((__LoadRegister(SDHCI_CLOCK_CONTROL, 2) >> SDHCI_CLOCK_DIVIDER_SHIFT) == 64U);
    __DestroyAll(&controller);
}

static void
__TestClock(void)
{
    __TestClockTable();
    __TestClockSequence();
    printf("SDHCI clock passed\n");
}

// ---------------------------------------------------------------------------
// Power, width, timing and card state
// ---------------------------------------------------------------------------

static void
__TestPower(void)
{
    SdhciController_t controller;
    SdBusSettings_t   settings;
    uint32_t          clockHz;
    int               start;
    int               hook;
    int               voltage;
    int               busPower;
    int               clockStop;
    int               off;

    __DefaultHost();
    __PrepareAndStart(&controller);

    // On: platform supply, then voltage select, then bus power, all after the
    // card clock was stopped.
    settings = __Settings(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    clockStop = __Find(EventWrite, SDHCI_CLOCK_CONTROL, 0, (uint32_t)start);
    hook = __Find(EventCardPower, 0, SdBusPowerOn, (uint32_t)start);
    voltage = __Find(EventWrite, SDHCI_POWER_CONTROL, ANY_VALUE, (uint32_t)start);
    busPower = __Find(EventWrite, SDHCI_POWER_CONTROL, ANY_VALUE, (uint32_t)voltage + 1);
    assert(clockStop >= 0 && hook > clockStop && voltage > hook && busPower > voltage);
    assert(g_events[voltage].Value == SDHCI_POWER_VOLTAGE_330 && g_events[voltage].Width == 1);
    assert(g_events[busPower].Value == (SDHCI_POWER_VOLTAGE_330 | SDHCI_POWER_BUS_ON));

    // Already on: nothing switches, so the bus keeps its power.
    start = (int)g_eventCount;
    settings.ClockHz = 25000000U;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(__Find(EventCardPower, 0, ANY_VALUE, (uint32_t)start) < 0);
    assert(__CountWrites(SDHCI_POWER_CONTROL, (uint32_t)start) == 0);

    // Off: clock stopped, bus power cleared, then the platform supply.
    settings = __Settings(0, SdBusWidth1, SdBusTimingDefault, SdBusPowerOff);
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    clockStop = __Find(EventWrite, SDHCI_CLOCK_CONTROL, 0, (uint32_t)start);
    off = __Find(EventWrite, SDHCI_POWER_CONTROL, 0, (uint32_t)start);
    hook = __Find(EventCardPower, 0, SdBusPowerOff, (uint32_t)start);
    assert(clockStop >= 0 && off > clockStop && hook > off);
    assert(g_registers[SDHCI_POWER_CONTROL] == 0);

    // Off again still clears the register and calls the platform step, so a
    // power cycle works from any state.
    start = (int)g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    off = __Find(EventWrite, SDHCI_POWER_CONTROL, 0, (uint32_t)start);
    assert(off >= 0 && __Find(EventCardPower, 0, SdBusPowerOff, (uint32_t)start) > off);
    __DestroyAll(&controller);
    printf("SDHCI power passed\n");
}

static void
__TestWidthAndTiming(void)
{
    SdhciController_t controller;
    SdBusSettings_t   settings;
    uint32_t          clockHz;
    uint32_t          start;

    __DefaultHost();
    __PrepareAndStart(&controller);
    settings = __Settings(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(g_registers[SDHCI_HOST_CONTROL1] == SDHCI_HOST_CONTROL1_DMA_ADMA2_32);
    settings.Width = SdBusWidth4;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(g_registers[SDHCI_HOST_CONTROL1] == (SDHCI_HOST_CONTROL1_DMA_ADMA2_32 | SDHCI_HOST_CONTROL1_4BIT));
    settings.Timing = SdBusTimingHighSpeed;
    settings.ClockHz = 50000000U;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(clockHz == 50000000U);
    assert(g_registers[SDHCI_HOST_CONTROL1] == (SDHCI_HOST_CONTROL1_DMA_ADMA2_32 |
        SDHCI_HOST_CONTROL1_4BIT | SDHCI_HOST_CONTROL1_HIGH_SPEED));

    // Eight lines are not wired on a removable slot. Nothing is written.
    settings.Width = SdBusWidth8;
    start = g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ENOTSUPPORTED);
    assert(g_eventCount == start);
    settings.Width = (enum SdBusWidth)2;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ENOTSUPPORTED);
    settings.Width = SdBusWidth4;
    settings.Power = (enum SdBusPower)7;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ENOTSUPPORTED);
    assert(g_eventCount == start);
    __DestroyAll(&controller);

    // Embedded eight-line host without high speed.
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, CAPS_BASE(100) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_VOLTAGE_330 |
        SDHCI_CAPS_8BIT | (1U << SDHCI_CAPS_SLOT_TYPE_SHIFT));
    __PrepareAndStart(&controller);
    settings = __Settings(400000, SdBusWidth8, SdBusTimingDefault, SdBusPowerOn);
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_EOK);
    assert(g_registers[SDHCI_HOST_CONTROL1] == (SDHCI_HOST_CONTROL1_DMA_ADMA2_32 | SDHCI_HOST_CONTROL1_8BIT));
    settings.Timing = SdBusTimingHighSpeed;
    start = g_eventCount;
    assert(SdhciControllerSetBus(&controller, &settings, &clockHz) == OS_ENOTSUPPORTED);
    assert(g_eventCount == start);
    __DestroyAll(&controller);
    printf("SDHCI width and timing passed\n");
}

static void
__TestCardState(void)
{
    SdhciController_t controller;
    uint32_t          state;

    __DefaultHost();
    __PrepareAndStart(&controller);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_STABLE |
        SDHCI_PRESENT_WRITE_ENABLED);
    assert(controller.Host.Operations->GetCardState(&controller.Host, &state) == OS_EOK);
    assert(state == SD_CARD_STATE_PRESENT);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_STABLE);
    assert(SdhciControllerGetCardState(&controller, &state) == OS_EOK);
    assert(state == (SD_CARD_STATE_PRESENT | SD_CARD_STATE_WRITE_PROTECTED));
    __StoreRegister(SDHCI_PRESENT_STATE, 4, SDHCI_PRESENT_CARD_INSERTED);
    assert(SdhciControllerGetCardState(&controller, &state) == OS_EOK);
    assert(state == 0);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, SDHCI_PRESENT_CARD_STABLE);
    assert(SdhciControllerGetCardState(&controller, &state) == OS_EOK);
    assert(state == 0);
    __DestroyAll(&controller);

    // An embedded slot has no write protect switch to report.
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, __DefaultCapabilities() | (1U << SDHCI_CAPS_SLOT_TYPE_SHIFT));
    __PrepareAndStart(&controller);
    __StoreRegister(SDHCI_PRESENT_STATE, 4, SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_STABLE);
    assert(SdhciControllerGetCardState(&controller, &state) == OS_EOK);
    assert(state == SD_CARD_STATE_PRESENT);
    __DestroyAll(&controller);

    // An integration without card detect reports that, and leaves the
    // output alone.
    __DefaultHost();
    g_integration.Flags &= ~SDHCI_INTEGRATION_CARD_DETECT;
    __PrepareAndStart(&controller);
    assert((controller.Host.Limits.Flags & SD_HOST_FLAG_WRITE_PROTECT) == 0);
    state = 0x55;
    assert(SdhciControllerGetCardState(&controller, &state) == OS_ENOTSUPPORTED);
    assert(state == 0x55);
    __DestroyAll(&controller);
    printf("SDHCI card state passed\n");
}

// ---------------------------------------------------------------------------
// PCI slot information
// ---------------------------------------------------------------------------

static oserr_t
__PrepareSlot(
    _In_  uint8_t            slotInformation,
    _Out_ SdhciController_t* controller)
{
    oserr_t status;

    g_slotInformation = slotInformation;
    __SetHost(SDHCI_SPEC_300, __DefaultCapabilities());
    memset(controller, 0, sizeof(*controller));
    status = SdhciControllerPrepare(controller, &g_device.Base, &g_integration);
    __DestroyAll(controller);
    return status;
}

static void
__TestPciSlot(void)
{
    static const uint32_t bars[] = { 0, 1, 2, 3, 5 };
    SdhciController_t     controller;
    char                  expected[96];
    uint32_t              i;
    uint32_t              slots;
    uint8_t               slotInformation;
    int                   ioctl;

    for (i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
        slots = 1 + (i % 3);
        __Reset();
        // A decoy memory BAR next to the slot's BAR must not be chosen.
        __AddBar(bars[i], 0x100, true);
        __AddBar(bars[i] == 4 ? 3 : 4, 0x1000, true);
        slotInformation = (uint8_t)(bars[i] | ((slots - 1) << SDHCI_PCI_SLOT_INFO_SLOTS_SHIFT));
        assert(__PrepareSlot(slotInformation, &controller) == OS_EOK);
        ioctl = __Find(EventIoctl, 0, ANY_VALUE, 0);
        assert(ioctl > __Find(EventPrepare, 0, ANY_VALUE, 0));
        assert(g_events[ioctl].Offset == SDHCI_PCI_SLOT_INFO && g_events[ioctl].Width == 1);
        assert(__Find(EventAcquire, 0, BAR_ID_BASE + bars[i], 0) > ioctl);
        assert(__Count(EventAcquire) == 1);
        snprintf(expected, sizeof(expected), "%u slot(s), first slot in BAR %u", slots, bars[i]);
        assert(strstr(g_log, expected) != NULL);
        assert((strstr(g_log, "only the first one is used") != NULL) == (slots > 1));
    }

    // BAR 6 and 7 do not exist.
    __Reset();
    __AddBar(0, 0x100, true);
    assert(__PrepareSlot(6, &controller) == OS_ENOTSUPPORTED);
    assert(__Count(EventAcquire) == 0);
    // The named BAR is an I/O port BAR.
    __Reset();
    __AddBar(1, 0x100, true);
    g_device.IoSpaces[1].Type = DeviceIoPortBased;
    assert(__PrepareSlot(1, &controller) == OS_ENOTSUPPORTED);
    // The named BAR is too small for the register map.
    __Reset();
    __AddBar(2, 0x80, true);
    assert(__PrepareSlot(2, &controller) == OS_ENOTSUPPORTED);
    // The named BAR is not among the device resources.
    __Reset();
    __AddBar(2, 0x100, false);
    __AddBar(0, 0x100, true);
    assert(__PrepareSlot(2, &controller) == OS_ENOENT);
    assert(__Count(EventAcquire) == 0);
    // A device that is not on PCI.
    __Reset();
    __AddBar(0, 0x100, true);
    g_device.Base.Length = sizeof(Device_t);
    assert(__PrepareSlot(0, &controller) == OS_ENOTSUPPORTED);
    assert(__Count(EventIoctl) == 0);
    printf("SDHCI PCI slot passed\n");
}

int
main(void)
{
    __TestSetupOrder();
    __TestSetupFailures();
    __TestTeardownRetry();
    __TestCapabilities();
    __TestReset();
    __TestClock();
    __TestPower();
    __TestWidthAndTiming();
    __TestCardState();
    __TestPciSlot();
    printf("SDHCI controller tests passed\n");
    return 0;
}
