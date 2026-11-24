// Host test for the GENET module entry points in main.c and the poll pass in
// service.c: device registration, I/O set event dispatch, the poll worker and
// the teardown retries.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctt_driver_service_server.h>
#include <ddk/convert.h>
#include <ddk/devices/platform.h>
#include <os/time.h>
#include <os/usched/job.h>
#include <ioset.h>
#include "../../modules/eth/genet/genet.h"

#define INTERRUPT_DESCRIPTOR 99
// Wall clock seconds reported to the poll worker.
// First wait before a failed teardown is tried again. Matches service.h.
#define DESTROY_RETRY_MS 1000U

extern unsigned g_diagnosticLogs;

// Module entry points from main.c. The module runtime declares them.
oserr_t OnLoad(void);
void    OnUnload(void);
oserr_t OnEvent(struct ioset_event* event);

gracht_protocol_t ctt_driver_server_protocol;

static usched_task_fn g_worker;
static unsigned       g_passes;
static uint64_t       g_now;
static uint32_t       g_periods[128];
static unsigned       g_periodCount;
static unsigned       g_kernelSleeps;
static uint32_t       g_kernelSleepMs;
static unsigned       g_stalls;
static int            g_jobSleepFails;
static int            g_locked;
static unsigned       g_creates;
static oserr_t        g_createStatus;
static enum GenetState g_createState;
static GenetController_t* g_controller;
static unsigned       g_destroys;
static unsigned       g_destroyFailures;
static unsigned       g_eventResets;
static unsigned       g_ringPolls;
static unsigned       g_linkReads;
static uint32_t       g_linkInterval = GENET_LINK_POLL_INTERVAL_MS;
static size_t         g_failCallocSize;

void* __real_calloc(size_t count, size_t size);

void*
__wrap_calloc(
    size_t count,
    size_t size)
{
    if (g_failCallocSize != 0 && size == g_failCallocSize) {
        return NULL;
    }
    return __real_calloc(count, size);
}

uuid_t
usched_job_queue(
    usched_task_fn entry,
    void*          argument)
{
    g_worker = entry;
    return 7;
}

int
usched_job_cancel(
    uuid_t jobID)
{
    assert(jobID == 7);
    return 0;
}

int
usched_job_join(
    uuid_t jobID,
    int*   exitCode)
{
    assert(jobID == 7);
    *exitCode = 0;
    return 0;
}

bool
usched_is_cancelled(
    const void* cancellationToken)
{
    if (g_passes == 0) {
        return true;
    }
    g_passes--;
    return false;
}

// Record how long the worker sleeps, and let that much time pass. A sleep
// set up to fail returns at once, as when the UTC clock cannot be read.
int
usched_job_sleep_us(
    uint64_t microseconds)
{
    uint32_t period;

    assert(!g_locked);
    if (g_jobSleepFails) {
        g_jobSleepFails--;
        return -1;
    }
    assert(microseconds % 1000 == 0);
    period = (uint32_t)(microseconds / 1000);
    assert(g_periodCount < sizeof(g_periods) / sizeof(g_periods[0]));
    g_periods[g_periodCount++] = period;
    g_now += period;
    return 0;
}

oserr_t
OSGetTime(
    enum OSTimeSource source,
    OSTimestamp_t*    time)
{
    assert(source == OSTimeSource_UTC);
    time->Seconds = 0;
    time->Nanoseconds = 0;
    return OS_EOK;
}

oserr_t
OSSleep(
    const OSTimestamp_t* deadline,
    OSTimestamp_t*       remaining)
{
    assert(!g_locked);
    g_kernelSleeps++;
    g_kernelSleepMs = (uint32_t)(deadline->Seconds * 1000 + deadline->Nanoseconds / 1000000);
    g_now += g_kernelSleepMs;
    return OS_EOK;
}

oserr_t
DDKStallUs(
    uint32_t microseconds)
{
    g_stalls++;
    return OS_EOK;
}

uint64_t
DDKGetTimeMs(
    enum OSTimeSource source)
{
    assert(source == OSTimeSource_MONOTONIC);
    return g_now;
}

oserr_t
EthernetServiceInitialize(void)
{
    return OS_EOK;
}

gracht_server_t*
EthernetServer(void)
{
    return NULL;
}

int
gracht_server_register_protocol(
    gracht_server_t*   server,
    gracht_protocol_t* protocol)
{
    assert(protocol == &ctt_driver_server_protocol);
    return 0;
}

void
EthernetLock(void)
{
    assert(!g_locked);
    g_locked = 1;
}

void
EthernetUnlock(void)
{
    assert(g_locked);
    g_locked = 0;
}

EthernetDevice_t*
EthernetFindDevice(
    uuid_t deviceId)
{
    return NULL;
}

int
ctt_driver_event_device_protocol_single(
    gracht_server_t*    server,
    const gracht_conn_t client,
    const uuid_t        deviceId,
    const char*         protocolName,
    const uint8_t       protocolId)
{
    return 0;
}

