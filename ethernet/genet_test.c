#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../modules/eth/genet/genet.h"

// Test packets implement only the MAC packet interface, without protocol state.
struct EthernetSlot {
    uint8_t* Bytes;
    uint32_t Length;
    bool Receive;
    bool Pending;
};

static unsigned g_receiveDrops[3];
static uint32_t g_registers[0x10000 / 4];
static uint64_t g_time;
static uint64_t g_resetDeadline;
static unsigned g_delay;
static unsigned g_commands;
static unsigned g_busy;
static int g_stuck;
static int g_mdioError;
static size_t g_failOffset;
static uint32_t g_maskAtDmaStop;
static int g_stopFails;
static unsigned g_completions;
static unsigned g_faults;
static EthernetSlot_t* g_lastSlot;
static uint32_t g_lastLength;
static unsigned g_txFlush;
static unsigned g_rxFlush;
static int g_delayFails;
static unsigned g_commandWrites;
static struct PhyLink g_link = {1000, 1, 1};

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t width)
{
    uint32_t value;
    (void)io;

    assert(width == 4 && !(offset & 3) && offset < sizeof(g_registers));
    if (offset == 0xe14 && !g_stuck && g_busy) {
        if (!--g_busy) {
            g_registers[offset / 4] &= ~(1U << 29);
            g_registers[offset / 4] |= 0x1234;
            if (g_mdioError) {
                g_registers[offset / 4] |= 1U << 28;
            }
        }
    }
    value = g_registers[offset / 4];
    return value;
}

oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t value,
    _In_ size_t width)
{
    (void)io;
    assert(width == 4 && !(offset & 3) && offset < sizeof(g_registers));
    assert(g_time >= g_resetDeadline);
    if (offset == 0xe14) {
        g_commands++;
        g_busy = 3;
    }
    if (offset == GENET_COMMAND) {
        g_commandWrites++;
    }
    if (offset == GENET_COMMAND && (g_registers[8 / 4] & 2)) {
        return OS_EDEVFAULT;
    }
    if (offset == GENET_COMMAND && (value & GENET_COMMAND_RESET)) {
        g_resetDeadline = g_time + 2;
    }
    if (offset == 0xb34 && value == 1) {
        g_txFlush++;
    }
    // Remember what the interrupt mask held when the first engine was stopped.
    if (offset == GENET_TX_DMA + GENET_DMA_CONTROL && !(value & GENET_DMA_ENABLE)) {
        g_maskAtDmaStop = g_registers[GENET_INTR0_MASK_SET / 4];
    }
    if (offset == 8 && value == 1) {
        g_rxFlush++;
    }
    g_registers[offset / 4] = value;
    if ((offset == GENET_RX_DMA + 4 || offset == GENET_TX_DMA + 4) && !g_stopFails) {
        g_registers[(offset + 4) / 4] = (value & 1) ? 0 : 1;
    }
    if (offset == g_failOffset) {
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

oserr_t
GenetDelay(
    _In_ void* unused,
    _In_ uint32_t microseconds)
{
    (void)unused;
    if (g_delayFails) {
        return OS_EDEVFAULT;
    }
    g_time += microseconds;
    g_delay += microseconds;
    return OS_EOK;
}

uint64_t
GenetMilliseconds(void)
{
    return g_time / 1000;
}

oserr_t
PhyReadLink(
    _In_ PhyDevice_t* phy,
    _In_ struct PhyLink* link)
{
    (void)phy;
    *link = g_link;
    return OS_EOK;
}

oserr_t
EthernetDmaAllocate(
    _In_ struct DMAContext* context,
    _In_ size_t length,
    _In_ struct EthernetDma* storage)
{
    (void)context;
    storage->Data = malloc(length);
    assert(storage->Data);
    storage->Buffer = (void*)storage->Data;
    storage->Address = 0x123400000ULL;
    return OS_EOK;
}

void*
EthernetPacketBytes(
    _In_ EthernetSlot_t* slot)
{
    return slot->Bytes;
}

uint32_t
EthernetPacketLength(
    _In_ const EthernetSlot_t* slot)
{
    return slot->Length;
}

bool
EthernetPacketIsReceive(
    _In_ const EthernetSlot_t* slot)
{
    return slot->Receive;
}

void
EthernetReportReceiveDrop(
    _InOut_ EthernetDevice_t*        device,
    _In_    enum EthernetReceiveDrop reason)
{
    g_receiveDrops[reason]++;
}

void
EthernetComplete(
    _In_ EthernetDevice_t* adapter,
    _In_ EthernetSlot_t* slot,
    _In_ oserr_t status,
    _In_ uint32_t length)
{
    (void)adapter;
    assert(status == OS_EOK && slot && slot->Pending);
    slot->Pending = false;
    g_completions++;
    g_lastSlot = slot;
    g_lastLength = length;
}

void
EthernetFault(
    _In_ EthernetDevice_t* adapter,
    _In_ oserr_t status)
{
    (void)adapter;
    assert(status != OS_EOK);
    g_faults++;
}

static void
__Mdio(void)
{
    DeviceIo_t registers = {0};
    struct GenetMdio mdio = { .Registers = &registers, .DelayUs = GenetDelay };
    MdioBus_t bus;
    uint16_t value = 0xabcd;
    unsigned commands;

    assert(MdioInitialize(&bus, &g_genetMdioOperations, &mdio, 1) == OS_EOK);
    assert(MdioRead(&bus, 2, &value) == OS_EOK && value == 0x1234);
    assert((g_registers[0xe14 / 4] & 0xff0000) == ((1U << 21) | (2U << 16)));
    assert((g_registers[0xe14 / 4] & (3U << 26)) == (2U << 26));
    assert(MdioWrite(&bus, 4, 0x55aa) == OS_EOK);
    assert((g_registers[0xe14 / 4] & (3U << 26)) == (1U << 26));
    g_mdioError = 1;
    value = 0xbeef;
    assert(MdioRead(&bus, 2, &value) == OS_EDEVFAULT && value == 0xbeef);
    g_mdioError = 0;
    g_stuck = 1;
    g_registers[0xe14 / 4] = 1U << 29;
    commands = g_commands;
    g_delay = 0;
    assert(MdioRead(&bus, 2, &value) == OS_ETIMEOUT && value == 0xbeef);
    assert(g_commands == commands && g_delay == 100000);
    g_registers[0xe14 / 4] = 0;
    assert(MdioRead(&bus, 2, &value) == OS_ETIMEOUT);
    assert(g_commands == commands + 1);
    g_stuck = 0;
    g_registers[0xe14 / 4] = 0;
}

static void
__Integration(void)
{
    struct GenetWrapperSettings integration = {0};
    DeviceIo_t registers = { .Type = DeviceIoMemoryBased };
    struct DeviceEthernetConfiguration config = {
        .Version = 1, .Mode = DeviceEthernetRgmiiRxId,
        .MdioOffset = 0xe14, .MdioLength = 8
    };

    registers.Access.Memory.Length = 0x10000;
    g_registers[0] = 6U << 24;
    g_registers[1] = 0x123;
    g_registers[0x8c / 4] = 0x10030;
    g_registers[0xe18 / 4] = 0x1000;
    assert(GenetWrapperConfigurePhy(&integration, &registers, &config) == OS_EOK);
    assert(g_registers[1] == 3);
    assert((g_registers[0x8c / 4] & 0x10070) == 0x40);
    assert(g_registers[0xe18 / 4] == (49U << 4 | 1));
    assert(GenetWrapperRestorePhy(&integration) == OS_EOK);
    assert(g_registers[1] == 0x123 && g_registers[0x8c / 4] == 0x10030);
    assert(g_registers[0xe18 / 4] == 0x1000);
    config.Mode = DeviceEthernetRgmiiId;
    assert(GenetWrapperConfigurePhy(&integration, &registers, &config) == OS_ENOTSUPPORTED);
    config.Mode = DeviceEthernetRgmiiRxId;
    config.MdioOffset = 0;
    assert(GenetWrapperConfigurePhy(&integration, &registers, &config) == OS_ENOTSUPPORTED);
    config.MdioOffset = 0xe14;
    g_failOffset = 0x8c;
    assert(GenetWrapperConfigurePhy(&integration, &registers, &config) == OS_EDEVFAULT);
    assert(integration.Changed == 2);
    assert(GenetWrapperRestorePhy(&integration) == OS_EDEVFAULT);
    g_failOffset = 0;
    assert(GenetWrapperRestorePhy(&integration) == OS_EOK);
}

static void
__FirmwareReset(void)
{
    struct Genet controller = {0};
    unsigned delay;
    unsigned commands;

    controller.Mapped = true;
    controller.Adapter.Controller = &controller;
    g_registers[8 / 4] = 2;
    g_registers[0x80c / 4] = 0x02000001;
    g_registers[0x810 / 4] = 0x0203;
    delay = g_delay;
    assert(GenetHardwareReleaseReset(&controller) == OS_EOK);
    assert(g_registers[8 / 4] == 0 && g_delay >= delay + 10);
    assert(controller.ResetReleased);
    assert(g_registers[0x80c / 4] == 0x02000001);
    assert(g_registers[0x810 / 4] == 0x0203);
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);

    g_failOffset = 8;
    delay = g_delay;
    assert(GenetHardwareReleaseReset(&controller) == OS_EDEVFAULT);
    assert(g_delay == delay);
    assert(!controller.ResetReleased);
    commands = g_commandWrites;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EDEVFAULT);
    assert(g_commandWrites == commands);
    g_failOffset = 0;
    g_delayFails = 1;
    assert(GenetHardwareReleaseReset(&controller) == OS_EDEVFAULT);
    assert(!controller.ResetReleased);
    assert(GenetQueuesStop(&controller.Adapter) == OS_EDEVFAULT);
    assert(g_commandWrites == commands);
    g_delayFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(controller.ResetReleased && g_commandWrites == commands + 1);
}

