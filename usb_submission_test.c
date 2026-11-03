// Submission and cleanup run on yielding service jobs. Exercise the real manager
// and RPC entry points with cancellation injected inside attachment and handoff.
#include "../modules/serial/usb/common/hci.h"
#define UsbManagerGetController __UnusedGetController
#include "../modules/serial/usb/common/manager.c"
#undef UsbManagerGetController
#include "../modules/serial/usb/common/transfer.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static UsbManagerController_t g_controller;
static int g_phase, g_destroyed, g_queues, g_responses;
static oserr_t g_responseStatus;

UsbManagerController_t* UsbManagerGetController(uuid_t id)
{ (void)id; return &g_controller; }
static void __CancelAndProcess(UsbManagerTransfer_t* transfer)
{
    CHECK(transfer->Submitting);
    transfer->State = USBTRANSFER_STATE_CLEANUP;
    transfer->ResultCode = USBTRANSFERCODE_CANCELLED;
    UsbManagerQueueTransferWork(&g_controller, transfer);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_destroyed == 0 && g_controller.TransactionList.head->value == transfer);
}
oserr_t SHMAttach(uuid_t id, OSHandle_t* handle)
{
    handle->ID = id;
    if (g_phase == 1) __CancelAndProcess(g_controller.TransactionList.head->value);
    return OS_EOK;
}
oserr_t SHMRelease(OSHandle_t* handle)
{ memset(handle, 0, sizeof(*handle)); return OS_EOK; }
oserr_t USBTransferDmaPrepare(UsbManagerTransfer_t* transfer)
{
    if (g_phase == 2) __CancelAndProcess(transfer);
    return OS_EOK;
}
oserr_t HCITransferQueue(UsbManagerTransfer_t* transfer)
{
    g_queues++;
    if (g_phase == 3) __CancelAndProcess(transfer);
    return transfer->ResultCode == USBTRANSFERCODE_CANCELLED ? OS_EBUSY : OS_EOK;
}
oserr_t HCITransferQueueIsochronous(UsbManagerTransfer_t* transfer)
{ return HCITransferQueue(transfer); }
oserr_t USBTransferDmaDestroy(UsbManagerTransfer_t* transfer)
{
    (void)transfer;
    g_destroyed++;
    return OS_EOK;
}
oserr_t USBTransferDmaComplete(UsbManagerTransfer_t* transfer, size_t bytes)
{ (void)transfer; (void)bytes; return OS_EOK; }
oserr_t HCITransferFinalize(UsbManagerController_t* controller, UsbManagerTransfer_t* transfer, bool deferred)
{ (void)controller; (void)transfer; (void)deferred; return OS_EOK; }
void gracht_server_defer_message(struct gracht_message* message, struct gracht_message* out)
{ memcpy(out, message, GRACHT_MESSAGE_DEFERRABLE_SIZE(message)); }
int ctt_usbhost_queue_response(struct gracht_message* m, oserr_t status, const enum ctt_usb_transfer_status result, const size_t bytes)
{ (void)m; (void)result; (void)bytes; g_responseStatus = status; g_responses++; return 0; }
int ctt_usbhost_reset_periodic_response(struct gracht_message* m, oserr_t status)
{ (void)m; CHECK(status == OS_EBUSY); g_responses++; return 0; }

int main(void)
{
    struct gracht_message message = { 0 };
    USBTransfer_t request = {
        .Type = USBTRANSFER_TYPE_BULK, .MaxPacketSize = 64,
        .Length = 64, .BufferHandle = 42
    };
    UsbManagerTransfer_t* transfer;
    enum UsbManagerControllerPhase controllerPhase;
    int phase;

    // Pending retries and executing cleanup both reject submission before any
    // transfer storage or DMA state is created, including final unregistration.
    for (controllerPhase = USB_CONTROLLER_DESTROY_PENDING;
         controllerPhase <= USB_CONTROLLER_DESTROYING; controllerPhase++) {
        g_controller.State.Phase = controllerPhase;
        g_responses = 0;
        ctt_usbhost_queue_invocation(&message, 0, 7, 8, (uint8_t*)&request, sizeof(request));
        CHECK(g_responseStatus == OS_ENOENT && g_responses == 1);
        CHECK(g_queues == 0 && g_destroyed == 0 && g_controller.TransactionList.count == 0);
        CHECK(g_controller.State.OperationsInFlight == 0);
    }
    for (phase = 1; phase <= 3; phase++) {
        memset(&g_controller, 0, sizeof(g_controller));
        g_controller.Kind = USBCONTROLLER_KIND_UHCI;
        g_phase = phase;
        g_destroyed = g_queues = g_responses = 0;
        ctt_usbhost_queue_invocation(&message, 0, 7, 8, (uint8_t*)&request, sizeof(request));
        CHECK(g_destroyed == 1 && g_responses == 1 && g_controller.TransactionList.count == 0);
        CHECK(g_controller.State.OperationsInFlight == 0);
    }
    // Resetting a stalled endpoint also holds its transfer across preparation.
    g_phase = 0;
    g_destroyed = g_queues = g_responses = 0;
    request.Type = USBTRANSFER_TYPE_INTERRUPT;
    request.PeriodicBandwith = 1;
    CHECK(USBTransferCreate(&message, &request, 8, 7, &transfer) == OS_EOK);
    CHECK(transfer->TData.Periodic.Bandwith == 0);
    transfer->State = USBTRANSFER_STATE_STALLED;
    transfer->Flags |= __USBTRANSFER_FLAG_SILENT;
    g_phase = 2;
    ctt_usbhost_reset_periodic_invocation(&message, 0, 7, 8);
    CHECK(g_destroyed == 0 && g_queues == 0 && g_responses == 1);
    CHECK(!transfer->Submitting && transfer->State == USBTRANSFER_STATE_CLEANUP);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_destroyed == 1 && g_controller.TransactionList.count == 0);
    puts("USB submission: nested cancellation during attach, preparation, handoff and endpoint reset passed");
    return 0;
}
