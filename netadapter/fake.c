/**
 * Software-only v2 controller for trusted Vali test images.
 *
 * One serialized Gracht executor owns all state. Accepted RX descriptors remain
 * posted until a TX frame is copied into them or stop cancels them. TX reads and
 * RX writes use attachments of netd's actual SHM, never addresses sent over IPC.
 * No DMA/hardware claim is made. Fixed replay/journal arrays make lost delivery
 * recoverable without allocating in the packet path.
 *
 * Run 1 is ordinary loopback. Run 2 deliberately loses the first TX admission,
 * every live completion, one drain chunk and one drain end. The same protocol
 * recovery used by a controller must get netd through this run and its stop fence.
 */
#include "fake.h"
#include <ctt_netadapter_service_server.h>
#include <os/shm.h>
#include <os/handle.h>
#include <string.h>
#ifndef NETADAPTER_FAKE_HOST_TEST
#include <ddk/service.h>
#include <ddk/utils.h>
#include <gracht/link/vali.h>
#include <io.h>
#include <internal/_utils.h>
#include <threads.h>
#include <stdlib.h>
#endif

#define POOLS 2u
#define SLOTS 32u
#define BATCH 4u
#define WINDOW 4u
#define JOURNAL 128u
#define TX CTT_NETADAPTER_DIRECTION_TX
#define RX CTT_NETADAPTER_DIRECTION_RX
#define SUCCESS CTT_NETADAPTER_COMPLETION_STATUS_SUCCESS
#define CANCELLED CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED

enum RunState { OPENED, PREPARED, RUNNING, STOPPED };
struct Slot {
    uint64_t Sequence;
    bool Owned;
    struct ctt_netadapter_packet Packet;
};
struct Pool {
    uint64_t Registration;
    struct ctt_netadapter_pool Description;
    OSHandle_t Memory;
    struct Slot Slots[SLOTS];
    bool Mapped;
};
struct Batch {
    uint64_t Id, Run;
    enum ctt_netadapter_direction Direction;
    uint32_t Count;
    struct ctt_netadapter_packet Packets[BATCH];
    struct ctt_netadapter_admission Admissions[BATCH];
};
static struct {
    gracht_server_t* Server;
    gracht_conn_t Owner;
    struct ctt_netadapter_session Session;
    struct ctt_netadapter_link Link;
    struct ctt_netadapter_counters Counters;
    struct ctt_netadapter_progress Progress;
    struct Pool Pools[POOLS];
    struct Batch Batches[WINDOW];
    struct ctt_netadapter_completion Journal[JOURNAL];
    uint64_t Run, StartFence, StopFence, StopBarrier, DrainId;
    uint32_t Outstanding, PoolCount;
    enum RunState State;
    bool Opened, Closed, Configured, LostAdmission;
    unsigned LostDrain;
} g_fake;

/** Routing identity is checked even for one-way calls. This test uses today's
 * endpoint metadata; it does not add kernel-authenticated principals. */
static bool Owner(struct gracht_message* m, const struct ctt_netadapter_session* s)
{
    return g_fake.Opened && m->client == g_fake.Owner &&
        s->id == g_fake.Session.id && s->generation == g_fake.Session.generation;
}
static bool Active(struct gracht_message* m, const struct ctt_netadapter_session* s)
{ return Owner(m, s) && !g_fake.Closed; }

static struct ctt_netadapter_info Info(void)
{
    struct ctt_netadapter_info info = {
        .port_count = 1, .min_version = 2, .max_version = 2,
        .framing = CTT_NETADAPTER_FRAMING_ETHERNET, .medium = CTT_NETADAPTER_MEDIUM_VIRTUAL,
        .min_mtu = 1500, .max_mtu = 1500, .current_mtu = 1500, .max_frame_size = 1514,
        .buffer_alignment = 1, .max_pools = POOLS, .max_pool_bytes = 128 * 1024,
        .max_registered_bytes = 256 * 1024, .max_slots_per_pool = SLOTS,
        .max_queue_pairs = 1, .max_segments = 1, .max_batch_size = BATCH,
        .max_pending_batches = WINDOW, .max_outstanding_tx = SLOTS,
        .max_outstanding_rx = SLOTS, .max_unacked_completions = JOURNAL, .min_rx_slots = 4
    };
    info.current_mac = info.permanent_mac = (struct ctt_netadapter_mac){ .octet0 = 2, .octet5 = 1 };
    return info;
}

