// Exercise production libusb routing with distinct device and driver handles.
#include "../librt/libusb/functions.c"
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static unsigned int g_calls;
static unsigned int g_waits;
static unsigned int g_results;
static int g_disable;
static int g_transportFailure;
static int g_abandons;

int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* context)
{
    (void)context;
    CHECK(client == GetGrachtClient() && g_transportFailure == 2);
    g_abandons++;
    return 0;
}

int ctt_usbhub_disable_port(gracht_client_t* client,
    struct gracht_message_context* context, uuid_t deviceId, uint8_t port)
{
    struct vali_link_message* message = (struct vali_link_message*)context;
    CHECK(client == GetGrachtClient());
    CHECK(message->address.Data.Handle == 91 && deviceId == 24 && port == 3);
    g_calls++;
    return g_transportFailure == 1 ? -1 : 0;
}

int ctt_usbhub_disable_port_result(gracht_client_t* client,
    struct gracht_message_context* context, oserr_t* result)
{
    (void)context;
    CHECK(client == GetGrachtClient() && g_waits == 1);
    if (g_transportFailure == 3) return -1;
    *result = g_disable ? OS_ETIMEOUT : OS_EOK;
    g_results++;
    return 0;
}

int ctt_usbhub_query_port(gracht_client_t* client,
    struct gracht_message_context* context, uuid_t deviceId, uint8_t port)
{ return ctt_usbhub_disable_port(client, context, deviceId, port); }
int ctt_usbhub_reset_port(gracht_client_t* client,
    struct gracht_message_context* context, uuid_t deviceId, uint8_t port)
{ return ctt_usbhub_disable_port(client, context, deviceId, port); }
int ctt_usbhub_query_port_result(gracht_client_t* client,
    struct gracht_message_context* context, oserr_t* status, uint8_t* descriptor, uint32_t* length)
{
    (void)descriptor; (void)length;
    return ctt_usbhub_disable_port_result(client, context, status);
}
int ctt_usbhub_reset_port_result(gracht_client_t* client,
    struct gracht_message_context* context, oserr_t* status, uint8_t* descriptor, uint32_t* length)
{ return ctt_usbhub_query_port_result(client, context, status, descriptor, length); }

gracht_client_t*
GetGrachtClient(void)
{
    return (gracht_client_t*)1;
}

int
ctt_usbhost_device_detach(
    _In_ gracht_client_t*              client,
    _In_ struct gracht_message_context* context,
    _In_ uuid_t                        controllerDeviceId,
    _In_ uint8_t                       hubAddress,
    _In_ uint8_t                       portAddress,
    _In_ uint8_t                       deviceAddress)
{
    struct vali_link_message* message = (struct vali_link_message*)context;

    CHECK(client == GetGrachtClient());
    CHECK(message->address.Type == IPC_ADDRESS_HANDLE);
    CHECK(message->address.Data.Handle == 73);
    CHECK(controllerDeviceId == 10);
    CHECK(hubAddress == 2 && portAddress == 3 && deviceAddress == 7);
    g_calls++;
    return 0;
}

int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                  flags)
{
    (void)context;
    CHECK(client == GetGrachtClient());
    CHECK(flags == GRACHT_AWAIT_ASYNC && g_calls == 1);
    g_waits++;
    return g_transportFailure == 2 ? -1 : 0;
}

int
ctt_usbhost_device_detach_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                      result)
{
    (void)context;
    CHECK(client == GetGrachtClient() && g_waits == 1);
    *result = OS_EOK;
    g_results++;
    return 0;
}

int
main(void)
{
    usb_device_context_t device = { 0 };
    USBPortDescriptor_t port;
    int stage;

    device.controller_device_id = 10;
    device.controller_driver_id = 73;
    device.hub_address = 2;
    device.port_address = 3;
    UsbDetachDevice(10, &device);
    CHECK(g_calls == 0 && g_waits == 0 && g_results == 0);

    device.device_address = 7;
    UsbDetachDevice(10, &device);
    CHECK(g_calls == 1 && g_waits == 1 && g_results == 1);
    for (stage = 1; stage <= 3; stage++) {
        g_transportFailure = stage;
        g_calls = g_waits = g_results = 0;
        CHECK(UsbHubDisablePort(91, 24, 3) == OS_EUNKNOWN);
        g_calls = g_waits = g_results = 0;
        CHECK(UsbHubQueryPort(91, 24, 3, &port) == OS_EUNKNOWN);
        g_calls = g_waits = g_results = 0;
        CHECK(UsbHubResetPort(91, 24, 3, &port) == OS_EUNKNOWN);
    }
    g_transportFailure = 0;
    CHECK(g_abandons == 3);
    g_calls = g_waits = g_results = 0;
    CHECK(UsbHubDisablePort(91, 24, 3) == OS_EOK);
    CHECK(g_calls == 1 && g_waits == 1 && g_results == 1);
    g_calls = g_waits = g_results = 0;
    g_disable = 1;
    CHECK(UsbHubDisablePort(91, 24, 3) == OS_ETIMEOUT);
    CHECK(g_calls == 1 && g_waits == 1 && g_results == 1);
    puts("USB detach: driver IPC destination, controller identity and unaddressed skip passed");
    return 0;
}