static void
__Queues(void)
{
    struct Genet controller = {0};
    EthernetSlot_t tx = {0};
    EthernetSlot_t rx = {0};
    uint8_t frame[1514];
    uint8_t* storage;
    uint32_t status;
    unsigned completions;
    struct PhyLink down = {0};

    memset(g_registers, 0, sizeof(g_registers));
    GenetCoalescingDefaults(&controller.Coalescing);
    controller.Mapped = true;
    controller.Prepared = true;
    controller.Adapter.Controller = &controller;
    controller.Mac[0] = 2;
    controller.Mac[5] = 1;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_txFlush && g_rxFlush);
    assert(g_registers[GENET_RX_RING / 4 + 7] == 95);
    assert(g_registers[GENET_RX_RING / 4 + 4] == ((32U << 16) | 2048));
    assert(g_registers[GENET_RX_DMA / 4] == 1U << 16);
    assert(g_registers[(GENET_RX_DESCRIPTORS + 8) / 4] == 1);
    assert(g_registers[(GENET_RX_DESCRIPTORS + 4) / 4] == 0x23400000);
    assert(g_registers[0xe50 / 4] == 0x18000 && g_registers[0xe5c / 4] == 0x200);
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);
    assert((g_registers[GENET_COMMAND / 4] & 15) == 11);
    assert(g_registers[0x8c / 4] & 0x10);

    memset(frame, 0xab, sizeof(frame));
    memcpy(frame, controller.Mac, 6);
    tx.Bytes = frame;
    tx.Length = 42;
    tx.Pending = true;
    controller.Queues.Transmit.Producer = 65535;
    controller.Queues.Transmit.Consumer = 65535;
    g_registers[(GENET_TX_RING + 8) / 4] = 65535;
    assert(GenetQueuePacket(&controller.Adapter, &tx) == OS_EOK);
    assert(controller.Queues.Transmit.Producer == 0);
    assert(g_registers[(GENET_TX_RING + 12) / 4] == 0);
    storage = controller.Queues.Storage.Data + (32 + 31) * 2048;
    assert(!memcmp(storage, frame, 42));
    for (unsigned i = 42; i < 60; i++) {
        assert(storage[i] == 0);
    }
    assert(g_registers[(GENET_TX_DESCRIPTORS + 31 * 12) / 4] ==
        ((60U << 16) | 0x6000 | (63U << 7) | 0x40));
    g_registers[(GENET_TX_RING + 8) / 4] = 0;
    GenetPoll(&controller.Adapter);
    assert(g_completions == 1 && g_lastSlot == &tx && g_lastLength == 42);

    rx.Bytes = frame + 3;
    rx.Length = sizeof(frame) - 3;
    rx.Receive = true;
    rx.Pending = true;
    assert(GenetQueuePacket(&controller.Adapter, &rx) == OS_EOK);
    controller.Queues.Receive.DmaConsumer = 65535;
    storage = controller.Queues.Storage.Data + 31 * 2048;
    status = ((66U + 60U) << 16) | 0x6000;
    memcpy(storage, &status, 4);
    memcpy(storage + 66, frame, 60);
    memset(frame, 0, sizeof(frame));
    g_registers[(GENET_RX_RING + 8) / 4] = 0;
    GenetPoll(&controller.Adapter);
    assert(g_completions == 2 && g_lastSlot == &rx && g_lastLength == 60);
    assert(!memcmp(frame + 3, controller.Mac, 6));
    assert(frame[3 + 59] == 0xab && frame[3 + 60] == 0);
    assert(controller.Queues.Receive.DmaConsumer == 0);

    // Malformed hardware lengths never escape the private receive buffer.
    rx.Pending = true;
    assert(GenetQueuePacket(&controller.Adapter, &rx) == OS_EOK);
    storage = controller.Queues.Storage.Data;
    status = (4095U << 16) | 0x6000;
    memcpy(storage, &status, 4);
    g_registers[(GENET_RX_RING + 8) / 4] = 1;
    GenetPoll(&controller.Adapter);
    assert(g_completions == 2 && g_receiveDrops[EthernetReceiveError] == 1);
    assert(controller.Queues.Receive.DmaConsumer == 1);
    assert(controller.Queues.Receive.PostedSlots.Head == 1);
    assert(controller.Queues.Receive.PostedSlots.Count == 1);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == &rx && rx.Pending);
    assert(GenetApplyLink(&controller, &down) == OS_EOK);
    assert(!(g_registers[GENET_COMMAND / 4] & 3));
    assert(!(g_registers[0x8c / 4] & 0x10));

    // Failed producer publication must keep the TX accepted until stop.
    tx.Pending = true;
    g_failOffset = GENET_TX_RING + 12;
    assert(GenetQueuePacket(&controller.Adapter, &tx) == OS_EINPROGRESS);
    g_failOffset = 0;
    g_stopFails = 1;
    completions = g_completions;
    assert(GenetQueuesStop(&controller.Adapter) == OS_ETIMEOUT);
    assert(controller.Queues.Transmit.Slots[0] == &tx && controller.Queues.DmaMayBeRunning);
    assert(controller.Queues.Receive.DmaConsumer == 1);
    assert(controller.Queues.Receive.PostedSlots.Head == 1);
    assert(controller.Queues.Receive.PostedSlots.Count == 1);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == &rx);
    assert(g_completions == completions);
    g_stopFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(!controller.Queues.DmaMayBeRunning && controller.Queues.Transmit.Slots[0] == NULL);
    assert(controller.Queues.Receive.DmaConsumer == 0);
    assert(controller.Queues.Receive.PostedSlots.Head == 0);
    assert(controller.Queues.Receive.PostedSlots.Count == 0);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == NULL);
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_txFlush == 2 && g_rxFlush == 2);
    assert(g_faults == 0);
