#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../modules/eth/common/netadapter-private.h"
#include <os/usched/mutex.h>

static unsigned g_submissions;
static unsigned g_completions;
static oserr_t g_batchStatus;
static oserr_t g_admission;
static oserr_t g_prepareStatus;
static int g_stopFails;
static unsigned g_stops;
static int g_callocFails;
static void* g_trackedCommon;
static void* g_trackedClosed;
static unsigned g_commonFrees;
static unsigned g_closedFrees;
static oserr_t g_linkStatus;
static struct PhyLink g_link;
static unsigned g_linkEvents;
static unsigned g_faults;
static oserr_t g_responseStatus;
static struct ctt_netadapter_session g_identity;
static struct ctt_netadapter_link g_openLink;
static int g_registrationFails;
static unsigned g_registrations;
static oserr_t g_prepareResult;
static unsigned g_prepares;
static oserr_t g_startResult;
static unsigned g_starts;
static unsigned g_polls;
static int g_releaseFails;
static OSHandle_t* g_releaseFailureHandle;
static unsigned g_releasedPools;
static int g_completionFails = 1;
static unsigned g_completionEvents;
static uint64_t g_firstCompletion;
static uint64_t g_lastCompletion;
static unsigned g_drainEvents;
static EthernetDrain_t g_drain;
static struct ctt_netadapter_info g_info;
static const struct EthernetRegistrationConfig g_configuration = {
    .MinRxSlots = 4
};

extern void* __real_calloc(size_t count, size_t size);
extern void __real_free(void* pointer);

void*
__wrap_calloc(
    _In_ size_t count,
    _In_ size_t size)
{
    return g_callocFails ? NULL : __real_calloc(count, size);
}

void
__wrap_free(
    _In_ void* pointer)
{
    if (pointer != NULL) {
        if (pointer == g_trackedCommon) {
            g_commonFrees++;
        }
        if (pointer == g_trackedClosed) {
            g_closedFrees++;
        }
    }
    __real_free(pointer);
}

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
__crt_get_server_iod(void)
{
    return 5;
}

uuid_t
GetNativeHandle(
    _In_ int descriptor)
{
    assert(descriptor == 5);
    return 12;
}

int
gracht_server_register_client(
    _In_ const struct gracht_message* message)
{
    assert(message->security.source == GRACHT_SECURITY_KERNEL);
    assert(message->security.principal != 0);
    g_registrations++;
    return g_registrationFails ? -1 : 0;
}

oserr_t
SHMRelease(
    _In_ OSHandle_t* handle)
{
    if (g_releaseFails || handle == g_releaseFailureHandle) {
        return OS_EBUSY;
    }
    if (handle->Payload != NULL) {
        free(handle->Payload);
        handle->Payload = NULL;
        g_releasedPools++;
    }
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return handle->Payload;
}

int
ctt_netadapter_get_info_response(
    _In_ struct gracht_message*      message,
    _In_ oserr_t                     status,
    _In_ struct ctt_netadapter_info* info)
{
    g_responseStatus = status;
    g_info = *info;
    return 0;
}

int
ctt_netadapter_open_response(
    _In_ struct gracht_message*         message,
    _In_ oserr_t                       status,
    _In_ struct ctt_netadapter_session* identity,
    _In_ uint64_t                      features,
    _In_ struct ctt_netadapter_link*    link)
{
    assert(features == 0);
    g_responseStatus = status;
    g_identity = *identity;
    g_openLink = *link;
    // Lost replies must not undo the completed operation.
    return -1;
}

int
ctt_netadapter_configure_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                status)
{
    g_responseStatus = status;
    return -1;
}

int
ctt_netadapter_start_run_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                status)
{
    g_responseStatus = status;
    return -1;
}

int
ctt_netadapter_close_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                status)
{
    g_responseStatus = status;
    return -1;
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
    uint32_t i;

    assert(count > 0 && count <= ETHERNET_BATCH);
    for (i = 1; i < count; ++i) {
        assert(records[i].completion_sequence == records[i - 1].completion_sequence + 1);
    }
    if (!g_completionEvents) {
        g_firstCompletion = records[0].completion_sequence;
    }
    g_lastCompletion = records[count - 1].completion_sequence;
    g_completionEvents++;
    g_completions += count;
    // Lost notifications leave the journal available for recovery.
    return g_completionFails ? -1 : 0;
}

