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
// PHY reads that fail before the stub returns g_link again.
static unsigned g_phyFails;
// The TX engine reports stopped only while the MAC transmitter is on.
static int g_txNeedsTransmitter;
// The card ignores writes to the ring hardware index registers.
static int g_stickyIndex;
// MAC command writes that switched traffic off.
static unsigned g_trafficOffWrites;

// Writes to the MAC command and DMA control registers, in order, with the
// total delay at the time of each write.
struct __Write {
    uint32_t Offset;
    uint32_t Value;
    unsigned Delay;
};
static struct __Write g_writes[64];
static unsigned       g_writeCount;
static int            g_recordWrites;

size_t
DeviceIoRead(
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
DeviceIoWrite(
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
        if (!(value & GENET_COMMAND_TRAFFIC)) {
            g_trafficOffWrites++;
        }
    }
    if (g_recordWrites && (offset == GENET_COMMAND || offset == GENET_TX_DMA + GENET_DMA_CONTROL ||
                           offset == GENET_RX_DMA + GENET_DMA_CONTROL)) {
        assert(g_writeCount < sizeof(g_writes) / sizeof(g_writes[0]));
        g_writes[g_writeCount++] = (struct __Write){ (uint32_t)offset, (uint32_t)value, g_delay };
    }
    if (g_stickyIndex && (offset == GENET_TX_RING + GENET_RING_HARDWARE_INDEX ||
                          offset == GENET_RX_RING + GENET_RING_HARDWARE_INDEX)) {
        return OS_EOK;
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
        if (offset == GENET_TX_DMA + 4 && g_txNeedsTransmitter &&
            !(g_registers[GENET_COMMAND / 4] & GENET_COMMAND_TX_ENABLE)) {
            g_registers[(offset + 4) / 4] = 0;
        }
    }
    if (offset == g_failOffset) {
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

oserr_t
DDKStallUs(
    _In_ uint32_t microseconds)
{
    if (g_delayFails) {
        return OS_EDEVFAULT;
    }
    g_time += microseconds;
    g_delay += microseconds;
    return OS_EOK;
}

// g_time counts microseconds and moves only when the driver busy-waits.
uint64_t
DDKGetTimeMs(
    _In_ enum OSTimeSource source)
{
    assert(source == OSTimeSource_MONOTONIC);
    return g_time / 1000;
}

oserr_t
PhyReadLink(
    _In_ PhyDevice_t* phy,
    _In_ struct PhyLink* link)
{
    (void)phy;
    if (g_phyFails) {
        g_phyFails--;
        memset(link, 0, sizeof(*link));
        return OS_ETIMEOUT;
    }
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
    GenetMdio_t mdio = { .Registers = &registers };
    MdioBus_t bus;
    uint16_t value = 0xabcd;
    unsigned commands;

    assert(MdioInitialize(&bus, &g_genetMdioOperations, &mdio, 1) == OS_EOK);

    // The model finishes a transaction on the third read after the command.
    // The wait must stay within tens of microseconds.
    g_delay = 0;
    assert(MdioRead(&bus, 2, &value) == OS_EOK && value == 0x1234);
    assert(g_delay > 0 && g_delay <= 50);
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
    GenetWrapper_t integration = {0};
    DeviceIo_t registers = { .Type = DeviceIoMemoryBased };
    struct DeviceEthernetConfiguration config = {
        .Version = 1, .Mode = DeviceEthernetRgmiiRxId,
        .MdioOffset = 0xe14, .MdioLength = 8
    };
    const struct {
        uint32_t               Offset;
        enum GenetWrapperState State;
    } failures[] = {
        { 4, GENET_WRAPPER_PORT_CHANGED },
        { 0x8c, GENET_WRAPPER_OOB_CHANGED },
        { 0xe18, GENET_WRAPPER_MDIO_CHANGED }
    };
    unsigned i;

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
    for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        g_failOffset = failures[i].Offset;
        assert(GenetWrapperConfigurePhy(&integration, &registers, &config) == OS_EDEVFAULT);
        assert(integration.State == failures[i].State);
        assert(GenetWrapperRestorePhy(&integration) == OS_EDEVFAULT);
        assert(integration.State == failures[i].State && integration.Registers == &registers);
        g_failOffset = 0;
        assert(GenetWrapperRestorePhy(&integration) == OS_EOK);
        assert(integration.State == GENET_WRAPPER_UNCHANGED && integration.Registers == NULL);
        assert(g_registers[1] == 0x123 && g_registers[0x8c / 4] == 0x10030);
        assert(g_registers[0xe18 / 4] == 0x1000);
    }
}

static void
__FirmwareReset(void)
{
    GenetController_t controller = {0};
    unsigned delay;
    unsigned commands;

    controller.State = GENET_STATE_MAPPED;
    controller.Adapter.Controller = &controller;
    g_registers[8 / 4] = 2;
    g_registers[0x80c / 4] = 0x02000001;
    g_registers[0x810 / 4] = 0x0203;
    delay = g_delay;
    assert(GenetWrapperReleaseReset(&controller.Wrapper, &controller.Registers) == OS_EOK);
    assert(g_registers[8 / 4] == 0 && g_delay >= delay + 10);
    assert(controller.Wrapper.Reset == GENET_WRAPPER_RESET_RELEASED);
    assert(g_registers[0x80c / 4] == 0x02000001);
    assert(g_registers[0x810 / 4] == 0x0203);
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);

    g_failOffset = 8;
    delay = g_delay;
    assert(GenetWrapperReleaseReset(&controller.Wrapper, &controller.Registers) == OS_EDEVFAULT);
    assert(g_delay == delay);
    assert(controller.Wrapper.Reset == GENET_WRAPPER_RESET_UNKNOWN);
    commands = g_commandWrites;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EDEVFAULT);
    assert(g_commandWrites == commands);
    g_failOffset = 0;
    g_delayFails = 1;
    assert(GenetWrapperReleaseReset(&controller.Wrapper, &controller.Registers) == OS_EDEVFAULT);
    assert(controller.Wrapper.Reset == GENET_WRAPPER_RESET_UNKNOWN);
    assert(GenetQueuesStop(&controller.Adapter) == OS_EDEVFAULT);
    assert(g_commandWrites == commands);
    g_delayFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    // The stop writes the command register twice: receiver off first, then
    // transmitter off once both engines have stopped.
    assert(controller.Wrapper.Reset == GENET_WRAPPER_RESET_RELEASED && g_commandWrites == commands + 2);
}

