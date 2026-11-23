// Unrelated libusb paths must not run in the detach routing fixture.
#include <stdlib.h>

void SHMCreate(void) { abort(); }
void BufferPoolCreate(void) { abort(); }
void OSHandleDestroy(void) { abort(); }
void BufferPoolDestroy(void) { abort(); }
void OSProcessCurrentID(void) { abort(); }
void ctt_usbhost_queue(void) { abort(); }
void ctt_usbhost_queue_result(void) { abort(); }
void ctt_usbhost_queue_periodic(void) { abort(); }
void ctt_usbhost_queue_periodic_result(void) { abort(); }
void ctt_usbhost_reset_periodic(void) { abort(); }
void ctt_usbhost_reset_periodic_result(void) { abort(); }
void ctt_usbhost_dequeue(void) { abort(); }
void ctt_usbhost_dequeue_result(void) { abort(); }
void ctt_usbhost_reset_endpoint(void) { abort(); }
void ctt_usbhost_reset_endpoint_result(void) { abort(); }
void BufferPoolAllocate(void) { abort(); }
void OSSysLogWrite(void) { abort(); }
void BufferPoolOffset(void) { abort(); }
void BufferPoolHandle(void) { abort(); }
void BufferPoolFree(void) { abort(); }
void mstr_new_u16(void) { abort(); }