/** Validate both ACKs before advancing either. Ring entries are overwritten only
 * after retirement; packet ownership is independently tracked in each slot. */
static oserr_t Acknowledge(const struct ctt_netadapter_ack* ack)
{
    if (ack->through_batch_id > g_fake.Progress.consumed_batch_id ||
        ack->through_completion_sequence > g_fake.Progress.highest_completion_sequence) return OS_EINVALPARAMS;
    if (ack->through_batch_id > g_fake.Progress.retired_batch_id)
        g_fake.Progress.retired_batch_id = ack->through_batch_id;
    if (ack->through_completion_sequence > g_fake.Progress.retired_completion_sequence)
        g_fake.Progress.retired_completion_sequence = ack->through_completion_sequence;
    return OS_EOK;
}

static void* Bytes(struct Pool* pool, const struct ctt_netadapter_packet* packet)
{
    return (char*)SHMBuffer(&pool->Memory) + (size_t)packet->id.slot_id * pool->Description.slot_size + packet->data_offset;
}

/** Admission reserves one journal record for each owned slot. Completion consumes
 * that reservation, so even cancelling every RX at stop cannot overflow the ring.
 * The slot becomes idle before sending events; publication failure changes no
 * ownership and records remain available to drain until cumulatively ACKed. */
static void Complete(struct Pool* pool, struct Slot* slot,
    enum ctt_netadapter_completion_status status, uint32_t length)
{
    uint64_t sequence = ++g_fake.Progress.highest_completion_sequence;
    g_fake.Journal[(sequence - 1) % JOURNAL] = (struct ctt_netadapter_completion){
        .completion_sequence = sequence, .direction = pool->Description.direction,
        .id = slot->Packet.id, .status = status,
        .detail = status == SUCCESS ? OS_EOK : OS_ECANCELLED, .length = length
    };
    slot->Owned = false;
    g_fake.Outstanding--;
}

static void Push(uint64_t after)
{
    if (g_fake.Run == 2) return; // deterministic lost-final-event recovery
    while (after < g_fake.Progress.highest_completion_sequence) {
        struct ctt_netadapter_completion records[BATCH];
        uint32_t count = 0;
        while (count < BATCH && after < g_fake.Progress.highest_completion_sequence)
            records[count++] = g_fake.Journal[after++ % JOURNAL];
        (void)ctt_netadapter_event_completions_single(g_fake.Server, g_fake.Owner,
            &g_fake.Session, 0, records, count, &g_fake.Progress);
    }
}

static bool SamePacket(const struct ctt_netadapter_packet* a, const struct ctt_netadapter_packet* b)
{
    return a->id.queue_id == b->id.queue_id && a->id.pool_id == b->id.pool_id &&
        a->id.slot_id == b->id.slot_id && a->id.submission_sequence == b->id.submission_sequence &&
        a->data_offset == b->data_offset && a->length == b->length && a->flags == b->flags;
}
static bool SamePool(const struct ctt_netadapter_pool* a, const struct ctt_netadapter_pool* b)
{
    return a->direction == b->direction && a->buffer_handle == b->buffer_handle &&
        a->region_offset == b->region_offset && a->region_size == b->region_size &&
        a->slot_size == b->slot_size && a->slot_count == b->slot_count;
}