static void
__Queues(void)
{
    GenetController_t controller = {0};
    EthernetSlot_t tx = {0};
    EthernetSlot_t rx = {0};
    uint8_t frame[1514];
    uint8_t* storage;
    uint32_t status;
    unsigned completions;
    struct PhyLink down = {0};

    memset(g_registers, 0, sizeof(g_registers));
    GenetThresholdsDefaults(&controller.Thresholds);
    controller.State = GENET_STATE_MAPPED;
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
    assert(GenetTransferSubmit(&controller.Adapter, &tx) == OS_EOK);
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
    GenetTransferPoll(&controller.Adapter);
    assert(g_completions == 1 && g_lastSlot == &tx && g_lastLength == 42);

    rx.Bytes = frame + 3;
    rx.Length = sizeof(frame) - 3;
    rx.Receive = true;
    rx.Pending = true;
    assert(GenetTransferSubmit(&controller.Adapter, &rx) == OS_EOK);
    controller.Queues.Receive.DmaConsumer = 65535;
    storage = controller.Queues.Storage.Data + 31 * 2048;
    status = ((66U + 60U) << 16) | 0x6000;
    memcpy(storage, &status, 4);
    memcpy(storage + 66, frame, 60);
    memset(frame, 0, sizeof(frame));
    g_registers[(GENET_RX_RING + 8) / 4] = 0;
    GenetTransferPoll(&controller.Adapter);
    assert(g_completions == 2 && g_lastSlot == &rx && g_lastLength == 60);
    assert(!memcmp(frame + 3, controller.Mac, 6));
    assert(frame[3 + 59] == 0xab && frame[3 + 60] == 0);
    assert(controller.Queues.Receive.DmaConsumer == 0);

    // Malformed hardware lengths never escape the private receive buffer.
    rx.Pending = true;
    assert(GenetTransferSubmit(&controller.Adapter, &rx) == OS_EOK);
    storage = controller.Queues.Storage.Data;
    status = (4095U << 16) | 0x6000;
    memcpy(storage, &status, 4);
    g_registers[(GENET_RX_RING + 8) / 4] = 1;
    GenetTransferPoll(&controller.Adapter);
    assert(g_completions == 2 && g_receiveDrops[EthernetReceiveError] == 1);
    assert(controller.Queues.Receive.DmaConsumer == 1);
    assert(controller.Queues.Receive.PostedSlots.Head == 1);
    assert(controller.Queues.Receive.PostedSlots.Count == 1);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == &rx && rx.Pending);
    // A down link clears only the RGMII link bit. MAC traffic stays on while
    // the queues run.
    assert(GenetLinkApply(&controller, &down) == OS_EOK);
    assert((g_registers[GENET_COMMAND / 4] & 3) == 3);
    assert(!(g_registers[0x8c / 4] & 0x10));

    // Failed producer publication must keep the TX accepted until stop.
    tx.Pending = true;
    g_failOffset = GENET_TX_RING + 12;
    assert(GenetTransferSubmit(&controller.Adapter, &tx) == OS_EINPROGRESS);
    g_failOffset = 0;
    g_stopFails = 1;
    completions = g_completions;
    assert(GenetQueuesStop(&controller.Adapter) == OS_ETIMEOUT);
    assert(controller.Queues.Transmit.Slots[0] == &tx && controller.Queues.DmaState == GENET_DMA_STATE_MAY_BE_RUNNING);
    assert(controller.Queues.Receive.DmaConsumer == 1);
    assert(controller.Queues.Receive.PostedSlots.Head == 1);
    assert(controller.Queues.Receive.PostedSlots.Count == 1);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == &rx);
    assert(g_completions == completions);
    g_stopFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(controller.Queues.DmaState == GENET_DMA_STATE_STOPPED && controller.Queues.Transmit.Slots[0] == NULL);
    assert(controller.Queues.Receive.DmaConsumer == 0);
    assert(controller.Queues.Receive.PostedSlots.Head == 0);
    assert(controller.Queues.Receive.PostedSlots.Count == 0);
    assert(controller.Queues.Receive.PostedSlots.Slots[1] == NULL);
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_txFlush == 2 && g_rxFlush == 2);
    assert(g_faults == 0);
#ifdef VALI_ETHERNET_DIAGNOSTICS
    assert(controller.Diagnostics.Packets[GENET_EVENT_TX_QUEUED] == 1);
    assert(controller.Diagnostics.Packets[GENET_EVENT_TX_COMPLETED] == 1);
    assert(controller.Diagnostics.Packets[GENET_EVENT_RX_RECEIVED] == 1);
    assert(controller.Diagnostics.Packets[GENET_EVENT_RX_DELIVERED] == 1);
    assert(controller.Diagnostics.Packets[GENET_EVENT_RX_ERROR] == 1);