int
ctt_netadapter_event_drain_end_single(
    _In_ gracht_server_t*                      server,
    _In_ gracht_conn_t                         client,
    _In_ const struct ctt_netadapter_session*  session,
    _In_ uint64_t                             request,
    _In_ oserr_t                              status,
    _In_ uint64_t                             after,
    _In_ uint64_t                             through,
    _In_ uint32_t                             count,
    _In_ uint64_t                             highest,
    _In_ const struct ctt_netadapter_progress* progress)
{
    assert(through == after + count);
    assert(highest == progress->highest_completion_sequence);
    g_drainEvents++;
    g_drain.Status = status;
    g_drain.Count = count;
    g_drain.Highest = highest;
    return -1;
}

int
ctt_netadapter_event_fault_single(
    _In_ gracht_server_t* server,
    _In_ gracht_conn_t client,
    _In_ const struct ctt_netadapter_session* session,
    _In_ const struct ctt_netadapter_fault* fault)
{
    g_faults++;
    return 0;
}

int
ctt_netadapter_event_link_changed_single(
    _In_ gracht_server_t* server,
    _In_ gracht_conn_t client,
    _In_ const struct ctt_netadapter_session* session,
    _In_ const struct ctt_netadapter_link* link)
{
    g_linkEvents++;
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
    assert(device->Common->Session.Outstanding[
        slot->Pool->Description.direction == CTT_NETADAPTER_DIRECTION_TX ? 0 : 1] != 0);
    assert(EthernetPacketIsReceive(slot) ==
        (slot->Pool->Description.direction == CTT_NETADAPTER_DIRECTION_RX));
    assert(EthernetPacketLength(slot) == slot->Packet.length);
    g_submissions++;
    return OS_EOK;
}

static oserr_t
__Stop(
    _In_ EthernetDevice_t* device)
{
    g_stops++;
    return g_stopFails ? OS_ETIMEOUT : OS_EOK;
}

static oserr_t
__Prepare(
    _In_ EthernetDevice_t* device)
{
    g_prepares++;
    return g_prepareResult;
}

static oserr_t
__Start(
    _In_ EthernetDevice_t* device)
{
    assert(device->Common->Session.State == ETHERNET_PREPARED);
    g_starts++;
    return g_startResult;
}

static void
__Poll(
    _In_ EthernetDevice_t* device)
{
    g_polls++;
}

static oserr_t
__ReadLink(
    _In_  EthernetDevice_t* device,
    _Out_ struct PhyLink*   link)
{
    // Even a failed callback may have partially changed its output.
    *link = g_link;
    return g_linkStatus;
}

