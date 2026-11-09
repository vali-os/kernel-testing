// Unrelated protocol and legacy-controller operations must not run here.
#include <stdlib.h>
void spinlock_init(void) { }
void spinlock_acquire(void) { }
void spinlock_release(void) { }
void SystemDebug(void) { }
void gracht_server_defer_message(void) { }
void SHMGetSGTable(void) { abort(); }
void HCITransferElementsNeeded(void) { abort(); }
void HCITransferElementFill(void) { abort(); }
void HCITransferQueue(void) { abort(); }
void HCITransferQueueIsochronous(void) { abort(); }
void HCITransferDequeue(void) { abort(); }
void HCIProcessElement(void) { abort(); }
void HCIProcessEvent(void) { abort(); }
void __crt_get_module_server(void) { abort(); }
void ctt_usbhost_queue_response(void) { abort(); }
void ctt_usbhost_queue_periodic_response(void) { abort(); }
void ctt_usbhost_reset_periodic_response(void) { abort(); }
void ctt_usbhost_dequeue_response(void) { abort(); }
void ctt_usbhost_event_transfer_status_single(void) { abort(); }
void
WaitForUsbService(void)
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
eventd(void)
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
hashtable_set(void)
{
    abort();
}

void
usched_job_queue(void)
{
    abort();
}

void
timespec_get(void)
{
    abort();
}

void
usched_is_cancelled(void)
{
    abort();
}

void
usched_job_sleep(void)
{
    abort();
}

void
hashtable_enumerate(void)
{
    abort();
}

void
UsbControllerRegister(void)
{
    abort();
}

void
hashtable_remove(void)
{
    abort();
}

void
UsbControllerUnregister(void)
{
    abort();
}

void
hashtable_get(void)
{
    abort();
}

void
ctt_usbhost_reset_endpoint_response(void)
{
    abort();
}

void
HCIEndpointReset(void)
{
    abort();
}

void
ctt_usbhost_configure_hub_response(void)
{
    abort();
}

void
HCIConfigureHub(void)
{
    abort();
}

void
ctt_usbhost_device_detach_response(void)
{
    abort();
}

void
HCIDeviceDetach(void)
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
HciTimerCallback(void)
{
    abort();
}

void
HCIControllerDestroy(void)
{
    abort();
}

void
HCITransferFinalize(void)
{
    abort();
}

void
UsbSchedulerGetPoolFromElement(void)
{
    abort();
}

void
UsbTransferDmaComplete(void)
{
    abort();
}