#endif
    free(controller.Queues.Storage.Data);
}

static void
__InterruptMasking(void)
{
    GenetController_t controller = {0};

    memset(g_registers, 0, sizeof(g_registers));
    GenetThresholdsDefaults(&controller.Thresholds);
    controller.State = GENET_STATE_MAPPED;
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
    assert(GenetMacUnmaskDmaInterrupts(&controller, GENET_INTR0_DMA_DONE) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == 0);

    // Interrupt mode unmasks only the packet causes, and only once both
    // engines run.
    controller.Interrupt.State = GENET_INTERRUPT_ENABLED;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == 0);
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);
    assert(g_registers[GENET_INTR0_MASK_CLEAR / 4] == (GENET_INTR0_RX_DONE | GENET_INTR0_TX_DONE));
    assert(GENET_INTR0_RX_DONE == (1U << 13) && GENET_INTR0_TX_DONE == (1U << 16));

    // Other causes are never unmasked, whatever the caller asks for.
    assert(GenetMacUnmaskDmaInterrupts(&controller, GENET_INTR_ALL) == OS_EOK);
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
    assert(controller.Queues.DmaState == GENET_DMA_STATE_MAY_BE_RUNNING && g_maskAtDmaStop == 0);
    g_failOffset = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(controller.Queues.DmaState == GENET_DMA_STATE_STOPPED);
    free(controller.Queues.Storage.Data);
}

static void
__Thresholds(void)
{
    GenetController_t controller = {0};
    size_t       timeout = (GENET_RX_DMA + GENET_DMA_RING16_TIMEOUT) / 4;
    size_t       txThreshold = (GENET_TX_RING + GENET_RING_DONE_THRESHOLD) / 4;
    size_t       rxThreshold = (GENET_RX_RING + GENET_RING_DONE_THRESHOLD) / 4;
    unsigned     txFlush;
    GenetThresholds_t invalid[] = {
        {0, 1, 50},     // No TX count would never interrupt.
        {33, 1, 50},    // More than the ring holds can never be reached.
        {10, 33, 50},
        {10, 4, 0},     // A batch of frames needs the timeout to flush it.
        {10, 0, 0},
        {10, 1, GENET_RX_TIMEOUT_MAX_US + 1}
    };

    // The defaults follow Linux: ten frames for TX, one for RX, 50 us timeout.
    GenetThresholdsDefaults(&controller.Thresholds);
    assert(controller.Thresholds.TxFrames == 10 && controller.Thresholds.RxFrames == 1);
    assert(controller.Thresholds.RxTimeoutUs == 50);
    assert(GenetThresholdsValidate(&controller.Thresholds, GENET_RING_COUNT) == OS_EOK);

    // Timeouts count in 8.192 us ticks and never round down.
    assert(GenetThresholdsTimeoutTicks(0) == 0);
    assert(GenetThresholdsTimeoutTicks(1) == 1);
    assert(GenetThresholdsTimeoutTicks(50) == 7);
    assert(GenetThresholdsTimeoutTicks(200) == 25);
    assert(GenetThresholdsTimeoutTicks(8192) == 1000);
    assert(GenetThresholdsTimeoutTicks(GENET_RX_TIMEOUT_MAX_US) == 64000);
    assert(GenetThresholdsTimeoutTicks(GENET_RX_TIMEOUT_MAX_US) <= GENET_DMA_TIMEOUT_MASK);

    // The edges that are valid.
    assert(GenetThresholdsValidate(&(GenetThresholds_t){1, 1, 0}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetThresholdsValidate(&(GenetThresholds_t){32, 32, 1}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetThresholdsValidate(&(GenetThresholds_t){10, 0, 100}, GENET_RING_COUNT) == OS_EOK);
    assert(GenetThresholdsValidate(
        &(GenetThresholds_t){10, 1, GENET_RX_TIMEOUT_MAX_US}, GENET_RING_COUNT) == OS_EOK);

    memset(g_registers, 0, sizeof(g_registers));
    controller.State = GENET_STATE_MAPPED;
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
    controller.Thresholds = (GenetThresholds_t){ .TxFrames = 1, .RxFrames = 0, .RxTimeoutUs = 200 };
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
        assert(GenetThresholdsValidate(&invalid[i], GENET_RING_COUNT) == OS_EINVALPARAMS);
        controller.Thresholds = invalid[i];
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
    GenetController_t controller = {0};
    unsigned drops = g_receiveDrops[EthernetReceiveNoBuffer];
    unsigned faults = g_faults;
    size_t   index = (GENET_RX_RING + GENET_RING_HARDWARE_INDEX) / 4;

    memset(g_registers, 0, sizeof(g_registers));
    controller.Adapter.Controller = &controller;
    controller.Queues.DmaState = GENET_DMA_STATE_MAY_BE_RUNNING;

    // Drops sit in the upper half of the hardware index, next to the frame
    // index. Each one is reported once, as a missing receive buffer.
    g_registers[index] = 5U << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 5);
    assert(controller.Queues.Receive.ReportedDiscards == 5);
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 5);
    g_registers[index] = 7U << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7);
    assert(g_registers[index] == 7U << GENET_RING_DISCARD_SHIFT);

    // A counter that went backwards is taken as the new starting point.
    g_registers[index] = 3U << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7);
    assert(controller.Queues.Receive.ReportedDiscards == 3);

    // The counter is cleared before it can run out of range.
    g_registers[index] = (GENET_RING_DISCARD_RESET_THRESHOLD - 1) << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_registers[index] == (GENET_RING_DISCARD_RESET_THRESHOLD - 1) << GENET_RING_DISCARD_SHIFT);
    g_registers[index] = GENET_RING_DISCARD_RESET_THRESHOLD << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops + 7 + GENET_RING_DISCARD_RESET_THRESHOLD - 3);
    assert(g_registers[index] == 0 && controller.Queues.Receive.ReportedDiscards == 0);

    // Drops arriving after the clear are counted from zero again.
    g_registers[index] = 2U << GENET_RING_DISCARD_SHIFT;
    GenetTransferPoll(&controller.Adapter);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] ==
        drops + 7 + GENET_RING_DISCARD_RESET_THRESHOLD - 3 + 2);

    // A failed clear is a device fault and the drops stay reported.
    g_registers[index] = GENET_RING_DISCARD_RESET_THRESHOLD << GENET_RING_DISCARD_SHIFT;
    g_failOffset = GENET_RX_RING + GENET_RING_HARDWARE_INDEX;
    GenetTransferPoll(&controller.Adapter);
    g_failOffset = 0;
    assert(g_faults == faults + 1);
    assert(controller.Queues.Receive.ReportedDiscards == GENET_RING_DISCARD_RESET_THRESHOLD);

    // Stopping the queues forgets the old count, as the ring registers restart at zero.
    controller.Queues.DmaState = GENET_DMA_STATE_STOPPED;
    controller.State = GENET_STATE_MAPPED;
    controller.Wrapper.Reset = GENET_WRAPPER_RESET_RELEASED;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(controller.Queues.Receive.ReportedDiscards == 0);