static void
__TestTransitions(void)
{
    EthernetDevice_t device = {0};
    struct EthernetRegistrationConfig configuration = g_configuration;
    struct gracht_message message = { .client = 17 };
    struct ctt_netadapter_session identity;
    struct ctt_netadapter_session closedIdentity;
    struct ctt_netadapter_ack ack = {0};
    struct ctt_netadapter_packet packets[4] = {0};
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 2};
    const uint32_t filter = CTT_NETADAPTER_RX_FILTER_UNICAST | CTT_NETADAPTER_RX_FILTER_BROADCAST;
    const struct EthernetOperations operations = {
        .Submit = __Submit, .Stop = __Stop, .Prepare = __Prepare,
        .Start = __Start, .Poll = __Poll, .ReadLink = __ReadLink
    };
    unsigned prepares;
    uint32_t i;

    device.Operations = &operations;
    assert(EthernetRegister(&device, 10, mac, &configuration) == OS_EOK);
    ctt_netadapter_open_invocation(&message, 11, 0, 0);
    assert(g_responseStatus == OS_ENOENT);
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EPERMISSIONS && !device.Common->Session.Active);
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = 42;
    ctt_netadapter_open_invocation(&message, 10, 1, 0);
    assert(g_responseStatus == OS_ENOENT);
    ctt_netadapter_open_invocation(&message, 10, 0, 1);
    assert(g_responseStatus == OS_ENOTSUPPORTED && g_registrations == 0);

    device.Common->Link.sequence = 9;
    g_registrationFails = 1;
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EOOM && !device.Common->Session.Active);
    assert(device.Common->Link.sequence == 9 && g_identity.id == 0 && g_openLink.sequence == 0);
    assert(device.Common->ClosedCapacity > 0 && device.Common->ClosedCount == 0);
    g_registrationFails = 0;
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EOK && device.Common->Session.State == ETHERNET_OPENED);
    assert(g_identity.id == 1 && g_identity.generation == 12 && g_openLink.sequence == 1);
    identity = g_identity;
    device.Common->Link.sequence = 4;
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EOK && g_identity.id == identity.id && g_openLink.sequence == 4);
    message.security.principal++;
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EBUSY);
    ctt_netadapter_configure_invocation(&message, &identity, ETHERNET_MTU, filter);
    assert(g_responseStatus == OS_ENOENT && !device.Common->Session.Configured);
    message.security.principal--;

    ctt_netadapter_configure_invocation(&message, &identity, ETHERNET_MTU + 1, filter);
    assert(g_responseStatus == OS_ENOTSUPPORTED && !device.Common->Session.Configured);
    ctt_netadapter_configure_invocation(&message, &identity, ETHERNET_MTU, filter);
    assert(g_responseStatus == OS_EOK && device.Common->Session.Configured);
    assert(EthernetPrepare(&device, 0) == OS_ENOENT);
    assert(EthernetPrepare(&device, 2) == OS_ENOENT);
    assert(EthernetPrepare(&device, 1) == OS_EINVALPARAMS);

    device.Common->Session.PoolCount = 2;
    device.Common->Session.Pools[0].Mapped = true;
    device.Common->Session.Pools[0].Description.direction = CTT_NETADAPTER_DIRECTION_TX;
    device.Common->Session.Pools[1].Mapped = true;
    device.Common->Session.Pools[1].Description.direction = CTT_NETADAPTER_DIRECTION_RX;
    device.Common->Session.Pools[1].Description.slot_count = 4;
    device.Common->Session.Pools[1].Description.slot_size = 2048;
    for (i = 0; i < 4; ++i) {
        device.Common->Session.Pools[1].Slots[i].Pool = &device.Common->Session.Pools[1];
        packets[i].id = (struct ctt_netadapter_packet_id){0, 2, i, 1};
        packets[i].length = ETHERNET_FRAME_SIZE;
    }

    prepares = g_prepares;
    ctt_netadapter_prepare_run_invocation(&message, &identity, 1);
    assert(g_prepareStatus == OS_EOK && g_prepares == prepares + 1);
    assert(EthernetPrepare(&device, 1) == OS_EOK && g_prepares == prepares + 1);
    assert(EthernetConfigure(&device, ETHERNET_MTU, filter) == OS_EBUSY);
    assert(EthernetStart(&device, 2, 0) == OS_ENOENT);
    assert(EthernetStart(&device, 1, 0) == OS_EBUSY && g_starts == 0);
    ctt_netadapter_post_rx_batch_invocation(&message, &identity, 1, 1, &ack, packets, 3);
    assert(g_batchStatus == OS_EOK && device.Common->Session.Outstanding[1] == 3);
    assert(EthernetStart(&device, 1, 1) == OS_EBUSY && g_starts == 0);
    ctt_netadapter_post_rx_batch_invocation(&message, &identity, 1, 2, &ack, &packets[3], 1);
    assert(device.Common->Session.Outstanding[1] == 4);
    assert(EthernetStart(&device, 1, 1) == OS_EBUSY && g_starts == 0);
    ctt_netadapter_start_run_invocation(&message, &identity, 1, 2);
    assert(g_responseStatus == OS_EOK && device.Common->Session.State == ETHERNET_RUNNING);
    assert(EthernetStart(&device, 1, 2) == OS_EOK && g_starts == 1);
    assert(EthernetStart(&device, 1, 1) == OS_EINVALPARAMS && g_starts == 1);
    assert(EthernetPrepare(&device, 1) == OS_EOK && g_prepares == prepares + 1);

    g_stopFails = 1;
    assert(EthernetStop(&device, 1, 2) == OS_ETIMEOUT);
    assert(device.Common->Session.State == ETHERNET_STOPPING && device.Common->Session.Outstanding[1] == 4);
    assert(EthernetStop(&device, 1, 1) == OS_EINVALPARAMS);
    g_stopFails = 0;
    assert(EthernetStop(&device, 1, 2) == OS_EOK && device.Common->Session.StopBarrier == 4);
    assert(!EthernetIsIdle(&device) && EthernetPrepare(&device, 2) == OS_EBUSY);
    ack.through_completion_sequence = 4;
    assert(EthernetAcknowledge(&device, &ack) == OS_EOK && EthernetIsIdle(&device));
    assert(EthernetPrepare(&device, 2) == OS_EBUSY);
    ack.through_batch_id = 2;
    assert(EthernetAcknowledge(&device, &ack) == OS_EOK);

    g_prepareResult = OS_EDEVFAULT;
    assert(EthernetPrepare(&device, 2) == OS_EDEVFAULT);
    assert(device.Common->QueuesPrepared && device.Common->Session.Faulted && device.Common->Session.Run == 1);
    g_prepareResult = OS_EOK;
    assert(EthernetPrepare(&device, 2) == OS_EBUSY);
    g_stopFails = 1;
    ctt_netadapter_close_invocation(&message, &identity);
    assert(g_responseStatus == OS_ETIMEOUT && device.Common->Session.Active && device.Common->ClosedCount == 0);
    g_stopFails = 0;
    g_releaseFails = 1;
    ctt_netadapter_close_invocation(&message, &identity);
    assert(g_responseStatus == OS_EBUSY && device.Common->Session.Active && device.Common->ClosedCount == 0);
    g_releaseFails = 0;
    ctt_netadapter_close_invocation(&message, &identity);
    assert(g_responseStatus == OS_EOK && !device.Common->Session.Active && device.Common->ClosedCount == 1);
    ctt_netadapter_close_invocation(&message, &identity);
    assert(g_responseStatus == OS_EOK && device.Common->ClosedCount == 1);
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_ENOENT && !device.Common->Session.Active);

    closedIdentity = identity;
    message.client++;
    ctt_netadapter_open_invocation(&message, 10, 0, 0);
    assert(g_responseStatus == OS_EOK && g_identity.id == identity.id + 1);
    identity = g_identity;
    message.client--;
    ctt_netadapter_close_invocation(&message, &closedIdentity);
    assert(g_responseStatus == OS_EOK && device.Common->Session.Identity.id == identity.id);
    message.client++;
    ctt_netadapter_close_invocation(&message, &identity);
    assert(g_responseStatus == OS_EOK && !device.Common->Session.Active);
    EthernetUnregister(&device);
    assert(EthernetCleanup(&device) == OS_EOK);

    // A failed start keeps the prepared run available only for stop recovery.
    device = (EthernetDevice_t){0};
    device.Operations = &operations;
    // Exercise the start callback failure independently of RX admission.
    configuration.MinRxSlots = 0;
    assert(EthernetRegister(&device, 10, mac, &configuration) == OS_EOK);
    device.Common->Session.Active = true;
    device.Common->Session.State = ETHERNET_PREPARED;
    device.Common->Session.Run = 1;
    g_startResult = OS_EDEVFAULT;
    assert(EthernetStart(&device, 1, 0) == OS_EDEVFAULT);
    assert(device.Common->Session.Faulted && device.Common->Session.State == ETHERNET_PREPARED);
    assert(EthernetStart(&device, 1, 0) == OS_EBUSY);
    g_startResult = OS_EOK;
    assert(EthernetCleanup(&device) == OS_EOK);
}