static oserr_t Admit(enum ctt_netadapter_direction direction, const struct ctt_netadapter_packet* packet)
{
    if (!packet->id.pool_id || packet->id.pool_id > g_fake.PoolCount) return OS_EINVALPARAMS;
    struct Pool* pool = &g_fake.Pools[packet->id.pool_id - 1];
    if (!pool->Mapped || pool->Description.direction != direction || packet->id.queue_id ||
        packet->id.slot_id >= pool->Description.slot_count) return OS_EINVALPARAMS;
    struct Slot* slot = &pool->Slots[packet->id.slot_id];
    if (!packet->id.submission_sequence || packet->id.submission_sequence <= slot->Sequence) return OS_EINVALPARAMS;
    slot->Sequence = packet->id.submission_sequence; // rejected descriptors also consume slot sequence
    if (slot->Owned) return OS_EBUSY;
    if (packet->flags || packet->data_offset > pool->Description.slot_size ||
        packet->length > pool->Description.slot_size - packet->data_offset ||
        (direction == TX && (packet->length < 14 || packet->length > 1514)) ||
        (direction == RX && packet->length < 1514)) return OS_EINVALPARAMS;
    if (g_fake.Progress.highest_completion_sequence - g_fake.Progress.retired_completion_sequence +
        g_fake.Outstanding >= JOURNAL) return OS_EBUSY;
    slot->Packet = *packet; slot->Owned = true; g_fake.Outstanding++;
    if (direction == TX) {
        // Copy before completing either slot. Loopback never retains a pointer
        // after completion and therefore has no hidden references at close.
        bool delivered = false;
        for (uint32_t p = 0; p < g_fake.PoolCount && !delivered; ++p) {
            struct Pool* rx = &g_fake.Pools[p];
            if (!rx->Mapped || rx->Description.direction != RX) continue;
            for (uint32_t i = 0; i < rx->Description.slot_count; ++i) if (rx->Slots[i].Owned) {
                memcpy(Bytes(rx, &rx->Slots[i].Packet), Bytes(pool, packet), packet->length);
                Complete(rx, &rx->Slots[i], SUCCESS, packet->length);
                g_fake.Counters.rx_packets++; g_fake.Counters.rx_bytes += packet->length;
                delivered = true; break;
            }
        }
        if (!delivered) { g_fake.Counters.rx_no_buffer++; g_fake.Counters.rx_dropped++; }
        Complete(pool, slot, SUCCESS, packet->length);
        g_fake.Counters.tx_packets++; g_fake.Counters.tx_bytes += packet->length;
    }
    return OS_EOK;
}

static void Submit(struct gracht_message* m, const struct ctt_netadapter_session* s,
    uint64_t run, uint64_t id, const struct ctt_netadapter_ack* ack,
    const struct ctt_netadapter_packet* packets, uint32_t count, enum ctt_netadapter_direction direction)
{
    if (!Active(m, s)) return;
    oserr_t status = Acknowledge(ack);
    struct Batch* batch = &g_fake.Batches[id ? (id - 1) % WINDOW : 0];
    bool fresh = false;
    if (status == OS_EOK) {
        if (run != g_fake.Run || !run || !id || id <= g_fake.Progress.retired_batch_id) status = OS_ENOENT;
        else if (!count || count > BATCH) status = OS_EINVALPARAMS;
        else if (id <= g_fake.Progress.consumed_batch_id) {
            if (batch->Id != id || batch->Run != run || batch->Direction != direction || batch->Count != count) status = OS_EPROTOCOL;
            else for (uint32_t i = 0; i < count; ++i)
                if (!SamePacket(&batch->Packets[i], &packets[i])) { status = OS_EPROTOCOL; break; }
        } else if (g_fake.State == STOPPED || g_fake.State == OPENED || (direction == TX && g_fake.State != RUNNING)) status = OS_EBUSY;
        else if (id != g_fake.Progress.consumed_batch_id + 1 ||
            id - g_fake.Progress.retired_batch_id > WINDOW) status = OS_EBUSY;
        else fresh = true;
    }
    if (fresh) {
        *batch = (struct Batch){ .Id = id, .Run = run, .Direction = direction, .Count = count };
        memcpy(batch->Packets, packets, count * sizeof(*packets));
        uint64_t before = g_fake.Progress.highest_completion_sequence;
        // Cache every result before attempting delivery. A duplicate batch must
        // neither copy a TX twice nor acquire RX ownership a second time.
        for (uint32_t i = 0; i < count; ++i) {
            batch->Admissions[i].id = packets[i].id;
            batch->Admissions[i].status = Admit(direction, &packets[i]);
        }
        g_fake.Progress.consumed_batch_id = id;
        Push(before); // intentionally permits completions before admission
        if (run == 2 && direction == TX && !g_fake.LostAdmission) {
            g_fake.LostAdmission = true;
            return;
        }
    }
    (void)ctt_netadapter_event_batch_admitted_single(g_fake.Server, g_fake.Owner, s, run, id, status,
        status == OS_EOK ? batch->Admissions : NULL, status == OS_EOK ? count : 0, &g_fake.Progress);
}