#ifdef VALI_ETHERNET_DIAGNOSTICS
    // 7 drops, then up to the clear threshold, 2 more, and the failed clear's rereport.
    assert(controller.Diagnostics.Packets[GENET_EVENT_RX_RING_FULL] ==
        7 + (GENET_RING_DISCARD_RESET_THRESHOLD - 3) + 2 + (GENET_RING_DISCARD_RESET_THRESHOLD - 2));
#endif
}

static void
__PollFaults(void)
{
    GenetController_t controller = {0};
    unsigned faults = g_faults;
    uint64_t time = g_time;

    memset(g_registers, 0, sizeof(g_registers));
    controller.Adapter.Controller = &controller;
    controller.Queues.DmaState = GENET_DMA_STATE_MAY_BE_RUNNING;

    // Invalid TX progress must fault before the RX consumer register is written.
    g_registers[(GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4] = 1;
    g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] = 1234;
    GenetTransferPoll(&controller.Adapter);
    assert(g_faults == faults + 1);
    assert(g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] == 1234);

    // A stalled TX descriptor must also prevent RX processing in this poll.
    // The stall check only counts time while the link is up.
    g_registers[(GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4] = 0;
    controller.Link.Applied = g_link;
    controller.Queues.Transmit.Producer = 1;
    controller.Queues.Transmit.ProgressAt = DDKGetTimeMs(OSTimeSource_MONOTONIC);
    g_time += 5001000;
    GenetTransferPoll(&controller.Adapter);
    assert(g_faults == faults + 2);
    assert(g_registers[(GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4] == 1234);
    g_time = time;
}

extern unsigned g_diagnosticLogs;

static void
__StartController(
    _Out_ GenetController_t* controller)
{
    memset(controller, 0, sizeof(*controller));
    memset(g_registers, 0, sizeof(g_registers));
    GenetThresholdsDefaults(&controller->Thresholds);
    controller->State = GENET_STATE_MAPPED;
    controller->Adapter.Controller = controller;
    controller->Mac[0] = 2;
    controller->Mac[5] = 1;
    assert(GenetQueuesPrepare(&controller->Adapter) == OS_EOK);
    assert(GenetQueuesStart(&controller->Adapter) == OS_EOK);
}

static void
__QueueTransmit(
    _In_ GenetController_t* controller,
    _In_ EthernetSlot_t*    slot,
    _In_ uint8_t*           frame)
{
    memset(frame, 0xab, 60);
    memcpy(frame, controller->Mac, 6);
    *slot = (EthernetSlot_t){ .Bytes = frame, .Length = 60, .Pending = true };
    assert(GenetTransferSubmit(&controller->Adapter, slot) == OS_EOK);
}

static void
__StopOrder(void)
{
    static GenetController_t controller;
    EthernetSlot_t      tx;
    uint8_t             frame[60];
    const uint32_t      txControl = GENET_TX_DMA + GENET_DMA_CONTROL;
    const uint32_t      rxControl = GENET_RX_DMA + GENET_DMA_CONTROL;
    const uint32_t      running = GENET_COMMAND_PAUSE_IGNORE | GENET_COMMAND_SPEED_1000 | GENET_COMMAND_TRAFFIC;
    const uint32_t      bootloader = GENET_COMMAND_SPEED_1000 | GENET_COMMAND_HALF_DUPLEX |
                                     GENET_COMMAND_TRAFFIC | (1U << 22);

    // A frame is still queued, and the model's TX engine only stops while
    // the transmitter is on. Linux switches the receiver off first and the
    // transmitter off last, after both engines have stopped.
    __StartController(&controller);
    __QueueTransmit(&controller, &tx, frame);
    assert(g_registers[GENET_COMMAND / 4] == running);
    g_txNeedsTransmitter = 1;
    g_recordWrites = 1;
    g_writeCount = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    g_recordWrites = 0;
    g_txNeedsTransmitter = 0;
    assert(g_writeCount == 6);
    assert(g_writes[0].Offset == GENET_COMMAND && g_writes[0].Value == (running & ~GENET_COMMAND_RX_ENABLE));
    assert(g_writes[1].Offset == txControl && !(g_writes[1].Value & GENET_DMA_ENABLE));
    assert(g_writes[2].Offset == txControl && g_writes[2].Value == 0);
    assert(g_writes[3].Offset == rxControl && !(g_writes[3].Value & GENET_DMA_ENABLE));
    assert(g_writes[3].Delay >= g_writes[2].Delay + 10000);
    assert(g_writes[4].Offset == rxControl && g_writes[4].Value == 0);
    assert(g_writes[5].Offset == GENET_COMMAND && g_writes[5].Value == GENET_COMMAND_PAUSE_IGNORE);
    assert(controller.Queues.DmaState == GENET_DMA_STATE_STOPPED);
    assert(controller.Queues.Transmit.Slots[0] == NULL);

    // A TX engine that never stops keeps the transmitter on, so a retry can
    // still drain it. The queue state stays as it was.
    __StartController(&controller);
    __QueueTransmit(&controller, &tx, frame);
    g_stopFails = 1;
    assert(GenetQueuesStop(&controller.Adapter) == OS_ETIMEOUT);
    assert(g_registers[GENET_COMMAND / 4] == (running & ~GENET_COMMAND_RX_ENABLE));
    assert(controller.Queues.DmaState == GENET_DMA_STATE_MAY_BE_RUNNING);
    assert(controller.Queues.Transmit.Slots[0] == &tx);
    g_stopFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    free(controller.Queues.Storage.Data);

    // During creation the command register holds whatever the bootloader
    // left. Only the receive enable bit is cleared first, and the final write
    // clears the rest.
    memset(&controller, 0, sizeof(controller));
    memset(g_registers, 0, sizeof(g_registers));
    controller.State = GENET_STATE_MAPPED;
    controller.Adapter.Controller = &controller;
    g_registers[GENET_COMMAND / 4] = bootloader;
    g_recordWrites = 1;
    g_writeCount = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    g_recordWrites = 0;
    assert(g_writeCount == 6);
    assert(g_writes[0].Offset == GENET_COMMAND && g_writes[0].Value == (bootloader & ~GENET_COMMAND_RX_ENABLE));
    assert(g_writes[5].Offset == GENET_COMMAND && g_writes[5].Value == GENET_COMMAND_PAUSE_IGNORE);
}

static void
__LinkDown(void)
{
    static GenetController_t controller;
    EthernetSlot_t      tx;
    uint8_t             frame[60];
    struct PhyLink      link;
    unsigned            faults = g_faults;
    unsigned            completions;
    unsigned            trafficOff;

    __StartController(&controller);
    __QueueTransmit(&controller, &tx, frame);

    // Pulling the cable clears the RGMII link bit only. The transmitter stays
    // on so the queued frame can still finish.
    g_link = (struct PhyLink){0};
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && !link.Carrier);
    assert((g_registers[GENET_COMMAND / 4] & GENET_COMMAND_TRAFFIC) == GENET_COMMAND_TRAFFIC);
    assert(!(g_registers[GENET_RGMII_OOB_CTRL / 4] & GENET_RGMII_LINK));
    assert(!controller.Link.Applied.Carrier);

    // However long the cable stays out, the session does not fault.
    g_time += 6000000;
    GenetTransferPoll(&controller.Adapter);
    g_time += 6000000;
    GenetTransferPoll(&controller.Adapter);
    assert(g_faults == faults);

    // The card finishes the frame on the dead link.
    completions = g_completions;
    g_registers[(GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4] = 1;
    GenetTransferPoll(&controller.Adapter);
    assert(g_completions == completions + 1 && !tx.Pending);

    // The same speed comes back. Nothing switches traffic off.
    trafficOff = g_trafficOffWrites;
    g_link = (struct PhyLink){1000, 1, 1};
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && link.Carrier);
    assert(g_registers[GENET_COMMAND / 4] ==
        (GENET_COMMAND_PAUSE_IGNORE | GENET_COMMAND_SPEED_1000 | GENET_COMMAND_TRAFFIC));
    assert(g_registers[GENET_RGMII_OOB_CTRL / 4] & GENET_RGMII_LINK);
    assert(g_trafficOffWrites == trafficOff);

    // A real speed change switches traffic off for a moment.
    g_link = (struct PhyLink){100, 1, 1};
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK);
    assert(g_trafficOffWrites == trafficOff + 1);
    assert(g_registers[GENET_COMMAND / 4] ==
        (GENET_COMMAND_PAUSE_IGNORE | GENET_COMMAND_SPEED_100 | GENET_COMMAND_TRAFFIC));

    // With the link up, a frame that makes no progress still faults.
    __QueueTransmit(&controller, &tx, frame);
    g_time += 5001000;
    GenetTransferPoll(&controller.Adapter);
    assert(g_faults == faults + 1);

    // With the queues stopped, a link that comes up leaves traffic off.
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    g_link = (struct PhyLink){1000, 1, 1};
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && link.Carrier);
    assert(!(g_registers[GENET_COMMAND / 4] & GENET_COMMAND_TRAFFIC));
    assert(g_registers[GENET_RGMII_OOB_CTRL / 4] & GENET_RGMII_LINK);
    free(controller.Queues.Storage.Data);
}