int
ctt_driver_ioctl_response(
    struct gracht_message* message,
    uint8_t*               in,
    uint32_t               inCount,
    oserr_t                result)
{
    return 0;
}

oserr_t
EthernetReadConfiguration(
    EthernetDevice_t* device)
{
    assert(g_locked && device == &g_controller->Adapter);
    g_linkReads++;
    return OS_EOK;
}

uint32_t
GenetLinkPollInterval(
    const GenetController_t* controller)
{
    return g_linkInterval;
}

oserr_t
GenetCreate(
    GenetController_t* controller,
    Device_t*     device)
{
    assert(g_locked);
    g_creates++;
    g_controller = controller;
    controller->Device = device;
    controller->State = g_createState;
    // With a packet interrupt the worker runs at the maintenance interval.
    controller->Interrupt.State = GENET_INTERRUPT_ENABLED;
    return g_createStatus;
}

// Fail the given number of times, then succeed.
oserr_t
GenetDestroy(
    GenetController_t* controller)
{
    assert(g_locked && controller == g_controller);
    g_destroys++;
    if (g_destroyFailures) {
        g_destroyFailures--;
        return OS_ETIMEOUT;
    }
    controller->State = GENET_STATE_DESTROYED;
    return OS_EOK;
}

bool
GenetInterruptIsEvent(
    const GenetController_t* controller,
    int                 descriptor)
{
    return descriptor == INTERRUPT_DESCRIPTOR;
}

// The service code resets the event before it takes the causes.
void
GenetInterruptResetEvent(
    GenetController_t* controller)
{
    assert(g_locked && controller == g_controller);
    g_eventResets++;
}

uint32_t
GenetInterruptTakeCauses(
    GenetController_t* controller)
{
    assert(g_locked && controller == g_controller);
    return 0;
}

// Without recorded causes the service code has nothing to unmask.
oserr_t
GenetMacUnmaskDmaInterrupts(
    GenetController_t* controller,
    uint32_t           causes)
{
    assert(0);
    return OS_EOK;
}

void
EthernetPoll(
    EthernetDevice_t* device)
{
    assert(g_locked && device == &g_controller->Adapter);
    g_ringPolls++;
}

void
EthernetFault(
    EthernetDevice_t* device,
    oserr_t           status)
{
    assert(0);
}

#ifdef VALI_ETHERNET_DIAGNOSTICS
void
GenetLogProgress(
    GenetController_t* controller)
{
}
#endif

static void
__Register(
    uuid_t id)
{
    static char            name[] = "genet";
    static uint8_t         compatible[] = "brcm,bcm2711-genet-v5";
    struct sys_device      description;

    memset(&description, 0, sizeof(description));
    description.content_type = SYS_DEVICE_CONTENT_PLATFORM;
    description.content.platform.id = id;
    description.content.platform.version = PLATFORM_DEVICE_VERSION;
    description.content.platform.identification.description = name;
    description.content.platform.identification.manufacturer = name;
    description.content.platform.identification.product = name;
    description.content.platform.identification.revision = name;
    description.content.platform.identification.serial = name;
    description.content.platform.compatibles_count = sizeof(compatible);
    description.content.platform.compatibles = compatible;
    ctt_driver_register_device_invocation(NULL, &description);
    assert(!g_locked);
}

static void
__Run(
    unsigned passes)
{
    g_periodCount = 0;
    g_passes = passes;
    g_worker(NULL, (void*)1);
    assert(g_periodCount == passes && !g_locked);
}

static void
__Events(void)
{
    struct ioset_event event = { .events = IOSETSYN, .data.iod = INTERRUPT_DESCRIPTOR };

    // Only a synchronization event on the interrupt descriptor is ours. Every
    // other event belongs to the gracht server.
    assert(OnEvent(NULL) == OS_ENOENT);
    event.events = IOSETIN;
    assert(OnEvent(&event) == OS_ENOENT);
    event.events = IOSETSYN;
    event.data.iod = INTERRUPT_DESCRIPTOR + 1;
    assert(OnEvent(&event) == OS_ENOENT);
    assert(g_eventResets == 0 && g_ringPolls == 0);

    // Our event resets the descriptor and empties the rings once.
    event.data.iod = INTERRUPT_DESCRIPTOR;
    assert(OnEvent(&event) == OS_EOK);
    assert(g_eventResets == 1 && g_ringPolls == 1 && !g_locked);
}

