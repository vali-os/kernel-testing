#include "../modules/serial/usb/common/manager.c"
#include "../modules/serial/usb/common/transfer_work.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static UsbManagerController_t* g_controller;
static int g_destroyCalls, g_notifications, g_scans;
static int g_finalizations;
static int g_failCpuSync, g_queueCalls, g_waitOnQueue;
static enum USBTransferCode g_notified[4];

oserr_t USBTransferDestroy(UsbManagerTransfer_t* transfer)
{
    CHECK(transfer->RootElement == NULL);
    CHECK(transfer->ReusableElements == NULL);
    if (++g_destroyCalls == 1) return OS_EBUSY;
    UsbManagerForgetTransferWork(transfer);
    g_controller->TransactionList.head = NULL;
    return OS_EOK;
}
void USBTransferNotify(UsbManagerTransfer_t* transfer)
{
    g_notified[g_notifications++] = transfer->ResultCode;
}
oserr_t USBTransferDmaComplete(UsbManagerTransfer_t* transfer, size_t bytes)
{
    CHECK(!transfer->Dma.HardwareActive);
    (void)bytes;
    if (g_failCpuSync) return OS_EUNKNOWN;
    transfer->Dma.DeviceOwned = false;
    return OS_EOK;
}
oserr_t USBTransferDmaPrepare(UsbManagerTransfer_t* transfer)
{ (void)transfer; return OS_EOOM; }
oserr_t HCITransferQueue(UsbManagerTransfer_t* transfer)
{
    CHECK(transfer->State == USBTRANSFER_STATE_WAITING);
    g_queueCalls++;
    if (g_waitOnQueue) {
        UsbManagerWaitTransfer(g_controller, transfer);
    } else {
        transfer->State = USBTRANSFER_STATE_QUEUED;
        transfer->Dma.HardwareActive = true;
    }
    return OS_EOK;
}
oserr_t HCITransferFinalize(UsbManagerController_t* controller, UsbManagerTransfer_t* transfer, bool deferred)
{
    (void)controller;
    CHECK(!deferred);
    g_finalizations++;
    transfer->RootElement = NULL;
    transfer->ReusableElements = NULL;
    return OS_EOK;
}
oserr_t UsbSchedulerGetPoolFromElement(UsbScheduler_t* scheduler, const uint8_t* element, UsbSchedulerPool_t** pool)
{ CHECK(element != NULL); *pool = &scheduler->Settings.Pools[0]; return OS_EOK; }
bool HCIProcessElement(UsbManagerController_t* controller, uint8_t* element, enum HCIProcessReason reason, void* context)
{
    struct HCIProcessReasonScanContext* scan = context;
    (void)controller; (void)element;
    CHECK(reason == HCIPROCESS_REASON_SCAN);
    g_scans++;
    scan->ElementsExecuted = scan->ElementsProcessed = 1;
    scan->BytesTransferred = 7;
    return true;
}
static int __Finished(UsbManagerController_t* controller, UsbManagerTransfer_t* transfer, void* context)
{ (void)controller; (void)transfer; (void)context; return ITERATOR_REMOVE; }