static void
__LinkReadTolerance(void)
{
    static GenetController_t controller;
    struct PhyLink      link;
    uint32_t            command;
    unsigned            commands;
    unsigned            logs;

    __StartController(&controller);
    command = g_registers[GENET_COMMAND / 4];
    commands = g_commandWrites;
    assert(GenetLinkPollInterval(&controller) == GENET_LINK_POLL_INTERVAL_MS);

    // Failed reads below the limit report a down link without an error, and
    // leave the MAC as it is.
    g_phyFails = GENET_LINK_READ_FAILURE_LIMIT - 1;
    for (unsigned i = 1; i < GENET_LINK_READ_FAILURE_LIMIT; i++) {
        logs = g_diagnosticLogs;
        link.Carrier = 1;
        assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && !link.Carrier);
        assert(controller.Link.ReadFailures == i);
        assert(g_diagnosticLogs > logs);
    }
    assert(g_registers[GENET_COMMAND / 4] == command && g_commandWrites == commands);
    assert(g_registers[GENET_RGMII_OOB_CTRL / 4] & GENET_RGMII_LINK);
    assert(controller.Link.Applied.Carrier);

    // A good read clears the count. The link matches what was applied, so the
    // MAC is not touched.
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && link.Carrier);
    assert(controller.Link.ReadFailures == 0 && g_commandWrites == commands);

    // At the limit the error is passed on, and the PHY is read less often.
    g_phyFails = GENET_LINK_READ_FAILURE_LIMIT + 1;
    for (unsigned i = 1; i < GENET_LINK_READ_FAILURE_LIMIT; i++) {
        assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK);
    }
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_ETIMEOUT && !link.Carrier);
    assert(GenetLinkPollInterval(&controller) == GENET_LINK_POLL_FAILED_INTERVAL_MS);
