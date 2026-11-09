// These fixtures exercise controllers/transfers without packet bounce. Fail if
// they unexpectedly enter the new path; uhci_bounce_test uses the real helper.
#define __get_reserved __UnusedBounceGetReserved
#define __set_reserved __UnusedBounceSetReserved
#include <stdlib.h>
#include "../modules/serial/usb/common/transfer/transfer.h"

oserr_t g_bounceReserveStatus;

static void
__NoPacketBounce(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    if (transfer->Dma.Packets != NULL) {
        abort();
    }
}

oserr_t
UsbTransferBounceInitialize(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return OS_EOK;
}

oserr_t
UsbTransferBounceReserve(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return g_bounceReserveStatus;
}

bool
UsbTransferBounceCanRetry(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return true;
}

oserr_t
UsbTransferBouncePrepare(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return OS_EOK;
}

oserr_t
UsbTransferBounceComplete(
    _InOut_ struct UsbManagerTransfer* transfer,
    _In_    size_t bytes)
{
    (void)bytes;
    __NoPacketBounce(transfer);
    return OS_EOK;
}

oserr_t
UsbTransferBounceAbort(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return OS_EOK;
}

oserr_t
UsbTransferBounceDestroy(
    _InOut_ struct UsbManagerTransfer* transfer)
{
    __NoPacketBounce(transfer);
    return OS_EOK;
}

oserr_t
UsbTransferBouncePoolCreate(
    _In_    struct DMAContext* context,
    _In_    uint32_t count,
    _InOut_ struct UsbTransferBouncePool** pool)
{
    (void)context;
    (void)count;
    (void)pool;
    abort();
}

oserr_t
UsbTransferBouncePoolDestroy(
    _InOut_ struct UsbTransferBouncePool** pool)
{
    if (*pool != NULL) {
        abort();
    }
    return OS_EOK;
}
