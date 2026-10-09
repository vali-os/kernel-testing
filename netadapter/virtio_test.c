/** Exercise the real driver and netd session engine against a deterministic DMA
 * boundary. Generated handlers are called directly; virtio_ipc_test.c covers
 * actual IPC, PCI transport and interrupt delivery in the guest.
 */
#include "virtio-net.h"
#include "fake.h"
#include <string.h>
#include "adapter.h"
#include "net_shm_mock.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "virtio_test:%d: %s\n", __LINE__, #x);                                 \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
#define OP(name) SERVICE_CTT_NETADAPTER_##name##_ID
static NetAdapterReply_t     reply;
static NetAdapterEvent_t     events[128];
static unsigned              eventCount, txCount, rxCount;
static uint64_t              txSeen, now;
static NetworkAdapter_t*     adapter;
static struct gracht_message message = {.client = 3};

static VirtioNetDevice_t g_device;
static bool              g_failReset;
static bool              g_failNotify;
static unsigned          g_submissions;
static bool              g_dropEvents;
static bool              g_dropAdmission;
static unsigned          g_interruptTeardowns;
static unsigned          g_transportTeardowns;
static VirtioNetDevice_t* g_retainedDevice;

#define VirtioNetReadConfiguration __DeviceReadConfiguration
#include "../../modules/virtio/net/device.c"
#undef VirtioNetReadConfiguration

void
SystemDebug(
        enum OSSysLogLevel level,
        const char*        format, ...)
{
    (void)level;
    (void)format;
}

void
VirtioNetRetainDevice(
        VirtioNetDevice_t* device)
{
    g_retainedDevice = device;
}

void
VirtioNetUnregisterInterrupt(
        VirtioNetDevice_t* device)
{
    CHECK(!device->ReceiveQueue && !device->TransmitQueue);
    g_interruptTeardowns++;
}

void
VirtioPciTransportDestroy(
        VirtioPciTransport_t* transport)
{
    g_transportTeardowns++;
}

struct TestChain {
    VirtioQueueBuffer_t Buffers[36];
    uint16_t            Count;
    void*               Context;
    bool                Used;
    bool                Done;
    uint32_t            Written;
};
struct VirtioSplitQueue {
    struct TestChain Chains[64];
    uint16_t         Size;
    bool             Receive;
};
static struct VirtioSplitQueue g_rxQueue, g_txQueue;

void
VirtioNetLock(
        void)
{
}
void
VirtioNetUnlock(
        void)
{
}
gracht_server_t*
VirtioNetServer(
        void)
{
    return NULL;
}
uint64_t
VirtioNetGeneration(
        void)
{
    return 100;
}
VirtioNetDevice_t*
VirtioNetFindDevice(
        uuid_t id)
{
    return id == NETADAPTER_FAKE_DEVICE ? &g_device : NULL;
}
VirtioNetDevice_t*
VirtioNetFindSession(
        const struct gracht_message*         m,
        const struct ctt_netadapter_session* s)
{
    return g_device.Session.Active && m->client == g_device.Session.Owner &&
                           s->id == g_device.Session.Identity.id &&
                           s->generation == g_device.Session.Identity.generation
                   ? &g_device
                   : NULL;
}
bool
VirtioNetWasClosed(
        const struct gracht_message*         m,
        const struct ctt_netadapter_session* s)
{
    for (size_t i = 0; i < g_device.ClosedCount; ++i) {
        if (g_device.Closed[i].Owner == m->client && g_device.Closed[i].Identity.id == s->id &&
            g_device.Closed[i].Identity.generation == s->generation) {
            return true;
        }
    }
    return false;
}
int
write(
        int          fd,
        const void*  data,
        unsigned int length)
{
    (void)fd;
    (void)data;
    return length;
}
oserr_t
VirtioNetReadConfiguration(
        VirtioNetDevice_t* d)
{
    (void)d;
    return OS_EOK;
}
oserr_t
VirtioPciReset(
        VirtioPciTransport_t* t)
{
    (void)t;
    return g_failReset ? OS_EDEVFAULT : OS_EOK;
}
oserr_t
VirtioPciNegotiateFeatures(
        VirtioPciTransport_t* t,
        uint64_t              supported,
        uint64_t              required,
        uint64_t*             out)
{
    (void)t;
    (void)required;
    *out = supported;
    return OS_EOK;
}
oserr_t
VirtioPciFinishInitialization(
        VirtioPciTransport_t* t)
{
    (void)t;
    return OS_EOK;
}
oserr_t
VirtioSplitQueueCreate(
        VirtioPciTransport_t* t,
        uint16_t              index,
        uint16_t              size,
        VirtioSplitQueue_t**  out)
{
    (void)t;
    *out = index ? &g_txQueue : &g_rxQueue;
    memset(*out, 0, sizeof(**out));
    (*out)->Size = size;
    (*out)->Receive = !index;
    return OS_EOK;
}
oserr_t
VirtioSplitQueueGetStats(
        VirtioSplitQueue_t* q,
        VirtioQueueStats_t* out)
{
    *out = (VirtioQueueStats_t){.QueueSize = q->Size};
    return OS_EOK;
}
oserr_t
VirtioSplitQueueDestroy(
        VirtioSplitQueue_t* q)
{
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(!q->Chains[i].Used);
    }
    return OS_EOK;
}

