// Unrelated HCI paths must not run in the DMA fixture.
#include <stdlib.h>
void UsbSchedulerResetInternalData(void) { abort(); }
void UsbSchedulerDestroy(void) { abort(); }
void XhciHalt(void) { abort(); }
void UsbManagerClearTransfers(void) { abort(); }
// Expected cleanup failures now emit a warning while retaining their owner.
void SystemDebug(void) { }
void timespec_get(void) { abort(); }
void timespec_add(void) { abort(); }
void usched_job_sleep(void) { abort(); }
void __errno(void) { abort(); }
// The fixture executes one thread; lists still use the production links.
void spinlock_init(void) { }
void spinlock_acquire(void) { }
void spinlock_release(void) { }
void
hashtable_set(void)
{
    abort();
}

void
hashtable_get(void)
{
    abort();
}

void
hashtable_enumerate(void)
{
    abort();
}

void
hashtable_construct(void)
{
    abort();
}

void
hashtable_destroy(void)
{
    abort();
}

void
hashtable_remove(void)
{
    abort();
}

void
WaitForUsbService(void)
{
    abort();
}

void
usched_job_queue(void)
{
    abort();
}

void
usched_is_cancelled(void)
{
    abort();
}

void
eventd(void)
{
    abort();
}

void
__crt_get_module_server(void)
{
    abort();
}

void
gracht_server_get_aio_handle(void)
{
    abort();
}

void
ioset_ctrl(void)
{
    abort();
}

void
ioctl(void)
{
    abort();
}

void
UsbControllerRegister(void)
{
    abort();
}

void
UsbControllerUnregister(void)
{
    abort();
}

void
ctt_usbhost_reset_endpoint_response(void)
{
    abort();
}

void
ctt_usbhost_configure_hub_response(void)
{
    abort();
}

void
ctt_usbhost_device_detach_response(void)
{
    abort();
}

void
DeviceInterruptInitialize(void)
{
    abort();
}

void
DeviceInterruptAllocate(void)
{
    abort();
}

void
DeviceInterruptSetHandler(void)
{
    abort();
}

void
DeviceInterruptAddIoResource(void)
{
    abort();
}

void
DeviceInterruptAddMemoryResource(void)
{
    abort();
}

void
DeviceInterruptSetDescriptor(void)
{
    abort();
}

void
HCIControllerDestroy(void)
{
    abort();
}

void
USBTransferNotify(void)
{
    abort();
}

void
USBTransferDestroy(void)
{
    abort();
}

void
UsbSchedulerGetPoolFromElement(void)
{
    abort();
}

