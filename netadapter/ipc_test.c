/** Boot-only IPC acceptance workload. Runs inside netd; the fake lives in a
 * separate service. Assertions observe callbacks and snapshots of the existing
 * adapter executor, never bypassing the generated client or pool manager.
 */
#include "fake.h"
#include "adapters/adapters.h"
#include <ddk/utils.h>
#include <internal/_syscalls.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>

static atomic_uint g_tx, g_rx;
static atomic_ullong g_txSeen;
static atomic_bool g_failed;
#define CHECK(condition) do { if (!(condition)) { \
    ERROR("NETADAPTER IPC FAIL line %d: %s", __LINE__, #condition); \
    atomic_store(&g_failed, true); return -1; } } while (0)

static void Pause(void) { thrd_sleep(&(struct timespec){ .tv_nsec = 10000000 }, NULL); }

/** Distinct Ethernet payloads expose stale-slot reads, truncation and duplicate
 * delivery. IDs run across both runs so a delayed prior-run callback also fails.
 */
static void Frame(unsigned id, unsigned char* bytes)
{
    for (unsigned i = 0; i < NETADAPTER_TEST_FRAME_BYTES; ++i) bytes[i] = (unsigned char)(id * 37 + i);
    memset(bytes, 0xff, 6);
    bytes[6] = 2; bytes[12] = 0x88; bytes[13] = 0xb5; // local experimental EtherType
    bytes[14] = (unsigned char)id;
}
static void Receive(uuid_t device, uint32_t port, const void* data, uint32_t length)
{
    if (device != NETADAPTER_FAKE_DEVICE || port) return;
    unsigned id = atomic_load(&g_rx);
    unsigned char expected[NETADAPTER_TEST_FRAME_BYTES]; Frame(id, expected);
    if (length != sizeof(expected) || memcmp(data, expected, sizeof(expected))) atomic_store(&g_failed, true);
    atomic_fetch_add(&g_rx, 1);
}
static void Transmitted(uuid_t device, uint32_t port, uint64_t cookie, oserr_t status)
{
    if (device != NETADAPTER_FAKE_DEVICE || port) return;
    if (status != OS_EOK || cookie >= 2 * NETADAPTER_TEST_FRAMES) atomic_store(&g_failed, true);
    else {
        unsigned long long mask = 1ull << cookie;
        if (atomic_fetch_or(&g_txSeen, mask) & mask) atomic_store(&g_failed, true);
    }
    atomic_fetch_add(&g_tx, 1);
}
static int WaitState(enum NetAdapterState state, uint64_t run)
{
    NetAdapterSnapshot_t snapshot;
    for (unsigned i = 0; i < 3000; ++i) {
        CHECK(!atomic_load(&g_failed));
        CHECK(NetworkAdaptersSnapshot(NETADAPTER_FAKE_DEVICE, 0, &snapshot) == OS_EOK);
        CHECK(snapshot.State != NET_ADAPTER_FAILED && snapshot.State != NET_ADAPTER_QUARANTINED);
        if (snapshot.State == state && snapshot.Run == run) return 0;
        Pause();
    }
    ERROR("NETADAPTER IPC timeout state=%u wanted=%u error=%u", snapshot.State, state, snapshot.LastError);
    CHECK(false);
}
static int Traffic(unsigned base)
{
    for (unsigned i = 0; i < NETADAPTER_TEST_FRAMES; ++i) {
        unsigned char frame[NETADAPTER_TEST_FRAME_BYTES]; Frame(base + i, frame);
        oserr_t status = OS_EBUSY;
        for (unsigned retry = 0; retry < 3000 && status == OS_EBUSY; ++retry) {
            status = NetworkAdaptersSend(NETADAPTER_FAKE_DEVICE, 0, frame, sizeof(frame), base + i);
            if (status == OS_EBUSY) Pause();
        }
        CHECK(status == OS_EOK);
    }
    for (unsigned i = 0; i < 3000; ++i) {
        CHECK(!atomic_load(&g_failed));
        if (atomic_load(&g_tx) == base + NETADAPTER_TEST_FRAMES && atomic_load(&g_rx) == base + NETADAPTER_TEST_FRAMES) return 0;
        Pause();
    }
    ERROR("NETADAPTER IPC traffic timeout tx=%u rx=%u", atomic_load(&g_tx), atomic_load(&g_rx));
    CHECK(false);
}
static int Exercise(void)
{
    NOTICE("NETADAPTER IPC START");
    uuid_t driver = UUID_INVALID;
    // Boot order is deliberately unspecified; retry named endpoint lookup, but
    // never turn a missing fake into an indefinite boot-test hang.
    for (unsigned i = 0; i < 3000; ++i) {
        if (Syscall_LookupHandle(NETADAPTER_FAKE_PATH, &driver) == OS_EOK) break;
        Pause();
    }
    CHECK(driver != UUID_INVALID);
    NetworkAdapterHooks_t hooks = { .Receive = Receive, .Transmitted = Transmitted };
    NetworkAdaptersSetHooks(&hooks);
    CHECK(NetworkAdaptersTestAttach(NETADAPTER_FAKE_DEVICE, driver) == OS_EOK);
    for (unsigned run = 1; run <= 2; ++run) {
        if (WaitState(NET_ADAPTER_RUNNING, run) || Traffic((run - 1) * NETADAPTER_TEST_FRAMES)) return -1;
        CHECK(NetworkAdaptersSetRunning(NETADAPTER_FAKE_DEVICE, 0, false) == OS_EOK);
        if (WaitState(NET_ADAPTER_STOPPED, run)) return -1;
        NetAdapterSnapshot_t snapshot;
        CHECK(NetworkAdaptersSnapshot(NETADAPTER_FAKE_DEVICE, 0, &snapshot) == OS_EOK);
        CHECK(!snapshot.Buffers.TxOutstanding && !snapshot.Buffers.RxOutstanding && !snapshot.PendingBatches);
        // Counters are asynchronously refreshed. Wait for them before asserting
        // that replay did not execute any frame twice in the remote process.
        for (unsigned i = 0; i < 3000 && snapshot.Counters.tx_packets < run * NETADAPTER_TEST_FRAMES; ++i) {
            Pause(); CHECK(NetworkAdaptersSnapshot(NETADAPTER_FAKE_DEVICE, 0, &snapshot) == OS_EOK);
        }
        CHECK(snapshot.Counters.tx_packets == run * NETADAPTER_TEST_FRAMES && snapshot.Counters.rx_packets == snapshot.Counters.tx_packets);
        CHECK(snapshot.Counters.tx_bytes == run * NETADAPTER_TEST_FRAMES * NETADAPTER_TEST_FRAME_BYTES &&
            snapshot.Counters.rx_bytes == snapshot.Counters.tx_bytes && !snapshot.Counters.rx_no_buffer);
        NOTICE("NETADAPTER IPC PASS run %u: payloads, exactly-once execution, stop and retirement", run);
        if (run == 1) CHECK(NetworkAdaptersSetRunning(NETADAPTER_FAKE_DEVICE, 0, true) == OS_EOK);
    }
    return 0;
}

