// Exercise identity string selection without issuing USB requests for absent fields.
#include <stdio.h>
#include <stdlib.h>
#include "../services/usbd/devices.c"

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned int g_calls;
static unsigned int g_failures;
static size_t g_indexes[4];

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format, ...)
{
    (void)level;
    (void)format;
}

enum USBTransferCode
UsbGetStringDescriptor(
    _In_  usb_device_context_t* device,
    _In_  size_t language,
    _In_  size_t index,
    _Out_ mstring_t** result)
{
    CHECK(device->device_address == 5 && language == USB_LANGUAGE_ENGLISH);
    CHECK(index != 0 && g_calls < 4);
    g_indexes[g_calls++] = index;
    if (g_failures != 0) {
        g_failures--;
        return USBTRANSFERCODE_STALL;
    }
    *result = (mstring_t*)(uintptr_t)index;
    return USBTRANSFERCODE_SUCCESS;
}

int
main(void)
{
    UsbPortDevice_t device = { 0 };

    device.Base.device_address = 5;
    CHECK(__GetDeviceIdentification(&device) == USBTRANSFERCODE_SUCCESS);
    CHECK(g_calls == 0 && device.Manufacturer == NULL && device.Product == NULL && device.Serial == NULL);

    device.ProductIndex = 2;
    CHECK(__GetDeviceIdentification(&device) == USBTRANSFERCODE_SUCCESS);
    CHECK(g_calls == 1 && g_indexes[0] == 2 && device.Product == (mstring_t*)2);
    CHECK(device.Manufacturer == NULL && device.Serial == NULL);

    g_calls = 0;
    g_failures = 1;
    CHECK(__GetDeviceString(&device, 3, &device.Serial) == USBTRANSFERCODE_SUCCESS);
    CHECK(g_calls == 2 && g_indexes[0] == 3 && g_indexes[1] == 3);

    g_calls = 0;
    g_failures = 2;
    CHECK(__GetDeviceString(&device, 3, &device.Serial) == USBTRANSFERCODE_STALL);
    CHECK(g_calls == 2 && device.Serial == NULL);
    puts("USB strings: absent indexes skipped, advertised indexes queried, bounded retry passed");
    return 0;
}