#ifdef VALI_ETHERNET_DIAGNOSTICS
    assert(controller.Diagnostics.Packets[GenetTxQueued] == 1);
    assert(controller.Diagnostics.Packets[GenetTxCompleted] == 1);
    assert(controller.Diagnostics.Packets[GenetRxReceived] == 1);
    assert(controller.Diagnostics.Packets[GenetRxDelivered] == 1);
    assert(controller.Diagnostics.Packets[GenetRxError] == 1);
#endif
    free(controller.Queues.Storage.Data);
}

static void
__InterruptMasking(void)
{
    struct Genet controller = {0};

    memset(g_registers, 0, sizeof(g_registers));
    GenetCoalescingDefaults(&controller.Coalescing);
    controller.Mapped = true;
    controller.Prepared = true;
    controller.Adapter.Controller = &controller;
    controller.Mac[0] = 2;
    controller.Mac[5] = 1;

    // Polling mode masks everything once, when the hardware is initialized,
    // and never touches the interrupt registers afterwards.
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_SET / 4] == GENET_INTR_ALL);
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == 0);
    g_registers[GENET_INTR0_MASK_SET / 4] = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_SET / 4] == 0);
    assert(GenetHardwareUnmaskDmaInterrupts(&controller, GENET_INTR0_DMA_DONE) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == 0);

    // Interrupt mode unmasks only the packet causes, and only once both
    // engines run.
    controller.Interrupt.Enabled = true;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == 0);
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == (GENET_INTR0_RX_DONE | GENET_INTR0_TX_DONE));
    assert(GENET_INTR0_RX_DONE == (1U << 13) && GENET_INTR0_TX_DONE == (1U << 16));

    // Other causes are never unmasked, whatever the caller asks for.
    assert(GenetHardwareUnmaskDmaInterrupts(&controller, GENET_INTR_ALL) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == GENET_INTR0_DMA_DONE);

    // Stop masks every cause before the first engine is switched off.
    g_registers[GENET_INTR0_MASK_SET / 4] = 0;
    g_maskAtDmaStop = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_SET / 4] == GENET_INTR_ALL);
    assert(g_maskAtDmaStop == GENET_INTR_ALL);

    // A failed unmask fails the start instead of leaving the causes silent.
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    g_failOffset = GENET_INTR0_MASK_CLEAR;
    assert(GenetQueuesStart(&controller.Adapter) == OS_EDEVFAULT);
    g_failOffset = 0;

    // A failed mask fails the stop before any engine is touched.
    g_maskAtDmaStop = 0;
    g_failOffset = GENET_INTR0_MASK_SET;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EDEVFAULT);
    assert(controller.Queues.DmaMayBeRunning && g_maskAtDmaStop == 0);
    g_failOffset = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(!controller.Queues.DmaMayBeRunning);
    free(controller.Queues.Storage.Data);
}

