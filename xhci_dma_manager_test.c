// Exercise real event dispatch, common completion handling and endpoint recovery.
#define XHCI_MANAGER_INTEGRATION
#define main __OriginalDmaFixtureMain
#include "xhci_dma_test.c"
#undef main
#define UsbManagerGetController __UnusedManagerGetController
#include "../modules/serial/usb/common/manager.c"
#undef UsbManagerGetController

static unsigned int g_notifications;
static unsigned int g_destroyed;
static enum USBTransferCode g_lastResult;

oserr_t
UsbSchedulerGetPoolFromElement(
    _In_ UsbScheduler_t* scheduler,
    _In_ const uint8_t* element,
    _Out_ UsbSchedulerPool_t** pool)
{
    CHECK(element >= (const uint8_t*)g_descriptors);
    CHECK(element < (const uint8_t*)(g_descriptors + 16));
    *pool = &scheduler->Settings.Pools[XHCI_TD_POOL];
    return OS_EOK;
}

UsbManagerController_t*
UsbManagerGetController(
    _In_ uuid_t device)
{
    CHECK(device == 77);
    return &g_cancelController.Base;
}

void
USBTransferNotify(
    _In_ UsbManagerTransfer_t* transfer)
{
    g_notifications++;
    g_lastResult = transfer->ResultCode;
}

oserr_t
USBTransferDestroy(
    _In_ UsbManagerTransfer_t* transfer)
{
    oserr_t status;

    CHECK(transfer->RootElement == NULL && transfer->ReusableElements == NULL);
    status = USBTransferDmaDestroy(transfer);
    if (status != OS_EOK) {
        return status;
    }
    UsbManagerForgetTransferWork(transfer);
    list_remove(&g_cancelController.Base.TransactionList, &transfer->ListHeader);
    free(transfer->Elements);
    transfer->Elements = NULL;
    g_destroyed++;
    return OS_EOK;
}

/** Hardware registers and scheduler backing retained throughout each scenario. */
struct __RecoveryFixture {
    UsbScheduler_t Scheduler;
    XhciDevice_t Device;
    XhciEndpoint_t Endpoint;
    reg32_t Doorbells[2];
};

static void
__InitializeRecovery(
    _Out_ struct __RecoveryFixture* fixture,
    _Out_ UsbManagerTransfer_t* transfer)
{
    XhciController_t* controller = &g_cancelController;
    XhciEndpoint_t* endpoint = &fixture->Endpoint;
    XhciDevice_t* device = &fixture->Device;

    memset(controller, 0, sizeof(*controller));
    memset(fixture, 0, sizeof(*fixture));
    memset(g_descriptors, 0, sizeof(g_descriptors));
    *transfer = (UsbManagerTransfer_t) {
        .DeviceID = 77,
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8,
        .Address = { .DeviceAddress = 5, .EndpointAddress = 1 },
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferLength = 64 }
    };
    controller->Base.DmaHandle = 42;
    controller->Base.Scheduler = &fixture->Scheduler;
    controller->Doorbells = fixture->Doorbells;
    controller->SlotCount = 1;
    controller->Slots[1] = device;
    fixture->Scheduler.Settings.Pools[XHCI_TD_POOL].ElementPool = (uint8_t*)g_descriptors;
    fixture->Scheduler.Settings.Pools[XHCI_TD_POOL].ElementAlignedSize = sizeof(g_descriptors[0]);
    fixture->Scheduler.Settings.Pools[XHCI_TD_POOL].ElementObjectOffset = offsetof(XhciTransferDescriptor_t, Object);
    device->SlotId = 1;
    device->UsbAddress = 5;
    device->State = XHCI_DEVICE_ADDRESSED;
    device->Endpoints[3] = endpoint;
    endpoint->Device = device;
    endpoint->SlotId = 1;
    endpoint->DeviceContextIndex = 3;
    endpoint->Address = transfer->Address;
    endpoint->State = XHCI_ENDPOINT_RUNNING;
    endpoint->ConfiguredMaxPacketSize = 8;
    ELEMENT_INIT(&device->Header, 0, device);
    ELEMENT_INIT(&endpoint->Header, 0, endpoint);
    ELEMENT_INIT(&transfer->ListHeader, 0, transfer);
    list_append(&controller->Devices, &device->Header);
    list_append(&controller->XhciEndpoints, &endpoint->Header);
    list_append(&controller->Base.TransactionList, &transfer->ListHeader);
    CHECK(XhciRingInitialize(42, &controller->CommandRing, XHCI_COMMAND_RING_ENTRIES) == OS_EOK);
    CHECK(XhciRingInitialize(42, &endpoint->TransferRing, XHCI_RING_TRB_COUNT) == OS_EOK);
    CHECK(USBTransferDmaPrepare(transfer) == OS_EOK);
    CHECK(HCITransferQueue(transfer) == OS_EOK);
    CHECK(transfer->Dma.HardwareActive);
    g_notifications = 0;
    g_destroyed = 0;
}

