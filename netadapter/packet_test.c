/** Exercise actual SHM leases and batch construction. Setup supplies negotiated
 * capabilities directly; close is delivered through the real control-reply API.
 * The SHM mock is the only storage replacement. No controller or scheduler
 * implementation is reproduced here.
 */
#include "session.h"
#include "net_shm_mock.h"
#include <assert.h>
#include <stdio.h>

/* This fixture never drives retry scheduling. Unexpected use is a test error. */
uint64_t __NetAdapterDeadline(uint64_t now, uint32_t interval)
{
    assert(!"packet fixture unexpectedly requested a retry deadline");
    return 0;
}

static unsigned callbacks;
static uint64_t lastCookie;
static oserr_t lastStatus;

static void Transmitted(void* context, uint64_t cookie, oserr_t status)
{
    ++callbacks;
    lastCookie = cookie;
    lastStatus = status;
}

static NetworkAdapter_t* CreatePolicy(uint32_t retained, uint32_t copies, uint32_t minimum)
{
    NetworkAdapter_t* adapter;
    NetAdapterConfig_t config;
    NetAdaterConfigInitializeDefault(&config);
    config.TxSlots = config.RxSlots = 4;
    config.RxRetainedSlots = retained;
    config.RxCopySlots = copies;
    NetAdapterCallbacks_t ops = {.Transmitted = Transmitted};
    assert(NetAdapterCreate(1, 2, 0, &config, &ops, &adapter) == OS_EOK);
    adapter->Session = (struct ctt_netadapter_session){10, 42};
    adapter->Mtu = 1500;
    adapter->Window.BatchSize = 4;
    adapter->Info = (struct ctt_netadapter_info){
        .min_version = 2, .max_version = 2, .max_frame_size = 1514, .max_batch_size = 4,
        .framing = CTT_NETADAPTER_FRAMING_ETHERNET,
        .buffer_alignment = 64, .required_headroom = 17, .required_tailroom = 7,
        .max_pools = 2, .max_pool_bytes = 131072, .max_registered_bytes = 262144,
        .max_slots_per_pool = 16, .max_queue_pairs = 1, .max_segments = 1,
        .max_outstanding_tx = 8, .max_outstanding_rx = 8,
        .max_unacked_completions = 16, .min_rx_slots = minimum
    };
    assert(NetAdapterSetupBuffers(adapter) == OS_EOK);
    for (int i = 0; i < 2; ++i) {
        enum ctt_netadapter_direction direction = i ? CTT_NETADAPTER_DIRECTION_RX : CTT_NETADAPTER_DIRECTION_TX;
        uint64_t id;
        struct ctt_netadapter_pool pool;
        assert(NetBuffersBeginRegistration(adapter->Buffers, direction, &id, &pool) == OS_EOK);
        assert(NetBuffersRegistered(adapter->Buffers, &adapter->Session, direction, i + 1) == OS_EOK);
    }
    adapter->State = NET_ADAPTER_RUNNING;
    adapter->Link.status = CTT_NETADAPTER_LINK_STATUS_UP;
    callbacks = 0;
    return adapter;
}

static NetworkAdapter_t* Create(void)
{
    return CreatePolicy(8, 8, 2);
}

static void Close(NetworkAdapter_t* adapter)
{
    NetAdapterClose(adapter);
    adapter->Control.Pending = true;
    adapter->Control.Request.Operation = SERVICE_CTT_NETADAPTER_CLOSE_ID;
    adapter->Control.Request.Serial = 17;
    NetAdapterReply_t reply = {.Status = OS_EOK};
    assert(HandleAdapterRequest(adapter, 2, 17, &reply, 0) == OS_EOK);
    assert(adapter->State == NET_ADAPTER_CLOSED);
}