static void
__Coalescing(void)
{
    struct Genet controller = {0};
    size_t       timeout = (GENET_RX_DMA + GENET_DMA_RING16_TIMEOUT) / 4;
    size_t       txThreshold = (GENET_TX_RING + GENET_RING_DONE_THRESHOLD) / 4;
    size_t       rxThreshold = (GENET_RX_RING + GENET_RING_DONE_THRESHOLD) / 4;
    unsigned     txFlush;
    struct GenetCoalescing invalid[] = {
        {0, 1, 50},     // No TX count would never interrupt.
        {33, 1, 50},    // More than the ring holds can never be reached.
        {10, 33, 50},
        {10, 4, 0},     // A batch of frames needs the timeout to flush it.
        {10, 0, 0},
        {10, 1, GENET_RX_TIMEOUT_MAX_US + 1}
    };

    // The defaults follow Linux: ten frames for TX, one for RX, 50 us timeout.
    GenetCoalescingDefaults(&controller.Coalescing);
    assert(controller.Coalescing.TxFrames == 10 && controller.Coalescing.RxFrames == 1);
    assert(controller.Coalescing.RxTimeoutUs == 50);
    assert(GenetCoalescingValidate(&controller.Coalescing, GENET_RING_COUNT) == OS_EOK);

    // Timeouts count in 8.192 us ticks and never round down.
    assert(GenetCoalescingTimeoutTicks(0) == 0);
    assert(GenetCoalescingTimeoutTicks(1) == 1);
    assert(GenetCoalescingTimeoutTicks(50) == 7);
    assert(GenetCoalescingTimeoutTicks(200) == 25);
    assert(GenetCoalescingTimeoutTicks(8192) == 1000);
    assert(GenetCoalescingTimeoutTicks(GENET_RX_TIMEOUT_MAX_US) == 64000);
    assert(GenetCoalescingTimeoutTicks(GENET_RX_TIMEOUT_MAX_US) <= GENET_DMA_TIMEOUT_MASK);

    // The edges that are valid.
    assert(GenetCoalescingValidate(&(struct GenetCoalescing){1, 1, 0}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetCoalescingValidate(&(struct GenetCoalescing){32, 32, 1}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetCoalescingValidate(&(struct GenetCoalescing){10, 0, 100}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetCoalescingValidate(
        &(struct GenetCoalescing){10, 1, GENET_RX_TIMEOUT_MAX_US}, GENET_RING_COUNT) == OS_EOK);

    memset(g_registers, 0, sizeof(g_registers));
    controller.Mapped = true;
    controller.Prepared = true;
    controller.Adapter.Controller = &controller;
    controller.Mac[0] = 2;
    controller.Mac[5] = 1;

    // Each ring gets its own threshold. The timeout keeps the register's
    // upper half, which the hardware owns.
    g_registers[timeout] = 0xabcd0000;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_registers[txThreshold] == 10);
    assert(g_registers[rxThreshold] == 1);
    assert(g_registers[timeout] == (0xabcd0000 | 7));

    // Other values reach the hardware too. Zero frames leaves RX to the timeout.
    controller.Coalescing = (struct GenetCoalescing){ .TxFrames = 1, .RxFrames = 0, .RxTimeoutUs = 200 };
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_registers[txThreshold] == 1);
    assert(g_registers[rxThreshold] == 0);
    assert(g_registers[timeout] == (0xabcd0000 | 25));

    // The timeout only applies to RX. TX has no such register.
    assert(g_registers[(GENET_TX_DMA + GENET_DMA_RING16_TIMEOUT) / 4] == 0);

    // A failed timeout write fails the prepare.
    g_failOffset = GENET_RX_DMA + GENET_DMA_RING16_TIMEOUT;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EDEVFAULT);
    g_failOffset = 0;

    // Unusable values fail before any register is written.
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(GenetCoalescingValidate(&invalid[i], GENET_RING_COUNT) == OS_EINVALPARAMS);
        controller.Coalescing = invalid[i];
        memset(g_registers, 0, sizeof(g_registers));
        txFlush = g_txFlush;
        assert(GenetQueuesPrepare(&controller.Adapter) == OS_EINVALPARAMS);
        assert(g_txFlush == txFlush && g_registers[txThreshold] == 0 && g_registers[timeout] == 0);
    }
    free(controller.Queues.Storage.Data);
}

