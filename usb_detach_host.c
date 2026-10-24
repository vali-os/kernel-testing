// Unrelated libusb paths must not run in the detach routing fixture.
#include <stdlib.h>

void SHMCreate(void) { abort(); }
void dma_pool_create(void) { abort(); }
void OSHandleDestroy(void) { abort(); }
void dma_pool_destroy(void) { abort(); }
void OSProcessCurrentID(void) { abort(); }
void ctt_usbhost_queue(void) { abort(); }
void ctt_usbhost_queue_result(void) { abort(); }
void ctt_usbhost_queue_periodic(void) { abort(); }
void ctt_usbhost_queue_periodic_result(void) { abort(); }
void ctt_usbhost_reset_periodic(void) { abort(); }
void ctt_usbhost_reset_periodic_result(void) { abort(); }
void ctt_usbhost_dequeue(void) { abort(); }
void ctt_usbhost_dequeue_result(void) { abort(); }
void ctt_usbhub_reset_port(void) { abort(); }
void ctt_usbhub_reset_port_result(void) { abort(); }
void ctt_usbhub_query_port(void) { abort(); }
void ctt_usbhub_query_port_result(void) { abort(); }
void ctt_usbhost_reset_endpoint(void) { abort(); }
void ctt_usbhost_reset_endpoint_result(void) { abort(); }
void dma_pool_allocate(void) { abort(); }
void SystemDebug(void) { abort(); }
void dma_pool_offset(void) { abort(); }
void dma_pool_handle(void) { abort(); }
void dma_pool_free(void) { abort(); }
void mstr_new_u16(void) { abort(); }