void ctt_netadapter_get_info_invocation(struct gracht_message* m, uuid_t device, uint32_t port)
{
    struct ctt_netadapter_info info = Info();
    ctt_netadapter_get_info_response(m, device == NETADAPTER_FAKE_DEVICE && !port ? OS_EOK : OS_ENOENT, &info);
}
void ctt_netadapter_open_invocation(struct gracht_message* m, uuid_t device, uint32_t port, uint32_t version, uint64_t features)
{
    oserr_t status = OS_EOK;
    if (device != NETADAPTER_FAKE_DEVICE || port) status = OS_ENOENT;
    else if (version != 2 || features) status = OS_ENOTSUPPORTED;
    else if (g_fake.Closed || (g_fake.Opened && m->client != g_fake.Owner)) status = OS_EBUSY;
    else if (gracht_server_register_client(m)) status = OS_EOOM;
    else if (!g_fake.Opened) { g_fake.Opened = true; g_fake.Owner = m->client; }
    struct ctt_netadapter_session session = status == OS_EOK ? g_fake.Session : (struct ctt_netadapter_session){0};
    ctt_netadapter_open_response(m, status, &session, 0, &g_fake.Link);
}
void ctt_netadapter_register_pool_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s,
    uint64_t key, const struct ctt_netadapter_pool* description)
{
    oserr_t status = OS_EOK; uint32_t id = 0;
    if (!Active(m, s)) status = OS_ENOENT;
    else {
        for (uint32_t i = 0; i < g_fake.PoolCount; ++i) if (g_fake.Pools[i].Registration == key) {
            struct Pool* pool = &g_fake.Pools[i];
            status = pool->Mapped && SamePool(&pool->Description, description) ? OS_EOK : OS_EPROTOCOL;
            ctt_netadapter_register_pool_response(m, status, status == OS_EOK ? i + 1 : 0); return;
        }
        // Restrict the fake to whole, nonaliased pools starting at zero. This
        // subset matches netd; it avoids pretending to validate storage aliases.
        if (!key || (g_fake.State != OPENED && g_fake.State != STOPPED) || g_fake.Outstanding ||
            g_fake.PoolCount == POOLS) status = OS_EBUSY;
        else if ((description->direction != TX && description->direction != RX) || description->region_offset ||
            !description->slot_count || description->slot_count > SLOTS || description->slot_size < 1514 ||
            description->region_size > 128 * 1024 ||
            (uint64_t)description->slot_count * description->slot_size > description->region_size) status = OS_EINVALPARAMS;
        else {
            for (uint32_t i = 0; i < g_fake.PoolCount; ++i)
                if (description->buffer_handle == g_fake.Pools[i].Description.buffer_handle ||
                    description->direction == g_fake.Pools[i].Description.direction) status = OS_EINVALPARAMS;
            if (status == OS_EOK) {
                struct Pool* pool = &g_fake.Pools[g_fake.PoolCount];
                status = SHMAttach(description->buffer_handle, &pool->Memory);
                if (status == OS_EOK) {
                    if (description->region_size > SHMBufferCapacity(&pool->Memory)) status = OS_EINVALPARAMS;
                    else status = SHMMap(&pool->Memory, 0, (size_t)description->region_size,
                        description->direction == TX ? SHM_ACCESS_READ : SHM_ACCESS_WRITE);
                    if (status != OS_EOK) OSHandleDestroy(&pool->Memory);
                    else { pool->Registration = key; pool->Description = *description; pool->Mapped = true; id = ++g_fake.PoolCount; }
                }
            }
        }
    }
    ctt_netadapter_register_pool_response(m, status, id);
}
void ctt_netadapter_configure_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, uint32_t mtu, uint32_t filter)
{
    oserr_t status = !Active(m, s) ? OS_ENOENT :
        (g_fake.State != OPENED && g_fake.State != STOPPED) || g_fake.Outstanding ||
        g_fake.Progress.highest_completion_sequence != g_fake.Progress.retired_completion_sequence ? OS_EBUSY :
        mtu != 1500 || filter != (CTT_NETADAPTER_RX_FILTER_UNICAST | CTT_NETADAPTER_RX_FILTER_BROADCAST) ? OS_EINVALPARAMS : OS_EOK;
    if (status == OS_EOK) g_fake.Configured = true;
    ctt_netadapter_configure_response(m, status);
}
void ctt_netadapter_get_link_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s)
{ ctt_netadapter_get_link_response(m, Active(m, s) ? OS_EOK : OS_ENOENT, &g_fake.Link); }
void ctt_netadapter_get_counters_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s)
{ ctt_netadapter_get_counters_response(m, Active(m, s) ? OS_EOK : OS_ENOENT, &g_fake.Counters); }
void ctt_netadapter_unregister_pool_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, uint32_t id)
{
    oserr_t status = !Active(m, s) ? OS_ENOENT : !id || id > g_fake.PoolCount ? OS_EINVALPARAMS :
        (g_fake.State != OPENED && g_fake.State != STOPPED) || g_fake.Outstanding ||
        g_fake.Progress.highest_completion_sequence != g_fake.Progress.retired_completion_sequence ? OS_EBUSY : OS_EOK;
    if (status == OS_EOK && g_fake.Pools[id - 1].Mapped) {
        OSHandleDestroy(&g_fake.Pools[id - 1].Memory); g_fake.Pools[id - 1].Mapped = false;
    }
    ctt_netadapter_unregister_pool_response(m, status);
}
void ctt_netadapter_close_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s)
{
    if (!Owner(m, s)) { ctt_netadapter_close_response(m, OS_ENOENT); return; }
    // No asynchronous backend exists: returning from the last copy quiesced all
    // accesses. Detach before replying, and retain the closed identity for retry.
    g_fake.Closed = true;
    for (uint32_t i = 0; i < g_fake.PoolCount; ++i) if (g_fake.Pools[i].Mapped) {
        OSHandleDestroy(&g_fake.Pools[i].Memory); g_fake.Pools[i].Mapped = false;
    }
    g_fake.Outstanding = 0;
    ctt_netadapter_close_response(m, OS_EOK);
}
void ctt_netadapter_post_rx_batch_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s,
    uint64_t run, uint64_t id, const struct ctt_netadapter_ack* ack, const struct ctt_netadapter_packet* p, uint32_t n)
{ Submit(m, s, run, id, ack, p, n, RX); }
void ctt_netadapter_submit_tx_batch_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s,
    uint64_t run, uint64_t id, const struct ctt_netadapter_ack* ack, const struct ctt_netadapter_packet* p, uint32_t n)
{ Submit(m, s, run, id, ack, p, n, TX); }
void ctt_netadapter_acknowledge_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, const struct ctt_netadapter_ack* ack)
{
    if (!Active(m, s)) return;
    oserr_t status = Acknowledge(ack);
    ctt_netadapter_event_ack_progress_single(g_fake.Server, g_fake.Owner, s, ack, status, &g_fake.Progress);
}
void ctt_netadapter_drain_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s,
    uint64_t id, uint64_t after, uint32_t limit, const struct ctt_netadapter_ack* ack)
{
    if (!Active(m, s) || id <= g_fake.DrainId) return;
    g_fake.DrainId = id;
    oserr_t status = Acknowledge(ack);
    uint64_t highest = g_fake.Progress.highest_completion_sequence;
    uint32_t count = 0;
    struct ctt_netadapter_completion records[BATCH];
    if (status == OS_EOK) {
        if (after < g_fake.Progress.retired_completion_sequence) status = OS_ENOENT;
        else if (after > highest || !limit || limit > BATCH) status = OS_EINVALPARAMS;
        else {
            count = (uint32_t)(highest - after < limit ? highest - after : limit);
            for (uint32_t i = 0; i < count; ++i) records[i] = g_fake.Journal[(after + i) % JOURNAL];
        }
    }
    // Snapshot copied before sending. A failed publication doesn't retain a
    // request buffer or block future drains. The journal is the recovery source.
    unsigned loss = g_fake.Run == 2 && count && g_fake.LostDrain < 2 ? ++g_fake.LostDrain : 0;
    if (count && loss != 1) ctt_netadapter_event_completions_single(g_fake.Server, g_fake.Owner, s, id, records, count, &g_fake.Progress);
    if (loss != 2) ctt_netadapter_event_drain_end_single(g_fake.Server, g_fake.Owner, s, id, status,
        after, after + count, count, highest, &g_fake.Progress);
}
void ctt_netadapter_prepare_run_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, uint64_t run)
{
    oserr_t status = OS_EOK;
    if (!Active(m, s)) status = OS_ENOENT;
    else if (run == g_fake.Run && (g_fake.State == PREPARED || g_fake.State == RUNNING)) { /* idempotent */ }
    else if (!run || run != g_fake.Run + 1) status = OS_ENOENT;
    else if (!g_fake.Configured || g_fake.PoolCount != POOLS ||
        !g_fake.Pools[0].Mapped || !g_fake.Pools[1].Mapped ||
        (g_fake.State != OPENED && g_fake.State != STOPPED) || g_fake.Outstanding ||
        g_fake.Progress.retired_batch_id != g_fake.Progress.consumed_batch_id ||
        g_fake.Progress.retired_completion_sequence != g_fake.Progress.highest_completion_sequence) status = OS_EBUSY;
    else { g_fake.Run = run; g_fake.State = PREPARED; }
    ctt_netadapter_prepare_run_response(m, status);
}
void ctt_netadapter_start_run_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, uint64_t run, uint64_t fence)
{
    oserr_t status = !Active(m, s) || run != g_fake.Run ? OS_ENOENT : OS_EOK;
    if (status == OS_EOK) {
        if (g_fake.State == RUNNING) status = fence == g_fake.StartFence ? OS_EOK : OS_EINVALPARAMS;
        else if (g_fake.State != PREPARED || fence != g_fake.Progress.consumed_batch_id || g_fake.Outstanding < 4) status = OS_EBUSY;
        else { g_fake.StartFence = fence; g_fake.State = RUNNING; }
    }
    ctt_netadapter_start_run_response(m, status);
}
void ctt_netadapter_stop_run_invocation(struct gracht_message* m, const struct ctt_netadapter_session* s, uint64_t run, uint64_t fence)
{
    oserr_t status = !Active(m, s) || run != g_fake.Run ? OS_ENOENT : OS_EOK;
    if (status == OS_EOK) {
        if (g_fake.State == STOPPED) status = fence == g_fake.StopFence ? OS_EOK : OS_EINVALPARAMS;
        else if ((g_fake.State != PREPARED && g_fake.State != RUNNING) || fence != g_fake.Progress.consumed_batch_id) status = OS_EBUSY;
        else {
            g_fake.State = STOPPED; g_fake.StopFence = fence;
            uint64_t before = g_fake.Progress.highest_completion_sequence;
            for (uint32_t p = 0; p < g_fake.PoolCount; ++p)
                for (uint32_t i = 0; i < g_fake.Pools[p].Description.slot_count; ++i)
                    if (g_fake.Pools[p].Slots[i].Owned) Complete(&g_fake.Pools[p], &g_fake.Pools[p].Slots[i], CANCELLED, 0);
            g_fake.StopBarrier = g_fake.Progress.highest_completion_sequence;
            Push(before);
        }
    }
    ctt_netadapter_stop_run_response(m, status, status == OS_EOK ? g_fake.StopBarrier : 0);
}

