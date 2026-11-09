// Exercise the shared legacy completion path with failures at ownership steps.
#include "../modules/serial/usb/common/manager.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static UsbManagerController_t g_controller;
static UsbScheduler_t g_scheduler;
static UsbSchedulerObject_t g_object;
static int g_scans, g_notifications, g_ends, g_queues, g_destroyed;
static bool g_failEnd, g_failFinalize, g_failDestroy, g_cancelNotify;
static size_t g_expectedBytes;
static bool g_batches;
static bool g_cancelQueue;
static oserr_t g_queueStatus;

oserr_t UsbSchedulerGetPoolFromElement(UsbScheduler_t* scheduler, const uint8_t* element, UsbSchedulerPool_t** pool)
{ CHECK(element != NULL); *pool = &scheduler->Settings.Pools[0]; return OS_EOK; }
bool HCIProcessElement(UsbManagerController_t* controller, uint8_t* element, enum HCIProcessReason reason, void* context)
{
    struct HCIProcessReasonScanContext* scan = context;
    (void)controller; (void)element;
    CHECK(reason == HCIPROCESS_REASON_SCAN);
    g_scans++;
    scan->ElementsProcessed = 1;
    scan->ElementsExecuted = g_batches ? 1 : g_scans;
    scan->BytesTransferred = g_batches || g_scans == 1 ? 7 : 5;
    return true;
}
oserr_t UsbTransferDmaComplete(UsbManagerTransfer_t* transfer, size_t bytes)
{
    if (!transfer->Dma.DeviceOwned) return OS_EOK;
    CHECK(bytes == g_expectedBytes);
    g_ends++;
    if (g_failEnd) return OS_EUNKNOWN;
    transfer->Dma.DeviceOwned = false;
    return OS_EOK;
}
void UsbTransferNotify(UsbManagerTransfer_t* transfer)
{
    int scans = g_scans;

    CHECK(!transfer->Dma.DeviceOwned && g_controller.State.OperationsInFlight != 0);
    // A completion callback may yield to another dispatch. The nested pass must
    // leave the current transfer and the outer operation count untouched.
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == scans && g_controller.State.OperationsInFlight == 1);
    CHECK(g_controller.State.ProcessingTransfers);
    if (transfer->Flags & __USBTRANSFER_FLAG_NOTIFIED) return;
    transfer->Flags |= __USBTRANSFER_FLAG_NOTIFIED;
    g_notifications++;
    if (g_cancelNotify) {
        transfer->State = USBTRANSFER_STATE_CLEANUP;
        transfer->ResultCode = USBTRANSFERCODE_CANCELLED;
    }
}
oserr_t HCITransferFinalize(UsbManagerController_t* controller, UsbManagerTransfer_t* transfer, bool deferred)
{
    (void)controller; (void)deferred;
    if (g_failFinalize) return OS_EBUSY;
    transfer->RootElement = NULL;
    transfer->ChainLength = 0;
    return OS_EOK;
}
oserr_t UsbTransferDmaPrepare(UsbManagerTransfer_t* transfer)
{ (void)transfer; return OS_EOK; }
oserr_t HCITransferQueue(UsbManagerTransfer_t* transfer)
{
    if (g_batches) {
        CHECK(transfer->Dma.DeviceOwned && g_ends == 0 && g_notifications == 0);
        transfer->RootElement = &g_object;
        transfer->ChainLength = 1;
        transfer->State = USBTRANSFER_STATE_QUEUED;
    }
    g_queues++;
    if (g_cancelQueue) {
        transfer->State = USBTRANSFER_STATE_CLEANUP;
        transfer->ResultCode = USBTRANSFERCODE_CANCELLED;
    }
    return g_queueStatus;
}
oserr_t UsbTransferDestroy(UsbManagerTransfer_t* transfer)
{
    CHECK(transfer->RootElement == NULL && !transfer->Dma.DeviceOwned);
    if (g_failDestroy) return OS_EUNKNOWN;
    list_remove(&g_controller.TransactionList, &transfer->ListHeader);
    g_destroyed++;
    return OS_EOK;
}

static void __Prepare(UsbManagerTransfer_t* transfer, enum USBTransferType type)
{
    memset(&g_controller, 0, sizeof(g_controller));
    memset(transfer, 0, sizeof(*transfer));
    g_controller.Kind = USBCONTROLLER_KIND_UHCI;
    g_controller.State.Phase = USB_CONTROLLER_READY;
    g_controller.Scheduler = &g_scheduler;
    g_object.DepthIndex = USB_ELEMENT_NO_INDEX;
    transfer->Type = type;
    transfer->State = USBTRANSFER_STATE_QUEUED;
    transfer->Dma.DeviceOwned = true;
    transfer->RootElement = &g_object;
    transfer->ChainLength = transfer->ElementCount = type == USBTRANSFER_TYPE_BULK ? 2 : 1;
    ELEMENT_INIT(&transfer->ListHeader, 0, transfer);
    list_append(&g_controller.TransactionList, &transfer->ListHeader);
    g_scans = g_notifications = g_ends = g_queues = g_destroyed = 0;
    g_failEnd = g_failFinalize = g_failDestroy = g_cancelNotify = false;
    g_batches = false;
    g_cancelQueue = false;
    g_queueStatus = OS_EOK;
}

