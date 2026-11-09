// Exercise registration rollback and status propagation through the real usbd path.
#include "../services/usbd/hubs.c"
#include "../librt/libds/hashtable.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static int g_errno;
int*
__errno(void)
{
    return &g_errno;
}

void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_ void* memory)
{
    free(memory);
}

static UsbController_t g_controller;
static int g_step;
static int g_failure;
static oserr_t g_result;

UsbController_t*
UsbCoreControllerGet(uuid_t deviceId)
{
    CHECK(deviceId == 10);
    return &g_controller;
}

gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

int
ctt_usbhost_configure_hub(
    gracht_client_t* client,
    struct gracht_message_context* context,
    const uuid_t device,
    const uint8_t address,
    const uint8_t ports,
    const uint16_t characteristics)
{
    CHECK(g_step++ == 0 && device == 10 && address == 5 && ports == 4);
    CHECK(characteristics == 0x44);
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
ctt_usbhost_configure_hub_result(
    gracht_client_t* client,
    struct gracht_message_context* context,
    oserr_t* status)
{
    CHECK(g_step++ == 2);
    *status = g_result;
    return g_failure == 3 ? -1 : 0;
}

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
}

int
main(void)
{
    UsbPortDevice_t device = { .DeviceId = 41, .Base.device_address = 5 };
    UsbPort_t port = { .Device = &device, .Address = 1 };
    struct UsbHub parent = { .DeviceId = 10, .ControllerDeviceId = 10 };
    int i;
    oserr_t status;

    parent.Ports[0] = &port;
    UsbCoreHubsInitialize();
    hashtable_set(&g_hubs, &parent);
    for (i = 0; i < 6; i++) {
        g_step = 0;
        g_failure = i;
        g_result = i == 4 ? OS_ETIMEOUT : i == 5 ? OS_EDEVFAULT : OS_EOK;
        status = UsbCoreHubsRegisterAndConfigure(10, 41, 343, 4, 0x44);
        CHECK(status == (i == 0 ? OS_EOK : i < 4 ? OS_EUNKNOWN : g_result));
        CHECK(g_step == (i > 0 && i < 4 ? i : 3));
        CHECK((UsbCoreHubsGet(41) != NULL) == (i == 0));
        CHECK(UsbCoreHubsGet(10) != NULL);
        hashtable_remove(&g_hubs, &(struct UsbHub) { .DeviceId = 41 });
    }
    g_step = 0;
    device.Base.device_address = 0;
    CHECK(UsbCoreHubsRegisterAndConfigure(10, 41, 343, 4, 0x44) == OS_ENOENT);
    CHECK(g_step == 0 && UsbCoreHubsGet(41) == NULL);
    hashtable_destroy(&g_hubs);
    puts("USB hub registration: command result, transport errors and rollback passed");
    return 0;
}
