/**
 * Host-side ownership tests for the real netd buffer manager.
 * Only kernel SHM/handle services are faked; protocol structs are generated from
 * netadapter.gr. Deliberately reorder replies, lose completions and reuse slots.
 * No controller hardware or running netd service is needed.
 */
#include "buffers.h"
#include <os/handle.h>
#include <os/memory.h>
#include <os/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == OS_EOK)
#define TX CTT_NETADAPTER_DIRECTION_TX
#define RX CTT_NETADAPTER_DIRECTION_RX
#define SUCCESS CTT_NETADAPTER_COMPLETION_STATUS_SUCCESS

#include "net_shm_mock.h"
static const struct ctt_netadapter_session g_session = { 10, 42 };

static struct ctt_netadapter_info Info(void)
{
    return (struct ctt_netadapter_info) {
        .min_version = 2, .max_version = 2, .framing = CTT_NETADAPTER_FRAMING_ETHERNET,
        .max_frame_size = 1514, .buffer_alignment = 64, .required_headroom = 17,
        .required_tailroom = 7, .max_pools = 2, .max_pool_bytes = 65536,
        .max_registered_bytes = 131072, .max_slots_per_pool = 8,
        .max_queue_pairs = 1, .max_segments = 1, .max_batch_size = 4,
        .max_pending_batches = 8, .max_outstanding_tx = 4, .max_outstanding_rx = 4,
        .max_unacked_completions = 8, .min_rx_slots = 1
    };
}

static NetBufferConfig_t Config(void)
{
    return (NetBufferConfig_t) { 4, 4, 1514, 1024 * 1024 };
}

static NetBufferManager_t* Create(void)
{
    NetBufferManager_t* manager = NULL;
    struct ctt_netadapter_info info = Info();
    NetBufferConfig_t config = Config();
    OK(NetBuffersCreate(&g_session, &info, &config, &manager));
    CHECK(manager && g_netShm.Live == 2);
    return manager;
}

static void Register(NetBufferManager_t* manager)
{
    for (int i = 0; i < 2; ++i) {
        enum ctt_netadapter_direction direction = i ? RX : TX;
        uint64_t key;
        struct ctt_netadapter_pool pool, retry;
        OK(NetBuffersBeginRegistration(manager, direction, &key, &pool));
        CHECK(key == (uint64_t)i + 1 && pool.direction == direction);
        CHECK(pool.slot_count == 4 && pool.slot_size == 1600);
        CHECK(pool.region_size == 8192 && pool.region_offset == 0);
        OK(NetBuffersBeginRegistration(manager, direction, &key, &retry));
        CHECK(retry.buffer_handle == pool.buffer_handle);
        OK(NetBuffersRegistered(manager, &g_session, direction, (uint32_t)i + 100));
        OK(NetBuffersRegistered(manager, &g_session, direction, (uint32_t)i + 100));
    }
}

static void CloseDestroy(NetBufferManager_t** manager)
{
    OK(NetBuffersClosed(*manager, &g_session));
    OK(NetBuffersDestroy(manager));
    CHECK(!*manager && !g_netShm.Live);
}

static struct ctt_netadapter_packet Prepare(NetBufferManager_t* manager,
    enum ctt_netadapter_direction direction, NetBufferLease_t* lease)
{
    struct ctt_netadapter_packet packet;
    OK(NetBuffersAcquire(manager, direction, lease));
    OK(NetBuffersPrepare(manager, lease, direction == TX ? 64 : 1514, &packet));
    CHECK(packet.data_offset == 64 && !packet.flags && !packet.id.queue_id);
    return packet;
}

static void Admit(NetBufferManager_t* manager, struct ctt_netadapter_packet packet, bool expectedReady)
{
    NetBufferLease_t lease;
    bool ready = !expectedReady;
    struct ctt_netadapter_admission admission = { .id = packet.id, .status = OS_EOK };
    OK(NetBuffersAdmission(manager, &g_session, &admission, &lease, &ready));
    CHECK(ready == expectedReady);
}