#ifndef VALI_ETHERNET_DIAGNOSTICS
    // The limit is logged once. Later failures add no log line.
    logs = g_diagnosticLogs;
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_ETIMEOUT);
    assert(g_diagnosticLogs == logs);
#else
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_ETIMEOUT);
#endif
    assert(g_registers[GENET_COMMAND / 4] == command && g_commandWrites == commands);
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && link.Carrier);
    assert(GenetLinkPollInterval(&controller) == GENET_LINK_POLL_INTERVAL_MS);
    free(controller.Queues.Storage.Data);

    // A failed read during start gives a down link. The start goes on, and
    // the next good read switches traffic on.
    memset(&controller, 0, sizeof(controller));
    memset(g_registers, 0, sizeof(g_registers));
    GenetThresholdsDefaults(&controller.Thresholds);
    controller.State = GENET_STATE_MAPPED;
    controller.Adapter.Controller = &controller;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(GenetLinkApply(&controller, &g_link) == OS_EOK);
    g_phyFails = 1;
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);
    assert(!controller.Link.Applied.Carrier);
    assert(!(g_registers[GENET_COMMAND / 4] & GENET_COMMAND_TRAFFIC));
    assert(GenetLinkRead(&controller.Adapter, &link) == OS_EOK && link.Carrier);
    assert((g_registers[GENET_COMMAND / 4] & GENET_COMMAND_TRAFFIC) == GENET_COMMAND_TRAFFIC);

    // Failures up to the limit during start fail the start.
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    g_phyFails = GENET_LINK_READ_FAILURE_LIMIT;
    controller.Link.ReadFailures = GENET_LINK_READ_FAILURE_LIMIT - 1;
    assert(GenetQueuesStart(&controller.Adapter) == OS_ETIMEOUT);
    g_phyFails = 0;
    free(controller.Queues.Storage.Data);
}

