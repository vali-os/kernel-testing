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
void UsbManagerChainEnumerate(void) { abort(); }
void __crt_get_module_server(void) { abort(); }
void ctt_usbhost_queue_response(void) { abort(); }
void ctt_usbhost_queue_periodic_response(void) { abort(); }
void ctt_usbhost_reset_periodic_response(void) { abort(); }
void ctt_usbhost_dequeue_response(void) { abort(); }
void ctt_usbhost_event_transfer_status_single(void) { abort(); }
