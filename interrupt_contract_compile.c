// Verify the wire layouts under each target ABI, including 32-bit i386.
#include <ddk/types/interrupt.h>
#include <ddk/types/pci-interrupt.h>
#include <ddk/interrupt-provider.h>
#include <ddk/interrupt.h>
#include <stddef.h>

_Static_assert(sizeof(InterruptMessageControllerIdentity_t) == 16, "domain query wire size");
_Static_assert(offsetof(InterruptMessageControllerIdentity_t, FirmwareNode) == 8, "firmware identity offset");
_Static_assert(offsetof(InterruptGroupRequest_t, ControllerID) == 0, "allocation controller offset");
_Static_assert(offsetof(InterruptBindRequest_t, Resources) == _Alignof(InterruptResourceTable_t), "child resource offset");
_Static_assert(sizeof(struct InterruptProviderVector) == 16, "provider vector wire size");
_Static_assert(offsetof(struct InterruptProviderVector, Message) == 4, "provider message offset");
_Static_assert(offsetof(struct InterruptProviderAttachRequest, Vectors) ==
    (sizeof(void*) == 8 ? 24 : 20), "provider attachment array offset");
_Static_assert(sizeof(struct InterruptProviderAttachRequest) ==
    (sizeof(void*) == 8 ? 32 : 24), "provider attachment wire size");

_Static_assert(sizeof(DeviceInterruptMessage_t) == 12, "message wire size");
_Static_assert(offsetof(DeviceInterruptMessage_t, AddressHigh) == 4, "message high address offset");
_Static_assert(offsetof(DeviceInterruptMessage_t, Data) == 8, "message data offset");
_Static_assert(sizeof(struct PciInterruptSupport) == 16, "PCI support wire size");
_Static_assert(sizeof(struct PciInterruptRequest) == 36, "PCI interrupt request wire size");
_Static_assert(offsetof(struct PciInterruptRequest, Strategy) == 8, "PCI interrupt strategy offset");
_Static_assert(offsetof(struct PciInterruptRequest, Count) == 12, "PCI interrupt count offset");
_Static_assert(offsetof(struct PciInterruptRequest, Index) == 16, "PCI interrupt index offset");
_Static_assert(offsetof(struct PciInterruptRequest, AddressHigh) == 24, "PCI interrupt high address offset");
_Static_assert(offsetof(struct PciInterruptSupport, MsiCapacity) == 4, "MSI capacity offset");
_Static_assert(offsetof(struct PciInterruptSupport, MsixCapacity) == 8, "MSI-X capacity offset");

_Static_assert(offsetof(struct PciInterruptSupport, ControllerID) == 12, "PCI controller offset");
