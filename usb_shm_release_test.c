// Use the real transfer lifetime and work queues with controlled SHM failures.
#include "../modules/serial/usb/common/hci.h"
#define UsbManagerGetController __UnusedGetController
#include "../modules/serial/usb/common/manager.c"
#undef UsbManagerGetController
#include "../modules/serial/usb/common/transfer/transfer.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static UsbManagerController_t g_controller;
static oserr_t g_prepareStatus;
static oserr_t g_destroyStatus;
static oserr_t g_releaseStatus;
static int g_releaseCalls;
static int g_attachments;

UsbManagerController_t*
UsbManagerGetController(
    uuid_t id)
{
    CHECK(id == 7);
    return &g_controller;
}

oserr_t
SHMAttachToken(
    const GrantToken_t* token,
    OSHandle_t* handle)
{
    CHECK(token->ExportGlobalHandleID == 7);
    handle->ID = 42;
    handle->Type = OSHANDLE_SHM;
    g_attachments++;
    return OS_EOK;
}

oserr_t
SHMRelease(
    OSHandle_t* handle)
{
    if (handle->ID == UUID_INVALID) {
        return OS_EOK;
    }
    g_releaseCalls++;
    if (g_releaseStatus != OS_EOK) {
        return g_releaseStatus;
    }
    g_attachments--;
    memset(handle, 0, sizeof(*handle));
    return OS_EOK;
}

oserr_t
UsbTransferDmaPrepare(
    UsbManagerTransfer_t* transfer)
{
    CHECK(transfer->Dma.Context == g_controller.DmaContext);
    return g_prepareStatus;
}

oserr_t
UsbTransferDmaDestroy(
    UsbManagerTransfer_t* transfer)
{
    (void)transfer;
    return g_destroyStatus;
}

int
main(
    void)
{
    struct gracht_message message = { 0 };
    USBTransfer_t metadata = {
        .Type = USBTRANSFER_TYPE_BULK,
        .MaxPacketSize = 64,
        .Length = 64,
        .BufferHandle = 42
    };
    struct ctt_usb_transfer request;
    SHMTransfer_t buffer = {
        .Token = { .Version = GRANT_TOKEN_VERSION, .ControlHandleID = 7, .ExportGlobalHandleID = 7,
            .Options = { .ResourceType = GrantTokenResourceTypeSHM,
                .TargetType = GrantTokenTargetTypeAny, .TargetGlobalHandleID = UUID_INVALID,
                .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
                .Data.SHM = { .Offset = 4096, .Length = 4096 } } },
        .RangeMode = SHMTransferRangeSubrange, .RelativeOffset = 13, .RequestedLength = 64
    };
    UsbManagerTransfer_t* transfer = NULL;
    int calls;

    CHECK(__ToUSBTransfer(&metadata, &buffer, &request) == OS_EOK);

    g_controller.DmaContext = (struct DMAContext*)1;
    CHECK(USBTransferCreate(&message, &request, 8, 7, &transfer) == OS_EOK);
    CHECK(g_controller.TransactionList.count == 1 && g_attachments == 1);
    UsbManagerQueueTransferWork(&g_controller, transfer);
    g_destroyStatus = OS_EBUSY;
    CHECK(UsbTransferDestroy(transfer) == OS_EBUSY && g_releaseCalls == 0);
    g_destroyStatus = OS_EOK;
    g_releaseStatus = OS_EUNKNOWN;
    CHECK(UsbTransferDestroy(transfer) == OS_EUNKNOWN);
    CHECK(g_attachments == 1 && g_controller.TransactionList.head->value == transfer);
    CHECK(g_controller.ReadyTransfers.head->value == transfer);
    g_releaseStatus = OS_EOK;
    CHECK(UsbTransferDestroy(transfer) == OS_EOK);
    CHECK(g_controller.TransactionList.count == 0 && g_controller.ReadyTransfers.count == 0);
    CHECK(g_attachments == 0);

    // The failed creator returns no transfer. Its rollback owner must still be
    // indexed and queued, otherwise the retained SHM reference becomes a leak.
    transfer = NULL;
    g_prepareStatus = OS_EOOM;
    g_releaseStatus = OS_EUNKNOWN;
    CHECK(USBTransferCreate(&message, &request, 9, 7, &transfer) == OS_EOOM);
    CHECK(transfer == NULL && g_attachments == 1);
    CHECK(g_controller.TransactionList.count == 1 && g_controller.ReadyTransfers.count == 1);
    transfer = g_controller.TransactionList.head->value;
    CHECK(transfer->State == USBTRANSFER_STATE_CLEANUP);
    CHECK(transfer->ResultCode == USBTRANSFERCODE_INVALID);
    CHECK(transfer->Flags & __USBTRANSFER_FLAG_SILENT);
    g_releaseStatus = OS_EOK;
    CHECK(UsbTransferDestroy(transfer) == OS_EOK && g_attachments == 0);
    CHECK(g_controller.TransactionList.count == 0 && g_controller.ReadyTransfers.count == 0);

    // A queue rejection has the same deferred-cleanup contract as creation.
    g_prepareStatus = OS_EOK;
    CHECK(USBTransferCreate(&message, &request, 10, 7, &transfer) == OS_EOK);
    g_releaseStatus = OS_EUNKNOWN;
    __DiscardTransfer(transfer);
    CHECK(g_controller.ReadyTransfers.head->value == transfer);
    CHECK(transfer->Flags & __USBTRANSFER_FLAG_SILENT);
    g_releaseStatus = OS_EOK;
    calls = g_releaseCalls;
    CHECK(transfer->ResultCode == USBTRANSFERCODE_INVALID);
    CHECK(UsbTransferDestroy(transfer) == OS_EOK && g_releaseCalls == calls + 1);
    CHECK(g_attachments == 0 && g_controller.TransactionList.count == 0);
    puts("USB SHM release: DMA ordering, indexed retries, failed creation and queue rejection passed");
    return 0;
}