static void TestPublication(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t building, packet;
    assert(NetAdapterTxAcquire(adapter, &building) == OS_EOK);
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    struct AdapterLease* queued = NetAdapterFindLease(adapter, &packet.Private.Storage.Pool);
    assert(queued && queued->State == ADAPTER_LEASE_TX_BUILDING);
    assert(NetAdapterReleaseLease(adapter, queued, OS_ECANCELLED, false) == OS_EBUSY);
    NetAdapterRequest_t batch = {0};
    assert(NetAdapterBuildBatch(adapter, false, &batch) == OS_ENOENT);
    void* data = packet.Data;
    memset(data, 0xa5, 64);
    NetAdapterTxPacket_t stale = packet;
    assert(NetAdapterTxSubmit(adapter, &packet, packet.Capacity + 1, 7) == OS_EINVALPARAMS);
    assert(packet.Data == data); // Failed submit preserves caller ownership.
    assert(NetAdapterTxSubmit(adapter, &packet, 64, 7) == OS_EOK);
    assert(queued->State == ADAPTER_LEASE_QUEUED);
    assert(!packet.Data && !packet.Capacity && packet.Private.Backing == NET_ADAPTER_PACKET_NONE);
    assert(NetAdapterTxCancel(adapter, &stale) == OS_ENOENT);
    assert(NetAdapterTxSubmit(adapter, &stale, 64, 7) == OS_ENOENT);
    assert(NetAdapterBuildBatch(adapter, false, &batch) == OS_EOK && batch.Count == 1);
    assert(queued->State == ADAPTER_LEASE_PREPARED);
    NetAdapterCancelQueued(adapter);
    assert(queued->State == ADAPTER_LEASE_PREPARED && !callbacks);

    // The driver's descriptor names the exact bytes the caller constructed.
    uint64_t registration;
    struct ctt_netadapter_pool pool;
    assert(NetBuffersBeginRegistration(adapter->Buffers, CTT_NETADAPTER_DIRECTION_TX,
                                       &registration, &pool) == OS_EOK);
    const struct ctt_netadapter_packet* wire = &batch.Packets[0];
    unsigned char* mapped = NetTestShmData(pool.buffer_handle);
    mapped += pool.region_offset + (uint64_t)wire->id.slot_id * pool.slot_size + wire->data_offset;
    assert(mapped == data && wire->length == 64);
    for (unsigned i = 0; i < 64; ++i) assert(mapped[i] == 0xa5);
    assert(building.Data && !callbacks);
    Close(adapter);
    assert(callbacks == 1 && lastCookie == 7 && lastStatus == OS_ECANCELLED);
    assert(queued->State == ADAPTER_LEASE_FREE); // The other builder still pins the arrays.
    assert(NetAdapterDestroy(&adapter) == OS_EBUSY);
    memset(building.Data, 0x5a, building.Capacity); // Still valid after remote close.
    assert(NetAdapterTxCancel(adapter, &building) == OS_EOK);
    assert(!g_netShm.Live);
    assert(NetAdapterDestroy(&adapter) == OS_EOK);
}

static void TestCancellationAndRuns(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t packet;
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    NetAdapterTxPacket_t stale = packet;
    NetAdapterStop(adapter);
    NetAdapterCancelQueued(adapter);
    assert(!callbacks && NetAdapterTxSubmit(adapter, &packet, 64, 1) == OS_ENOTCONNECTED);
    adapter->State = NET_ADAPTER_STOPPED;
    assert(NetAdapterStart(adapter) == OS_EOK);
    adapter->State = NET_ADAPTER_RUNNING;
    assert(NetAdapterTxSubmit(adapter, &packet, 64, 1) == OS_ENOTCONNECTED);
    assert(NetAdapterTxCancel(adapter, &packet) == OS_EOK);
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    assert(NetAdapterTxCancel(adapter, &stale) == OS_ENOENT);
    stale = packet;
    ++stale.Private.Storage.Pool.Session.generation;
    assert(NetAdapterTxCancel(adapter, &stale) == OS_ENOENT);
    assert(NetAdapterTxCancel(adapter, &packet) == OS_EOK);
    assert(NetAdapterTxCancel(adapter, &packet) == OS_ENOENT);
    assert(!callbacks);

    unsigned char copied[64];
    memset(copied, 0x33, sizeof(copied));
    assert(NetAdapterSend(adapter, copied, sizeof(copied), 9) == OS_EOK);
    NetAdapterStop(adapter);
    NetAdapterCancelQueued(adapter);
    assert(callbacks == 1 && lastCookie == 9 && lastStatus == OS_ECANCELLED);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK && !g_netShm.Live);
}