static struct ctt_netadapter_completion Completion(struct ctt_netadapter_packet packet,
    enum ctt_netadapter_direction direction, uint64_t sequence)
{
    return (struct ctt_netadapter_completion) {
        .completion_sequence = sequence, .direction = direction, .id = packet.id,
        .status = SUCCESS, .detail = OS_EOK, .length = direction == TX ? packet.length : 128
    };
}

static void TestCreation(void)
{
    struct ctt_netadapter_info info = Info();
    NetBufferConfig_t config = Config();
    NetBufferManager_t* manager = NULL;
    unsigned int before = g_netShm.Calls;
    config.MemoryBudget = 1;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EOVERFLOW);
    CHECK(!manager && g_netShm.Calls == before);
    config = Config();
    info.buffer_alignment = 8192;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) != OS_EOK);
    info = Info();
    info.required_headroom = UINT32_MAX;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EOVERFLOW);
    info = Info();
    config.TxSlots = UINT32_MAX;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) != OS_EOK);
    config = Config();
    info.max_registered_bytes = 8192;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EOVERFLOW);
    info = Info();
    // Failure of the second pool must unwind the first pool's SHM and metadata.
    g_netShm.FailCall = g_netShm.Calls + 2;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EOOM);
    CHECK(!manager && !g_netShm.Live);
    g_netShm.FailCall = 0;
    manager = Create();
    OK(NetBuffersDestroy(&manager));
    CHECK(!g_netShm.Live);
    OK(NetBuffersDestroy(&manager));
}

static void TestRegistrationAndLeases(void)
{
    NetBufferManager_t* manager = Create();
    NetBufferLease_t lease[4], extra;
    struct ctt_netadapter_pool pool;
    uint64_t key;
    CHECK(NetBuffersAcquire(manager, TX, &extra) == OS_ENOENT);
    OK(NetBuffersBeginRegistration(manager, TX, &key, &pool));
    // No reply does not imply the driver failed to attach the memory.
    CHECK(NetBuffersDestroy(&manager) == OS_EBUSY && g_netShm.Live == 2);
    CHECK(NetBuffersRegistered(manager, &g_session, RX, 100) == OS_EPROTOCOL);
    Register(manager);
    CHECK(NetBuffersRegistered(manager, &g_session, RX, 100) == OS_EPROTOCOL);
    for (int i = 0; i < 4; ++i) OK(NetBuffersAcquire(manager, TX, &lease[i]));
    CHECK(NetBuffersAcquire(manager, TX, &extra) == OS_EBUSY);
    NetBufferView_t view;
    OK(NetBuffersView(manager, &lease[0], &view));
    CHECK(!((uintptr_t)view.Data & 63) && view.Capacity == 1514 && !view.Completed);
    memset(view.Data, 0xa5, view.Capacity);
    OK(NetBuffersRelease(manager, &lease[0]));
    CHECK(NetBuffersRelease(manager, &lease[0]) == OS_EINVALPARAMS);
    OK(NetBuffersAcquire(manager, TX, &extra));
    CHECK(extra.Slot == lease[0].Slot && extra.Sequence != lease[0].Sequence);
    CHECK(NetBuffersRelease(manager, &lease[0]) == OS_EINVALPARAMS);
    OK(NetBuffersView(manager, &extra, &view));
    for (uint32_t n = 0; n < view.Capacity; ++n) CHECK(((uint8_t*)view.Data)[n] == 0);
    OK(NetBuffersRelease(manager, &extra));
    for (int i = 1; i < 4; ++i) OK(NetBuffersRelease(manager, &lease[i]));
    CloseDestroy(&manager);
}

