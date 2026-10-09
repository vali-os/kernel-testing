/** Real PCI/IRQ/DMA/IPC acceptance test, enabled only in dedicated test images.
 * Uses normal deviced discovery and QEMU's isolated user-network gateway ARP.
 */
#include "adapters/adapters.h"
#include <ddk/utils.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>

static atomic_uint g_tx;
static atomic_uint g_rx;
static atomic_bool g_failed;
static uuid_t      g_device;
static uint8_t     g_mac[6];
static NetAdapterTxPacket_t g_closePacket;
static NetAdapterRxPacket_t g_heldRx[2];
static atomic_uint g_heldRxCount;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ERROR("VIRTIO NET FAIL line %d: %s", __LINE__, #condition);                            \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)

static void
Pause(
        void)
{
    thrd_sleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
}

static void
Receive(
        uuid_t      device,
        uint32_t    port,
        const void* data,
        uint32_t    length)
{
    if (device != g_device || port || length < 42) {
        return;
    }
    const uint8_t* frame = data;
    const uint8_t  gateway[4] = {10, 0, 2, 2};
    const uint8_t  local[4] = {10, 0, 2, 15};
    if (frame[12] != 8 || frame[13] != 6 || frame[20] != 0 || frame[21] != 2) {
        return;
    }
    if (memcmp(frame, g_mac, 6) || memcmp(frame + 28, gateway, 4) || memcmp(frame + 32, g_mac, 6) ||
        memcmp(frame + 38, local, 4)) {
        atomic_store(&g_failed, true);
    }
    atomic_fetch_add(&g_rx, 1);
}

/** Publish accepted RX ownership to the test thread. It deliberately retains
 * the first reply through restart and both replies through remote close.
 */
static bool
ReceivePacket(uuid_t device, uint32_t port, const NetAdapterRxPacket_t* packet)
{
    const uint8_t* frame = packet->Data;
    if (device != g_device || port || packet->Length < 42 ||
        frame[12] != 8 || frame[13] != 6 || frame[20] != 0 || frame[21] != 2) {
        return false;
    }
    unsigned index = atomic_load(&g_heldRxCount);
    if (index >= 2) {
        return false;
    }
    g_heldRx[index] = *packet;
    atomic_store(&g_heldRxCount, index + 1);
    Receive(device, port, packet->Data, packet->Length);
    return true;
}

static void
Transmitted(
        uuid_t   device,
        uint32_t port,
        uint64_t cookie,
        oserr_t  status)
{
    if (device != g_device || port) {
        return;
    }
    if (status != OS_EOK || cookie != atomic_load(&g_tx) + 1) {
        atomic_store(&g_failed, true);
    }
    atomic_fetch_add(&g_tx, 1);
}

static int
WaitState(
        enum NetAdapterState state,
        uint64_t             run)
{
    NetAdapterSnapshot_t snapshot;
    for (unsigned retry = 0; retry < 3000; ++retry) {
        oserr_t status = NetworkAdaptersSnapshot(g_device, 0, &snapshot);
        if (state == NET_ADAPTER_CLOSED && status == OS_ENOENT && !g_closePacket.Data && !atomic_load(&g_heldRxCount)) {
            return 0; // Safe destruction may already have removed the entry.
        }
        CHECK(status == OS_EOK);
        if (snapshot.State == state && (!run || snapshot.Run == run)) {
            return 0;
        }
        if (snapshot.State == NET_ADAPTER_FAILED || snapshot.State == NET_ADAPTER_QUARANTINED) {
            ERROR("VIRTIO NET state=%u error=%u", snapshot.State, snapshot.LastError);
            CHECK(false);
        }
        Pause();
    }
    ERROR("VIRTIO NET timeout state=%u wanted=%u run=%llu",
          snapshot.State,
          state,
          (unsigned long long)snapshot.Run);
    CHECK(false);
}

static int
Exercise(
        void)
{
    NOTICE("VIRTIO NET START");
    bool found = false;
    for (unsigned retry = 0; retry < 3000; ++retry) {
        if (NetworkAdaptersTestFindVirtual(&g_device) == OS_EOK) {
            found = true;
            break;
        }
        Pause();
    }
    CHECK(found);
    NetAdapterSnapshot_t snapshot;
    CHECK(NetworkAdaptersSnapshot(g_device, 0, &snapshot) == OS_EOK);
    struct ctt_netadapter_mac mac = snapshot.Info.current_mac;
    uint8_t address[] = {mac.octet0, mac.octet1, mac.octet2, mac.octet3, mac.octet4, mac.octet5};
    memcpy(g_mac, address, sizeof(g_mac));
    NetworkAdapterOps_t hooks = {.Receive = Receive, .Transmitted = Transmitted,
                                 .ReceivePacket = ReceivePacket};
    NetworkAdaptersSetHooks(&hooks);
    for (unsigned run = 1; run <= 2; ++run) {
        CHECK(WaitState(NET_ADAPTER_RUNNING, run) == 0);
        // Deliberately short: driver must append initialized Ethernet padding
        // without changing the client-visible 42-byte completion length.
        // Construct the second run directly in registered TX storage; the first
        // retains coverage of the compatibility copying API.
        NetAdapterTxPacket_t packet = {0};
        uint8_t copiedFrame[42] = {0};
        uint8_t* frame = copiedFrame;
        if (run == 2) {
            // This second builder stays unsubmitted through stop and close.
            CHECK(NetworkAdaptersTxAcquire(g_device, 0, &g_closePacket) == OS_EOK);
            CHECK(NetworkAdaptersTxAcquire(g_device, 0, &packet) == OS_EOK);
            CHECK(packet.Capacity >= sizeof(copiedFrame));
            frame = packet.Data;
            memset(frame, 0, sizeof(copiedFrame));
        }
        memset(frame, 0xff, 6);
        memcpy(frame + 6, g_mac, 6);
        frame[12] = 8;
        frame[13] = 6;
        frame[15] = 1;
        frame[16] = 8;
        frame[18] = 6;
        frame[19] = 4;
        frame[21] = 1;
        memcpy(frame + 22, g_mac, 6);
        frame[28] = 10;
        frame[30] = 2;
        frame[31] = 15;
        frame[38] = 10;
        frame[40] = 2;
        frame[41] = 2;
        if (run == 2) {
            oserr_t status = NetworkAdaptersTxSubmit(g_device, 0, &packet, sizeof(copiedFrame), run);
            if (status != OS_EOK) {
                (void)NetworkAdaptersTxCancel(g_device, 0, &packet);
            }
            CHECK(status == OS_EOK && packet.Data == NULL);
        } else {
            CHECK(NetworkAdaptersSend(g_device, 0, frame, sizeof(copiedFrame), run) == OS_EOK);
        }
        for (unsigned retry = 0;
             retry < 1000 && (atomic_load(&g_tx) < run || atomic_load(&g_rx) < run);
             ++retry) {
            Pause();
        }
        CHECK(!atomic_load(&g_failed) && atomic_load(&g_tx) == run && atomic_load(&g_rx) >= run);
        CHECK(NetworkAdaptersSetRunning(g_device, 0, false) == OS_EOK);
        CHECK(WaitState(NET_ADAPTER_STOPPED, run) == 0);
        CHECK(NetworkAdaptersSnapshot(g_device, 0, &snapshot) == OS_EOK);
        CHECK(!snapshot.Buffers.TxOutstanding && !snapshot.Buffers.RxOutstanding &&
              !snapshot.PendingBatches);
        for (unsigned retry = 0; retry < 300 && snapshot.Counters.tx_packets < run; ++retry) {
            Pause();
            CHECK(NetworkAdaptersSnapshot(g_device, 0, &snapshot) == OS_EOK);
        }
        CHECK(snapshot.Counters.tx_packets == run &&
              snapshot.Counters.tx_bytes == run * sizeof(copiedFrame));
        CHECK(snapshot.Counters.rx_packets >= run && !snapshot.Counters.tx_errors &&
              !snapshot.Counters.rx_errors);
        NOTICE("VIRTIO NET PASS run %u: discovered PCI, ARP TX/RX, DMA stop and retirement", run);
        if (run == 1) {
            CHECK(NetworkAdaptersSetRunning(g_device, 0, true) == OS_EOK);
        }
    }
    return 0;
}

static int
Run(
        void* unused)
{
    (void)unused;
    int result = Exercise();
    if (g_device) {
        if (NetworkAdaptersClose(g_device, 0) != OS_EOK || WaitState(NET_ADAPTER_CLOSED, 0)) {
            result = -1;
        }
    }
    if (g_closePacket.Data) {
        // Remote close fences DMA but must preserve our local writable lease.
        memset(g_closePacket.Data, 0x5a, g_closePacket.Capacity);
        oserr_t status = NetworkAdaptersTxCancel(g_device, 0, &g_closePacket);
        if (status != OS_EOK) {
            ERROR("VIRTIO NET FAIL returning packet after close: %i", status);
            result = -1;
        }
    }
    unsigned retained = atomic_load(&g_heldRxCount);
    if (!result && retained != 2) {
        ERROR("VIRTIO NET FAIL expected two retained RX replies, got %u", retained);
        result = -1;
    }
    for (unsigned i = 0; i < retained; ++i) {
        const uint8_t* data = g_heldRx[i].Data;
        if (g_heldRx[i].Length < 42 || data[12] != 8 || data[13] != 6 || data[21] != 2 ||
            memcmp(data + 32, g_mac, 6)) {
            ERROR("VIRTIO NET FAIL retained RX data changed across close");
            result = -1;
        }
        if (NetworkAdaptersRxRelease(g_device, 0, &g_heldRx[i]) != OS_EOK) {
            ERROR("VIRTIO NET FAIL releasing retained RX after close");
            result = -1;
        }
    }
    if (!result) {
        NOTICE("VIRTIO NET PASS retained RX: replies survive restart and safe close");
        NOTICE("VIRTIO NET PASS direct TX: pool construction and held view across close");
        NOTICE("VIRTIO NET PASS all: discovery, real virtqueues, IRQ, restart and safe close");
    }
    return result;
}

void
NetworkAdaptersRunVirtioTests(
        void)
{
    thrd_t thread;
    if (thrd_create(&thread, Run, NULL) != thrd_success) {
        ERROR("VIRTIO NET FAIL starting test thread");
        return;
    }
    thrd_detach(thread);
}
