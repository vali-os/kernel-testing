#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../modules/eth/genet/genet.h"

static uint32_t g_registers[0x10000 / 4];
static uint64_t g_time;
static uint64_t g_resetDeadline;
static unsigned g_delay;
static unsigned g_commands;
static unsigned g_busy;
static int g_stuck;
static int g_mdioError;
static size_t g_failOffset;
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
    return (uint8_t*)slot->Pool->Memory.Payload + slot->Packet.data_offset;
}

void
EthernetComplete(
    _In_ EthernetDevice_t* adapter,
    _In_ EthernetSlot_t* slot,
    _In_ oserr_t status,
    _In_ uint32_t length)
{
    (void)adapter;
    assert(status == OS_EOK && slot && slot->Owned);
    slot->Owned = false;
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
    struct GenetIntegration integration = {0};
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
    assert(GenetIntegrationPrepare(&integration, &registers, &config) == OS_EOK);
    assert(g_registers[1] == 3);
    assert((g_registers[0x8c / 4] & 0x10070) == 0x40);
    assert(g_registers[0xe18 / 4] == (49U << 4 | 1));
    assert(GenetIntegrationRelease(&integration) == OS_EOK);
    assert(g_registers[1] == 0x123 && g_registers[0x8c / 4] == 0x10030);
    assert(g_registers[0xe18 / 4] == 0x1000);
    config.Mode = DeviceEthernetRgmiiId;
    assert(GenetIntegrationPrepare(&integration, &registers, &config) == OS_ENOTSUPPORTED);
    config.Mode = DeviceEthernetRgmiiRxId;
    config.MdioOffset = 0;
    assert(GenetIntegrationPrepare(&integration, &registers, &config) == OS_ENOTSUPPORTED);
    config.MdioOffset = 0xe14;
    g_failOffset = 0x8c;
    assert(GenetIntegrationPrepare(&integration, &registers, &config) == OS_EDEVFAULT);
    assert(integration.Changed == 2);
    assert(GenetIntegrationRelease(&integration) == OS_EDEVFAULT);
    g_failOffset = 0;
    assert(GenetIntegrationRelease(&integration) == OS_EOK);
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
    EthernetPool_t pool = {0};
    EthernetSlot_t tx = {0};
    EthernetSlot_t rx = {0};
    uint8_t frame[1514];
    uint8_t* storage;
    uint32_t status;
    unsigned completions;
    struct PhyLink down = {0};

    memset(g_registers, 0, sizeof(g_registers));
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
    pool.Memory.Payload = frame;
    pool.Description.direction = CTT_NETADAPTER_DIRECTION_TX;
    tx.Pool = &pool;
    tx.Packet.length = 42;
    tx.Owned = true;
    controller.Queues.TxProducer = 65535;
    controller.Queues.TxConsumer = 65535;
    g_registers[(GENET_TX_RING + 8) / 4] = 65535;
    assert(GenetQueuePacket(&controller.Adapter, &tx) == OS_EOK);
    assert(controller.Queues.TxProducer == 0);
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

    pool.Description.direction = CTT_NETADAPTER_DIRECTION_RX;
    rx.Pool = &pool;
    rx.Packet.length = sizeof(frame);
    rx.Packet.data_offset = 3;
    rx.Owned = true;
    assert(GenetQueuePacket(&controller.Adapter, &rx) == OS_EOK);
    controller.Queues.RxConsumer = 65535;
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
    assert(controller.Queues.RxConsumer == 0);

    // Malformed hardware lengths never escape the private receive buffer.
    storage = controller.Queues.Storage.Data;
    status = (4095U << 16) | 0x6000;
    memcpy(storage, &status, 4);
    g_registers[(GENET_RX_RING + 8) / 4] = 1;
    GenetPoll(&controller.Adapter);
    assert(g_completions == 2 && controller.Adapter.Session.Counters.rx_errors == 1);
    assert(GenetApplyLink(&controller, &down) == OS_EOK);
    assert(!(g_registers[GENET_COMMAND / 4] & 3));
    assert(!(g_registers[0x8c / 4] & 0x10));

    // Failed producer publication must keep the TX accepted until stop.
    pool.Description.direction = CTT_NETADAPTER_DIRECTION_TX;
    tx.Owned = true;
    g_failOffset = GENET_TX_RING + 12;
    assert(GenetQueuePacket(&controller.Adapter, &tx) == OS_EINPROGRESS);
    g_failOffset = 0;
    g_stopFails = 1;
    completions = g_completions;
    assert(GenetQueuesStop(&controller.Adapter) == OS_ETIMEOUT);
    assert(controller.Queues.Transmit[0] == &tx && controller.Queues.Running);
    assert(g_completions == completions);
    g_stopFails = 0;
    assert(GenetQueuesStop(&controller.Adapter) == OS_EOK);
    assert(!controller.Queues.Running && controller.Queues.Transmit[0] == NULL);
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
}
#endif

int
main(void)
{
    __Mdio();
    __Integration();
    __FirmwareReset();
    __Queues();
#ifdef VALI_ETHERNET_DIAGNOSTICS
    __Diagnostics();
#endif
    puts("GENET register and packet tests passed");
    return 0;
}