static void TestAdmissionAndEarlyCompletion(void)
{
    NetBufferManager_t* manager = Create();
    Register(manager);
    NetBufferLease_t tx, rx, output;
    NetBufferView_t view;
    bool ready;
    struct ctt_netadapter_packet txPacket = Prepare(manager, TX, &tx);
    struct ctt_netadapter_packet rxPacket = Prepare(manager, RX, &rx);
    CHECK(NetBuffersView(manager, &tx, &view) == OS_EBUSY);
    CHECK(NetBuffersRelease(manager, &tx) == OS_EBUSY);
    struct ctt_netadapter_admission rejection = { .id = txPacket.id, .status = OS_EBUSY };
    OK(NetBuffersAdmission(manager, &g_session, &rejection, &output, &ready));
    CHECK(!ready);
    CHECK(NetBuffersPrepare(manager, &tx, 64, &txPacket) == OS_EBUSY);
    OK(NetBuffersRelease(manager, &tx));

    struct ctt_netadapter_completion completion = Completion(rxPacket, RX, 1);
    OK(NetBuffersComplete(manager, &g_session, &completion, &output, &ready));
    CHECK(!ready);
    CHECK(NetBuffersView(manager, &rx, &view) == OS_EBUSY);
    CHECK(NetBuffersRelease(manager, &rx) == OS_EBUSY);
    rejection.id = rxPacket.id;
    CHECK(NetBuffersAdmission(manager, &g_session, &rejection, &output, &ready) == OS_EPROTOCOL);
    Admit(manager, rxPacket, true);
    OK(NetBuffersView(manager, &rx, &view));
    CHECK(view.Completed && view.Status == SUCCESS && view.Length == 128);
    CHECK(NetBuffersComplete(manager, &g_session, &completion, &output, &ready) == OS_EEXISTS);
    OK(NetBuffersRelease(manager, &rx));
    CHECK(NetBuffersUnregistered(manager, &g_session, RX) == OS_EBUSY);
    OK(NetBuffersConfirmAcknowledged(manager, &g_session, 1));
    OK(NetBuffersUnregistered(manager, &g_session, RX));
    OK(NetBuffersUnregistered(manager, &g_session, TX));
    OK(NetBuffersDestroy(&manager));
    CHECK(!g_netShm.Live);
}

static void TestReplayWindow(void)
{
    NetBufferManager_t* manager = Create();
    Register(manager);
    NetBufferLease_t first, second, output;
    NetBufferStats_t stats;
    bool ready;
    struct ctt_netadapter_packet a = Prepare(manager, TX, &first);
    struct ctt_netadapter_packet b = Prepare(manager, RX, &second);
    Admit(manager, a, false);
    Admit(manager, b, false);
    struct ctt_netadapter_completion ca = Completion(a, TX, 1);
    struct ctt_netadapter_completion cb = Completion(b, RX, 2);
    OK(NetBuffersComplete(manager, &g_session, &cb, &output, &ready));
    CHECK(ready);
    NetBuffersGetStats(manager, &stats);
    CHECK(!stats.ProcessedCompletion && stats.TxOutstanding == 1 && !stats.RxOutstanding);
    CHECK(NetBuffersConfirmAcknowledged(manager, &g_session, 2) == OS_EINVALPARAMS);
    CHECK(NetBuffersComplete(manager, &g_session, &cb, &output, &ready) == OS_EEXISTS);
    cb.length++;
    CHECK(NetBuffersComplete(manager, &g_session, &cb, &output, &ready) == OS_EPROTOCOL);
    OK(NetBuffersComplete(manager, &g_session, &ca, &output, &ready));
    NetBuffersGetStats(manager, &stats);
    CHECK(stats.ProcessedCompletion == 2);
    OK(NetBuffersConfirmAcknowledged(manager, &g_session, 2));
    OK(NetBuffersConfirmAcknowledged(manager, &g_session, 1));
    OK(NetBuffersRelease(manager, &first));
    OK(NetBuffersRelease(manager, &second));

    // Move through several ledger wraps. Delayed completion for an old lease
    // must never complete/release the replacement slot.
    for (uint64_t sequence = 3; sequence < 40; ++sequence) {
        a = Prepare(manager, TX, &first);
        Admit(manager, a, false);
        CHECK(NetBuffersComplete(manager, &g_session, &ca, &output, &ready) == OS_EEXISTS);
        CHECK(NetBuffersRelease(manager, &first) == OS_EBUSY);
        struct ctt_netadapter_completion c = Completion(a, TX, sequence);
        OK(NetBuffersComplete(manager, &g_session, &c, &output, &ready));
        OK(NetBuffersRelease(manager, &first));
        OK(NetBuffersConfirmAcknowledged(manager, &g_session, sequence));
    }
    CloseDestroy(&manager);
}