static void TestCapacityAndClose(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t packets[4], extra;
    for (unsigned i = 0; i < 4; ++i) assert(NetAdapterTxAcquire(adapter, &packets[i]) == OS_EOK);
    assert(NetAdapterTxAcquire(adapter, &extra) == OS_EBUSY && !extra.Data);
    Close(adapter);
    for (unsigned i = 0; i < 4; ++i) {
        assert(NetAdapterDestroy(&adapter) == OS_EBUSY);
        assert(NetAdapterTxSubmit(adapter, &packets[i], 64, 0) == OS_ENOTCONNECTED);
        memset(packets[i].Data, 0, packets[i].Capacity);
        assert(NetAdapterTxCancel(adapter, &packets[i]) == OS_EOK);
        assert(g_netShm.Live == (i == 3 ? 0 : 2));
    }
    assert(!callbacks && NetAdapterDestroy(&adapter) == OS_EOK);
}

static void TestQuarantine(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t packet;
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    NetAdapterClose(adapter);
    adapter->Control.Pending = true;
    adapter->Control.Request.Operation = SERVICE_CTT_NETADAPTER_CLOSE_ID;
    adapter->Control.Request.Serial = 17;
    NetAdapterReply_t reply = {.Status = OS_EUNKNOWN};
    assert(HandleAdapterRequest(adapter, 2, 17, &reply, 0) == OS_EUNKNOWN);
    assert(adapter->State == NET_ADAPTER_QUARANTINED);
    memset(packet.Data, 0x11, packet.Capacity);
    assert(NetAdapterTxCancel(adapter, &packet) == OS_EOK);
    // Returning a local builder is not proof that remote mappings are retired.
    assert(g_netShm.Live == 2 && NetAdapterDestroy(&adapter) == OS_EBUSY);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK && !g_netShm.Live);
}

static void TestForeignAdapter(void)
{
    NetworkAdapter_t* first = Create();
    NetworkAdapter_t* second = Create(); // Intentionally identical wire session IDs.
    NetAdapterTxPacket_t a, b;
    assert(NetAdapterTxAcquire(first, &a) == OS_EOK);
    assert(NetAdapterTxAcquire(second, &b) == OS_EOK);
    assert(NetAdapterTxSubmit(second, &a, 64, 1) == OS_ENOENT);
    assert(NetAdapterTxCancel(second, &a) == OS_ENOENT);
    assert(NetAdapterTxCancel(first, &a) == OS_EOK);
    assert(NetAdapterTxCancel(second, &b) == OS_EOK);
    Close(first);
    Close(second);
    assert(NetAdapterDestroy(&first) == OS_EOK);
    assert(NetAdapterDestroy(&second) == OS_EOK && !g_netShm.Live);
}

/** RX fixture: the controller writes mock SHM and returns real admission and
 * completion records. Reposting uses production batch construction, so held
 * slots and completion credits are exercised together rather than mocked.
 */
struct RxFixture {
    NetworkAdapter_t* Adapter;
    NetAdapterRxPacket_t Held[16];
    unsigned Accepted, Offers, Borrowed, PostedCount;
    bool Decline;
    uint64_t Sequence;
    struct ctt_netadapter_packet Posted[16];
};

static bool AcceptRx(void* context, const NetAdapterRxPacket_t* packet)
{
    struct RxFixture* f = context;
    ++f->Offers;
    if (f->Decline) return false;
    assert(f->Accepted < 16);
    f->Held[f->Accepted++] = *packet; // Transfer one owned value from the offer.
    return true;
}

static void BorrowRx(void* context, const void* data, uint32_t length)
{
    struct RxFixture* f = context;
    assert(data && length == 64);
    ++f->Borrowed;
}

static void Repost(struct RxFixture* f)
{
    NetAdapterRequest_t batch = {0};
    oserr_t status = NetAdapterBuildBatch(f->Adapter, true, &batch);
    assert(status == OS_EOK || status == OS_ENOENT);
    for (unsigned i = 0; i < batch.Count; ++i) {
        assert(f->PostedCount < 16);
        f->Posted[f->PostedCount++] = batch.Packets[i];
        // Admission is deliberately delivered with each completion below.
    }
}

static void StartRx(struct RxFixture* f)
{
    memset(f, 0, sizeof(*f));
    f->Adapter = Create();
    NetAdapterCallbacks_t ops = {.ReceivePacket = AcceptRx, .Context = f};
    NetAdapterSetCallbacks(f->Adapter, &ops);
    Repost(f);
    assert(f->PostedCount == 4);
}