/** Treat mock physical addresses as host pointers, then check the actual driver's
 * SG descriptors, header placement and initialized short-frame padding.
 */
oserr_t
VirtioSplitQueueSubmit(
        VirtioSplitQueue_t*        q,
        const VirtioQueueBuffer_t* buffers,
        uint16_t                   count,
        void*                      context,
        uint16_t*                  head)
{
    (void)head;
    struct TestChain* chain = NULL;
    CHECK(count <= 36);
    for (unsigned i = 0; i < 64; ++i) {
        if (!q->Chains[i].Used) {
            chain = &q->Chains[i];
            break;
        }
    }
    if (!chain) {
        return OS_EBUSY;
    }
    *chain = (struct TestChain){.Count = count, .Context = context, .Used = true};
    memcpy(chain->Buffers, buffers, count * sizeof(*buffers));
    g_submissions++;
    if (g_failNotify) {
        return OS_EINPROGRESS;
    }
    if (q->Receive) {
        return OS_EOK;
    }
    unsigned char wire[1600];
    unsigned      length = 0;
    for (unsigned i = 0; i < count; ++i) {
        CHECK(!buffers[i].Flags);
        CHECK(length + buffers[i].Length <= sizeof(wire));
        memcpy(wire + length, (void*)(uintptr_t)buffers[i].Address, buffers[i].Length);
        length += buffers[i].Length;
    }
    for (unsigned i = 0; i < 12; ++i) {
        CHECK(wire[i] == 0);
    }
    VirtioNetSlot_t* slot = context;
    CHECK(length == 12 + (slot->Packet.length < 60 ? 60 : slot->Packet.length));
    for (unsigned i = 12 + slot->Packet.length; i < length; ++i) {
        CHECK(wire[i] == 0);
    }
    chain->Done = true;
    // Cover the standard encoding and the QEMU non-mergeable zero value.
    wire[10] = slot->Packet.length < 60 ? 0 : 1;
    for (unsigned i = 0; i < 64; ++i) {
        struct TestChain* receive = &g_rxQueue.Chains[i];
        if (!receive->Used || receive->Done) {
            continue;
        }
        unsigned offset = 0;
        for (unsigned j = 0; j < receive->Count && offset < length; ++j) {
            CHECK(receive->Buffers[j].Flags == VIRTIO_SPLIT_DESC_F_WRITE);
            unsigned bytes = receive->Buffers[j].Length;
            if (bytes > length - offset) {
                bytes = length - offset;
            }
            memcpy((void*)(uintptr_t)receive->Buffers[j].Address, wire + offset, bytes);
            offset += bytes;
        }
        CHECK(offset == length);
        receive->Done = true;
        receive->Written = length;
        break;
    }
    return OS_EOK;
}
oserr_t
VirtioSplitQueuePoll(
        VirtioSplitQueue_t*      q,
        VirtioQueueCompletion_t* out)
{
    for (unsigned i = 0; i < 64; ++i) {
        if (q->Chains[i].Used && q->Chains[i].Done) {
            *out = (VirtioQueueCompletion_t){.Context = q->Chains[i].Context,
                                             .Length = q->Chains[i].Written};
            q->Chains[i].Used = false;
            return OS_EOK;
        }
    }
    return OS_ENOENT;
}
oserr_t
VirtioSplitQueueAbort(
        VirtioSplitQueue_t*      q,
        VirtioQueueCompletion_t* out)
{
    for (unsigned i = 0; i < 64; ++i) {
        if (q->Chains[i].Used) {
            *out = (VirtioQueueCompletion_t){.Context = q->Chains[i].Context};
            q->Chains[i].Used = false;
            return OS_EOK;
        }
    }
    return OS_ENOENT;
}
oserr_t
SHMGetSGTable(
        OSHandle_t*   h,
        SHMSGTable_t* table,
        int           maximum)
{
    (void)maximum;
    size_t capacity = SHMBufferCapacity(h);
    // Metadata is contiguous. Odd pool segment boundaries force some packets
    // across SG entries, independently of the caller's slot alignment.
    size_t stride = h == &g_device.Metadata ? capacity : 4093;
    table->Count = (capacity + stride - 1) / stride;
    table->Entries = malloc(table->Count * sizeof(*table->Entries));
    CHECK(table->Entries);
    for (int i = 0; i < table->Count; ++i) {
        size_t offset = i * stride;
        size_t length = capacity - offset;
        if (length > stride) {
            length = stride;
        }
        table->Entries[i] = (SHMSG_t){(uintptr_t)SHMBuffer(h) + offset, length};
    }
    return OS_EOK;
}
oserr_t
SHMSGTableOffset(
        SHMSGTable_t* table,
        size_t        offset,
        int*          index,
        size_t*       inner)
{
    for (int i = 0; i < table->Count; ++i) {
        if (offset < table->Entries[i].Length) {
            *index = i;
            *inner = offset;
            return OS_EOK;
        }
        offset -= table->Entries[i].Length;
    }
    return OS_EINVALPARAMS;
}
int
ctt_netadapter_event_fault_single(
        gracht_server_t*                     server,
        gracht_conn_t                        owner,
        const struct ctt_netadapter_session* identity,
        const struct ctt_netadapter_fault*   fault)
{
    (void)server;
    (void)owner;
    (void)identity;
    CHECK(fault->status != OS_EOK);
    return 0;
}