static void TestMalformedAndClose(void)
{
    NetBufferManager_t* manager = Create();
    Register(manager);
    NetBufferLease_t lease, output, held;
    bool ready;
    struct ctt_netadapter_packet packet = Prepare(manager, RX, &lease);
    struct ctt_netadapter_completion valid = Completion(packet, RX, 1), bad;
    struct ctt_netadapter_session stale = g_session;
    stale.generation++;
    CHECK(NetBuffersComplete(manager, &stale, &valid, &output, &ready) == OS_EINVALPARAMS);
    CHECK(NetBuffersClosed(manager, &stale) == OS_EINVALPARAMS);
    for (int i = 0; i < 8; ++i) {
        bad = valid;
        switch (i) {
            case 0: bad.length = 1515; break;
            case 1: bad.id.pool_id = 900; break;
            case 2: bad.id.queue_id = 1; break;
            case 3: bad.id.submission_sequence++; break;
            case 4: bad.direction = TX; break;
            case 5: bad.detail = OS_EDEVFAULT; break;
            case 6: bad.status = 99; break;
            case 7: bad.length = 0; break;
        }
        CHECK(NetBuffersComplete(manager, &g_session, &bad, &output, &ready) == OS_EPROTOCOL);
        CHECK(NetBuffersRelease(manager, &lease) == OS_EBUSY);
    }
    bad = valid;
    bad.completion_sequence = 9;
    CHECK(NetBuffersComplete(manager, &g_session, &bad, &output, &ready) == OS_EBUSY);
    OK(NetBuffersAcquire(manager, TX, &held));
    CHECK(NetBuffersDestroy(&manager) == OS_EBUSY);
    OK(NetBuffersClosed(manager, &g_session));
    OK(NetBuffersClosed(manager, &g_session));
    CHECK(NetBuffersDestroy(&manager) == OS_EBUSY); // local leases still pin SHM
    NetBufferView_t view;
    OK(NetBuffersView(manager, &lease, &view));
    CHECK(view.Completed && view.Status == CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED);
    CHECK(view.Detail == OS_ECANCELLED && !view.Length);
    CHECK(NetBuffersComplete(manager, &g_session, &valid, &output, &ready) == OS_ENOENT);
    CHECK(NetBuffersAcquire(manager, RX, &output) == OS_ENOENT);
    OK(NetBuffersRelease(manager, &lease));
    OK(NetBuffersRelease(manager, &held));
    OK(NetBuffersDestroy(&manager));
    CHECK(!g_netShm.Live);
}