static void
__CompleteRecoveryCommand(
    _In_ unsigned int code)
{
    XhciController_t* controller = &g_cancelController;
    XhciTrb_t event = {
        .Parameter = controller->CommandRing.DeviceBase +
            controller->CommandRing.DequeueIndex * sizeof(XhciTrb_t),
        .Status = code << 24,
        .Control = XHCI_TRB_CONTROL_SLOT_ID(1)
    };

    XhciCommandHandleCompletion(controller, &event);
    UsbManagerProcessTransfers(&controller->Base);
}

static void
__CompleteRecoveryTransfer(
    _In_ UsbManagerTransfer_t* transfer,
    _In_ unsigned int code)
{
    XhciTransferDescriptor_t* descriptor = transfer->RootElement;
    XhciTrb_t event = {
        .Parameter = descriptor->Endpoint->TransferRing.DeviceBase +
            descriptor->FirstTrbIndex * sizeof(XhciTrb_t),
        .Status = code << 24,
        .Control = XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_EP_ID(3)
    };

    CHECK(XhciTransferHandleEvent(&g_cancelController, &event));
    CHECK(!XhciTransferHandleEvent(&g_cancelController, &event));
    CHECK(g_cancelController.Base.ReadyTransfers.count == 1);
    UsbManagerProcessTransfers(&g_cancelController.Base);
}

static void
__DestroyRecovery(
    _In_ struct __RecoveryFixture* fixture)
{
    CHECK(g_destroyed == 1 && g_maps == 2);
    CHECK(fixture->Endpoint.Pending.head == NULL);
    CHECK(g_cancelController.Base.ReadyTransfers.head == NULL);
    CHECK(XhciRingDestroy(&fixture->Endpoint.TransferRing) == OS_EOK);
    CHECK(XhciRingDestroy(&g_cancelController.CommandRing) == OS_EOK);
    CHECK(g_maps == 0 && g_stagingBuffers == 0);
}

