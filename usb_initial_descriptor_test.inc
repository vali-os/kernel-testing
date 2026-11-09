// Exercise the real client request and completion path with a controlled RPC reply.
static enum ctt_usb_transfer_status g_usbResult;
static oserr_t g_usbStatus;
static unsigned int g_descriptorLogs;
static char g_descriptorLog[256];
static bool g_initialRequest;
static bool g_replyReady;
static unsigned int g_receivedBytes;
static const uint8_t g_descriptorBytes[8] = { 18, 1, 0x10, 1, 0, 0, 0, 8 };

static void
__CaptureDescriptorLog(
    const char* format,
    va_list     arguments)
{
    if (strstr(format, "initial-descriptor buffer after DMA") != NULL) {
        CHECK(g_replyReady);
        g_descriptorLogs++;
        vsnprintf(g_descriptorLog, sizeof(g_descriptorLog), format, arguments);
    }
}

gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

uuid_t
OSProcessCurrentID(void)
{
    return 1;
}

int
ctt_usbhost_queue(
    gracht_client_t*                client,
    struct gracht_message_context* context,
    const uuid_t                   processId,
    const uuid_t                   deviceId,
    const uuid_t                   transferId,
    const uint8_t*                 transfer,
    const uint32_t                 transfer_count)
{
    const USBTransfer_t* request = (const USBTransfer_t*)transfer;
    uint8_t* descriptor = g_bytes + request->BufferOffset + sizeof(usb_packet_t);
    unsigned int i;

    CHECK(transfer_count == sizeof(*request));
    CHECK(request->BufferHandle == 7);
    CHECK(g_receivedBytes <= request->Length - sizeof(usb_packet_t));
    if (g_initialRequest) {
        for (i = 0; i < 8; i++) {
            CHECK(descriptor[i] == 0);
        }
    }
    g_replyReady = false;
    memcpy(descriptor, g_descriptorBytes, g_receivedBytes);
    return 0;
}

int
ctt_usbhost_queue_result(
    gracht_client_t*                client,
    struct gracht_message_context* context,
    oserr_t*                       status_out,
    enum ctt_usb_transfer_status*  result_out,
    size_t*                        bytesTransferred_out)
{
    *status_out = g_usbStatus;
    *result_out = g_usbResult;
    *bytesTransferred_out = sizeof(usb_packet_t) + g_receivedBytes;
    g_replyReady = true;
    return 0;
}

int
gracht_client_await(
    gracht_client_t*                client,
    struct gracht_message_context* context,
    unsigned int                   flags)
{
    return 0;
}

static void
__TestInitialDescriptor(void)
{
    usb_device_context_t device = { .device_mps = 64 };
    uint8_t output[8];
    enum USBTransferCode result;
    unsigned int i;

    CHECK(UsbInitialize() == OS_EOK);
    g_initialRequest = true;
    g_usbStatus = OS_EOK;
    g_usbResult = CTT_USB_TRANSFER_STATUS_STALL;
    g_receivedBytes = 3;
    memset(output, 0xa5, sizeof(output));
    result = UsbExecutePacket(&device, USBPACKET_DIRECTION_IN, USBPACKET_TYPE_GET_DESC,
        0, USB_DESCRIPTOR_DEVICE, 0, 8, output);
    CHECK(result == USBTRANSFERCODE_STALL && g_descriptorLogs == 1);
    CHECK(strstr(g_descriptorLog, "12 01 10 00 00 00 00 00") != NULL);
    for (i = 0; i < sizeof(output); i++) {
        CHECK(output[i] == 0xa5);
    }

    // No data and an OS error must not expose an earlier allocation's bytes.
    g_receivedBytes = 0;
    result = UsbExecutePacket(&device, USBPACKET_DIRECTION_IN, USBPACKET_TYPE_GET_DESC,
        0, USB_DESCRIPTOR_DEVICE, 0, 8, output);
    CHECK(result == USBTRANSFERCODE_STALL && g_descriptorLogs == 2);
    CHECK(strstr(g_descriptorLog, "00 00 00 00 00 00 00 00") != NULL);
    g_usbStatus = OS_EUNKNOWN;
    UsbExecutePacket(&device, USBPACKET_DIRECTION_IN, USBPACKET_TYPE_GET_DESC,
        0, USB_DESCRIPTOR_DEVICE, 0, 8, output);
    CHECK(g_descriptorLogs == 2);

    // Success still copies the descriptor, without a failure dump.
    g_usbStatus = OS_EOK;
    g_usbResult = CTT_USB_TRANSFER_STATUS_SUCCESS;
    g_receivedBytes = 8;
    result = UsbExecutePacket(&device, USBPACKET_DIRECTION_IN, USBPACKET_TYPE_GET_DESC,
        0, USB_DESCRIPTOR_DEVICE, 0, 8, output);
    CHECK(result == USBTRANSFERCODE_SUCCESS && g_descriptorLogs == 2);
    CHECK(memcmp(output, g_descriptorBytes, 8) == 0);

    g_initialRequest = false;
    g_usbResult = CTT_USB_TRANSFER_STATUS_STALL;
    g_receivedBytes = 0;
    for (i = 0; i < 7; i++) {
        device.device_address = i == 0 ? 1 : 0;
        UsbExecutePacket(&device, i == 1 ? 0 : USBPACKET_DIRECTION_IN,
            i == 2 ? USBPACKET_TYPE_SET_FEATURE : USBPACKET_TYPE_GET_DESC,
            i == 3 ? 1 : 0, i == 4 ? USB_DESCRIPTOR_CONFIG : USB_DESCRIPTOR_DEVICE,
            i == 5 ? 1 : 0, i == 6 ? 7 : 8, output);
        CHECK(g_descriptorLogs == 2);
    }
    UsbCleanup();
    puts("USB initial descriptor: failure bytes after completion, zero fill, exact selection and success copy passed");
}