static void TestCreditsAndErrors(void)
{
    NetBufferManager_t* manager;
    struct ctt_netadapter_info info = Info();
    NetBufferConfig_t config = Config();
    info.max_unacked_completions = 2;
    OK(NetBuffersCreate(&g_session, &info, &config, &manager));
    Register(manager);
    NetBufferLease_t tx, rx, spare, output;
    bool ready;
    struct ctt_netadapter_packet r = Prepare(manager, RX, &rx), unused;
    OK(NetBuffersAcquire(manager, RX, &spare));
    // RX cannot reserve the last completion credit needed to send an ARP query.
    CHECK(NetBuffersPrepare(manager, &spare, 1514, &unused) == OS_EBUSY);
    OK(NetBuffersRelease(manager, &spare));
    struct ctt_netadapter_packet t = Prepare(manager, TX, &tx);
    Admit(manager, t, false);
    Admit(manager, r, false);
    OK(NetBuffersAcquire(manager, TX, &spare));
    CHECK(NetBuffersPrepare(manager, &spare, 64, &unused) == OS_EBUSY);
    struct ctt_netadapter_completion c = Completion(t, TX, 1);
    c.status = CTT_NETADAPTER_COMPLETION_STATUS_ERROR;
    c.detail = OS_EDEVFAULT;
    c.length = 0;
    OK(NetBuffersComplete(manager, &g_session, &c, &output, &ready));
    CHECK(ready);
    NetBufferView_t view;
    OK(NetBuffersView(manager, &tx, &view));
    CHECK(view.Completed && view.Detail == OS_EDEVFAULT && !view.Length);
    OK(NetBuffersRelease(manager, &tx));
    // Returning a CPU lease does not acknowledge the driver's completion log.
    CHECK(NetBuffersPrepare(manager, &spare, 64, &unused) == OS_EBUSY);
    OK(NetBuffersConfirmAcknowledged(manager, &g_session, 1));
    OK(NetBuffersPrepare(manager, &spare, 64, &unused));
    c = Completion(r, RX, 2);
    c.status = CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED;
    c.detail = OS_ECANCELLED;
    c.length = 0;
    OK(NetBuffersComplete(manager, &g_session, &c, &output, &ready));
    OK(NetBuffersRelease(manager, &rx));
    // Close reconciles the newly prepared TX even when no admission reply came.
    OK(NetBuffersClosed(manager, &g_session));
    OK(NetBuffersRelease(manager, &spare));
    OK(NetBuffersDestroy(&manager));
    CHECK(!g_netShm.Live);
}

static void TestLostRegistrationAndValidation(void)
{
    NetBufferManager_t* manager = Create();
    struct ctt_netadapter_pool pool;
    uint64_t key;
    OK(NetBuffersBeginRegistration(manager, RX, &key, &pool));
    CHECK(NetBuffersDestroy(&manager) == OS_EBUSY);
    // A confirmed close releases an uncertain registration without requiring a
    // fabricated successful register_pool reply or losing the SHM handle.
    CloseDestroy(&manager);

    manager = Create();
    Register(manager);
    NetBufferLease_t lease;
    struct ctt_netadapter_packet packet;
    OK(NetBuffersAcquire(manager, TX, &lease));
    CHECK(NetBuffersPrepare(manager, &lease, 13, &packet) == OS_EINVALPARAMS);
    CHECK(NetBuffersPrepare(manager, &lease, 1515, &packet) == OS_EINVALPARAMS);
    OK(NetBuffersPrepare(manager, &lease, 1514, &packet));
    struct ctt_netadapter_admission a = { .id = packet.id, .status = OS_EOK };
    NetBufferLease_t output;
    bool ready;
    OK(NetBuffersAdmission(manager, &g_session, &a, &output, &ready));
    CHECK(NetBuffersAdmission(manager, &g_session, &a, &output, &ready) == OS_EEXISTS);
    a.status = OS_EBUSY;
    CHECK(NetBuffersAdmission(manager, &g_session, &a, &output, &ready) == OS_EPROTOCOL);
    CHECK(NetBuffersUnregistered(manager, &g_session, TX) == OS_EBUSY);
    OK(NetBuffersClosed(manager, &g_session));
    OK(NetBuffersRelease(manager, &lease));
    OK(NetBuffersDestroy(&manager));
    CHECK(!g_netShm.Live);
}

int main(void)
{
    TestCreation();
    TestRegistrationAndLeases();
    TestAdmissionAndEarlyCompletion();
    TestReplayWindow();
    TestMalformedAndClose();
    TestCreditsAndErrors();
    TestLostRegistrationAndValidation();
    puts("net_buffers_test: all tests passed");
    return 0;
}
