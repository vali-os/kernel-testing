// The real DDK wrapper must report conversion and transport errors to its caller.
#include "../librt/libddk/usb.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int g_sendStatus;
static unsigned int g_sends;

uuid_t
GetUsbService(void)
{
    return 10;
}
uuid_t
GetNativeHandle(
    _In_ int fd)
{
    return 20;
}
int
__crt_get_server_iod(void)
{
    return 3;
}
gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

int
sys_usb_register_controller(
    _In_ gracht_client_t* client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t driverId,
    _In_ const struct sys_device* device,
    _In_ const int kind,
    _In_ const int ports)
{
    CHECK(driverId == 20 && ports == 4);
    CHECK(kind == USBCONTROLLER_KIND_XHCI);
    g_sends++;
    return g_sendStatus;
}

static int g_hubStep;
static int g_hubFailure;
static oserr_t g_hubStatus;

int
sys_usb_register_hub(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const uuid_t parent,
    const uuid_t device,
    const uuid_t driver,
    const int ports,
    const uint16_t characteristics)
{
    CHECK(g_hubStep++ == 0);
    CHECK(device == 7 && driver == 20 && ports == 4);
    return g_hubFailure == 1 ? -1 : 0;
}

int
gracht_client_await(
    gracht_client_t* client,
    struct gracht_message_context* context,
    unsigned int flags)
{
    CHECK(g_hubStep++ == 1 && flags == GRACHT_AWAIT_ASYNC);
    return g_hubFailure == 2 ? -1 : 0;
}

int
sys_usb_register_hub_result(
    gracht_client_t* client,
    struct gracht_message_context* context,
    oserr_t* status)
{
    CHECK(g_hubStep++ == 2);
    *status = g_hubStatus;
    return g_hubFailure == 3 ? -1 : 0;
}

static void
__TestHubRegistration(void)
{
    UsbDevice_t device = { .Base.Id = 7 };
    int i;

    for (i = 0; i <= 3; i++) {
        g_hubStep = 0;
        g_hubFailure = i;
        CHECK(UsbHubRegister(&device, 4, 0) == (i ? OS_EUNKNOWN : OS_EOK));
        CHECK(g_hubStep == (i ? i : 3));
    }
    g_hubStep = 0;
    g_hubFailure = 0;
    g_hubStatus = OS_ETIMEOUT;
    CHECK(UsbHubRegister(&device, 4, 0) == OS_ETIMEOUT);
    CHECK(g_hubStep == 3);
}

int
main(void)
{
    Device_t device = { .Id = 7, .Length = sizeof(Device_t) };

    CHECK(UsbControllerRegister(&device, USBCONTROLLER_KIND_XHCI, 4) == OS_EOK);
    CHECK(g_sends == 1);
    g_sendStatus = -1;
    CHECK(UsbControllerRegister(&device, USBCONTROLLER_KIND_XHCI, 4) == OS_EUNKNOWN);
    CHECK(g_sends == 2);
    device.Length = 0;
    CHECK(UsbControllerRegister(&device, USBCONTROLLER_KIND_XHCI, 4) == OS_EINVALPARAMS);
    CHECK(g_sends == 2);
    __TestHubRegistration();
    puts("USB registration: successful send, transport failure and conversion failure passed");
    return 0;
}
