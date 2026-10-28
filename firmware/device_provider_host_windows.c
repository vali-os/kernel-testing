// Windows linking retains these unrelated service paths. The provider fixture
// must fail if any is reached instead of silently pretending it succeeded.
#include <stdlib.h>
void GetGrachtClient(void) { abort(); }
void ctt_driver_register_device(void) { abort(); }
void ctt_driver_get_device_protocols(void) { abort(); }
void __crt_get_service_server(void) { abort(); }
void sys_device_event_protocol_device_single(void) { abort(); }
void sys_device_event_protocol_device_all(void) { abort(); }
void DeviceInterruptMessageControllerResolve(void) { abort(); }

void PciToDevClass(void) { abort(); }
void PciToDevSubClass(void) { abort(); }
void PciToString(void) { abort(); }
void PciRegisterBars(void) { abort(); }
void FdtFindResources(void) { abort(); }
void FdtCompatible(void) { abort(); }
void FdtScalar(void) { abort(); }
void FdtProperty(void) { abort(); }