static void
__ReadyPolling(void)
{
    // A ready controller reads the link once per link interval and polls the
    // rings every maintenance interval.
    g_linkReads = 0;
    g_ringPolls = 0;
    __Run(10);
    assert(g_linkReads == 1 && g_ringPolls == 10);
    for (unsigned i = 0; i < 10; i++) {
        assert(g_periods[i] == GENET_MAINTENANCE_INTERVAL_MS);
    }
    __Run(1);
    assert(g_linkReads == 2);

    // Once the PHY reads keep failing, the link is read less often.
    g_linkInterval = GENET_LINK_POLL_FAILED_INTERVAL_MS;
    g_linkReads = 0;
    __Run(100);
    assert(g_linkReads == 1);
    __Run(10);
    assert(g_linkReads == 2);
    g_linkInterval = GENET_LINK_POLL_INTERVAL_MS;

    // When the job cannot sleep, the worker blocks the thread instead of
    // spinning.
    g_jobSleepFails = 1;
    g_kernelSleeps = 0;
    g_passes = 1;
    g_periodCount = 0;
    g_worker(NULL, (void*)1);
    assert(g_periodCount == 0 && g_kernelSleeps == 1);
    assert(g_kernelSleepMs == GENET_MAINTENANCE_INTERVAL_MS && g_stalls == 0);
}

static void
__DestroyRetries(void)
{
    const uint32_t expected[] = {
        DESTROY_RETRY_MS, 2 * DESTROY_RETRY_MS, 4 * DESTROY_RETRY_MS, 8 * DESTROY_RETRY_MS,
        16 * DESTROY_RETRY_MS, 30000, 30000
    };
    struct ioset_event event = { .events = IOSETSYN, .data.iod = INTERRUPT_DESCRIPTOR };
    unsigned logs;

    // A controller that left GENET_STATE_READY is torn down by the worker. Each
    // failure is logged, and the wait before the next attempt doubles up to
    // the limit.
    g_controller->State = GENET_STATE_STOPPING;
    g_destroys = 0;
    g_destroyFailures = 7;
    logs = g_diagnosticLogs;
    __Run(7);
    assert(g_destroys == 7 && g_diagnosticLogs == logs + 7);
    for (unsigned i = 0; i < 7; i++) {
        assert(g_periods[i] == expected[i]);
    }

    // Until the teardown finishes, the controller still has its interrupt
    // descriptor, so its events are still handled.
    assert(OnEvent(&event) == OS_EOK);

    // The next attempt succeeds. The worker goes back to the short period
    // and logs that the teardown finished.
    logs = g_diagnosticLogs;
    __Run(2);
    assert(g_destroys == 8 && g_diagnosticLogs == logs + 1);
    assert(g_periods[0] == GENET_POLL_INTERVAL_MS && g_periods[1] == GENET_POLL_INTERVAL_MS);
    assert(OnEvent(&event) == OS_ENOENT);

    // A failed create is cleaned up right away. If that fails, the worker
    // waits the retry interval before the next attempt, and the wait starts
    // short again.
    g_createState = GENET_STATE_MAPPED;
    g_createStatus = OS_EDEVFAULT;
    g_destroyFailures = 2;
    g_destroys = 0;
    __Register(43);
    assert(g_destroys == 1);
    __Run(1);
    assert(g_destroys == 1 && g_periods[0] == DESTROY_RETRY_MS);
    __Run(2);
    assert(g_destroys == 3 && g_periods[0] == 2 * DESTROY_RETRY_MS);
    assert(g_periods[1] == GENET_POLL_INTERVAL_MS);
    g_createStatus = OS_EOK;
    g_createState = GENET_STATE_READY;
}

static void
__Registration(void)
{
    unsigned logs;

    // Registration creates the controller.
    g_createState = GENET_STATE_READY;
    __Register(42);
    assert(g_creates == 1 && g_controller->Device->Id == 42);

    // A second device is refused and logged.
    logs = g_diagnosticLogs;
    __Register(43);
    assert(g_creates == 1 && g_diagnosticLogs == logs + 1);
}

static void
__AllocationFailure(void)
{
    struct ioset_event event = { .events = IOSETSYN, .data.iod = INTERRUPT_DESCRIPTOR };
    unsigned logs;
    unsigned creates = g_creates;

    // A failed controller allocation is logged and leaves no controller.
    g_failCallocSize = sizeof(GenetController_t);
    logs = g_diagnosticLogs;
    __Register(44);
    g_failCallocSize = 0;
    assert(g_diagnosticLogs == logs + 1 && g_creates == creates);
    assert(OnEvent(&event) == OS_ENOENT);
}

static void
__Unload(void)
{
    unsigned logs;

    // Unloading waits for the teardown. It sleeps a second between attempts
    // instead of busy-waiting, logs when it starts waiting, then every ten
    // attempts, and once more when it is done.
    __Register(45);
    g_controller->State = GENET_STATE_STOPPING;
    g_destroys = 0;
    g_destroyFailures = 25;
    g_kernelSleeps = 0;
    g_stalls = 0;
    logs = g_diagnosticLogs;
    OnUnload();
    assert(g_destroys == 26 && g_kernelSleeps == 25 && g_stalls == 0);
    assert(g_kernelSleepMs == 1000);
    assert(g_diagnosticLogs == logs + 4);
}

int
main(void)
{
    assert(OnLoad() == OS_EOK && g_worker != NULL);
    __Registration();
    __Events();
    __ReadyPolling();
    __DestroyRetries();
    __AllocationFailure();
    __Unload();
    puts("GENET module entry tests passed");
    return 0;
}
