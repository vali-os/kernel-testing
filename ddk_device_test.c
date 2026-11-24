// Tests DeviceRegister and DeviceUnregister in libddk. A request is sent,
// waited for and decoded in three steps. A failure in any step must end the
// call there, must not run the later steps, and must leave the id of the
// device unchanged.
#ifndef DDK_DEVICE_SOURCE
#define DDK_DEVICE_SOURCE "../librt/libddk/devices/device.c"
#endif
#include DDK_DEVICE_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// The step of a request that fails. STEP_NONE lets the whole request succeed.
enum { STEP_NONE, STEP_SEND, STEP_WAIT, STEP_DECODE };

static int     g_failStep;
static int     g_step;
static oserr_t g_serviceStatus;
static int     g_errors;
static int     g_warnings;

uuid_t
OSServiceGetDevice(void)
{
    return 10;
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
    } else if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
}

int
sys_device_register(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    const struct sys_device*       device,
    const unsigned int             flags)
{
    CHECK(g_step++ == 0);
    CHECK(device->content_type == SYS_DEVICE_CONTENT_BASE && flags == DEVICE_REGISTER_FLAG_LOADDRIVER);
    return g_failStep == STEP_SEND ? -1 : 0;
}

int
sys_device_unregister(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    const uuid_t                   deviceId)
{
    CHECK(g_step++ == 0);
    CHECK(deviceId == 7);
    return g_failStep == STEP_SEND ? -1 : 0;
}

int
gracht_client_await(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    unsigned int                   flags)
{
    CHECK(g_step++ == 1 && flags == GRACHT_AWAIT_ASYNC);
    return g_failStep == STEP_WAIT ? -1 : 0;
}

int
sys_device_register_result(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    oserr_t*                       status,
    uuid_t*                        id)
{
    CHECK(g_step++ == 2);
    if (g_failStep == STEP_DECODE) {
        return -1;
    }
    *status = g_serviceStatus;
    *id = 77;
    return 0;
}

int
sys_device_unregister_result(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    oserr_t*                       status)
{
    CHECK(g_step++ == 2);
    if (g_failStep == STEP_DECODE) {
        return -1;
    }
    *status = g_serviceStatus;
    return 0;
}

static void
__Reset(
    _In_ int     failStep,
    _In_ oserr_t serviceStatus)
{
    g_failStep = failStep;
    g_serviceStatus = serviceStatus;
    g_step = 0;
    g_errors = 0;
    g_warnings = 0;
}

static void
__TestRegister(void)
{
    Device_t device = { .Id = 5, .Length = sizeof(Device_t) };
    int      step;

    // A description that cannot be converted is refused before anything is sent.
    __Reset(STEP_NONE, OS_EOK);
    device.Length = 0;
    CHECK(DeviceRegister(&device, DEVICE_REGISTER_FLAG_LOADDRIVER) == UUID_INVALID);
    CHECK(g_step == 0 && device.Id == 5 && g_errors == 1);
    device.Length = sizeof(Device_t);

    // A failed step ends the request. g_step shows that no later step ran.
    for (step = STEP_SEND; step <= STEP_DECODE; step++) {
        __Reset(step, OS_EOK);
        CHECK(DeviceRegister(&device, DEVICE_REGISTER_FLAG_LOADDRIVER) == UUID_INVALID);
        CHECK(g_step == step && device.Id == 5 && g_errors == 1 && g_warnings == 0);
    }

    // The device service answered, but refused the device.
    __Reset(STEP_NONE, OS_EEXISTS);
    CHECK(DeviceRegister(&device, DEVICE_REGISTER_FLAG_LOADDRIVER) == UUID_INVALID);
    CHECK(g_step == 3 && device.Id == 5 && g_errors == 0 && g_warnings == 1);

    __Reset(STEP_NONE, OS_EOK);
    CHECK(DeviceRegister(&device, DEVICE_REGISTER_FLAG_LOADDRIVER) == 77);
    CHECK(g_step == 3 && device.Id == 77 && g_errors == 0 && g_warnings == 0);
}

static void
__TestUnregister(void)
{
    int step;

    for (step = STEP_SEND; step <= STEP_DECODE; step++) {
        __Reset(step, OS_EOK);
        CHECK(DeviceUnregister(7) == OS_EPROTOCOL);
        CHECK(g_step == step && g_warnings == 1);
    }

    // The answer of the device service is passed on unchanged.
    __Reset(STEP_NONE, OS_ENOENT);
    CHECK(DeviceUnregister(7) == OS_ENOENT && g_step == 3 && g_warnings == 1);

    __Reset(STEP_NONE, OS_EOK);
    CHECK(DeviceUnregister(7) == OS_EOK && g_step == 3 && g_warnings == 0);
}

int
main(void)
{
    __TestRegister();
    __TestUnregister();
    puts("DDK device registration: conversion, send, wait, decode and rejection failures passed");
    return 0;
}
