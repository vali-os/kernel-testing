// Compile the real work queues separately from the fixture's controller stubs.
#define __get_reserved __UnusedManagerGetReserved
#define __set_reserved __UnusedManagerSetReserved
#define UsbManagerGetController __UnusedManagerGetController
#define UsbManagerChainEnumerate __UnusedManagerChainEnumerate
#define UsbManagerProcessTransfers __UnusedManagerProcessTransfers
#define UsbManagerClearTransfers __UnusedManagerClearTransfers
#include "../modules/serial/usb/common/manager.c"