static void
__RingIndexBase(void)
{
    static GenetController_t controller;
    EthernetSlot_t      tx;
    EthernetSlot_t      rx = {0};
    uint8_t             frame[60];
    uint8_t             received[1514];
    uint8_t*            storage;
    uint32_t            status;
    unsigned            faults = g_faults;
    unsigned            completions = g_completions;
    unsigned            drops = g_receiveDrops[EthernetReceiveNoBuffer];
    unsigned            logs;
    const size_t        txHardware = (GENET_TX_RING + GENET_RING_HARDWARE_INDEX) / 4;
    const size_t        rxHardware = (GENET_RX_RING + GENET_RING_HARDWARE_INDEX) / 4;
    const size_t        txSoftware = (GENET_TX_RING + GENET_RING_SOFTWARE_INDEX) / 4;
    const size_t        rxSoftware = (GENET_RX_RING + GENET_RING_SOFTWARE_INDEX) / 4;

    // The card keeps its old indices, and three old ring-full drops.
    memset(&controller, 0, sizeof(controller));
    memset(g_registers, 0, sizeof(g_registers));
    GenetThresholdsDefaults(&controller.Thresholds);
    controller.State = GENET_STATE_MAPPED;
    controller.Adapter.Controller = &controller;
    controller.Mac[0] = 2;
    controller.Mac[5] = 1;
    g_stickyIndex = 1;
    g_registers[txHardware] = 0xfff0;
    g_registers[rxHardware] = (3U << GENET_RING_DISCARD_SHIFT) | 0x0567;
    logs = g_diagnosticLogs;
    assert(GenetQueuesPrepare(&controller.Adapter) == OS_EOK);
    assert(g_diagnosticLogs > logs);
    assert(controller.Queues.Transmit.HardwareBase == 0xfff0);
    assert(controller.Queues.Receive.HardwareBase == 0x0567);
    assert(controller.Queues.Receive.ReportedDiscards == 3);
    assert(g_registers[txSoftware] == 0xfff0 && g_registers[rxSoftware] == 0x0567);
    assert(GenetQueuesStart(&controller.Adapter) == OS_EOK);

    // Sent frames start at descriptor 0. The producer index moves on from the
    // card's value and wraps around with it.
    for (unsigned i = 0; i < 20; i++) {
        __QueueTransmit(&controller, &tx, frame);
        assert(g_registers[txSoftware] == ((0xfff0U + i + 1) & 0xffff));
        assert(g_registers[(GENET_TX_DESCRIPTORS + i * GENET_DESCRIPTOR_SIZE) / 4] & GENET_DESCRIPTOR_EOP);
        g_registers[txHardware] = (0xfff0U + i + 1) & 0xffff;
        GenetTransferPoll(&controller.Adapter);
        assert(g_completions == completions + i + 1 && !tx.Pending);
    }

    // Received frames start at descriptor 0 too. Old drops are not reported.
    rx = (EthernetSlot_t){ .Bytes = received, .Length = sizeof(received), .Receive = true, .Pending = true };
    assert(GenetTransferSubmit(&controller.Adapter, &rx) == OS_EOK);
    storage = controller.Queues.Storage.Data;
    status = ((GENET_RX_PREFIX + 60U) << GENET_DESCRIPTOR_LENGTH_SHIFT) | GENET_PACKET_FLAGS;
    memcpy(storage, &status, sizeof(status));
    memcpy(storage + GENET_RX_PREFIX, frame, 60);
    g_registers[rxHardware] = (3U << GENET_RING_DISCARD_SHIFT) | 0x0568;
    GenetTransferPoll(&controller.Adapter);
    assert(!rx.Pending && g_lastSlot == &rx && g_lastLength == 60);
    assert(g_receiveDrops[EthernetReceiveNoBuffer] == drops);
    assert(g_registers[rxSoftware] == 0x0568);
    assert(g_faults == faults);
    g_stickyIndex = 0;
    free(controller.Queues.Storage.Data);
}

#ifdef VALI_ETHERNET_DIAGNOSTICS
extern unsigned g_diagnosticLogs;

static void
__Diagnostics(void)
{
    GenetController_t controller = {0};
    unsigned before;
    unsigned i;

    before = g_diagnosticLogs;
    for (i = 0; i < 100; i++) {
        GenetLogPacket(&controller, GENET_EVENT_TX_QUEUED, 60, 0x88b5);
    }
    assert(controller.Diagnostics.Packets[GENET_EVENT_TX_QUEUED] == 100);
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

    // A PHY read result is logged only when it differs from the one before.
    before = g_diagnosticLogs;
    GenetLogLinkStatus(&controller, OS_EOK);
    assert(g_diagnosticLogs == before);
    GenetLogLinkStatus(&controller, OS_EDEVFAULT);
    GenetLogLinkStatus(&controller, OS_EDEVFAULT);
    assert(g_diagnosticLogs == before + 1);
    GenetLogLinkStatus(&controller, OS_EOK);
    assert(g_diagnosticLogs == before + 2);
}
#endif

static void
__RegisterList(void)
{
    DeviceIo_t registers = { .Type = DeviceIoMemoryBased };
    const GenetRegisterSetting_t settings[] = {
        { 0x10, 0x11111111 },
        GENET_REGISTER_WAIT(7),
        { 0x14, 0x22222222 },
        { 0x18, 0x33333333 }
    };
    const size_t count = sizeof(settings) / sizeof(settings[0]);
    unsigned delay;
    unsigned logs;

    // Writes go to the base plus the offset, in array order. A wait step only
    // waits. It writes nothing, and the register model rejects a write to the
    // offset that marks it.
    delay = g_delay;
    logs = g_diagnosticLogs;
    assert(GenetRegisterWriteList(&registers, 0x100, settings, count) == OS_EOK);
    assert(g_registers[0x110 / 4] == 0x11111111 && g_registers[0x114 / 4] == 0x22222222);
    assert(g_registers[0x118 / 4] == 0x33333333);
    assert(g_delay == delay + 7 && g_diagnosticLogs == logs);

    // A failed write ends the list. It is logged once, and the entries after
    // it are not written.
    g_registers[0x118 / 4] = 0;
    g_failOffset = 0x114;
    assert(GenetRegisterWriteList(&registers, 0x100, settings, count) == OS_EDEVFAULT);
    assert(g_registers[0x118 / 4] == 0 && g_diagnosticLogs == logs + 1);
    g_failOffset = 0;

    // A failed wait also ends the list, before the write that follows it.
    g_registers[0x110 / 4] = 0;
    g_registers[0x114 / 4] = 0;
    g_delayFails = 1;
    assert(GenetRegisterWriteList(&registers, 0x100, settings, count) == OS_EDEVFAULT);
    assert(g_registers[0x110 / 4] == 0x11111111 && g_registers[0x114 / 4] == 0);
    assert(g_diagnosticLogs == logs + 2);
    g_delayFails = 0;

    // An empty list does nothing.
    assert(GenetRegisterWriteList(&registers, 0x100, settings, 0) == OS_EOK);
    memset(&g_registers[0x110 / 4], 0, 12);
}