int
gracht_server_register_client(
        const struct gracht_message* m)
{
    (void)m;
    return 0;
}
int
ctt_netadapter_get_info_response(
        struct gracht_message*      m,
        oserr_t                     s,
        struct ctt_netadapter_info* i)
{
    (void)m;
    reply.Status = s;
    reply.Info = *i;
    return 0;
}
int
ctt_netadapter_open_response(
        struct gracht_message*         m,
        oserr_t                        s,
        struct ctt_netadapter_session* session,
        uint64_t                       f,
        struct ctt_netadapter_link*    link)
{
    (void)m;
    reply.Status = s;
    reply.Session = *session;
    reply.Value = f;
    reply.Link = *link;
    return 0;
}
int
ctt_netadapter_register_pool_response(
        struct gracht_message* m,
        oserr_t                s,
        uint32_t               id)
{
    (void)m;
    reply.Status = s;
    reply.PoolId = id;
    return 0;
}
#define STATUS_RESPONSE(name)                                                                      \
    int ctt_netadapter_##name##_response(struct gracht_message* m, oserr_t s)                      \
    {                                                                                              \
        (void)m;                                                                                   \
        reply.Status = s;                                                                          \
        return 0;                                                                                  \
    }
STATUS_RESPONSE(
        configure)
STATUS_RESPONSE(
        unregister_pool)
STATUS_RESPONSE(
        close)
STATUS_RESPONSE(
        prepare_run)
STATUS_RESPONSE(
        start_run)