static void ReceiveOne(struct RxFixture* f, unsigned char value, bool early)
{
    assert(f->PostedCount);
    struct ctt_netadapter_packet wire = f->Posted[--f->PostedCount];
    uint64_t registration;
    struct ctt_netadapter_pool pool;
    assert(NetBuffersBeginRegistration(f->Adapter->Buffers, CTT_NETADAPTER_DIRECTION_RX,
                                       &registration, &pool) == OS_EOK);
    unsigned char* data = NetTestShmData(pool.buffer_handle);
    data += pool.region_offset + (uint64_t)wire.id.slot_id * pool.slot_size + wire.data_offset;
    memset(data, value, 64);
    struct ctt_netadapter_admission admission = {.id = wire.id, .status = OS_EOK};
    NetBufferLease_t lease;
    bool ready;
    if (!early) {
        assert(NetBuffersAdmission(f->Adapter->Buffers, &f->Adapter->Session,
                                   &admission, &lease, &ready) == OS_EOK && !ready);
    }
    struct ctt_netadapter_completion completion = {
        .completion_sequence = ++f->Sequence, .direction = CTT_NETADAPTER_DIRECTION_RX,
        .id = wire.id, .status = CTT_NETADAPTER_COMPLETION_STATUS_SUCCESS,
        .detail = OS_EOK, .length = 64
    };
    unsigned offers = f->Offers;
    assert(NetAdapterCompletePacket(f->Adapter, &completion) == OS_EOK);
    if (early) {
        assert(f->Offers == offers); // No retention until admission also resolves.
        assert(f->Adapter->Queues[NET_ADAPTER_RX].Leases[wire.id.slot_id].State == ADAPTER_LEASE_PREPARED);
        assert(NetBuffersAdmission(f->Adapter->Buffers, &f->Adapter->Session,
                                   &admission, &lease, &ready) == OS_EOK && ready);
        assert(NetAdapterReleaseCompletedLease(f->Adapter, &lease) == OS_EOK);
    }
    offers = f->Offers;
    assert(NetAdapterCompletePacket(f->Adapter, &completion) == OS_EOK);
    assert(f->Offers == offers); // Replay cannot redeliver a retained packet.
    if (f->Accepted && f->Held[f->Accepted - 1].Data == data) {
        assert(f->Held[f->Accepted - 1].Private.Backing == NET_ADAPTER_PACKET_POOL);
        assert(f->Adapter->Queues[NET_ADAPTER_RX].Leases[wire.id.slot_id].State == ADAPTER_LEASE_RX_RETAINED);
    } else {
        // Declined and copy-backed delivery return the original pool slot before reposting.
        assert(f->Adapter->Queues[NET_ADAPTER_RX].Leases[wire.id.slot_id].State == ADAPTER_LEASE_FREE);
    }
    NetBufferStats_t stats;
    NetBuffersGetStats(f->Adapter->Buffers, &stats);
    assert(stats.ProcessedCompletion == f->Sequence);
    assert(NetBuffersConfirmAcknowledged(f->Adapter->Buffers, &f->Adapter->Session,
                                        f->Sequence) == OS_EOK);
    NetBuffersGetStats(f->Adapter->Buffers, &stats);
    assert(!stats.UnackedCompletions); // Holding bytes must not hold journal credit.
    Repost(f);
}