static void
__ReceiveDiscards(void)
{
    struct Genet controller = {0};
    unsigned drops = g_receiveDrops[EthernetReceiveNoBuffer];
    unsigned faults = g_faults;
    size_t   index = (GENET_RX_RING + GENET_RING_HARDWARE_INDEX) / 4;

    memset(g_registers, 0, sizeof(g_registers));
    controller.Adapter.Controller = &controller;
    controller.Queues.DmaMayBeRunning = true;

    // Drops sit in the upper half of the hardware index, next to the frame
    // index. Each one is reported once, as a missing receive buffer.
    g_registers[index] = 5U << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 5);
    assert(controller.Queues.Receive.ReportedDiscards == 5);
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 5);
    g_registers[index] = 7U << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7);
    assert(g_registers[index] == 7U << GENET_RING_DISCARD_SHIFT);

    // A counter that went backwards is taken as the new starting point.
    g_registers[index] = 3U << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7);
    assert(controller.Queues.Receive.ReportedDiscards == 3);

    // The counter is cleared before it can run out of range.
    g_registers[index] = (GENET_RING_DISCARD_RESET_THRESHOLD - 1) << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_registers[index] == (GENET_RING_DISCARD_RESET_THRESHOLD - 1) << GENET_RING_DISCARD_SHIFT);
    g_registers[index] = GENET_RING_DISCARD_RESET_THRESHOLD << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7 + GENET_RING_DISCARD_RESET_THRESHOLD - 3);
    assert(g_registers[index] == 0 && controller.Queues.Receive.ReportedDiscards == 0);

    // Drops arriving after the clear are counted from zero again.
    g_registers[index] = 2U << GENET_RING_DISCARD_SHIFT;
    GenetPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] ==
        drops + 7 + GENET_RING_DISCARD_RESET_THRESHOLD - 3 + 2);

    // A failed clear is a device fault and the drops stay reported.
    g_registers[index] = GENET_RING_DISCARD_RESET_THRESHOLD << GENET_RING_DISCARD_SHIFT;
    g_failOffset = GENET_RX_RING + GENET_RING_HARDWARE_INDEX;
    GenetPoll(&controller.Adapter);
    g_failOffset = 0;
    assert(g_faults == faults + 1);
    assert(controller.Queues.Receive.ReportedDiscards == GENET_RING_DISCARD_RESET_THRESHOLD);

    // Stopping the queues forgets the old count, as the ring registers restart at zero.
    controller.Queues.DmaMayBeRunning = false;
    controller.Mapped = true;
    controller.ResetReleased = true;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(controller.Queues.Receive.ReportedDiscards == 0);
