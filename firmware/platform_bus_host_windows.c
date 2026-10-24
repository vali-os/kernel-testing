// Unrelated retained PE sections must never be reached by this fixture.
#include <stdlib.h>
void GetGrachtClient(void) { abort(); }
void ctt_driver_register_device(void) { abort(); }
void ctt_driver_get_device_protocols(void) { abort(); }
void __crt_get_service_server(void) { abort(); }
void sys_device_event_protocol_device_single(void) { abort(); }
void sys_device_event_protocol_device_all(void) { abort(); }
void OSDeviceIOCtl2(void) { abort(); }
