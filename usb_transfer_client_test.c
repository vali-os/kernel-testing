// Exercise typed USB submission and token revocation after import acknowledgement.
#include <assert.h>
#include <stdio.h>
#include "../librt/libusb/functions.c"

static unsigned int g_tokens;
static unsigned int g_revokes;
static unsigned int g_sends;
static unsigned int g_acks;
static int g_sendError;
static int g_waitError;
static int g_decodeError;
static oserr_t g_remoteError;
static size_t g_completed = 13;
static USBTransfer_t g_metadata;
static SHMTransfer_t g_buffer;

size_t MemoryPageSize(void) { return 4096; }
gracht_client_t* GetGrachtClient(void) { return (void*)1; }
uuid_t OSProcessCurrentID(void) { return 2; }
oserr_t OSGrantTokenCreate(uuid_t id, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    assert(id == 42 && g_tokens == g_revokes);
    g_tokens++;
    *token = (GrantToken_t) { .Version = GRANT_TOKEN_VERSION, .ControlHandleID = 7,
        .ExportGlobalHandleID = 7, .Options = *options };
    return OS_EOK;
}
oserr_t OSGrantTokenRevoke(uuid_t id)
{
    assert(id == 7 && g_tokens == g_revokes + 1);
    if (!g_sendError && !g_waitError) {
        assert(g_acks == g_sends);
    }
    g_revokes++;
    return OS_EOK;
}
int gracht_client_await(gracht_client_t* c, struct gracht_message_context* m, unsigned int flags)
{ return g_waitError; }
int gracht_client_get_status(gracht_client_t* c, struct gracht_message_context* m)
{ return GRACHT_MESSAGE_COMPLETED; }
int gracht_client_abandon(gracht_client_t* c, struct gracht_message_context* m) { return 0; }
int ctt_usbhost_queue(gracht_client_t* c, struct gracht_message_context* m,
                     uuid_t process, uuid_t device, uuid_t id, const struct ctt_usb_transfer* transfer)
{
    assert(g_tokens == g_revokes + 1 && process == 2 && device == 4);
    assert(__FromUSBTransfer(transfer, &g_metadata, &g_buffer) == OS_EOK);
    g_sends++;
    return g_sendError;
}
int ctt_usbhost_queue_periodic(gracht_client_t* c, struct gracht_message_context* m,
                              uuid_t process, uuid_t device, uuid_t id, const struct ctt_usb_transfer* transfer)
{ return ctt_usbhost_queue(c, m, process, device, id, transfer); }
int ctt_usbhost_queue_result(gracht_client_t* c, struct gracht_message_context* m,
                            oserr_t* status, enum ctt_usb_transfer_status* result, size_t* completed)
{
    g_acks = g_sends;
    *status = g_remoteError;
    *result = CTT_USB_TRANSFER_STATUS_SUCCESS;
    *completed = g_completed;
    return g_decodeError;
}
int ctt_usbhost_queue_periodic_result(gracht_client_t* c, struct gracht_message_context* m, oserr_t* status)
{ g_acks = g_sends; *status = g_remoteError; return g_decodeError; }

int main(void)
{
    usb_device_context_t device = { .controller_device_id = 4, .controller_driver_id = 5 };
    USBTransfer_t transfer = { .Type = USBTRANSFER_TYPE_BULK, .Direction = USBTRANSFER_DIRECTION_OUT,
        .Speed = USBSPEED_HIGH, .Address = { 1, 2, 3, 4 }, .MaxPacketSize = 64,
        .BufferHandle = 42, .BufferOffset = 4121, .Length = 129 };
    struct ctt_usb_transfer description;
    enum USBTransferCode result;
    size_t completed;
    uuid_t id;
    unsigned int sends;
    int failure;

    assert(UsbTransferQueue(&device, &transfer, &result, &completed) == OS_EOK);
    assert(completed == 13 && result == USBTRANSFERCODE_SUCCESS && g_tokens == g_revokes);
    assert(g_buffer.Token.Options.Data.SHM.Offset == 4096 && g_metadata.BufferOffset == 25);
    assert(g_metadata.Length == 129 && g_metadata.Address.EndpointAddress == 4);
    assert(g_buffer.Token.Options.Scope == GrantTokenScopeRead);
    transfer.Type = USBTRANSFER_TYPE_CONTROL;
    assert(UsbTransferQueue(&device, &transfer, &result, &completed) == OS_EOK);
    assert(g_buffer.Token.Options.Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    transfer.Type = USBTRANSFER_TYPE_INTERRUPT;
    transfer.Direction = USBTRANSFER_DIRECTION_IN;
    assert(UsbTransferQueuePeriodic(&device, &transfer, &id) == OS_EOK && id != UUID_INVALID);
    assert(g_tokens == g_revokes && g_acks == g_sends);
    assert(__ToUSBTransfer(&transfer, &g_buffer, &description) == OS_EOK);
    description.buffer.token.scope = GrantTokenScopeRead;
    assert(__FromUSBTransfer(&description, &g_metadata, &g_buffer) == OS_EPERMISSIONS);
    description.type = (enum ctt_usb_transfer_type)99;
    assert(__FromUSBTransfer(&description, &g_metadata, &g_buffer) == OS_EINVALPARAMS);
    sends = g_sends;
    transfer.Length = 0;
    assert(UsbTransferQueuePeriodic(&device, &transfer, &id) == OS_EINVALPARAMS);
    assert(id == UUID_INVALID && g_sends == sends);
    transfer.Length = 129;
    for (failure = 0; failure < 5; failure++) {
        g_sendError = failure == 0 ? -1 : 0;
        g_waitError = failure == 1 ? -1 : 0;
        g_decodeError = failure == 2 ? -1 : 0;
        g_completed = failure == 3 ? 130 : 13;
        g_remoteError = failure == 4 ? OS_EDEVFAULT : OS_EOK;
        assert(UsbTransferQueue(&device, &transfer, &result, &completed) != OS_EOK);
        assert(completed == (failure == 4 ? 13 : 0) && g_tokens == g_revokes);
    }
    puts("USB clients: typed metadata, token-relative ranges, scopes, acknowledgement lifetime and RPC failures passed");
    return 0;
}