static void
__TestStallRecovery(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;
    XhciTransferDescriptor_t* descriptor;
    unsigned int cycle;
    unsigned int notifications;

    __InitializeRecovery(&fixture, &transfer);
    for (cycle = 0; cycle < 3; cycle++) {
        __CompleteRecoveryTransfer(&transfer, 6);
        CHECK(transfer.State == USBTRANSFER_STATE_STALLED);
        CHECK(!transfer.Dma.DeviceOwned && !transfer.Dma.HardwareActive);
        descriptor = transfer.RootElement;
        CHECK(descriptor != NULL && fixture.Endpoint.Pending.head == &descriptor->QueueHeader);
        CHECK(g_lastResult == USBTRANSFERCODE_STALL);
        notifications = g_notifications;
        UsbManagerProcessTransfers(&g_cancelController.Base);
        CHECK(g_notifications == notifications);

        CHECK(HCIEndpointReset(&g_cancelController.Base, &transfer.Address) == OS_EOK);
        CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_RESET_PENDING);
        __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
        CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_DEQUEUE_PENDING);
        CHECK(!transfer.Dma.HardwareActive);
        __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
        CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_RUNNING);
        CHECK(transfer.State == USBTRANSFER_STATE_QUEUED);
        CHECK(transfer.Dma.DeviceOwned && transfer.Dma.HardwareActive);
        CHECK(descriptor->Flags == XHCI_TD_FLAG_QUEUED);
        CHECK(descriptor->CompletionCode == 0 && descriptor->BytesTransferred == 0);

        __CompleteRecoveryTransfer(&transfer, XHCI_TRB_COMPLETION_SUCCESS);
        CHECK(g_notifications == notifications + 1 && g_lastResult == USBTRANSFERCODE_SUCCESS);
        CHECK(transfer.Dma.HardwareActive);
    }
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(g_lastResult == USBTRANSFERCODE_CANCELLED);
    __DestroyRecovery(&fixture);
}

static void
__TestStallFailureAndCancel(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;
    unsigned int notifications;

    __InitializeRecovery(&fixture, &transfer);
    __CompleteRecoveryTransfer(&transfer, 6);
    CHECK(HCIEndpointReset(&g_cancelController.Base, &transfer.Address) == OS_EOK);
    __CompleteRecoveryCommand(5);
    CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_FAILED);
    CHECK(transfer.State == USBTRANSFER_STATE_STALLED && !transfer.Dma.HardwareActive);
    notifications = g_notifications;
    UsbManagerProcessTransfers(&g_cancelController.Base);
    CHECK(g_notifications == notifications);
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    CHECK(g_lastResult == USBTRANSFERCODE_CANCELLED);
    __DestroyRecovery(&fixture);
}

static void
__TestCancelDuringReset(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;
    unsigned int notifications;

    __InitializeRecovery(&fixture, &transfer);
    __CompleteRecoveryTransfer(&transfer, 6);
    CHECK(HCIEndpointReset(&g_cancelController.Base, &transfer.Address) == OS_EOK);
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    CHECK(g_destroyed == 1 && g_lastResult == USBTRANSFERCODE_CANCELLED);
    notifications = g_notifications;
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(g_destroyed == 1 && g_notifications == notifications);
    CHECK(!transfer.Dma.HardwareActive && fixture.Endpoint.TransferRing.Used == 0);
    __DestroyRecovery(&fixture);
}

static void
__TestRecoverySyncFailures(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;

    __InitializeRecovery(&fixture, &transfer);
    g_failSync = 1;
    __CompleteRecoveryTransfer(&transfer, 6);
    CHECK(g_notifications == 0 && transfer.Dma.DeviceOwned);
    CHECK(g_cancelController.Base.ReadyTransfers.count == 1);
    g_failSync = 0;
    UsbManagerProcessTransfers(&g_cancelController.Base);
    CHECK(g_notifications == 1 && transfer.State == USBTRANSFER_STATE_STALLED);
    CHECK(HCIEndpointReset(&g_cancelController.Base, &transfer.Address) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    g_failSync = 1;
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    g_failSync = 0;
    CHECK(g_destroyed == 1 && g_lastResult == USBTRANSFERCODE_INVALID);
    __DestroyRecovery(&fixture);
}

#include "xhci_dma_recovery_test.inc"

int
main(void)
{
    __TestStallRecovery();
    __TestStallFailureAndCancel();
    __TestCancelDuringReset();
    __TestRecoverySyncFailures();
    __TestCancelReclaimRetry();
    __TestReclaimBeforeResubmit();
    __TestDetachReleaseRetries();
    __TestStoppedRingInvalidation();
    puts("xHCI recovery: timer reclaim, all-payload handoff, detach release retries, slot reuse and shutdown passed");
    puts("xHCI/common manager: repeated STALL/reset/rearm, duplicate events, failed reset, cancel during reset and sync failures passed");
    return 0;
}
