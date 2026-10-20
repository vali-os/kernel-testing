// Verify the wire layouts under each target ABI, including 32-bit i386.
#include <ddk/types/interrupt.h>
#include <ddk/types/pci-interrupt.h>
#include <stddef.h>

_Static_assert(sizeof(DeviceInterruptMessage_t) == 12, "message wire size");
_Static_assert(offsetof(DeviceInterruptMessage_t, AddressHigh) == 4, "message high address offset");
_Static_assert(offsetof(DeviceInterruptMessage_t, Data) == 8, "message data offset");
_Static_assert(sizeof(struct PciInterruptSupport) == 12, "PCI support wire size");
_Static_assert(sizeof(struct PciInterruptRequest) == 36, "PCI interrupt request wire size");
_Static_assert(offsetof(struct PciInterruptRequest, Strategy) == 8, "PCI interrupt strategy offset");
_Static_assert(offsetof(struct PciInterruptRequest, Count) == 12, "PCI interrupt count offset");
_Static_assert(offsetof(struct PciInterruptRequest, Index) == 16, "PCI interrupt index offset");
_Static_assert(offsetof(struct PciInterruptRequest, AddressHigh) == 24, "PCI interrupt high address offset");
_Static_assert(offsetof(struct PciInterruptSupport, MsiCapacity) == 4, "MSI capacity offset");
_Static_assert(offsetof(struct PciInterruptSupport, MsixCapacity) == 8, "MSI-X capacity offset");