static void TestRxRetentionAndFallback(void)
{
    struct RxFixture f;
    StartRx(&f);
    unsigned shmCalls = g_netShm.Calls;
    for (unsigned i = 0; i < 6; ++i) ReceiveOne(&f, 0x20 + i, i == 0);
    assert(f.Accepted == 6 && f.Adapter->Rx.PoolRetained == 2 && f.Adapter->Rx.Copy.Retained == 4);
    assert(f.PostedCount == 2); // min_rx_slots survives full pool retention.
    assert(f.Held[0].Private.Backing == NET_ADAPTER_PACKET_POOL && f.Held[1].Private.Backing == NET_ADAPTER_PACKET_POOL);
    for (unsigned i = 2; i < 6; ++i) assert(f.Held[i].Private.Backing == NET_ADAPTER_PACKET_COPY);
    ReceiveOne(&f, 0xee, false); // Both bounded budgets full: drop and repost.
    assert(f.Accepted == 6 && f.Adapter->Rx.Dropped == 1 && f.PostedCount == 2);
    assert(g_netShm.Calls == shmCalls);
    for (unsigned i = 0; i < 6; ++i) {
        for (unsigned j = 0; j < 64; ++j) assert(((const unsigned char*)f.Held[i].Data)[j] == 0x20 + i);
    }
    NetAdapterSnapshot_t snapshot;
    NetAdapterSnapshot(f.Adapter, &snapshot);
    assert(snapshot.RxPoolRetained == 2 && snapshot.RxCopyRetained == 4 && snapshot.RxFallbackCopies == 4);

    NetworkAdapter_t* foreign = Create();
    assert(NetAdapterRxRelease(foreign, &f.Held[0]) == OS_ENOENT);
    assert(NetAdapterRxRelease(foreign, &f.Held[2]) == OS_ENOENT);
    Close(foreign);
    assert(NetAdapterDestroy(&foreign) == OS_EOK);

    NetAdapterRxPacket_t staleCopy = f.Held[2];
    assert(NetAdapterRxRelease(f.Adapter, &f.Held[2]) == OS_EOK);
    ReceiveOne(&f, 0x77, false);
    assert(f.Accepted == 7 && f.Held[6].Private.Backing == NET_ADAPTER_PACKET_COPY);
    assert(NetAdapterRxRelease(f.Adapter, &staleCopy) == OS_ENOENT);
    NetAdapterRxPacket_t stalePool = f.Held[0];
    assert(NetAdapterRxRelease(f.Adapter, &f.Held[0]) == OS_EOK);
    Repost(&f);
    ReceiveOne(&f, 0x88, false);
    assert(f.Accepted == 8 && f.Held[7].Private.Backing == NET_ADAPTER_PACKET_POOL);
    assert(NetAdapterRxRelease(f.Adapter, &stalePool) == OS_ENOENT);

    NetAdapterStop(f.Adapter);
    NetAdapterCancelQueued(f.Adapter);
    assert(((const unsigned char*)f.Held[1].Data)[0] == 0x21);
    f.Adapter->State = NET_ADAPTER_STOPPED;
    assert(NetAdapterStart(f.Adapter) == OS_EOK);
    Close(f.Adapter);
    assert(NetAdapterDestroy(&f.Adapter) == OS_EBUSY);
    // Remove hooks while old consumer-owned packets remain valid.
    NetAdapterSetCallbacks(f.Adapter, NULL);
    for (unsigned i = 0; i < f.Accepted; ++i) {
        if (f.Held[i].Data) {
            assert(f.Held[i].Length == 64);
            assert(NetAdapterRxRelease(f.Adapter, &f.Held[i]) == OS_EOK);
            assert(!f.Held[i].Data);
            assert(NetAdapterRxRelease(f.Adapter, &f.Held[i]) == OS_ENOENT);
        }
    }
    assert(!g_netShm.Live && NetAdapterDestroy(&f.Adapter) == OS_EOK);
}

static void TestRxDeclineAndCopyOnly(void)
{
    struct RxFixture f;
    StartRx(&f);
    f.Decline = true;
    f.Adapter->Callbacks.Receive = BorrowRx;
    ReceiveOne(&f, 1, false);
    assert(f.Borrowed == 1 && !f.Adapter->Rx.PoolRetained && f.PostedCount == 4);
    f.Adapter->Rx.RetentionLimit = 0;
    ReceiveOne(&f, 2, false);
    assert(f.Borrowed == 2 && !f.Adapter->Rx.Copy.Retained && f.PostedCount == 4);
    f.Decline = false;
    for (unsigned i = 0; i < 4; ++i) ReceiveOne(&f, 3, false);
    ReceiveOne(&f, 4, false);
    assert(f.Accepted == 4 && f.Borrowed == 3 && !f.Adapter->Rx.Dropped);
    assert(!f.Adapter->Rx.PoolRetained && f.PostedCount == 4);
    Close(f.Adapter);
    // Copies alone must pin the adapter too, even though no pool lease is held.
    assert(NetAdapterDestroy(&f.Adapter) == OS_EBUSY);
    for (unsigned i = 0; i < f.Accepted; ++i) {
        assert(((const unsigned char*)f.Held[i].Data)[0] == 3);
        assert(NetAdapterRxRelease(f.Adapter, &f.Held[i]) == OS_EOK);
    }
    assert(!g_netShm.Live && NetAdapterDestroy(&f.Adapter) == OS_EOK);
}