static void
__TestRegistration(void)
{
    const struct EthernetOperations operations = { .Start = __Start, .Stop = __Stop };
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 5};
    const uint32_t minimums[] = {0, 1, 8, ETHERNET_SLOTS};
    struct EthernetRegistrationConfig configuration;
    EthernetDevice_t device = { .Operations = &operations };
    struct gracht_message message = {0};
    unsigned starts;
    size_t i;

    // Reject an impossible requirement before allocation or publication.
    g_callocFails = 1;
    configuration.MinRxSlots = ETHERNET_SLOTS + 1;
    assert(EthernetRegister(&device, 13, mac, &configuration) == OS_EINVALPARAMS);
    assert(device.Common == NULL && EthernetFindDevice(13) == NULL);
    configuration.MinRxSlots = UINT32_MAX;
    assert(EthernetRegister(&device, 13, mac, &configuration) == OS_EINVALPARAMS);
    assert(device.Common == NULL && EthernetFindDevice(13) == NULL);
    assert(EthernetCleanup(&device) == OS_EOK);
    g_callocFails = 0;

    for (i = 0; i < sizeof(minimums) / sizeof(minimums[0]); ++i) {
        configuration.MinRxSlots = minimums[i];
        assert(EthernetRegister(&device, 13, mac, &configuration) == OS_EOK);
        assert(EthernetFindDevice(13) == &device);

        // Registration copies the requirement; caller storage can be reused.
        configuration.MinRxSlots = UINT32_MAX;
        ctt_netadapter_get_info_invocation(&message, 13, 0);
        assert(g_responseStatus == OS_EOK && g_info.min_rx_slots == minimums[i]);
        assert(g_info.max_slots_per_pool == ETHERNET_SLOTS);
        assert(g_info.max_outstanding_rx == ETHERNET_SLOTS);
        assert(g_info.max_batch_size == ETHERNET_BATCH);
        assert(g_info.max_pending_batches == ETHERNET_WINDOW);
        assert(g_info.max_unacked_completions == ETHERNET_JOURNAL);

        device.Common->Session.Active = true;
        device.Common->Session.State = ETHERNET_PREPARED;
        device.Common->Session.Run = 1;
        starts = g_starts;
        if (minimums[i] != 0) {
            device.Common->Session.Outstanding[1] = minimums[i] - 1;
            assert(EthernetStart(&device, 1, 0) == OS_EBUSY);
            assert(g_starts == starts && device.Common->Session.State == ETHERNET_PREPARED);
        }
        device.Common->Session.Outstanding[1] = minimums[i];
        assert(EthernetStart(&device, 1, 0) == OS_EOK);
        assert(g_starts == starts + 1 && device.Common->Session.State == ETHERNET_RUNNING);
        assert(EthernetCleanup(&device) == OS_EOK);
        assert(device.Common == NULL && EthernetFindDevice(13) == NULL);
    }
}