static void
__TestWaitingFailure(void)
{
    UsbManagerTransfer_t transfer;
    int cancelled;

    for (cancelled = 0; cancelled < 2; cancelled++) {
        __Prepare(&transfer, USBTRANSFER_TYPE_BULK);
        transfer.State = USBTRANSFER_STATE_WAITING;
        transfer.RootElement = NULL;
        transfer.Dma.DeviceOwned = false;
        g_queueStatus = cancelled ? OS_EBUSY : OS_EOOM;
        g_cancelQueue = cancelled != 0;

        // A failed retry must enter cleanup. Cancellation during a yielding
        // queue call must preserve CANCELLED rather than replacing it with INVALID.
        __QueueWaitingTransfers(&g_controller);
        CHECK(g_queues == 1 && transfer.State == USBTRANSFER_STATE_CLEANUP);
        CHECK(transfer.ResultCode == (cancelled ? USBTRANSFERCODE_CANCELLED : USBTRANSFERCODE_INVALID));
    }
}

int main(void)
{
    UsbManagerTransfer_t transfer;
    enum UsbManagerControllerPhase phase;

    __TestWaitingFailure();

    // Destruction closes transfer dispatch in both retry and executing phases.
    // Neither path may scan descriptors or change an existing DMA handoff.
    for (phase = USB_CONTROLLER_DESTROY_PENDING; phase <= USB_CONTROLLER_DESTROYING; phase++) {
        __Prepare(&transfer, USBTRANSFER_TYPE_BULK);
        g_controller.State.Phase = phase;
        UsbManagerProcessTransfers(&g_controller);
        CHECK(g_scans == 0 && g_notifications == 0 && g_destroyed == 0);
        CHECK(transfer.Dma.DeviceOwned && transfer.RootElement == &g_object);
        CHECK(g_controller.State.OperationsInFlight == 0 && !g_controller.State.ProcessingTransfers);
    }

    __Prepare(&transfer, USBTRANSFER_TYPE_BULK);
    g_expectedBytes = 12;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == 1 && transfer.Dma.Completed == 7 && g_ends == 0);
    g_failEnd = true;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == 2 && transfer.Dma.Completed == 12 && g_ends == 1 && g_notifications == 0);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == 2 && g_ends == 2 && transfer.Dma.DeviceOwned);
    g_failEnd = false;
    g_failFinalize = true;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_notifications == 1 && !transfer.Dma.DeviceOwned && g_destroyed == 0);
    g_failFinalize = false;
    g_failDestroy = true;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(transfer.State == USBTRANSFER_STATE_CLEANUP && g_notifications == 1);
    g_failDestroy = false;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_destroyed == 1 && g_notifications == 1 && g_scans == 2);

    // Cancellation while sending the periodic notification must suppress the
    // next Begin/publication and keep the old chain for retryable unlink.
    __Prepare(&transfer, USBTRANSFER_TYPE_INTERRUPT);
    g_expectedBytes = 7;
    g_cancelNotify = true;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_notifications == 1 && g_queues == 0 && transfer.State == USBTRANSFER_STATE_CLEANUP);
    g_failFinalize = true;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_destroyed == 0 && transfer.RootElement != NULL && g_ends == 1);
    g_failFinalize = false;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_destroyed == 1 && g_notifications == 1 && g_queues == 0);
    // A request larger than the pool spans batches without ending its DMA
    // handoff. Retirement retries must not recount the finished descriptors.
    __Prepare(&transfer, USBTRANSFER_TYPE_BULK);
    g_batches = true;
    transfer.ElementCount = 3;
    transfer.ChainLength = 1;
    g_expectedBytes = 21;
    g_failFinalize = true;
    UsbManagerProcessTransfers(&g_controller);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == 1 && g_queues == 0 && transfer.Dma.Completed == 7 && g_ends == 0);
    g_failFinalize = false;
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_queues == 1 && transfer.TData.Async.ElementsCompleted == 1);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_queues == 2 && transfer.TData.Async.ElementsCompleted == 2);
    UsbManagerProcessTransfers(&g_controller);
    CHECK(g_scans == 3 && g_ends == 1 && g_notifications == 1 && g_destroyed == 1);
    CHECK(transfer.Dma.Completed == 21 && transfer.TData.Async.BytesTransferred == 21);
    puts("Legacy USB manager: batch handoff, cumulative prefixes, end/unmap retries and cancellation passed");
    return 0;
}