int
ctt_netadapter_stop_run_response(
        struct gracht_message* m,
        oserr_t                s,
        uint64_t               sequence)
{
    (void)m;
    reply.Status = s;
    reply.Value = sequence;
    return 0;
}
int
ctt_netadapter_get_link_response(
        struct gracht_message*      m,
        oserr_t                     s,
        struct ctt_netadapter_link* link)
{
    (void)m;
    reply.Status = s;
    reply.Link = *link;
    return 0;
}
int
ctt_netadapter_get_counters_response(
        struct gracht_message*          m,
        oserr_t                         s,
        struct ctt_netadapter_counters* c)
{
    (void)m;
    reply.Status = s;
    reply.Counters = *c;
    return 0;
}
static NetAdapterEvent_t*
Event(
        gracht_server_t*                      server,
        gracht_conn_t                         owner,
        const struct ctt_netadapter_session*  s,
        const struct ctt_netadapter_progress* p,
        uint8_t                               op)
{
    (void)server;
    CHECK(owner == 3 && eventCount < 128);
    NetAdapterEvent_t* e = &events[eventCount++];
    *e = (NetAdapterEvent_t){.Session = *s, .Progress = *p, .Operation = op};
    return e;
}
int
ctt_netadapter_event_batch_admitted_single(
        gracht_server_t*                       server,
        gracht_conn_t                          owner,
        const struct ctt_netadapter_session*   s,
        uint64_t                               run,
        uint64_t                               id,
        oserr_t                                status,
        const struct ctt_netadapter_admission* records,
        uint32_t                               count,
        const struct ctt_netadapter_progress*  p)
{
    if (g_dropAdmission) {
        g_dropAdmission = false;
        return -1;
    }
    NetAdapterEvent_t* e = Event(server, owner, s, p, OP(EVENT_BATCH_ADMITTED));
    e->Run = run;
    e->Id = id;
    e->Status = status;
    e->Count = count;
    if (count) {
        memcpy(e->Admissions, records, count * sizeof(*records));
    }
    return 0;
}
int
ctt_netadapter_event_completions_single(
        gracht_server_t*                        server,
        gracht_conn_t                           owner,
        const struct ctt_netadapter_session*    s,
        uint64_t                                id,
        const struct ctt_netadapter_completion* records,
        uint32_t                                count,
        const struct ctt_netadapter_progress*   p)
{
    if (g_dropEvents && !id) {
        return -1;
    }
    NetAdapterEvent_t* e = Event(server, owner, s, p, OP(EVENT_COMPLETIONS));
    e->Id = id;
    e->Count = count;
    memcpy(e->Completions, records, count * sizeof(*records));
    return 0;
}
int
ctt_netadapter_event_drain_end_single(
        gracht_server_t*                      server,
        gracht_conn_t                         owner,
        const struct ctt_netadapter_session*  s,
        uint64_t                              id,
        oserr_t                               status,
        uint64_t                              after,
        uint64_t                              through,
        uint32_t                              count,
        uint64_t                              highest,
        const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e = Event(server, owner, s, p, OP(EVENT_DRAIN_END));
    e->Id = id;
    e->Status = status;
    e->After = after;
    e->Through = through;
    e->Count = count;
    e->Highest = highest;
    return 0;
}
int
ctt_netadapter_event_ack_progress_single(
        gracht_server_t*                      server,
        gracht_conn_t                         owner,
        const struct ctt_netadapter_session*  s,
        const struct ctt_netadapter_ack*      ack,
        oserr_t                               status,
        const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e = Event(server, owner, s, p, OP(EVENT_ACK_PROGRESS));
    e->Ack = *ack;
    e->Status = status;
    return 0;
}