static void
__TestCleanup(void)
{
    const struct EthernetOperations operations = {
        .Submit = __Submit,
        .Stop = __Stop,
        .Prepare = __Prepare
    };
    EthernetDevice_t device = { .Operations = &operations };
    struct EthernetAdapterState* common;
    EthernetPool_t* pool;
    EthernetPool_t* incomplete;
    EthernetClosedSession_t* closed;
    struct gracht_message message = { .client = 37 };
    struct ctt_netadapter_packet packet = {
        .id = {0, 1, 0, 1}, .length = 60, .data_offset = 7
    };
    struct ctt_netadapter_ack ack = {0};
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 4};
    unsigned stops = g_stops;

    // Teardown must also work before common registration can allocate state.
    assert(EthernetCleanup(&device) == OS_EOK && g_stops == stops);
    g_stopFails = 1;
    assert(EthernetQueuesReset(&device) == OS_ETIMEOUT);
    g_stopFails = 0;
    assert(EthernetQueuesReset(&device) == OS_EOK);
    g_callocFails = 1;
    assert(EthernetRegister(&device, 12, mac, &g_configuration) == OS_EOOM);
    assert(device.Common == NULL && EthernetFindDevice(12) == NULL);
    assert(EthernetCleanup(&device) == OS_EOK);
    g_callocFails = 0;
    assert(EthernetRegister(&device, 12, mac, &g_configuration) == OS_EOK);

    // Keep close history from an earlier session while a new one has packets.
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = 42;
    assert(EthernetOpen(&device, &message, 0) == OS_EOK);
    assert(EthernetClose(&device) == OS_EOK);
    message.client++;
    assert(EthernetOpen(&device, &message, 0) == OS_EOK);
    common = device.Common;
    closed = common->Closed;
    g_trackedCommon = common;
    g_trackedClosed = closed;
    assert(common->ClosedCount == 1);
    pool = &common->Session.Pools[0];
    pool->Memory.Payload = calloc(1, 2048);
    pool->AliasGeometry.Entries = malloc(sizeof(SHMSG_t));
    assert(pool->Memory.Payload && pool->AliasGeometry.Entries);
    pool->Mapped = true;
    pool->Description.direction = CTT_NETADAPTER_DIRECTION_TX;
    pool->Description.slot_size = 2048;
    pool->Description.slot_count = 1;
    pool->Slots[0].Pool = pool;
    common->Session.PoolCount = 1;
    common->Session.State = ETHERNET_RUNNING;
    common->Session.Run = 1;
    common->Link.status = CTT_NETADAPTER_LINK_STATUS_UP;
    assert(EthernetQueuesPrepare(&device) == OS_EOK);
    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_admission == OS_EOK && pool->Slots[0].Owned);
    assert(EthernetPacketLength(&pool->Slots[0]) == 60);
    assert(EthernetPacketBytes(&pool->Slots[0]) == (uint8_t*)pool->Memory.Payload + 7);
    assert(!EthernetPacketIsReceive(&pool->Slots[0]));
    EthernetReportReceiveDrop(&device, EthernetReceiveFiltered);
    EthernetReportReceiveDrop(&device, EthernetReceiveNoBuffer);
    EthernetReportReceiveDrop(&device, EthernetReceiveError);
    assert(common->Session.Counters.rx_dropped == 1);
    assert(common->Session.Counters.rx_no_buffer == 1);
    assert(common->Session.Counters.rx_errors == 1);

    // Failed registration can leave a handle and geometry outside PoolCount.
    incomplete = &common->Session.Pools[1];
    incomplete->Memory.Payload = calloc(1, 2048);
    incomplete->AliasGeometry.Entries = malloc(sizeof(SHMSG_t));
    assert(incomplete->Memory.Payload && incomplete->AliasGeometry.Entries);
    assert(!incomplete->Mapped);
    stops = g_stops;
    g_stopFails = 1;
    assert(EthernetCleanup(&device) == OS_ETIMEOUT);
    assert(EthernetFindDevice(12) == NULL && device.Common == common);
    assert(!common->Registered && common->QueuesPrepared);
    assert(pool->Mapped && pool->Slots[0].Owned && g_releasedPools == 0);
    assert(common->Closed == closed && common->ClosedCount == 1);
    assert(common->Session.Progress.highest_completion_sequence == 0);
    assert(g_commonFrees == 0 && g_closedFrees == 0);

    // Successful stop cancels once; a later SHM error keeps remaining state.
    g_stopFails = 0;
    g_releaseFailureHandle = &incomplete->Memory;
    assert(EthernetCleanup(&device) == OS_EBUSY);
    assert(device.Common == common && !common->QueuesPrepared);
    assert(!pool->Slots[0].Owned && !pool->Memory.Payload && !pool->AliasGeometry.Entries);
    assert(incomplete->Memory.Payload && incomplete->AliasGeometry.Entries);
    assert(common->Session.Outstanding[0] == 0 && g_releasedPools == 1);
    assert(common->Session.Progress.highest_completion_sequence == 1);
    assert(common->Session.Journal[0].status == CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED);
    assert(common->Closed == closed && common->ClosedCount == 1);
    assert(EthernetCleanup(&device) == OS_EBUSY);
    assert(g_stops == stops + 2 && g_releasedPools == 1);
    assert(common->Session.Progress.highest_completion_sequence == 1);
    assert(g_commonFrees == 0 && g_closedFrees == 0);
    g_releaseFailureHandle = NULL;
    assert(EthernetCleanup(&device) == OS_EOK);
    assert(device.Common == NULL && g_releasedPools == 2);
    assert(EthernetCleanup(&device) == OS_EOK);
    assert(g_stops == stops + 2 && g_releasedPools == 2);
    assert(g_commonFrees == 1 && g_closedFrees == 1);
    g_trackedCommon = NULL;
    g_trackedClosed = NULL;

    // Reusing the handle starts clean. Failed Prepare still requires MAC stop.
    assert(EthernetRegister(&device, 12, mac, &g_configuration) == OS_EOK);
    assert(!device.Common->Session.Active && !device.Common->Closed);
    g_prepareResult = OS_EDEVFAULT;
    assert(EthernetQueuesPrepare(&device) == OS_EDEVFAULT);
    assert(EthernetCleanup(&device) == OS_EOK);
    assert(device.Common == NULL && g_stops == stops + 3);
    g_prepareResult = OS_EOK;
}