static void
__WrapperReset(void)
{
    GenetWrapper_t wrapper = { .Reset = GENET_WRAPPER_RESET_RELEASED };
    DeviceIo_t registers = { .Type = DeviceIoMemoryBased };
    unsigned delay;
    unsigned logs;

    // Asserting the reset sets the MAC reset bit and holds it. From then on
    // the reset counts as not released.
    g_registers[8 / 4] = 0;
    delay = g_delay;
    assert(GenetWrapperAssertReset(&wrapper, &registers) == OS_EOK);
    assert(g_registers[8 / 4] == GENET_SYS_MAC_RESET);
    assert(g_delay == delay + GENET_WRAPPER_RESET_SETTLE_US);
    assert(wrapper.Reset == GENET_WRAPPER_RESET_UNKNOWN);
    assert(GenetWrapperReleaseReset(&wrapper, &registers) == OS_EOK);
    assert(g_registers[8 / 4] == 0 && wrapper.Reset == GENET_WRAPPER_RESET_RELEASED);

    // A failed write is logged and skips the wait. The write may still have
    // reached the card, so the reset no longer counts as released.
    g_failOffset = 8;
    delay = g_delay;
    logs = g_diagnosticLogs;
    assert(GenetWrapperAssertReset(&wrapper, &registers) == OS_EDEVFAULT);
    assert(g_delay == delay && wrapper.Reset == GENET_WRAPPER_RESET_UNKNOWN);
    assert(g_diagnosticLogs > logs);
    g_failOffset = 0;

    // A failed wait is reported too.
    wrapper.Reset = GENET_WRAPPER_RESET_RELEASED;
    g_delayFails = 1;
    logs = g_diagnosticLogs;
    assert(GenetWrapperAssertReset(&wrapper, &registers) == OS_EDEVFAULT);
    assert(wrapper.Reset == GENET_WRAPPER_RESET_UNKNOWN && g_diagnosticLogs > logs);
    g_delayFails = 0;
    assert(GenetWrapperReleaseReset(&wrapper, &registers) == OS_EOK);
    assert(g_registers[8 / 4] == 0);
}

static void
__WrapperRejections(void)
{
    GenetWrapper_t wrapper = {0};
    DeviceIo_t registers = { .Type = DeviceIoMemoryBased };
    struct DeviceEthernetConfiguration config = {
        .Version = 1, .Mode = DeviceEthernetRgmiiRxId,
        .MdioOffset = GENET_MDIO_COMMAND, .MdioLength = GENET_MDIO_WINDOW_SIZE
    };
    unsigned logs;

    // Every setup this driver does not handle is refused before the first
    // register write, with one log line that names the reason. The wrapper
    // then still counts as unchecked, so cleanup leaves the MAC alone.
    g_registers[0] = GENET_SYS_REVISION_MAJOR_V5 << GENET_SYS_REVISION_MAJOR_SHIFT;
    g_registers[GENET_SYS_PORT_CTRL / 4] = 0x123;

    // A register window that is too short to reach every register.
    registers.Access.Memory.Length = GENET_REGISTER_WINDOW_SIZE - 4;
    logs = g_diagnosticLogs;
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_ENOTSUPPORTED);
    assert(g_diagnosticLogs == logs + 1 && !GenetWrapperIsVerified(&wrapper));
    registers.Access.Memory.Length = GENET_REGISTER_WINDOW_SIZE;

    // A configuration version from a newer firmware interface.
    config.Version = 2;
    logs = g_diagnosticLogs;
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_ENOTSUPPORTED);
    assert(g_diagnosticLogs == logs + 1 && !GenetWrapperIsVerified(&wrapper));
    config.Version = 1;

    // An MDIO block of the wrong length.
    config.MdioLength = GENET_MDIO_WINDOW_SIZE + 4;
    logs = g_diagnosticLogs;
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_ENOTSUPPORTED);
    assert(g_diagnosticLogs == logs + 1 && !GenetWrapperIsVerified(&wrapper));
    config.MdioLength = GENET_MDIO_WINDOW_SIZE;

    // A revision register that does not report GENET v5.
    g_registers[0] = 5U << GENET_SYS_REVISION_MAJOR_SHIFT;
    logs = g_diagnosticLogs;
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_ENOTSUPPORTED);
    assert(!GenetWrapperIsVerified(&wrapper) && g_diagnosticLogs > logs);
    assert(g_registers[GENET_SYS_PORT_CTRL / 4] == 0x123);
    g_registers[0] = GENET_SYS_REVISION_MAJOR_V5 << GENET_SYS_REVISION_MAJOR_SHIFT;

    // A wrapper that is already configured is not configured twice.
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_EOK);
    assert(GenetWrapperIsVerified(&wrapper));
    logs = g_diagnosticLogs;
    assert(GenetWrapperConfigurePhy(&wrapper, &registers, &config) == OS_EBUSY);
    assert(g_diagnosticLogs == logs + 1);
    assert(GenetWrapperRestorePhy(&wrapper) == OS_EOK);
    assert(!GenetWrapperIsVerified(&wrapper) && g_registers[GENET_SYS_PORT_CTRL / 4] == 0x123);
}

int
main(void)
{
    __Mdio();
    __Integration();
    __WrapperRejections();
    __FirmwareReset();
    __WrapperReset();
    __RegisterList();
    __Queues();
    __InterruptMasking();
    __Thresholds();
    __ReceiveDiscards();
    __PollFaults();
    __StopOrder();
    __LinkDown();
    __LinkReadTolerance();
    __RingIndexBase();
#ifdef VALI_ETHERNET_DIAGNOSTICS
    __Diagnostics();
#endif
    puts("GENET register and packet tests passed");
    return 0;
}