static void
__TestDeviceDestroy(
        void)
{
    VirtioNetDevice_t* device = calloc(1, sizeof(*device));
    unsigned int      live = g_netShm.Live;
    unsigned int      interrupts = g_interruptTeardowns;
    unsigned int      transports = g_transportTeardowns;
    SHM_t             memory = {
        .Flags = SHM_DEVICE | SHM_CLEAN,
        .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE,
        .Size = 4096
    };
    uuid_t            metadataId;
    uuid_t            poolId;
    oserr_t           status;

    CHECK(device != NULL);
    device->BusDevice = calloc(1, sizeof(*device->BusDevice));
    CHECK(device->BusDevice != NULL);
    device->Transport.Device = device->BusDevice;
    device->InterruptId = 123;
    device->EventDescriptor = 42;
    CHECK(SHMCreate(&memory, &device->Metadata) == OS_EOK);
    CHECK(SHMGetSGTable(&device->Metadata, &device->MetadataSg, -1) == OS_EOK);
    CHECK(SHMCreate(&memory, &device->Session.Pools[0].Memory) == OS_EOK);
    CHECK(SHMGetSGTable(&device->Session.Pools[0].Memory,
                        &device->Session.Pools[0].ScatterGather, -1) == OS_EOK);
    device->Session.PoolCount = 1;
    device->Session.Pools[0].Mapped = true;
    metadataId = device->Metadata.ID;
    poolId = device->Session.Pools[0].Memory.ID;
    CHECK(VirtioNetQueuesPrepare(device) == OS_EOK);

    g_failReset = true;
    status = VirtioNetDeviceDestroy(device);
    CHECK(status == OS_EDEVFAULT);
    CHECK(device->ReceiveQueue == &g_rxQueue && device->TransmitQueue == &g_txQueue);
    CHECK(device->InterruptId == 123 && device->EventDescriptor == 42);
    CHECK(device->Session.Pools[0].Mapped);
    CHECK(device->MetadataSg.Entries != NULL);
    CHECK(device->Session.Pools[0].ScatterGather.Entries != NULL);
    CHECK(NetTestShmData(metadataId) != NULL && NetTestShmData(poolId) != NULL);
    CHECK(g_netShm.Live == live + 2);
    CHECK(g_interruptTeardowns == interrupts && g_transportTeardowns == transports);

    g_failReset = false;
    status = VirtioNetDeviceDestroy(device);
    CHECK(status == OS_EOK);
    CHECK(g_netShm.Live == live);
    CHECK(g_interruptTeardowns == interrupts + 1);
    CHECK(g_transportTeardowns == transports + 1);
    CHECK(VirtioNetDeviceDestroy(NULL) == OS_EOK);
}

static void
__TestCloseHistory(
        void)
{
    struct ctt_netadapter_session oldest;
    struct ctt_netadapter_session current;
    size_t                       before = g_device.ClosedCount;

    for (unsigned cycle = 0; cycle < 64; ++cycle) {
        message.client = 100 + cycle;
        ctt_netadapter_open_invocation(&message, NETADAPTER_FAKE_DEVICE, 0, 0);
        CHECK(reply.Status == OS_EOK && g_device.Session.Active);
        CHECK(g_device.ClosedCount < g_device.ClosedCapacity);
        current = g_device.Session.Identity;
        if (!cycle) {
            oldest = current;
        }
        ctt_netadapter_open_invocation(&message, NETADAPTER_FAKE_DEVICE, 0, 0);
        CHECK(reply.Status == OS_EOK && g_device.Session.Identity.id == current.id);
        ctt_netadapter_close_invocation(&message, &current);
        CHECK(reply.Status == OS_EOK && !g_device.Session.Active);
        ctt_netadapter_close_invocation(&message, &current);
        CHECK(reply.Status == OS_EOK);
        ctt_netadapter_open_invocation(&message, NETADAPTER_FAKE_DEVICE, 0, 0);
        CHECK(reply.Status == OS_ENOENT && !g_device.Session.Active);
        CHECK(g_device.ClosedCount == before + cycle + 1);
    }
    message.client = 100;
    ctt_netadapter_close_invocation(&message, &oldest);
    CHECK(reply.Status == OS_EOK);
    ctt_netadapter_open_invocation(&message, NETADAPTER_FAKE_DEVICE, 0, 0);
    CHECK(reply.Status == OS_ENOENT);
    message.client = 200;
    ctt_netadapter_close_invocation(&message, &oldest);
    CHECK(reply.Status == OS_ENOENT);
}