static void
__TestJournalSelection(void)
{
    EthernetDevice_t device = {0};
    struct ctt_netadapter_completion records[ETHERNET_BATCH];
    struct ctt_netadapter_ack ack = {0};
    struct gracht_message message = { .client = 27 };
    struct ctt_netadapter_session identity = {50, 12};
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 3};
    const struct EthernetOperations operations = { .Poll = __Poll, .Stop = __Stop };
    uint64_t retired = ETHERNET_JOURNAL - 2;
    uint64_t highest = ETHERNET_JOURNAL + ETHERNET_BATCH + 3;
    uint64_t sequence;
    uint32_t count;
    unsigned polls;

    device.Operations = &operations;
    assert(EthernetRegister(&device, 11, mac, &g_configuration) == OS_EOK);
    device.Common->QueuesPrepared = true;
    device.Common->Session.Active = true;
    device.Common->Session.Owner = message.client;
    device.Common->Session.Principal = 42;
    device.Common->Session.Identity = identity;
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = 42;
    device.Common->Session.Progress.retired_completion_sequence = retired;
    device.Common->Session.Progress.highest_completion_sequence = highest;
    for (sequence = retired + 1; sequence <= highest; ++sequence) {
        device.Common->Session.Journal[(sequence - 1) % ETHERNET_JOURNAL].completion_sequence = sequence;
    }

    assert(EthernetReadCompletions(&device, retired, ETHERNET_BATCH, records, &count) == OS_EOK);
    assert(count == ETHERNET_BATCH && records[0].completion_sequence == retired + 1);
    assert(records[count - 1].completion_sequence == retired + count);
    assert(EthernetReadCompletions(&device, highest, 1, records, &count) == OS_EOK && count == 0);
    assert(EthernetReadCompletions(&device, retired - 1, 1, records, &count) == OS_ENOENT && count == 0);
    assert(EthernetReadCompletions(&device, highest + 1, 1, records, &count) == OS_EINVALPARAMS && count == 0);
    assert(EthernetReadCompletions(&device, retired, 0, records, &count) == OS_EINVALPARAMS && count == 0);
    assert(EthernetReadCompletions(&device, retired, ETHERNET_BATCH + 1, records, &count) == OS_EINVALPARAMS);

    g_completions = 0;
    g_completionEvents = 0;
    EthernetPushCompletions(&device, retired);
    assert(g_completions == ETHERNET_BATCH && g_completionEvents == 1);
    assert(device.Common->Session.Progress.retired_completion_sequence == retired);
    g_completions = 0;
    g_completionEvents = 0;
    g_completionFails = 0;
    EthernetPushCompletions(&device, retired);
    assert(g_completions == highest - retired && g_completionEvents == 2);
    assert(g_firstCompletion == retired + 1 && g_lastCompletion == highest);
    g_completionFails = 1;

    polls = g_polls;
    ctt_netadapter_drain_invocation(&message, &identity, 1, retired, ETHERNET_BATCH, &ack);
    assert(g_polls == polls + 1 && g_drainEvents == 1);
    assert(g_drain.Status == OS_EOK && g_drain.Count == ETHERNET_BATCH && g_drain.Highest == highest);
    assert(device.Common->Session.Progress.retired_completion_sequence == retired);
    ack.through_completion_sequence = highest;
    ctt_netadapter_drain_invocation(&message, &identity, 1, retired, ETHERNET_BATCH, &ack);
    assert(g_polls == polls + 1 && g_drainEvents == 1);
    assert(device.Common->Session.Progress.retired_completion_sequence == retired);
    ctt_netadapter_drain_invocation(&message, &identity, 2, retired, ETHERNET_BATCH, &ack);
    assert(g_drainEvents == 2 && g_drain.Status == OS_ENOENT && g_drain.Count == 0);
    assert(device.Common->Session.Progress.retired_completion_sequence == highest);
    ack.through_completion_sequence++;
    ctt_netadapter_drain_invocation(&message, &identity, 3, highest, ETHERNET_BATCH, &ack);
    assert(g_drainEvents == 3 && g_drain.Status == OS_EINVALPARAMS && g_drain.Count == 0);
    assert(device.Common->Session.Progress.retired_completion_sequence == highest);
    assert(EthernetCleanup(&device) == OS_EOK);
}