static void TestRxPolicyAndQuarantine(void)
{
    NetworkAdapter_t* adapter = CreatePolicy(8, 0, 0);
    assert(adapter->Rx.RetentionLimit == 3); // A zero advertised minimum still reserves one slot.
    assert(!adapter->Rx.Copy.Entries && !adapter->Rx.Copy.Bytes);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK);
    adapter = CreatePolicy(8, 0, 4);
    assert(!adapter->Rx.RetentionLimit && !adapter->Rx.Copy.Entries);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK);

    struct RxFixture f;
    StartRx(&f);
    ReceiveOne(&f, 0x42, false);
    ReceiveOne(&f, 0x43, false);
    ReceiveOne(&f, 0x44, false);
    NetAdapterClose(f.Adapter);
    f.Adapter->Control.Pending = true;
    f.Adapter->Control.Request.Operation = SERVICE_CTT_NETADAPTER_CLOSE_ID;
    f.Adapter->Control.Request.Serial = 17;
    NetAdapterReply_t reply = {.Status = OS_EUNKNOWN};
    assert(HandleAdapterRequest(f.Adapter, 2, 17, &reply, 0) == OS_EUNKNOWN);
    assert(f.Adapter->State == NET_ADAPTER_QUARANTINED);
    for (unsigned i = 0; i < f.Accepted; ++i) {
        assert(((const unsigned char*)f.Held[i].Data)[0] == 0x42 + i);
        assert(NetAdapterRxRelease(f.Adapter, &f.Held[i]) == OS_EOK);
    }
    assert(g_netShm.Live == 2 && NetAdapterDestroy(&f.Adapter) == OS_EBUSY);
    Close(f.Adapter);
    assert(NetAdapterDestroy(&f.Adapter) == OS_EOK && !g_netShm.Live);
}

static void TestPacketIdentityValidation(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t packet;
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    struct AdapterLease* entry = NetAdapterFindLease(adapter, &packet.Private.Storage.Pool);
    for (unsigned fault = 0; fault < 9; ++fault) {
        NetAdapterTxPacket_t invalid = packet;
        switch (fault) {
            case 0: invalid.Private.Backing = NET_ADAPTER_PACKET_NONE; break;
            case 1: invalid.Private.Backing = (enum NetAdapterPacketBacking)99; break;
            case 2: invalid.Private.Backing = NET_ADAPTER_PACKET_COPY; break;
            case 3: invalid.Private.Storage.Pool.Direction = CTT_NETADAPTER_DIRECTION_RX; break;
            case 4: invalid.Private.Storage.Pool.Direction = (enum ctt_netadapter_direction)99; break;
            case 5: ++invalid.Private.Storage.Pool.Session.id; break;
            case 6: ++invalid.Private.Storage.Pool.Session.generation; break;
            case 7: invalid.Private.Storage.Pool.Slot = UINT32_MAX; break;
            case 8: invalid.Private.Storage.Pool.Sequence = 0; break;
        }
        assert(NetAdapterTxSubmit(adapter, &invalid, 64, 0) == OS_ENOENT);
        assert(NetAdapterTxCancel(adapter, &invalid) == OS_ENOENT);
        assert(entry->State == ADAPTER_LEASE_TX_BUILDING);
    }
    assert(NetAdapterTxCancel(adapter, &packet) == OS_EOK);
    assert(entry->State == ADAPTER_LEASE_FREE);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK);

    struct RxFixture f;
    StartRx(&f);
    for (unsigned i = 0; i < 3; ++i) ReceiveOne(&f, 0x61 + i, false);
    for (unsigned i = 0; i < 3; ++i) {
        NetAdapterRxPacket_t invalid = f.Held[i];
        invalid.Private.Backing = (enum NetAdapterPacketBacking)99;
        assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
        invalid.Private.Backing = NET_ADAPTER_PACKET_NONE;
        assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
        invalid = f.Held[i];
        if (invalid.Private.Backing == NET_ADAPTER_PACKET_POOL) {
            invalid.Private.Storage.Pool.Direction = CTT_NETADAPTER_DIRECTION_TX;
            assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
            invalid = f.Held[i];
            ++invalid.Private.Storage.Pool.Session.generation;
            assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
            invalid = f.Held[i];
            invalid.Private.Storage.Pool.Slot = UINT32_MAX;
        } else {
            invalid.Private.Storage.Copy.Sequence = 0;
            assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
            invalid = f.Held[i];
            invalid.Private.Storage.Copy.Slot = UINT32_MAX;
        }
        assert(NetAdapterRxRelease(f.Adapter, &invalid) == OS_ENOENT);
        assert(f.Adapter->Rx.PoolRetained == 2 && f.Adapter->Rx.Copy.Retained == 1);
    }
    Close(f.Adapter);
    for (unsigned i = 0; i < 3; ++i) {
        assert(NetAdapterRxRelease(f.Adapter, &f.Held[i]) == OS_EOK);
        assert(f.Held[i].Private.Backing == NET_ADAPTER_PACKET_NONE);
    }
    assert(NetAdapterDestroy(&f.Adapter) == OS_EOK && !g_netShm.Live);
}

