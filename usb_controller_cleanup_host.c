// Unrelated creation/transfer callbacks are outside the teardown fixture.
#include <stdlib.h>
void spinlock_init(void) { }
void spinlock_acquire(void) { }
void spinlock_release(void) { }
void usched_job_sleep(void) { }
void thrd_sleep(void) { }
int timespec_get(void) { return 0; }
void timespec_add(void) { }
void usched_job_queue(void) { abort(); }
void usched_is_cancelled(void) { abort(); }
void WaitForUsbService(void) { abort(); }
void eventd(void) { abort(); }
void* __crt_get_module_server(void) { return NULL; }
int gracht_server_get_aio_handle(void) { return -1; }
int ioset_ctrl(void) { return 0; }
void ioctl(void) { abort(); }
void UsbControllerRegister(void) { abort(); }
void ctt_usbhost_reset_endpoint_response(void) { abort(); }
void ctt_usbhost_configure_hub_response(void) { abort(); }
void ctt_usbhost_device_detach_response(void) { abort(); }
void DeviceInterruptInitialize(void) { abort(); }
void DeviceInterruptAllocate(void) { abort(); }
void DeviceInterruptSetHandler(void) { abort(); }
void DeviceInterruptAddIoResource(void) { abort(); }
void DeviceInterruptAddMemoryResource(void) { abort(); }
void DeviceInterruptSetDescriptor(void) { abort(); }
void USBTransferDestroy(void) { abort(); }
void USBTransferNotify(void) { abort(); }
void USBTransferDmaComplete(void) { abort(); }
void HCITransferFinalize(void) { abort(); }
void HCIProcessElement(void) { abort(); }
void HCIProcessEvent(void) { abort(); }
void HCITransferQueue(void) { abort(); }
void HCITransferQueueIsochronous(void) { abort(); }
void HCIEndpointReset(void) { abort(); }
void HCIConfigureHub(void) { abort(); }
void HCIDeviceDetach(void) { abort(); }
int* __errno(void) { static int error; return &error; }
void DeviceGetResources(void) { abort(); }
void AcquireDeviceIo(void) { abort(); }
void OnFastInterrupt(void) { abort(); }
void DeviceInterruptEnable(void) { abort(); }
void XhciQueueInitialize(void) { abort(); }
void XhciPortScan(void) { abort(); }
void* dsalloc(size_t size) { return malloc(size); }
void dsfree(void* data) { free(data); }
void UsbManagerForgetTransferWork(void) { abort(); }
void UsbManagerQueueTransferWork(void) { abort(); }
void UsbSchedulerGetPoolFromElement(void) { abort(); }
void USBTransferDmaPrepare(void) { abort(); }
int close(int descriptor) { (void)descriptor; return 0; }