int
main(void)
{
    EthernetDevice_t device = {0};
    EthernetPool_t* pool;
    struct ctt_netadapter_packet packet = { .id = {0, 1, 0, 1}, .length = 60 };
    struct ctt_netadapter_ack ack = {0};
    struct ctt_netadapter_session identity = {1, 2};
    struct gracht_message message = {0};
    struct ctt_netadapter_link savedLink;
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 1};
    const struct EthernetOperations operations = {
        .Submit = __Submit, .Stop = __Stop, .Prepare = __Prepare,
        .ReadLink = __ReadLink
    };

    device.Operations = &operations;
    assert(EthernetRegister(&device, 9, mac, &g_configuration) == OS_EOK);
    pool = &device.Common->Session.Pools[0];
    device.Common->Link.status = CTT_NETADAPTER_LINK_STATUS_UP;
    device.Common->QueuesPrepared = true;
    device.Common->Session.Active = true;
    device.Common->Session.Configured = true;
    device.Common->Session.Owner = 7;
    device.Common->Session.Principal = 42;
    device.Common->Session.Identity = identity;
    device.Common->Session.Run = 1;
    device.Common->Session.State = ETHERNET_RUNNING;
    device.Common->Session.PoolCount = 1;
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
    assert(device.Common->Session.Progress.retired_batch_id == 0);
    ack = (struct ctt_netadapter_ack){0};
    EthernetComplete(&device, &pool->Slots[0], OS_EOK, 60);
    EthernetComplete(&device, &pool->Slots[0], OS_EOK, 60);
    assert(device.Common->Session.Progress.highest_completion_sequence == 1);
    assert(device.Common->Session.Counters.tx_packets == 1);
    assert(device.Common->Session.Journal[0].length == 60);
    EthernetSubmit(&device, 1, 3, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_EBUSY && device.Common->Session.Progress.consumed_batch_id == 1);

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
    assert(device.Common->Session.State == ETHERNET_STOPPING && pool->Slots[0].Owned);
    assert(EthernetStop(&device, 1, 2) == OS_EINVALPARAMS);
    g_stopFails = 0;
    assert(EthernetStop(&device, 1, 3) == OS_EOK);
    assert(device.Common->Session.StopBarrier == 2 && !pool->Slots[0].Owned);
    assert(device.Common->Session.Journal[1].status == CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED);
    assert(device.Common->Session.Journal[1].length == 0 && g_completions == 1);
    assert(EthernetStop(&device, 1, 3) == OS_EOK && g_completions == 1);
    assert(device.Common->Session.Progress.retired_completion_sequence == 0);
    ctt_netadapter_prepare_run_invocation(&message, &identity, 2);
    assert(g_prepareStatus == OS_EBUSY);
    ack = (struct ctt_netadapter_ack){3, 2};
    assert(EthernetAcknowledge(&device, &ack) == OS_EOK);
    EthernetSubmit(&device, 1, 1, &ack, &packet, 1, CTT_NETADAPTER_DIRECTION_TX);
    assert(g_batchStatus == OS_ENOENT && g_submissions == 2);
    device.Common->Session.Pools[1].Mapped = true;
    device.Common->Session.Pools[1].Description.direction = CTT_NETADAPTER_DIRECTION_RX;
    device.Common->Session.PoolCount = 2;
    ctt_netadapter_prepare_run_invocation(&message, &identity, 2);
    assert(g_prepareStatus == OS_EOK && device.Common->Session.State == ETHERNET_PREPARED);
    assert(EthernetStop(&device, 1, 3) == OS_ENOENT);

    g_link = (struct PhyLink){ .Speed = 1000, .FullDuplex = 1, .Carrier = 1 };
    assert(EthernetReadConfiguration(&device) == OS_EOK);
    assert(device.Common->Link.speed_bps == 1000000000 && g_linkEvents == 1);
    savedLink = device.Common->Link;
    g_link = (struct PhyLink){0};
    g_linkStatus = OS_EDEVFAULT;
    assert(EthernetReadConfiguration(&device) == OS_EDEVFAULT);
    assert(memcmp(&device.Common->Link, &savedLink, sizeof(savedLink)) == 0);
    assert(device.Common->Session.Faulted && g_faults == 1 && g_linkEvents == 1);
    assert(EthernetReadConfiguration(&device) == OS_EDEVFAULT);
    assert(g_faults == 1 && g_linkEvents == 1);

    assert(EthernetCleanup(&device) == OS_EOK);
    assert(EthernetFindDevice(9) == NULL);
    __TestTransitions();
    __TestJournalSelection();
    __TestCleanup();
    __TestRegistration();
    puts("Ethernet session and recovery tests passed");
    return 0;
}
