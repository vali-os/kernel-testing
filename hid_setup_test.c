// Production HID setup and report handling with a device that exposes its HID
// descriptor only in the configuration, as QEMU and physical devices may do.
#include "../modules/input/hid/functions.c"
#include "../modules/input/hid/report_parser.c"
#include "../modules/input/hid/report_handler.c"
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static const uint8_t g_report[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
    0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0
};
static const uint8_t g_configuration[] = {
    9, USB_DESCRIPTOR_CONFIG, 34, 0, 1, 1, 0, 0x80, 50,
    9, USB_DESCRIPTOR_INTERFACE, 2, 1, 1, USB_CLASS_HID, 1, 1, 0,
    9, DESCRIPTOR_TYPE_HID, 0x11, 1, 0, 1, DESCRIPTOR_TYPE_REPORT, sizeof(g_report), 0,
    7, USB_DESCRIPTOR_ENDPOINT, 0x81, 3, 8, 0, 10
};
static unsigned int g_requests;
static unsigned int g_buttonEvents;
static uint8_t      g_keys[4];
static uint16_t     g_modifiers[4];
static int          g_failConfiguration;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*       format,
    ...)
{
    (void)level;
    (void)format;
}

enum USBTransferCode
UsbExecutePacket(
    _In_ usb_device_context_t* context,
    _In_ uint8_t               direction,
    _In_ uint8_t               type,
    _In_ uint8_t               valueLow,
    _In_ uint8_t               valueHigh,
    _In_ uint16_t              index,
    _In_ uint16_t              length,
    _In_ void*                buffer)
{
    (void)context;
    CHECK(valueLow == 0);
    g_requests++;
    if (type == USBPACKET_TYPE_GET_DESC && valueHigh == USB_DESCRIPTOR_CONFIG) {
        CHECK(direction == USBPACKET_DIRECTION_IN && index == 0);
        CHECK(length == sizeof(g_configuration));
        if (g_failConfiguration) {
            return USBTRANSFERCODE_STALL;
        }
        memcpy(buffer, g_configuration, length);
    } else if (type == USBPACKET_TYPE_GET_DESC && valueHigh == DESCRIPTOR_TYPE_REPORT) {
        CHECK(direction == (USBPACKET_DIRECTION_INTERFACE | USBPACKET_DIRECTION_IN));
        CHECK(index == 2 && length == sizeof(g_report));
        memcpy(buffer, g_report, length);
    } else if (type == HID_SET_IDLE) {
        CHECK(index == 2 && length == 0);
    } else {
        // In particular, a standalone GET_DESCRIPTOR(HID) must never be sent.
        CHECK(0);
    }
    return USBTRANSFERCODE_SUCCESS;
}

gracht_server_t*
__crt_get_module_server(void)
{
    return NULL;
}

int
ctt_input_event_button_event_all(
    _In_ gracht_server_t* server,
    _In_ uuid_t           device,
    _In_ uint8_t          key,
    _In_ uint16_t         modifiers)
{
    CHECK(server == NULL && device == 91 && g_buttonEvents < 4);
    g_keys[g_buttonEvents] = key;
    g_modifiers[g_buttonEvents++] = modifiers;
    return 0;
}

int
ctt_input_event_cursor_event_all(
    _In_ gracht_server_t* server,
    _In_ uuid_t           device,
    _In_ uint16_t         flags,
    _In_ int16_t          x,
    _In_ int16_t          y,
    _In_ int16_t          z)
{
    (void)server;
    (void)device;
    (void)flags;
    (void)x;
    (void)y;
    (void)z;
    CHECK(0);
    return 0;
}

static void
__TestDescriptorBounds(void)
{
    HidDevice_t        device = { .InterfaceId = 2, .InterfaceSetting = 1 };
    UsbHidDescriptor_t descriptor;
    uint8_t            bytes[sizeof(g_configuration)];
    uint8_t            multiple[] = {
        9, USB_DESCRIPTOR_CONFIG, 48, 0, 2, 1, 0, 0x80, 50,
        9, USB_DESCRIPTOR_INTERFACE, 0, 0, 0, USB_CLASS_HID, 0, 0, 0,
        9, DESCRIPTOR_TYPE_HID, 0x11, 1, 0, 1, DESCRIPTOR_TYPE_REPORT, 99, 0,
        9, USB_DESCRIPTOR_INTERFACE, 2, 1, 0, USB_CLASS_HID, 0, 0, 0,
        12, DESCRIPTOR_TYPE_HID, 0x11, 1, 0, 2, 0x23, 5, 0,
        DESCRIPTOR_TYPE_REPORT, sizeof(g_report), 0
    };

    memcpy(bytes, g_configuration, sizeof(bytes));
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_EOK);
    CHECK(descriptor.ClassDescriptorLength == sizeof(g_report));
    CHECK(__FindHidDescriptor(multiple, sizeof(multiple), &device, &descriptor) == OS_EOK);
    CHECK(descriptor.ClassDescriptorLength == sizeof(g_report));
    device.InterfaceId = 0;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_ENOENT);
    device.InterfaceId = 2;
    device.InterfaceSetting = 0;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_ENOENT);
    device.InterfaceSetting = 1;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes) - 1, &device, &descriptor) == OS_EINVALPARAMS);
    bytes[18] = 0;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_EINVALPARAMS);
    bytes[18] = 255;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_EINVALPARAMS);
    bytes[18] = 9;
    bytes[23] = 2;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_EINVALPARAMS);
    bytes[23] = 1;
    bytes[24] = 0x23;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_ENOENT);
    bytes[24] = DESCRIPTOR_TYPE_REPORT;
    bytes[25] = 0;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_EINVALPARAMS);
    memcpy(bytes, g_configuration, sizeof(bytes));
    bytes[14] = 0xff;
    CHECK(__FindHidDescriptor(bytes, sizeof(bytes), &device, &descriptor) == OS_ENOENT);
}

static void
__TestSetupAndKeyboardReports(void)
{
    UsbDevice_t base = { .Base.Id = 91,
        .DeviceContext.configuration_length = sizeof(g_configuration) };
    HidDevice_t device = { .Base = &base, .InterfaceId = 2, .InterfaceSetting = 1,
        .CurrentProtocol = HID_DEVICE_PROTOCOL_REPORT };
    uintptr_t   storage[3] = { 0 };
    uint8_t*    reports = (uint8_t*)storage;

    CHECK(HidSetupGeneric(&device) == OS_EOK);
    CHECK(g_requests == 3 && device.ReportLength == 8 && device.Collection != NULL);
    device.Buffer = storage;
    device.BufferSize = sizeof(storage);
    reports[2] = 0x05;
    CHECK(HidHandleReportEvent(&device, device.Collection, 0) != 0);
    device.PreviousDataValid = 1;
    device.PreviousDataIndex = 0;
    CHECK(HidHandleReportEvent(&device, device.Collection, 8) != 0);
    CHECK(g_buttonEvents == 2 && g_keys[0] == VK_B && g_keys[1] == VK_B);
    CHECK(g_modifiers[0] == 0 && g_modifiers[1] == VK_MODIFIER_RELEASED);
    HidCollectionCleanup(&device);

    g_failConfiguration = 1;
    CHECK(HidSetupGeneric(&device) == OS_EUNKNOWN);
}

int
main(void)
{
    __TestDescriptorBounds();
    __TestSetupAndKeyboardReports();
    puts("HID setup: configuration-only descriptor, interface/alternate selection, bounds and keyboard press/release passed");
    return 0;
}