#ifdef VALI_ETHERNET_DIAGNOSTICS
    // 7 drops, then up to the clear threshold, 2 more, and the failed clear's rereport.
    assert(controller.Diagnostics.Packets[GenetRxRingFull] ==
        7 + (GENET_RING_DISCARD_RESET_THRESHOLD - 3) + 2 + (GENET_RING_DISCARD_RESET_THRESHOLD - 2));
#endif
}

static void
__PollFaults(void)
{
    struct Genet controller = {0};
    unsigned faults = g_faults;
    uint64_t time = g_time;

    memset(g_registers, 0, sizeof(g_registers));
    controller.Adapter.Controller = &controller;
    controller.Queues.DmaMayBeRunning = true;

    // Invalid TX progress must fault before the RX consumer register is written.
    g_registers[(GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4] = 1;
    g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] = 1234;
    GenetPoll(&controller.Adapter);
    assert(g_faults == faults + 1);
    assert(g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] == 1234);

    // A stalled TX descriptor must also prevent RX processing in this poll.
    g_registers[(GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4] = 0;
    controller.Queues.Transmit.Producer = 1;
    controller.Queues.Transmit.SubmittedAt[0] = GenetMilliseconds();
    g_time += 5001000;
    GenetPoll(&controller.Adapter);
    assert(g_faults == faults + 2);
    assert(g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] == 1234);
    g_time = time;
}

#ifdef VALI_ETHERNET_DIAGNOSTICS
extern unsigned g_diagnosticLogs;

static void
__Diagnostics(void)
{
    struct Genet controller = {0};
    unsigned before;
    unsigned i;

    before = g_diagnosticLogs;
    for (i = 0; i < 100; i++) {
        GenetLogPacket(&controller, GenetTxQueued, 60, 0x88b5);
    }
    assert(controller.Diagnostics.Packets[GenetTxQueued] == 100);
    assert(g_diagnosticLogs == before + 8);
    before = g_diagnosticLogs;
    g_time = 0;
    GenetLogProgress(&controller);
    assert(g_diagnosticLogs == before + 2);
    g_time = 4999000;
    GenetLogProgress(&controller);
    assert(g_diagnosticLogs == before + 2);
    g_time = 5000000;
    GenetLogProgress(&controller);
    assert(g_diagnosticLogs == before + 4);

    // Interrupts are counted all the time, but only the first eight are logged.
    before = g_diagnosticLogs;
    for (i = 0; i < 100; i++) {
        GenetLogInterrupt(&controller, GENET_INTR0_RX_DONE);
    }
    assert(controller.Diagnostics.Interrupts == 100);
    assert(g_diagnosticLogs == before + 8);
}
#endif

int
main(void)
{
    __Mdio();
    __Integration();
    __FirmwareReset();
    __Queues();
    __InterruptMasking();
    __Coalescing();
    __ReceiveDiscards();
    __PollFaults();
#ifdef VALI_ETHERNET_DIAGNOSTICS
    __Diagnostics();
#endif
    puts("GENET register and packet tests passed");
    return 0;
}
