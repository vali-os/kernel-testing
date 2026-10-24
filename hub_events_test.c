// The client receive callback must not issue any blocking USB control request.
#include "../modules/serial/usb/hub/events.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static usched_task_fn g_worker;
static void*         g_context;
static unsigned int  g_queued;
static unsigned int  g_handled;
static unsigned int  g_recovery;
static unsigned int  g_joins;
static unsigned int  g_scans;
static bool          g_failQueue;
static bool          g_inCallback;
static uint8_t       g_changes[3][32];

void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { }

void
HubEnumeratePorts(
    _In_ HubDevice_t* hub)
{
    CHECK(!g_inCallback);
    g_scans++;
}

uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void*         argument)
{
    g_queued++;
    if (g_failQueue) {
        return UUID_INVALID;
    }
    g_worker = entry;
    g_context = argument;
    return 17;
}

int
usched_job_join(
    _In_ uuid_t job,
    _Out_ int*  exitCode)
{
    CHECK(job == 17 && exitCode != NULL);
    g_joins++;
    g_worker(g_context, NULL);
    *exitCode = 0;
    return 0;
}

enum USBTransferCode
UsbClearFeature(
    _In_ usb_device_context_t* context,
    _In_ uint8_t               target,
    _In_ uint16_t              index,
    _In_ uint16_t              feature)
{
    CHECK(!g_inCallback && g_recovery++ == 0);
    CHECK(target == USBPACKET_DIRECTION_ENDPOINT && index == 1 && feature == USB_FEATURE_HALT);
    return USBTRANSFERCODE_SUCCESS;
}

oserr_t
UsbEndpointReset(
    _In_ usb_device_context_t* context,
    _In_ uint8_t               endpoint)
{
    CHECK(!g_inCallback && g_recovery++ == 1 && endpoint == 1);
    return OS_EOK;
}

oserr_t
UsbTransferResetPeriodic(
    _In_ usb_device_context_t* context,
    _In_ uuid_t                transferId)
{
    CHECK(!g_inCallback && g_recovery++ == 2 && transferId == 23);
    return OS_EOK;
}

void
HubInterrupt(
    _In_ HubDevice_t*   hub,
    _In_ const uint8_t* changes)
{
    CHECK(!g_inCallback && g_handled < 3);
    memcpy(g_changes[g_handled++], changes, 32);
    if (g_handled == 1) {
        // Model the receive loop delivering another bitmap during a control RPC.
        ((uint8_t*)hub->Buffer)[0] = 0x80;
        HubEventsQueue(hub, false, 0);
        CHECK(changes[0] == 0x0a && g_queued == 1);
    }
}

int
main(void)
{
    uint8_t                   buffer[256] = {0};
    UsbDevice_t               device = {0};
    usb_endpoint_descriptor_t endpoint = { .Address = 0x81 };
    HubDevice_t               hub = {0};

    hub.Base = &device;
    hub.Buffer = (uintptr_t*)buffer;
    hub.Interrupt = &endpoint;
    hub.PortCount = 255;
    hub.TransferId = 23;
    hub.Events.Job = UUID_INVALID;
    g_inCallback = true;
    HubEventsStart(&hub);
    CHECK(g_scans == 0);
    buffer[32] = 2;
    buffer[63] = 0x80;
    HubEventsQueue(&hub, false, 32);
    buffer[32] = 8;
    HubEventsQueue(&hub, false, 32);
    HubEventsQueue(&hub, true, 0);
    memset(buffer, 0, sizeof(buffer));
    CHECK(g_queued == 1 && g_handled == 0 && g_recovery == 0);
    g_inCallback = false;
    g_worker(g_context, NULL);
    CHECK(g_scans == 1);
    CHECK(g_handled == 2 && g_recovery == 3 && hub.Events.Job == UUID_INVALID);
    CHECK(g_changes[0][0] == 0x0a && g_changes[0][31] == 0x80);
    CHECK(g_changes[1][0] == 0x80 && g_changes[1][31] == 0);

    // Invalid offsets cannot read beyond the periodic buffer or schedule work.
    HubEventsQueue(&hub, false, 225);
    CHECK(g_queued == 1);
    g_failQueue = true;
    buffer[0] = 4;
    HubEventsQueue(&hub, false, 0);
    CHECK(hub.Events.Job == UUID_INVALID && hub.Events.Changes[0] == 4);
    g_failQueue = false;
    buffer[0] = 2;
    HubEventsQueue(&hub, false, 0);
    g_worker(g_context, NULL);
    CHECK(g_handled == 3 && g_changes[2][0] == 6);

    HubEventsStart(&hub);
    HubEventsStop(&hub);
    CHECK(g_joins == 1 && g_handled == 3 && g_scans == 1 && hub.Events.Job == UUID_INVALID);
    HubEventsQueue(&hub, false, 0);
    CHECK(g_queued == 4);
    puts("USB hub deferred status, bitmap ownership, coalescing and teardown: PASS");
    return 0;
}