#ifndef NETADAPTER_FAKE_HOST_TEST
static int Serve(void* unused)
{
    (void)unused;
    struct gracht_link_vali* link;
    gracht_server_configuration_t config;
    gracht_server_configuration_init(&config);
    gracht_server_configuration_set_num_workers(&config, 0); // ordered controller executor
    gracht_server_configuration_set_max_msg_size(&config, CTT_NETADAPTER_LIMIT_FRAME_BYTES);
    if (gracht_link_vali_create(&link)) goto fail;
    gracht_link_vali_set_listen(link, 1);
    gracht_link_vali_set_send_timeout(link, 1);
    gracht_link_vali_set_address(link, &(IPCAddress_t){ .Type = IPC_ADDRESS_PATH, .Data.Path = NETADAPTER_FAKE_PATH });
    if (gracht_server_create(&config, &g_fake.Server) ||
        gracht_server_register_protocol(g_fake.Server, &ctt_netadapter_server_protocol) ||
        gracht_server_add_link(g_fake.Server, (struct gracht_link*)link)) goto fail;
    // This process serves one session only. Its endpoint identifies the process
    // lifetime; closing it never resets IDs to admit another owner.
    g_fake.Session = (struct ctt_netadapter_session){ .id = 1,
        .generation = GetNativeHandle(gracht_link_get_handle((struct gracht_link*)link)) };
    g_fake.Link = (struct ctt_netadapter_link){ .sequence = 1,
        .status = CTT_NETADAPTER_LINK_STATUS_UP, .duplex = CTT_NETADAPTER_DUPLEX_FULL, .speed_bps = 1000000000 };
    NOTICE("NETADAPTER FAKE ready at %s", NETADAPTER_FAKE_PATH);
    if (!gracht_server_main_loop(g_fake.Server)) return 0;
fail:
    ERROR("NETADAPTER FAKE startup/executor failure");
    exit(-1);
    return -1;
}
void ServiceInitialize(struct ServiceStartupOptions* options)
{
    (void)options;
    thrd_t thread;
    if (thrd_create(&thread, Serve, NULL) != thrd_success) { ERROR("NETADAPTER FAKE thread failure"); exit(-1); }
    thrd_detach(thread);
}
#endif