#ifdef VIRTIO_NET_SESSION_TEST_ONLY
int
main(
        void)
{
    __TestDeviceDestroy();
    __TestCloseHistory();
    free(g_device.Closed);
    puts("virtio-net: failed-destroy retention/retry, 64 open/close cycles and retained endpoint retries passed");
    return 0;
}
#else
static void
Handle(
        const NetAdapterRequest_t* input)
{
    NetAdapterRequest_t r = *input;
    reply = (NetAdapterReply_t){0};
    switch (r.Operation) {
        case OP(GET_INFO):
            ctt_netadapter_get_info_invocation(&message, r.Device, r.Port);
            break;
        case OP(OPEN):
            ctt_netadapter_open_invocation(&message, r.Device, r.Port, 0);
            break;
        case OP(REGISTER_POOL):
            ctt_netadapter_register_pool_invocation(&message, &r.Session, r.Value, &r.Pool);
            break;
        case OP(CONFIGURE):
            ctt_netadapter_configure_invocation(&message, &r.Session, r.Mtu, 3);
            break;
        case OP(PREPARE_RUN):
            ctt_netadapter_prepare_run_invocation(&message, &r.Session, r.Run);
            break;
        case OP(START_RUN):
            ctt_netadapter_start_run_invocation(&message, &r.Session, r.Run, r.Value);
            break;
        case OP(STOP_RUN):
            ctt_netadapter_stop_run_invocation(&message, &r.Session, r.Run, r.Value);
            break;
        case OP(CLOSE):
            ctt_netadapter_close_invocation(&message, &r.Session);
            break;
        case OP(GET_LINK):
            ctt_netadapter_get_link_invocation(&message, &r.Session);
            break;
        case OP(GET_COUNTERS):
            ctt_netadapter_get_counters_invocation(&message, &r.Session);
            break;
        case OP(POST_RX_BATCH):
            ctt_netadapter_post_rx_batch_invocation(
                    &message, &r.Session, r.Run, r.Value, &r.Ack, r.Packets, r.Count);
            break;
        case OP(SUBMIT_TX_BATCH):
            ctt_netadapter_submit_tx_batch_invocation(
                    &message, &r.Session, r.Run, r.Value, &r.Ack, r.Packets, r.Count);
            break;
        case OP(ACKNOWLEDGE):
            ctt_netadapter_acknowledge_invocation(&message, &r.Session, &r.Ack);
            break;
        case OP(DRAIN):
            ctt_netadapter_drain_invocation(
                    &message, &r.Session, r.Value, r.After, r.Count, &r.Ack);
            break;
        default:
            CHECK(false);
    }
    if (r.Operation == OP(SUBMIT_TX_BATCH) && r.Run == 1) {
        uint64_t packets = g_device.Session.Counters.tx_packets;
        unsigned submissions = g_submissions;
        unsigned queued = eventCount;
        // Identical replay keeps cached admission and never repeats payload
        // access. A changed descriptor is a non-consuming protocol error.
        ctt_netadapter_submit_tx_batch_invocation(
                &message, &r.Session, r.Run, r.Value, &r.Ack, r.Packets, r.Count);
        CHECK(g_submissions == submissions && g_device.Session.Counters.tx_packets == packets &&
              eventCount == queued + 1 && events[queued].Status == OS_EOK);
        eventCount = queued;
        r.Packets[0].flags = 1;
        ctt_netadapter_submit_tx_batch_invocation(
                &message, &r.Session, r.Run, r.Value, &r.Ack, r.Packets, r.Count);
        CHECK(g_submissions == submissions && g_device.Session.Counters.tx_packets == packets &&
              eventCount == queued + 1 && events[queued].Status == OS_EPROTOCOL);
        eventCount = queued;
    }
    if (NetAdapterRequestIsControl(&r)) {
        if (reply.Status) {
            fprintf(stderr, "operation %u status %u\n", r.Operation, reply.Status);
        }
        oserr_t result = HandleAdapterRequest(adapter, 2, r.Serial, &reply, now);
        if (result) {
            fprintf(stderr, "core reply operation=%u status=%u\n", r.Operation, result);
        }
        CHECK(result == OS_EOK);
    }
}
static void
Pump(
        void)
{
    const NetAdapterRequest_t* r;
    for (unsigned i = 0; i < 16 && NetAdapterNextRequest(adapter, now, &r) == OS_EOK; ++i) {
        Handle(r);
    }
    VirtioNetPoll(&g_device);
    for (unsigned i = 0; i < eventCount; ++i) {
        CHECK(NetAdapterEvent(adapter, 2, &events[i], now) == OS_EOK);
    }
    eventCount = 0;
    now += 10;
}
static void
State(
        enum NetAdapterState state)
{
    NetAdapterSnapshot_t s;
    for (unsigned i = 0; i < 3000; ++i) {
        NetAdapterSnapshot(adapter, &s);
        if (s.State == state) {
            return;
        }
        CHECK(s.State != NET_ADAPTER_FAILED && s.State != NET_ADAPTER_QUARANTINED);
        Pump();
    }
    fprintf(stderr, "state %u wanted %u error %u\n", s.State, state, s.LastError);
    CHECK(false);
}
static void
Receive(
        void*       unused,
        const void* bytes,
        uint32_t    length)
{
    (void)unused;
    unsigned payload = rxCount % 2 ? 42 : NETADAPTER_TEST_FRAME_BYTES;
    CHECK(length == (payload < 60 ? 60 : payload));
    for (unsigned i = 6; i < payload; ++i) {
        CHECK(((const unsigned char*)bytes)[i] == (unsigned char)(rxCount + i));
    }
    for (unsigned i = payload; i < length; ++i) {
        CHECK(((const unsigned char*)bytes)[i] == 0);
    }
    rxCount++;
}
static void
Transmitted(
        void*    unused,
        uint64_t cookie,
        oserr_t  status)
{
    (void)unused;
    if (cookie == 32) {
        CHECK(status == OS_ECANCELLED);
        return;
    }
    CHECK(status == OS_EOK && cookie < 32 && !(txSeen & (1ull << cookie)));
    txSeen |= 1ull << cookie;
    txCount++;
}
int
main(
        void)
{
    g_device.Info = (struct ctt_netadapter_info){.port_count = 1,
                                                 .min_version = 2,
                                                 .max_version = 2,
                                                 .framing = CTT_NETADAPTER_FRAMING_ETHERNET,
                                                 .medium = CTT_NETADAPTER_MEDIUM_VIRTUAL,
                                                 .min_mtu = 1500,
                                                 .max_mtu = 1500,
                                                 .current_mtu = 1500,
                                                 .max_frame_size = 1514,
                                                 .buffer_alignment = 1,
                                                 .max_pools = VIRTIO_NET_POOLS,
                                                 .max_pool_bytes = VIRTIO_NET_POOL_BYTES,
                                                 .max_registered_bytes = 2 * VIRTIO_NET_POOL_BYTES,
                                                 .max_slots_per_pool = 32,
                                                 .max_queue_pairs = 1,
                                                 .max_segments = 1,
                                                 .max_batch_size = VIRTIO_NET_BATCH,
                                                 .max_pending_batches = VIRTIO_NET_WINDOW,
                                                 .max_outstanding_tx = 32,
                                                 .max_outstanding_rx = 32,
                                                 .max_unacked_completions = VIRTIO_NET_JOURNAL,
                                                 .min_rx_slots = 4};
    g_device.Link = (struct ctt_netadapter_link){
            1, CTT_NETADAPTER_LINK_STATUS_UP, CTT_NETADAPTER_DUPLEX_FULL, 0};
    CHECK(SHMCreate(&(SHM_t){.Flags = SHM_DEVICE | SHM_CLEAN, .Access = 3, .Size = 8192},
                    &g_device.Metadata) == OS_EOK);
    CHECK(SHMGetSGTable(&g_device.Metadata, &g_device.MetadataSg, -1) == OS_EOK);
    NetAdapterConfig_t config;
    NetAdapterConfigInitializeDefault(&config);
    NetAdapterCallbacks_t callbacks = {.Receive = Receive, .Transmitted = Transmitted};
    CHECK(NetAdapterCreate(NETADAPTER_FAKE_DEVICE, 2, 0, &config, &callbacks, &adapter) == OS_EOK);
    for (unsigned run = 1; run <= 2; ++run) {
        State(NET_ADAPTER_RUNNING);
        g_dropEvents = run == 2;
        g_dropAdmission = run == 2;
        for (unsigned id = (run - 1) * 16; id < run * 16; ++id) {
            unsigned char frame[NETADAPTER_TEST_FRAME_BYTES];
            for (unsigned i = 0; i < sizeof(frame); ++i) {
                frame[i] = (unsigned char)(id + i);
            }
            memset(frame, 0xff, 6);
            CHECK(NetAdapterSend(adapter, frame, id % 2 ? 42 : sizeof(frame), id) == OS_EOK);
        }
        for (unsigned i = 0; i < 3000 && (txCount < run * 16 || rxCount < run * 16); ++i) {
            Pump();
        }
        CHECK(txCount == run * 16 && rxCount == run * 16);
        NetAdapterStop(adapter);
        State(NET_ADAPTER_STOPPED);
        CHECK(!g_device.Session.Outstanding[0] && !g_device.Session.Outstanding[1]);
        CHECK(g_device.Session.Progress.retired_batch_id ==
              g_device.Session.Progress.consumed_batch_id);
        CHECK(g_device.Session.Progress.retired_completion_sequence ==
              g_device.Session.Progress.highest_completion_sequence);
        if (run == 1) {
            CHECK(NetAdapterStart(adapter) == OS_EOK);
        }
    }
    struct ctt_netadapter_progress before = g_device.Session.Progress;
    CHECK(VirtioNetAcknowledge(&g_device, &(struct ctt_netadapter_ack){UINT64_MAX, 0}) ==
          OS_EINVALPARAMS);
    CHECK(!memcmp(&before, &g_device.Session.Progress, sizeof(before)));
    struct ctt_netadapter_session identity = g_device.Session.Identity;
    message.client = 9;
    ctt_netadapter_close_invocation(&message, &identity);
    CHECK(reply.Status == OS_ENOENT);
    message.client = 3;
    // A failed notify occurs after publication. The driver must admit and keep
    // the lease until a successful reset, even though no completion will arrive.
    CHECK(NetAdapterStart(adapter) == OS_EOK);
    State(NET_ADAPTER_RUNNING);
    g_failNotify = true;
    unsigned char finalFrame[42] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(NetAdapterSend(adapter, finalFrame, sizeof(finalFrame), 32) == OS_EOK);
    Pump();
    CHECK(g_device.Session.Faulted &&
          g_device.Session.Outstanding[CTT_NETADAPTER_DIRECTION_TX - 1] == 1);
    g_failReset = true;
    CHECK(VirtioNetClose(&g_device) == OS_EDEVFAULT);
    CHECK(g_device.Session.Active && g_device.Session.Pools[0].Mapped &&
          g_device.Session.Pools[1].Mapped);
    g_failReset = false;
    g_failNotify = false;
    NetAdapterClose(adapter);
    State(NET_ADAPTER_CLOSED);
    ctt_netadapter_close_invocation(&message, &identity);
    CHECK(reply.Status == OS_EOK);
    CHECK(NetAdapterDestroy(&adapter) == OS_EOK);
    __TestCloseHistory();
    message.client = 3;
    ctt_netadapter_close_invocation(&message, &identity);
    CHECK(reply.Status == OS_EOK);
    ctt_netadapter_open_invocation(&message, NETADAPTER_FAKE_DEVICE, 0, 0);
    CHECK(reply.Status == OS_ENOENT);
    free(g_device.Closed);
    OSHandleDestroy(&g_device.Metadata);
    free(g_device.MetadataSg.Entries);
    CHECK(!g_netShm.Live);
    puts("virtio-net: real session/pools/SG code, replay, event loss, restart and failed-reset "
         "retention passed");
    return 0;
}
#endif
