#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../modules/eth/common/netadapter.h"
#include <os/usched/mutex.h>

static unsigned g_submissions;
static unsigned g_completions;
static oserr_t g_batchStatus;
static oserr_t g_admission;
static oserr_t g_prepareStatus;
static int g_stopFails;

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}
void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}
void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
gracht_server_t*
__crt_get_module_server(void)
{
    return NULL;
}

int
ctt_netadapter_event_batch_admitted_single(
    _In_ gracht_server_t* server,
    _In_ gracht_conn_t client,
    _In_ const struct ctt_netadapter_session* session,
    _In_ uint64_t run,
    _In_ uint64_t batch,
    _In_ oserr_t status,
    _In_ const struct ctt_netadapter_admission* admissions,
    _In_ uint32_t count,
    _In_ const struct ctt_netadapter_progress* progress)
{
    g_batchStatus = status;
    g_admission = count ? admissions[0].status : OS_ENOENT;
    assert((status == OS_EOK) == (count != 0));
    return 0;
}

int
ctt_netadapter_event_completions_single(
    _In_ gracht_server_t* server,
    _In_ gracht_conn_t client,
    _In_ const struct ctt_netadapter_session* session,
    _In_ uint64_t request,
    _In_ const struct ctt_netadapter_completion* records,
    _In_ uint32_t count,
    _In_ const struct ctt_netadapter_progress* progress)
{
    g_completions += count;
    // Lost notifications leave the journal available for recovery.
    return -1;
}

int
ctt_netadapter_event_fault_single(
    _In_ gracht_server_t* server,
    _In_ gracht_conn_t client,
    _In_ const struct ctt_netadapter_session* session,
    _In_ const struct ctt_netadapter_fault* fault)
{
    return 0;
}

int
ctt_netadapter_prepare_run_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t status)
{
    g_prepareStatus = status;
    return 0;
}

static oserr_t
__Submit(
    _In_ EthernetDevice_t* device,
    _In_ EthernetSlot_t* slot)
{
    assert(slot->Owned);
    assert(device->Session.Outstanding[0] != 0);
    g_submissions++;
    return OS_EOK;
}

static oserr_t
__Stop(
    _In_ EthernetDevice_t* device)
{
    return g_stopFails ? OS_ETIMEOUT : OS_EOK;
}

static oserr_t
__Prepare(
    _In_ EthernetDevice_t* device)
{
    return OS_EOK;
}

int
main(void)
{
    EthernetDevice_t device = {0};
    EthernetPool_t* pool = &device.Session.Pools[0];
    struct ctt_netadapter_packet packet = { .id = {0, 1, 0, 1}, .length = 60 };
    struct ctt_netadapter_ack ack = {0};
    struct ctt_netadapter_session identity = {1, 2};
    struct gracht_message message = {0};
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    const struct EthernetOperations operations = {
        .Submit = __Submit, .Stop = __Stop, .Prepare = __Prepare
    };

    EthernetRegister(&device, 9, mac);
    device.Operations = &operations;
    device.Link.status = CTT_NETADAPTER_LINK_STATUS_UP;
    device.QueuesPrepared = true;
    device.Session.Active = true;
    device.Session.Configured = true;
    device.Session.Owner = 7;
    device.Session.Principal = 42;
    device.Session.Identity = identity;
    device.Session.Run = 1;
    device.Session.State = ETHERNET_RUNNING;
    device.Session.PoolCount = 1;
    pool->Mapped = true;
    pool->Description.direction = CTT_NETADAPTER_DIRECTION_TX;
    pool->Description.slot_size = 2048;
    pool->Description.slot_count = 2;
    pool->Slots[0].Pool = pool;
    pool->Slots[1].Pool = pool;

    message.client = 7;
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = 99;
    assert(EthernetFindSession(&message, &identity) == NULL);
    message.security.principal = 42;
    assert(EthernetFindSession(&message, &identity) == &device);
    identity.generation++;
    assert(EthernetFindSession(&message, &identity) == NULL);
    identity.generation--;
    message.security.source = GRACHT_SECURITY_NONE;
    assert(EthernetFindSession(&message, &identity) == NULL);
    message.security.source = GRACHT_SECURITY_KERNEL;

    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EOK && g_admission == OS_EOK && g_submissions == 1);
    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EOK && g_submissions == 1);
    packet.length++;
    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EPROTOCOL && g_submissions == 1);
    packet.length--;
    ack.through_batch_id = 1;
    ack.through_completion_sequence = 1;
    assert(EthernetAcknowledge(&device, &ack) == OS_EINVALPARAMS);
    assert(device.Session.Progress.retired_batch_id == 0);
    ack = (struct ctt_netadapter_ack){0};
    EthernetComplete(&device, &pool->Slots[0], OS_EOK, 60);
    EthernetComplete(&device, &pool->Slots[0], OS_EOK, 60);
    assert(device.Session.Progress.highest_completion_sequence == 1);
    assert(device.Session.Counters.tx_packets == 1);
    assert(device.Session.Journal[0].length == 60);
    EthernetSubmit(&device, 1, 3, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EBUSY && device.Session.Progress.consumed_batch_id == 1);

    packet.id.submission_sequence = 2;
    packet.data_offset = 2040;
    EthernetSubmit(&device, 1, 2, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EOK && g_admission == OS_EINVALPARAMS);
    assert(pool->Slots[0].Sequence == 2 && !pool->Slots[0].Owned);
    packet.data_offset = 0;
    packet.id.submission_sequence = 3;
    EthernetSubmit(&device, 1, 3, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EOK && g_admission == OS_EOK && g_submissions == 2);
    g_stopFails = 1;
    assert(EthernetStop(&device, 1, 3) == OS_ETIMEOUT);
    assert(device.Session.State == ETHERNET_STOPPING && pool->Slots[0].Owned);
    assert(EthernetStop(&device, 1, 2) == OS_EINVALPARAMS);
    g_stopFails = 0;
    assert(EthernetStop(&device, 1, 3) == OS_EOK);
    assert(device.Session.StopBarrier == 2 && !pool->Slots[0].Owned);
    assert(device.Session.Journal[1].status == CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED);
    assert(device.Session.Journal[1].length == 0 && g_completions == 1);
    assert(EthernetStop(&device, 1, 3) == OS_EOK && g_completions == 1);
    assert(device.Session.Progress.retired_completion_sequence == 0);
    ctt_netadapter_prepare_run_invocation(&message, &identity, 2);
    assert(g_prepareStatus == OS_EBUSY);
    ack = (struct ctt_netadapter_ack){3, 2};
    assert(EthernetAcknowledge(&device, &ack) == OS_EOK);
    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_ENOENT && g_submissions == 2);
    device.Session.Pools[1].Mapped = true;
    device.Session.Pools[1].Description.direction = CTT_NETADAPTER_DIRECTION_RX;
    device.Session.PoolCount = 2;
    ctt_netadapter_prepare_run_invocation(&message, &identity, 2);
    assert(g_prepareStatus == OS_EOK && device.Session.State == ETHERNET_PREPARED);
    assert(EthernetStop(&device, 1, 3) == OS_ENOENT);
    EthernetUnregister(&device);
    assert(EthernetFindDevice(9) == NULL);
    puts("Ethernet session and recovery tests passed");
    return 0;
}
