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
    return 0;
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

    device.controller_device_id = 10;
    device.controller_driver_id = 73;
    device.hub_address = 2;
    device.port_address = 3;
    UsbDetachDevice(10, &device);
    CHECK(g_calls == 0 && g_waits == 0 && g_results == 0);

    device.device_address = 7;
    UsbDetachDevice(10, &device);
    CHECK(g_calls == 1 && g_waits == 1 && g_results == 1);
    puts("USB detach: driver IPC destination, controller identity and unaddressed skip passed");
    return 0;
}