static void TestRxPreparationRollback(void)
{
    NetworkAdapter_t* adapter = Create();
    // Inject invalid descriptor geometry at preparation, after RX acquisition.
    // Failure must return both the pool slot and its local scheduling entry.
    adapter->Mtu = 1600;
    NetAdapterRequest_t batch = {0};
    assert(NetAdapterBuildBatch(adapter, true, &batch) == OS_EINVALPARAMS);
    assert(!batch.Count);
    NetBufferStats_t stats;
    NetBuffersGetStats(adapter->Buffers, &stats);
    assert(stats.RxFree == adapter->Queues[NET_ADAPTER_RX].Slots);
    for (unsigned i = 0; i < adapter->Queues[NET_ADAPTER_RX].Slots; ++i) {
        assert(adapter->Queues[NET_ADAPTER_RX].Leases[i].State == ADAPTER_LEASE_FREE);
    }
    adapter->Mtu = 1500;
    assert(NetAdapterBuildBatch(adapter, true, &batch) == OS_EOK);
    for (unsigned i = 0; i < batch.Count; ++i) {
        assert(adapter->Queues[NET_ADAPTER_RX].Leases[batch.Packets[i].id.slot_id].State == ADAPTER_LEASE_PREPARED);
    }
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK && !g_netShm.Live);
}

static void TestTxCompletionStates(void)
{
    NetworkAdapter_t* adapter = Create();
    NetAdapterTxPacket_t packet;
    assert(NetAdapterTxAcquire(adapter, &packet) == OS_EOK);
    struct AdapterLease* entry = NetAdapterFindLease(adapter, &packet.Private.Storage.Pool);
    assert(NetAdapterTxSubmit(adapter, &packet, 64, 123) == OS_EOK);
    NetAdapterRequest_t batch = {0};
    assert(NetAdapterBuildBatch(adapter, false, &batch) == OS_EOK && batch.Count == 1);
    struct ctt_netadapter_admission admission = {.id = batch.Packets[0].id, .status = OS_EOK};
    NetBufferLease_t lease;
    bool ready;
    assert(NetBuffersAdmission(adapter->Buffers, &adapter->Session, &admission,
                               &lease, &ready) == OS_EOK && !ready);
    // Admission is a pool/DMA fact. Local scheduling stays PREPARED until the terminal packet result.
    assert(entry->State == ADAPTER_LEASE_PREPARED);
    struct ctt_netadapter_completion completion = {
        .completion_sequence = 1, .direction = CTT_NETADAPTER_DIRECTION_TX,
        .id = batch.Packets[0].id, .status = CTT_NETADAPTER_COMPLETION_STATUS_SUCCESS,
        .detail = OS_EOK, .length = 64
    };
    assert(NetAdapterCompletePacket(adapter, &completion) == OS_EOK);
    assert(entry->State == ADAPTER_LEASE_FREE);
    assert(callbacks == 1 && lastCookie == 123 && lastStatus == OS_EOK);
    assert(NetAdapterCompletePacket(adapter, &completion) == OS_EOK && callbacks == 1);
    Close(adapter);
    assert(NetAdapterDestroy(&adapter) == OS_EOK && !g_netShm.Live);
}

int main(void)
{
    TestPublication();
    TestCancellationAndRuns();
    TestCapacityAndClose();
    TestQuarantine();
    TestForeignAdapter();
    TestRxRetentionAndFallback();
    TestRxDeclineAndCopyOnly();
    TestRxPolicyAndQuarantine();
    TestPacketIdentityValidation();
    TestRxPreparationRollback();
    TestTxCompletionStates();
    puts("TX/RX packet tests passed: direct SHM, publication, stale leases, stop/restart, close, exhaustion, RX retention/copy fallback/credits/quarantine");
    return 0;
}
