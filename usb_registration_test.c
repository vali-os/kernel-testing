// The real DDK wrappers must report conversion and transport errors to their
// caller. A request with an answer is sent, waited for and decoded in three
// steps, and a failed step must end the request without running the next one.
#include "../librt/libddk/services/usb.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int g_sendStatus;
static unsigned int g_sends;
static int g_errors;

uuid_t
ServiceGetUsb(void)
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

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
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

// g_step counts the steps of the running request. g_failure selects the step
// that fails: 1 is the send, 2 the wait and 3 the decode. 0 lets all succeed.
static int g_step;
static int g_failure;
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
    CHECK(g_step++ == 0);
    CHECK(device == 7 && driver == 20 && ports == 4);
    return g_failure == 1 ? -1 : 0;
}

int
gracht_client_await(
    gracht_client_t* client,
    struct gracht_message_context* context,
    unsigned int flags)
{
    CHECK(g_step++ == 1 && flags == GRACHT_AWAIT_ASYNC);
    return g_failure == 2 ? -1 : 0;
}

int
sys_usb_register_hub_result(
    gracht_client_t* client,
    struct gracht_message_context* context,
    oserr_t* status)
{
    CHECK(g_step++ == 2);
    *status = g_hubStatus;
    return g_failure == 3 ? -1 : 0;
}

static void
__TestHubRegistration(void)
{
    UsbDevice_t device = { .Base.Id = 7 };
    int i;

    for (i = 0; i <= 3; i++) {
        g_step = 0;
        g_failure = i;
        CHECK(UsbHubRegister(&device, 4, 0) == (i ? OS_EUNKNOWN : OS_EOK));
        CHECK(g_step == (i ? i : 3));
    }
    g_step = 0;
    g_failure = 0;
    g_hubStatus = OS_ETIMEOUT;
    CHECK(UsbHubRegister(&device, 4, 0) == OS_ETIMEOUT);
    CHECK(g_step == 3);
}

// Messages without an answer. Only the send can fail.
int
sys_usb_unregister_hub(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const uuid_t deviceId)
{
    CHECK(g_step++ == 0 && deviceId == 7);
    return g_failure == 1 ? -1 : 0;
}

int
sys_usb_port_event(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const uuid_t hubDeviceId,
    const uint8_t portAddress)
{
    CHECK(g_step++ == 0 && hubDeviceId == 7 && portAddress == 3);
    return g_failure == 1 ? -1 : 0;
}

int
sys_usb_port_error(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const uuid_t hubDeviceId,
    const uint8_t portAddress)
{
    CHECK(g_step++ == 0 && hubDeviceId == 7 && portAddress == 3);
    return g_failure == 1 ? -1 : 0;
}

int
sys_usb_get_controller_count(
    gracht_client_t* client,
    struct gracht_message_context* context)
{
    CHECK(g_step++ == 0);
    return g_failure == 1 ? -1 : 0;
}

int
sys_usb_get_controller_count_result(
    gracht_client_t* client,
    struct gracht_message_context* context,
    int* count)
{
    CHECK(g_step++ == 2);
    if (g_failure == 3) {
        return -1;
    }
    *count = 2;
    return 0;
}

static uint32_t g_controllerLength;

int
sys_usb_get_controller(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const int index)
{
    CHECK(g_step++ == 0 && index == 1);
    return g_failure == 1 ? -1 : 0;
}

int
sys_usb_get_controller_result(
    gracht_client_t* client,
    struct gracht_message_context* context,
    uint8_t* controller,
    uint32_t* length)
{
    CHECK(g_step++ == 2 && *length == sizeof(USBControllerDevice_t));
    if (g_failure == 3) {
        return -1;
    }
    memset(controller, 0xA5, sizeof(USBControllerDevice_t));
    *length = g_controllerLength;
    return 0;
}

static void
__Begin(
    _In_ int failure)
{
    g_step = 0;
    g_failure = failure;
    g_errors = 0;
}

static void
__TestMessages(void)
{
    __Begin(0);
    CHECK(UsbHubUnregister(7) == OS_EOK && g_step == 1 && g_errors == 0);
    __Begin(1);
    CHECK(UsbHubUnregister(7) == OS_EUNKNOWN && g_step == 1 && g_errors == 1);

    __Begin(0);
    CHECK(UsbEventPort(7, 3) == OS_EOK && g_step == 1 && g_errors == 0);
    __Begin(1);
    CHECK(UsbEventPort(7, 3) == OS_EUNKNOWN && g_step == 1 && g_errors == 1);

    __Begin(0);
    CHECK(UsbPortError(7, 3) == OS_EOK && g_step == 1 && g_errors == 0);
    __Begin(1);
    CHECK(UsbPortError(7, 3) == OS_EUNKNOWN && g_step == 1 && g_errors == 1);
}

static void
__TestControllerQueries(void)
{
    USBControllerDevice_t controller;
    USBControllerDevice_t untouched;
    int                   count;
    int                   i;

    // A failed step leaves the count of the caller unchanged.
    for (i = 1; i <= 3; i++) {
        __Begin(i);
        count = -5;
        CHECK(UsbQueryControllerCount(&count) == OS_EUNKNOWN);
        CHECK(g_step == i && count == -5 && g_errors == 1);
    }
    __Begin(0);
    CHECK(UsbQueryControllerCount(&count) == OS_EOK && g_step == 3 && count == 2 && g_errors == 0);
    CHECK(UsbQueryControllerCount(NULL) == OS_EINVALPARAMS && g_step == 3);

    memset(&untouched, 0, sizeof(untouched));
    g_controllerLength = sizeof(USBControllerDevice_t);
    for (i = 1; i <= 3; i++) {
        __Begin(i);
        memset(&controller, 0, sizeof(controller));
        CHECK(UsbQueryController(1, &controller) == OS_EUNKNOWN);
        CHECK(g_step == i && g_errors == 1);
        CHECK(memcmp(&controller, &untouched, sizeof(controller)) == 0);
    }
    __Begin(0);
    CHECK(UsbQueryController(1, &controller) == OS_EOK && g_step == 3 && g_errors == 0);
    CHECK(UsbQueryController(1, NULL) == OS_EINVALPARAMS && g_step == 3);

    // An answer of the wrong size is not a controller description.
    __Begin(0);
    g_controllerLength = sizeof(USBControllerDevice_t) - 1;
    CHECK(UsbQueryController(1, &controller) == OS_EPROTOCOL && g_step == 3 && g_errors == 1);
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
    __TestMessages();
    __TestControllerQueries();
    puts("USB registration: successful send, transport failure, conversion failure, "
         "one-way messages and controller queries passed");
    return 0;
}