static void
__TestReadyWork(void)
{
    UsbManagerController_t controller = { .DmaHandle = 42 };
    UsbScheduler_t scheduler = { 0 };
    UsbSchedulerObject_t object = { .DepthIndex = USB_ELEMENT_NO_INDEX };
    UsbManagerTransfer_t* transfers = calloc(1024, sizeof(*transfers));
    UsbManagerTransfer_t cleanup = {
        .State = USBTRANSFER_STATE_CLEANUP,
        .ResultCode = USBTRANSFERCODE_CANCELLED,
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Dma.Context = 42
    };
    unsigned int i;

    CHECK(transfers != NULL);
    g_controller = &controller;
    controller.Scheduler = &scheduler;
    g_scans = g_notifications = g_queueCalls = 0;
    for (i = 0; i < 1024; i++) {
        transfers[i].Type = USBTRANSFER_TYPE_INTERRUPT;
        transfers[i].State = USBTRANSFER_STATE_QUEUED;
        transfers[i].Dma.Context = 42;
        transfers[i].RootElement = &object;
        transfers[i].ChainLength = 1;
        ELEMENT_INIT(&transfers[i].ListHeader, 0, &transfers[i]);
        list_append(&controller.TransactionList, &transfers[i].ListHeader);
    }
    UsbManagerWaitTransfer(&controller, &transfers[200]);
    UsbManagerQueueTransferWork(&controller, &transfers[100]);
    UsbManagerQueueTransferWork(&controller, &transfers[100]);
    CHECK(controller.ReadyTransfers.count == 1);
    g_failCpuSync = 1;
    UsbManagerProcessTransfers(&controller);
    CHECK(g_scans == 1 && g_notifications == 0 && controller.ReadyTransfers.count == 1);
    g_failCpuSync = 0;
    UsbManagerProcessTransfers(&controller);
    CHECK(g_scans == 2 && g_notifications == 2 && controller.ReadyTransfers.count == 0);
    CHECK(transfers[100].State == USBTRANSFER_STATE_COMPLETED);
    for (i = 0; i < 1024; i++) {
        if (i != 100 && i != 200) {
            CHECK(transfers[i].State == USBTRANSFER_STATE_QUEUED);
            CHECK(transfers[i].RootElement == &object);
        }
    }

    // Waiting submissions only run after a command wakeup, once per dispatch.
    UsbManagerProcessTransfers(&controller);
    CHECK(g_queueCalls == 0 && g_scans == 2);
    g_waitOnQueue = 1;
    UsbManagerWakeTransfers(&controller);
    UsbManagerProcessTransfers(&controller);
    CHECK(g_queueCalls == 1 && controller.WaitingTransfers.count == 1);
    g_waitOnQueue = 0;
    UsbManagerWakeTransfers(&controller);
    UsbManagerProcessTransfers(&controller);
    CHECK(g_queueCalls == 2 && controller.WaitingTransfers.count == 0);
    CHECK(transfers[200].Dma.HardwareActive);

    // Cancellation moves a waiter to work; forgetting either queue is idempotent.
    UsbManagerWaitTransfer(&controller, &transfers[201]);
    transfers[201].State = USBTRANSFER_STATE_CLEANUP;
    UsbManagerQueueTransferWork(&controller, &transfers[201]);
    CHECK(controller.WaitingTransfers.count == 0 && controller.ReadyTransfers.count == 1);
    UsbManagerForgetTransferWork(&transfers[201]);
    UsbManagerForgetTransferWork(&transfers[201]);
    CHECK(controller.ReadyTransfers.count == 0);
    free(transfers);
    list_construct(&controller.TransactionList);

    // A failed destruction remains queued instead of being lost or spinning.
    g_destroyCalls = g_notifications = 0;
    UsbManagerQueueTransferWork(&controller, &cleanup);
    UsbManagerProcessTransfers(&controller);
    CHECK(g_destroyCalls == 1 && controller.ReadyTransfers.count == 1);
    UsbManagerProcessTransfers(&controller);
    CHECK(g_destroyCalls == 2 && controller.ReadyTransfers.count == 0);
    puts("USB ready work: one signalled transfer among 1024, deduplication, sync/unmap retry and command waits passed");
}

int main(void)
{
    UsbManagerController_t controller = { .DmaHandle = 42 };
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_BULK,
        .State = USBTRANSFER_STATE_QUEUED, .ResultCode = USBTRANSFERCODE_SUCCESS,
        .Dma = { .Context = 42 } };
    UsbScheduler_t scheduler = { 0 };
    UsbSchedulerObject_t object = { .DepthIndex = USB_ELEMENT_NO_INDEX };
    g_controller = &controller;
    controller.Scheduler = &scheduler;
    controller.TransactionList.head = &transfer.ListHeader;
    transfer.ListHeader.value = &transfer;
    UsbManagerIterateTransfers(&controller, __Finished, NULL);
    CHECK(g_destroyCalls == 1 && transfer.State == USBTRANSFER_STATE_CLEANUP);
    UsbManagerIterateTransfers(&controller, __ProcessTransfer, NULL);
    CHECK(g_destroyCalls == 2 && controller.TransactionList.head == NULL && g_scans == 0);

    transfer.Dma.HardwareActive = true;
    transfer.State = USBTRANSFER_STATE_QUEUED;
    transfer.RootElement = NULL;
    CHECK(__CheckTransfer(&controller, &transfer) == ITERATOR_CONTINUE && g_scans == 0);

    g_notifications = 0;
    transfer.Type = USBTRANSFER_TYPE_INTERRUPT;
    transfer.Dma.HardwareActive = false;
    transfer.RootElement = &object;
    transfer.ChainLength = 1;
    CHECK(__CheckTransfer(&controller, &transfer) == ITERATOR_CONTINUE);
    CHECK(g_scans == 1 && g_notifications == 2);
    CHECK(g_notified[0] == USBTRANSFERCODE_SUCCESS && g_notified[1] == USBTRANSFERCODE_BUFFERERROR);
    CHECK(transfer.State == USBTRANSFER_STATE_COMPLETED && transfer.RootElement == NULL);

    // Cancellation after a failed rearm must finalize cached-only descriptors.
    g_notifications = 0;
    g_finalizations = 0;
    transfer.State = USBTRANSFER_STATE_CLEANUP;
    transfer.ReusableElements = &object;
    transfer.ResultCode = USBTRANSFERCODE_CANCELLED;
    CHECK(__ProcessCleanup(&controller, &transfer) == ITERATOR_REMOVE);
    CHECK(g_finalizations == 1 && transfer.ReusableElements == NULL);
    __TestReadyWork();
    puts("USB DMA manager: active-transfer scan gate, failed destroy retry and periodic rearm error notification passed");
    return 0;
}