static int Run(void* unused)
{
    (void)unused;
    int result = Exercise();
    // Every exit after attachment requests a safe close. Failure does not grant
    // permission to free an uncertain session; the core may quarantine it.
    if (NetworkAdaptersClose(NETADAPTER_FAKE_DEVICE, 0) == OS_EOK) {
        NetAdapterSnapshot_t snapshot;
        bool closed = false;
        for (unsigned i = 0; i < 3000; ++i) {
            if (NetworkAdaptersSnapshot(NETADAPTER_FAKE_DEVICE, 0, &snapshot) != OS_EOK) break;
            if (snapshot.State == NET_ADAPTER_CLOSED) { closed = true; break; }
            if (snapshot.State == NET_ADAPTER_QUARANTINED) break;
            Pause();
        }
        if (!closed) { ERROR("NETADAPTER IPC FAIL safe close not confirmed"); result = -1; }
    } else result = -1;
    if (result) return -1;
    // Keep observation hooks installed so delayed duplicates are not hidden.
    for (unsigned i = 0; i < 100; ++i) Pause();
    CHECK(!atomic_load(&g_failed) && atomic_load(&g_tx) == 2 * NETADAPTER_TEST_FRAMES && atomic_load(&g_rx) == atomic_load(&g_tx));
    NOTICE("NETADAPTER IPC PASS all: real IPC/SHM, replay, drain loss, restart and safe close");
    return 0;
}

void NetworkAdaptersRunIpcTests(void)
{
    thrd_t thread;
    if (thrd_create(&thread, Run, NULL) != thrd_success) { ERROR("NETADAPTER IPC FAIL starting test thread"); return; }
    thrd_detach(thread);
}
