/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Native SMMUv3 self-test: PCI setup and register protocol of QEMU's
 * iommu-testdev. Compiled only with VALI_ARM64_SMMU_SELFTEST, or into the
 * host test with ARM_SMMU_NATIVE_PCI_HOST_TEST.
 *
 * This file checks a fixed QEMU topology, assigns BAR0 of both endpoints from
 * the ACPI PCI root window and runs single requests. It uses the kernel's
 * copied ACPI root list (AcpiPciQuery) and the MCFG-backed configuration
 * access (AcpiPciConfig). It does not enumerate other PCI hardware and is not
 * a PCI driver. Register layout reference: hw/misc/iommu-testdev.c and
 * include/hw/misc/iommu-testdev.h of QEMU v11.1.3.
 */

#include "native_private.h"
#include "../../kernel/acpi/pci.h"
#include "../../kernel/arch/aarch64/common/device.h"
#include <debug.h>
#include <string.h>

// PCI configuration space offsets (type 0 header).
#define __PCI_VENDOR_DEVICE   0x00U // 32-bit: vendor ID (low 16), device ID (high 16).
#define __PCI_COMMAND         0x04U // 16-bit command register. Status follows at 0x06.
#define __PCI_CLASS_REVISION  0x08U // 32-bit: revision (7:0), prog-if, subclass, class (31:24).
#define __PCI_HEADER_TYPE     0x0EU // 8-bit: layout (6:0) and multi-function flag (7).
#define __PCI_BAR0            0x10U // First of six 32-bit BAR registers.
#define __PCI_BAR_COUNT       6U

// Command register bits that let a function respond to or start bus cycles.
#define __PCI_COMMAND_IO         0x0001U // Respond to I/O space accesses.
#define __PCI_COMMAND_MEMORY     0x0002U // Respond to memory space accesses (BAR decode).
#define __PCI_COMMAND_BUS_MASTER 0x0004U // Allow the function to issue DMA.
#define __PCI_COMMAND_DECODE     (__PCI_COMMAND_IO | __PCI_COMMAND_MEMORY | __PCI_COMMAND_BUS_MASTER)

// BAR low bits: bit 0 selects I/O space, bits 2:1 the memory BAR width and
// bit 3 the prefetchable flag. The rest holds the address.
#define __PCI_BAR_FLAGS_MASK     0x0000000FU
#define __PCI_BAR_ADDRESS_MASK   0xFFFFFFF0U

// The value of the header type byte for a single-function type 0 header.
#define __PCI_HEADER_ENDPOINT    0x00U
#define __PCI_HEADER_MULTI       0x80U

// A read of all ones means no function answered at that address.
#define __PCI_VENDOR_ABSENT      0xFFFFU

// Fixed QEMU identities. iommu-testdev shares 1b36:0005 with pci-testdev, so
// the class, BAR layout and register reset values are checked as well.
#define __QEMU_VENDOR_ID         0x1B36U     // PCI_VENDOR_ID_REDHAT.
#define __QEMU_HOST_DEVICE_ID    0x0008U     // PCI_DEVICE_ID_REDHAT_PCIE_HOST (gpex-root).
#define __QEMU_TEST_DEVICE_ID    0x0005U     // PCI_DEVICE_ID_REDHAT_TEST.
#define __QEMU_HOST_CLASS        0x06000000U // Host bridge, revision 0.
#define __QEMU_TEST_CLASS        0x00FF0000U // Class 0x00, subclass 0xFF (PCI_CLASS_OTHERS), revision 0.

// Fixed slots of the fixture on the root bus: host bridge, then both endpoints.
#define __HOST_BRIDGE_SLOT       0U
#define __FIRST_TEST_SLOT        1U
#define __PCI_SLOT_COUNT         32U
#define __PCI_FUNCTION_COUNT     8U

// Upper limits for the copied ACPI root windows and the 32-bit BAR.
#define __MAX_WINDOWS            16U
#define __BAR32_LIMIT            0xFFFFFFFFULL

// iommu-testdev BAR0 register offsets. All accesses are 32 bits wide.
#define __ITD_TRIGGER            0x00U // Read: runs the armed request. Always returns 0.
#define __ITD_IOVA_LOW           0x04U // Device address, bits 31:0.
#define __ITD_IOVA_HIGH          0x08U // Device address, bits 63:32.
#define __ITD_LENGTH             0x0CU // Request length in bytes.
#define __ITD_RESULT             0x10U // Result code, see ARM_SMMU_TEST_RESULT_*.
#define __ITD_DOORBELL           0x14U // Write only in practice: bit 0 arms, 0 disarms.
#define __ITD_ATTRIBUTES         0x18U // Bus attributes used for the device write.
#define __ITD_PHYSICAL_LOW       0x1CU // Physical readback address, bits 31:0.
#define __ITD_PHYSICAL_HIGH      0x20U // Physical readback address, bits 63:32.

// Doorbell values. Arming sets RESULT to BUSY, disarming sets it to IDLE.
#define __ITD_DOORBELL_ARM       0x1U
#define __ITD_DOORBELL_DISARM    0x0U

// Attribute register fields: bit 0 secure, bits 2:1 Arm security space, bit 3
// marks the space field valid. NonSecure is space 1 with the secure bit clear.
#define __ITD_ATTR_SPACE_NONSECURE (1U << 1)
#define __ITD_ATTR_SPACE_VALID     (1U << 3)
#define __ITD_ATTR_NONSECURE       (__ITD_ATTR_SPACE_VALID | __ITD_ATTR_SPACE_NONSECURE)

// Attribute value after device reset: NonSecure space without the valid bit.
#define __ITD_ATTR_RESET           __ITD_ATTR_SPACE_NONSECURE

// Host tests compile with ARM_SMMU_NATIVE_PCI_HOST_TEST to send register
// accesses to a model. The kernel build has no runtime dispatch.
#if defined(ARM_SMMU_NATIVE_PCI_HOST_TEST)
uint32_t ArmSMMUTestHostRead32(uintptr_t address);
void ArmSMMUTestHostWrite32(uintptr_t address, uint32_t value);
void ArmSMMUTestHostBarrier(void);
#define __DeviceRead32  ArmSMMUTestHostRead32
#define __DeviceWrite32 ArmSMMUTestHostWrite32
#define __DeviceBarrier ArmSMMUTestHostBarrier
#else
static inline uint32_t
__DeviceRead32(
    _In_ uintptr_t address)
{
    return *(volatile uint32_t*)address;
}

static inline void
__DeviceWrite32(
    _In_ uintptr_t address,
    _In_ uint32_t  value)
{
    *(volatile uint32_t*)address = value;
}

// Full-system barrier. Orders CPU writes to the test pages before the device
// request, and the device result before later CPU reads of those pages.
static inline void
__DeviceBarrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

// One PCI function address on a segment.
typedef struct __PciFunction {
    uint16_t Segment;
    uint8_t  Bus;
    uint8_t  Device;
    uint8_t  Function;
} __PciFunction_t;

// The single ACPI PCI root of the fixture and its copied CPU windows.
typedef struct __PciRootInfo {
    struct AcpiPciRoot      Root;
    struct PciAddressWindow Windows[__MAX_WINDOWS];
    uint32_t                WindowCount;
} __PciRootInfo_t;

static __PciFunction_t
__FunctionFromDevice(
    _In_ const ArmSMMUTestDevice_t* device)
{
    __PciFunction_t function;

    function.Segment = device->Segment;
    function.Bus = (uint8_t)(device->RequesterId >> 8);
    function.Device = (uint8_t)((device->RequesterId >> 3) & 0x1FU);
    function.Function = (uint8_t)(device->RequesterId & 0x7U);
    return function;
}

// Reads one configuration register. width is 8, 16 or 32 bits.
static oserr_t
__ConfigRead(
    _In_  const __PciFunction_t* function,
    _In_  uint32_t               offset,
    _In_  uint32_t               width,
    _Out_ uint32_t*              valueOut)
{
    ACPI_PCI_ID identifier;
    UINT64      value = 0;
    ACPI_STATUS status;

    identifier.Segment = function->Segment;
    identifier.Bus = function->Bus;
    identifier.Device = function->Device;
    identifier.Function = function->Function;
    status = AcpiPciConfig(&identifier, offset, &value, width, 0);
    if (ACPI_FAILURE(status)) {
        ERROR("__ConfigRead: %u:%02x:%02x.%u offset 0x%x width %u failed, ACPI status 0x%x",
              function->Segment, function->Bus, function->Device, function->Function,
              offset, width, status);
        return OS_EDEVFAULT;
    }
    *valueOut = (uint32_t)value;
    return OS_EOK;
}

// Writes one configuration register. width is 8, 16 or 32 bits.
static oserr_t
__ConfigWrite(
    _In_ const __PciFunction_t* function,
    _In_ uint32_t               offset,
    _In_ uint32_t               width,
    _In_ uint32_t               value)
{
    ACPI_PCI_ID identifier;
    UINT64      writeValue = value;
    ACPI_STATUS status;

    identifier.Segment = function->Segment;
    identifier.Bus = function->Bus;
    identifier.Device = function->Device;
    identifier.Function = function->Function;
    status = AcpiPciConfig(&identifier, offset, &writeValue, width, 1);
    if (ACPI_FAILURE(status)) {
        ERROR("__ConfigWrite: %u:%02x:%02x.%u offset 0x%x width %u value 0x%x failed, ACPI status 0x%x",
              function->Segment, function->Bus, function->Device, function->Function,
              offset, width, value, status);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// Clears the bits in clearMask and sets the bits in setMask of the 16-bit
// command register, then reads it back. A 16-bit access cannot write the
// status register at 0x06, whose bits are cleared by writing ones.
static oserr_t
__UpdateCommand(
    _In_ const __PciFunction_t* function,
    _In_ uint32_t               clearMask,
    _In_ uint32_t               setMask)
{
    uint32_t command;
    uint32_t readback;
    oserr_t  status;

    status = __ConfigRead(function, __PCI_COMMAND, 16, &command);
    if (status != OS_EOK) {
        return status;
    }

    command = (command & ~clearMask) | setMask;
    status = __ConfigWrite(function, __PCI_COMMAND, 16, command);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(function, __PCI_COMMAND, 16, &readback);
    if (status != OS_EOK) {
        return status;
    }

    if ((readback & (clearMask | setMask)) != setMask) {
        ERROR("__UpdateCommand: %u:%02x:%02x.%u command reads 0x%x, expected set 0x%x and clear 0x%x",
              function->Segment, function->Bus, function->Device, function->Function,
              readback, setMask, clearMask);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// Reads the ACPI PCI root list and requires exactly one root, then copies
// its CPU windows.
static oserr_t
__ReadRoot(
    _Out_ __PciRootInfo_t* rootInfo)
{
    struct AcpiPciQuery query;
    union AcpiPciResult result;
    oserr_t             status;

    memset(&query, 0, sizeof(query));
    memset(&result, 0, sizeof(result));
    query.Type = AcpiPciQueryRoot;
    query.Index = 0;
    status = AcpiPciQuery(&query, &result);
    if (status != OS_EOK) {
        ERROR("__ReadRoot: ACPI PCI root 0 is unavailable: %u", status);
        return status == OS_ENOENT ? OS_ENOENT : OS_ENOTSUPPORTED;
    }
    rootInfo->Root = result.Root;

    query.Index = 1;
    status = AcpiPciQuery(&query, &result);
    if (status != OS_ENOENT) {
        ERROR("__ReadRoot: the fixture needs exactly one ACPI PCI root, second query returned %u", status);
        return OS_ENOTSUPPORTED;
    }

    rootInfo->WindowCount = 0;
    for (;;) {
        memset(&query, 0, sizeof(query));
        memset(&result, 0, sizeof(result));
        query.Type = AcpiPciQueryWindow;
        query.RootId = rootInfo->Root.Id;
        query.Index = rootInfo->WindowCount;
        status = AcpiPciQuery(&query, &result);
        if (status == OS_ENOENT) {
            break;
        }

        if (status != OS_EOK) {
            ERROR("__ReadRoot: window %u of root %u failed: %u",
                  rootInfo->WindowCount, rootInfo->Root.Id, status);
            return OS_ENOTSUPPORTED;
        }

        if (rootInfo->WindowCount == __MAX_WINDOWS) {
            ERROR("__ReadRoot: root %u has more than %u windows", rootInfo->Root.Id, __MAX_WINDOWS);
            return OS_ENOTSUPPORTED;
        }
        rootInfo->Windows[rootInfo->WindowCount++] = result.Window;
    }
    return OS_EOK;
}

// Reads the identity of one function. presentOut is false when nothing
// answers at that address.
static oserr_t
__ReadIdentity(
    _In_  const __PciFunction_t* function,
    _Out_ uint32_t*              vendorDeviceOut,
    _Out_ uint32_t*              classRevisionOut,
    _Out_ uint32_t*              headerTypeOut,
    _Out_ bool*                  presentOut)
{
    oserr_t status;

    *presentOut = false;
    status = __ConfigRead(function, __PCI_VENDOR_DEVICE, 32, vendorDeviceOut);
    if (status != OS_EOK) {
        return status;
    }

    if ((*vendorDeviceOut & 0xFFFFU) == __PCI_VENDOR_ABSENT) {
        return OS_EOK;
    }

    status = __ConfigRead(function, __PCI_CLASS_REVISION, 32, classRevisionOut);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(function, __PCI_HEADER_TYPE, 8, headerTypeOut);
    if (status != OS_EOK) {
        return status;
    }
    *presentOut = true;
    return OS_EOK;
}

// Returns the identity that the fixture expects in a slot of the root bus,
// or false when the slot must be empty.
static bool
__ExpectedIdentity(
    _In_  uint8_t   slot,
    _Out_ uint32_t* vendorDeviceOut,
    _Out_ uint32_t* classRevisionOut)
{
    if (slot == __HOST_BRIDGE_SLOT) {
        *vendorDeviceOut = ((uint32_t)__QEMU_HOST_DEVICE_ID << 16) | __QEMU_VENDOR_ID;
        *classRevisionOut = __QEMU_HOST_CLASS;
        return true;
    }

    if (slot >= __FIRST_TEST_SLOT && slot < __FIRST_TEST_SLOT + ARM_SMMU_TEST_DEVICE_COUNT) {
        *vendorDeviceOut = ((uint32_t)__QEMU_TEST_DEVICE_ID << 16) | __QEMU_VENDOR_ID;
        *classRevisionOut = __QEMU_TEST_CLASS;
        return true;
    }
    return false;
}

// Checks function 0 of one slot against the fixture, and that no other
// function of that slot answers. A bridge or unknown device fails, because
// it could put more requesters behind the same SMMU route.
static oserr_t
__CheckSlot(
    _In_ uint16_t segment,
    _In_ uint8_t  bus,
    _In_ uint8_t  slot)
{
    __PciFunction_t function = { segment, bus, slot, 0 };
    uint32_t        vendorDevice = 0;
    uint32_t        classRevision = 0;
    uint32_t        headerType = 0;
    uint32_t        expectedVendorDevice = 0;
    uint32_t        expectedClass = 0;
    bool            expected;
    bool            present;
    oserr_t         status;

    expected = __ExpectedIdentity(slot, &expectedVendorDevice, &expectedClass);
    status = __ReadIdentity(&function, &vendorDevice, &classRevision, &headerType, &present);
    if (status != OS_EOK) {
        return status;
    }

    if (!present) {
        if (expected) {
            ERROR("__CheckSlot: %u:%02x:%02x.0 is missing", segment, bus, slot);
            return OS_ENOENT;
        }
        return OS_EOK;
    }

    if (!expected || vendorDevice != expectedVendorDevice || classRevision != expectedClass ||
        headerType != __PCI_HEADER_ENDPOINT) {
        ERROR("__CheckSlot: unexpected function %u:%02x:%02x.0, id 0x%08x class 0x%08x header 0x%02x",
              segment, bus, slot, vendorDevice, classRevision, headerType);
        return OS_ENOTSUPPORTED;
    }

    // The header type above has the multi-function flag clear, so only
    // function 0 may answer. Check the others anyway, since a device that
    // reports the flag wrongly would still issue DMA with its own ID.
    for (function.Function = 1; function.Function < __PCI_FUNCTION_COUNT; function.Function++) {
        status = __ReadIdentity(&function, &vendorDevice, &classRevision, &headerType, &present);
        if (status != OS_EOK) {
            return status;
        }

        if (present) {
            ERROR("__CheckSlot: unexpected function %u:%02x:%02x.%u, id 0x%08x",
                  segment, bus, slot, function.Function, vendorDevice);
            return OS_ENOTSUPPORTED;
        }
    }
    return OS_EOK;
}

// Checks every slot of the root bus. Without bridges no other bus can hold
// a function, so this covers the whole root.
static oserr_t
__CheckTopology(
    _In_ const __PciRootInfo_t* rootInfo)
{
    oserr_t status;

    if (rootInfo->Root.BusStart > rootInfo->Root.BusEnd) {
        ERROR("__CheckTopology: root %u has an empty bus range", rootInfo->Root.Id);
        return OS_ENOTSUPPORTED;
    }

    for (uint8_t slot = 0; slot < __PCI_SLOT_COUNT; slot++) {
        status = __CheckSlot(rootInfo->Root.Segment, rootInfo->Root.BusStart, slot);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

// Writes all ones to a BAR, reads the size mask and writes the original
// value back. Memory decode must already be off.
static oserr_t
__ProbeBar(
    _In_  const __PciFunction_t* function,
    _In_  uint32_t               index,
    _Out_ uint32_t*              maskOut)
{
    uint32_t offset = __PCI_BAR0 + (index * 4U);
    uint32_t original;
    uint32_t restored;
    oserr_t  status;

    status = __ConfigRead(function, offset, 32, &original);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigWrite(function, offset, 32, 0xFFFFFFFFU);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(function, offset, 32, maskOut);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigWrite(function, offset, 32, original);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(function, offset, 32, &restored);
    if (status != OS_EOK) {
        return status;
    }

    if (restored != original) {
        ERROR("__ProbeBar: BAR%u of %02x:%02x.%u reads 0x%x after restoring 0x%x",
              index, function->Bus, function->Device, function->Function, restored, original);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// BAR0 must be a 4 KiB, 32-bit, nonprefetchable memory BAR and BAR1-5 must
// be absent (size mask 0). This also tells iommu-testdev apart from
// pci-testdev, which has an I/O BAR.
static oserr_t
__CheckBars(
    _In_ const __PciFunction_t* function)
{
    uint32_t mask;
    uint32_t size;
    oserr_t  status;

    status = __ProbeBar(function, 0, &mask);
    if (status != OS_EOK) {
        return status;
    }

    size = ~(mask & __PCI_BAR_ADDRESS_MASK) + 1U;
    if ((mask & __PCI_BAR_FLAGS_MASK) != 0 || size != ARM_SMMU_TEST_DEVICE_BAR_SIZE) {
        ERROR("__CheckBars: BAR0 of %02x:%02x.%u has mask 0x%x, expected a 4 KiB 32-bit memory BAR",
              function->Bus, function->Device, function->Function, mask);
        return OS_ENOTSUPPORTED;
    }

    for (uint32_t index = 1; index < __PCI_BAR_COUNT; index++) {
        status = __ProbeBar(function, index, &mask);
        if (status != OS_EOK) {
            return status;
        }

        if (mask != 0) {
            ERROR("__CheckBars: BAR%u of %02x:%02x.%u is implemented (mask 0x%x)",
                  index, function->Bus, function->Device, function->Function, mask);
            return OS_ENOTSUPPORTED;
        }
    }
    return OS_EOK;
}

// Selects the first bus address for the two BARs. The window must be the
// only nonprefetchable memory window that can hold two aligned 4 KiB ranges
// below 4 GiB, since a 32-bit BAR cannot hold a higher address.
static oserr_t
__SelectBarRange(
    _In_  const __PciRootInfo_t* rootInfo,
    _Out_ uint64_t*              busAddressOut)
{
    const uint64_t needed = (uint64_t)ARM_SMMU_TEST_DEVICE_COUNT * ARM_SMMU_TEST_DEVICE_BAR_SIZE;
    uint32_t       matches = 0;
    uint64_t       selected = 0;

    for (uint32_t i = 0; i < rootInfo->WindowCount; i++) {
        const struct PciAddressWindow* window = &rootInfo->Windows[i];
        uint64_t                       first;
        uint64_t                       last;

        if (window->Space == PciAddressIo || (window->Attributes & PCI_WINDOW_PREFETCHABLE) ||
            !window->Length || window->Length - 1 > UINT64_MAX - window->BusBase ||
            window->BusBase > __BAR32_LIMIT) {
            continue;
        }

        last = window->BusBase + (window->Length - 1);
        if (last > __BAR32_LIMIT) {
            last = __BAR32_LIMIT;
        }

        // Round the start up to the BAR size. BusBase is below 4 GiB here,
        // so the addition cannot overflow 64 bits.
        first = (window->BusBase + (ARM_SMMU_TEST_DEVICE_BAR_SIZE - 1U)) &
            ~((uint64_t)ARM_SMMU_TEST_DEVICE_BAR_SIZE - 1U);
        if (first > last || last - first < needed - 1U) {
            continue;
        }

        matches++;
        selected = first;
    }

    if (matches != 1) {
        ERROR("__SelectBarRange: found %u usable nonprefetchable memory windows below 4 GiB, need exactly 1",
              matches);
        return OS_ENOTSUPPORTED;
    }
    *busAddressOut = selected;
    return OS_EOK;
}

// Programs BAR0 with busAddress, checks the readback and records the CPU
// address and mapping of the range.
static oserr_t
__AssignBar(
    _In_    const __PciRootInfo_t* rootInfo,
    _InOut_ ArmSMMUTestDevice_t*   device,
    _In_    uint64_t               busAddress)
{
    __PciFunction_t function = __FunctionFromDevice(device);
    uint64_t        physical = 0;
    uintptr_t       registers;
    uint32_t        readback;
    oserr_t         status;

    // __PciTranslateWindow checks every window again and rejects a range
    // that more than one window covers.
    status = __PciTranslateWindow(
        rootInfo->Windows,
        rootInfo->WindowCount,
        PciAddressMemory32,
        0,
        busAddress,
        ARM_SMMU_TEST_DEVICE_BAR_SIZE,
        &physical
    );
    if (status != OS_EOK) {
        ERROR("__AssignBar: bus range 0x%llx is not covered by one window: %u", busAddress, status);
        return OS_ENOTSUPPORTED;
    }

    status = __ConfigWrite(&function, __PCI_BAR0, 32, (uint32_t)busAddress);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(&function, __PCI_BAR0, 32, &readback);
    if (status != OS_EOK) {
        return status;
    }

    if (readback != (uint32_t)busAddress) {
        ERROR("__AssignBar: BAR0 of %02x:%02x.%u reads 0x%x after writing 0x%llx",
              function.Bus, function.Device, function.Function, readback, busAddress);
        return OS_EDEVFAULT;
    }

    registers = Arm64MapDevice(physical, ARM_SMMU_TEST_DEVICE_BAR_SIZE);
    if (registers == 0) {
        ERROR("__AssignBar: mapping BAR0 at physical 0x%llx failed", physical);
        return OS_EOOM;
    }

    device->BarBusAddress = busAddress;
    device->BarPhysicalAddress = physical;
    device->Registers = registers;
    return OS_EOK;
}

// Reads one device register and compares it with the expected value.
static oserr_t
__ExpectRegister(
    _In_ const ArmSMMUTestDevice_t* device,
    _In_ uint32_t                   offset,
    _In_ uint32_t                   expected)
{
    uint32_t value = __DeviceRead32(device->Registers + offset);

    if (value != expected) {
        ERROR("__ExpectRegister: device %04x register 0x%02x reads 0x%x, expected 0x%x",
              device->RequesterId, offset, value, expected);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// Writes one device register and reads it back.
static oserr_t
__WriteRegister(
    _In_ const ArmSMMUTestDevice_t* device,
    _In_ uint32_t                   offset,
    _In_ uint32_t                   value)
{
    __DeviceWrite32(device->Registers + offset, value);
    return __ExpectRegister(device, offset, value);
}

// Disarms the device. The doorbell cannot be read back, so the result
// register (IDLE after a disarm) is the only proof.
static oserr_t
__Disarm(
    _In_ const ArmSMMUTestDevice_t* device)
{
    __DeviceWrite32(device->Registers + __ITD_DOORBELL, __ITD_DOORBELL_DISARM);
    return __ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_IDLE);
}

// Checks the reset values of a freshly decoded device and disarms it. The
// values come from iommu_testdev_reset() in QEMU.
static oserr_t
__CheckResetState(
    _In_ const ArmSMMUTestDevice_t* device)
{
    static const struct {
        uint32_t Offset;
        uint32_t Value;
    } resetValues[] = {
        { __ITD_RESULT,        ARM_SMMU_TEST_RESULT_IDLE },
        { __ITD_IOVA_LOW,      0 },
        { __ITD_IOVA_HIGH,     0 },
        { __ITD_PHYSICAL_LOW,  0 },
        { __ITD_PHYSICAL_HIGH, 0 },
        { __ITD_LENGTH,        0 },
        { __ITD_ATTRIBUTES,    __ITD_ATTR_RESET }
    };
    oserr_t status;

    for (size_t i = 0; i < sizeof(resetValues) / sizeof(resetValues[0]); i++) {
        status = __ExpectRegister(device, resetValues[i].Offset, resetValues[i].Value);
        if (status != OS_EOK) {
            return status;
        }
    }
    return __Disarm(device);
}

// Step 1: check the topology, then record both endpoint identities.
static oserr_t
__IdentifyDevices(
    _In_    const __PciRootInfo_t* rootInfo,
    _InOut_ ArmSMMUTestDevice_t    devices[2])
{
    oserr_t status;

    status = __CheckTopology(rootInfo);
    if (status != OS_EOK) {
        return status;
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        devices[i].Segment = rootInfo->Root.Segment;
        devices[i].RequesterId = (uint16_t)(((uint32_t)rootInfo->Root.BusStart << 8) |
            ((__FIRST_TEST_SLOT + i) << 3));
        devices[i].State = ArmSMMUTestDeviceIdentified;
    }
    return OS_EOK;
}

// Step 2: stop decode and DMA on both endpoints, then check their BARs.
// Both are stopped first so that no stale firmware BAR still decodes while
// the other device receives its new address.
static oserr_t
__QuiesceDevices(
    _In_ ArmSMMUTestDevice_t devices[2])
{
    __PciFunction_t function;
    oserr_t         status;

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        function = __FunctionFromDevice(&devices[i]);
        status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, 0);
        if (status != OS_EOK) {
            return status;
        }
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        function = __FunctionFromDevice(&devices[i]);
        status = __CheckBars(&function);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

// Steps 3 and 4: assign both BARs, then enable memory decode and check the
// register protocol. Bus mastering stays off.
static oserr_t
__EnableDevices(
    _In_    const __PciRootInfo_t* rootInfo,
    _InOut_ ArmSMMUTestDevice_t    devices[2])
{
    __PciFunction_t function;
    uint64_t        busAddress;
    oserr_t         status;

    status = __SelectBarRange(rootInfo, &busAddress);
    if (status != OS_EOK) {
        return status;
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        status = __AssignBar(rootInfo, &devices[i], busAddress + ((uint64_t)i * ARM_SMMU_TEST_DEVICE_BAR_SIZE));
        if (status != OS_EOK) {
            return status;
        }
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        function = __FunctionFromDevice(&devices[i]);
        status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, __PCI_COMMAND_MEMORY);
        if (status != OS_EOK) {
            return status;
        }

        status = __CheckResetState(&devices[i]);
        if (status != OS_EOK) {
            return status;
        }
        devices[i].State = ArmSMMUTestDeviceReady;
    }
    return OS_EOK;
}

// Reads back what Prepare set up for one Ready endpoint and prints it, so the
// serial log shows what was verified. The slot check runs again, so the
// identity, the single-function header and the absence of functions 1-7 come
// from the device and are not copied from the first pass. BAR0 must still
// hold the assigned bus address, and the command register must have memory
// decode on with bus mastering and I/O decode off.
static oserr_t
__ReportDevice(
    _In_ const ArmSMMUTestDevice_t* device)
{
    __PciFunction_t function = __FunctionFromDevice(device);
    uint32_t        bar0;
    uint32_t        command;
    oserr_t         status;

    status = __CheckSlot(function.Segment, function.Bus, function.Device);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(&function, __PCI_BAR0, 32, &bar0);
    if (status != OS_EOK) {
        return status;
    }

    status = __ConfigRead(&function, __PCI_COMMAND, 16, &command);
    if (status != OS_EOK) {
        return status;
    }

    if (bar0 != (uint32_t)device->BarBusAddress || (command & __PCI_COMMAND_DECODE) != __PCI_COMMAND_MEMORY) {
        ERROR("__ReportDevice: device %04x reads BAR0 0x%x and command 0x%x, expected BAR0 0x%llx and decode only",
              device->RequesterId, bar0, command, device->BarBusAddress);
        return OS_EDEVFAULT;
    }

    // __CheckSlot passed, so the identity registers hold exactly these values.
    DEBUG("SMMU-SELFTEST STEP endpoint rid=%04x id=%04x:%04x class=0x%06x header=0x%02x functions=1",
          device->RequesterId, __QEMU_VENDOR_ID, __QEMU_TEST_DEVICE_ID, __QEMU_TEST_CLASS >> 8,
          __PCI_HEADER_ENDPOINT);
    DEBUG("SMMU-SELFTEST STEP endpoint rid=%04x bar0=0x%08x command=0x%04x decode=on master=off",
          device->RequesterId, bar0, command);
    return OS_EOK;
}

static bool
__IsZeroed(
    _In_ const ArmSMMUTestDevice_t* device)
{
    return device->State == ArmSMMUTestDeviceEmpty && device->Segment == 0 &&
        device->RequesterId == 0 && device->BarBusAddress == 0 &&
        device->BarPhysicalAddress == 0 && device->Registers == 0;
}

oserr_t
ArmSMMUTestDevicesPrepare(
    _InOut_ ArmSMMUTestDevice_t devices[2])
{
    __PciRootInfo_t rootInfo;
    oserr_t         status;

    if (devices == NULL || !__IsZeroed(&devices[0]) || !__IsZeroed(&devices[1])) {
        ERROR("ArmSMMUTestDevicesPrepare: two zeroed records are required");
        return OS_EINVALPARAMS;
    }

    memset(&rootInfo, 0, sizeof(rootInfo));
    status = __ReadRoot(&rootInfo);
    if (status != OS_EOK) {
        return status;
    }

    status = __IdentifyDevices(&rootInfo, devices);
    if (status != OS_EOK) {
        return status;
    }

    status = __QuiesceDevices(devices);
    if (status != OS_EOK) {
        return status;
    }

    status = __EnableDevices(&rootInfo, devices);
    if (status != OS_EOK) {
        return status;
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        status = __ReportDevice(&devices[i]);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

// Writes the request registers in the order QEMU uses them and reads each
// one back. Nothing is armed yet.
static oserr_t
__LoadRequest(
    _In_ const ArmSMMUTestDevice_t* device,
    _In_ uint64_t                   deviceAddress,
    _In_ uint64_t                   expectedPhysicalAddress,
    _In_ uint32_t                   length)
{
    const struct {
        uint32_t Offset;
        uint32_t Value;
    } request[] = {
        { __ITD_IOVA_LOW,      (uint32_t)deviceAddress },
        { __ITD_IOVA_HIGH,     (uint32_t)(deviceAddress >> 32) },
        { __ITD_PHYSICAL_LOW,  (uint32_t)expectedPhysicalAddress },
        { __ITD_PHYSICAL_HIGH, (uint32_t)(expectedPhysicalAddress >> 32) },
        { __ITD_LENGTH,        length },
        { __ITD_ATTRIBUTES,    __ITD_ATTR_NONSECURE }
    };
    oserr_t status;

    for (size_t i = 0; i < sizeof(request) / sizeof(request[0]); i++) {
        status = __WriteRegister(device, request[i].Offset, request[i].Value);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

oserr_t
ArmSMMUTestDeviceWrite(
    _In_  ArmSMMUTestDevice_t* device,
    _In_  uint64_t             deviceAddress,
    _In_  uint64_t             expectedPhysicalAddress,
    _In_  uint32_t             length,
    _Out_ uint32_t*            resultOut)
{
    __PciFunction_t function;
    uint32_t        result;
    oserr_t         status;

    if (device == NULL || resultOut == NULL || device->State != ArmSMMUTestDeviceReady ||
        length == 0 || length > ARM_SMMU_TEST_DEVICE_MAX_LENGTH || (length & 3U) ||
        (deviceAddress & 3U) || (expectedPhysicalAddress & 3U)) {
        ERROR("ArmSMMUTestDeviceWrite: invalid request, state %u, iova 0x%llx, physical 0x%llx, length %u",
              device != NULL ? device->State : 0, deviceAddress, expectedPhysicalAddress, length);
        return OS_EINVALPARAMS;
    }
    function = __FunctionFromDevice(device);

    status = __LoadRequest(device, deviceAddress, expectedPhysicalAddress, length);
    if (status != OS_EOK) {
        return status;
    }

    status = __UpdateCommand(&function, __PCI_COMMAND_IO, __PCI_COMMAND_MEMORY | __PCI_COMMAND_BUS_MASTER);
    if (status != OS_EOK) {
        return status;
    }

    // Arming sets RESULT to BUSY. Checking it proves the doorbell write
    // arrived before the trigger read, which would otherwise report NOT_ARMED.
    __DeviceBarrier();
    __DeviceWrite32(device->Registers + __ITD_DOORBELL, __ITD_DOORBELL_ARM);
    status = __ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_BUSY);
    if (status != OS_EOK) {
        return status;
    }

    // QEMU runs the whole request inside this read: the write through the
    // SMMU, then the direct physical readback. The returned value is unused.
    (void)__DeviceRead32(device->Registers + __ITD_TRIGGER);
    result = __DeviceRead32(device->Registers + __ITD_RESULT);
    __DeviceBarrier();

    status = __Disarm(device);
    if (status != OS_EOK) {
        return status;
    }

    status = __UpdateCommand(&function, __PCI_COMMAND_IO | __PCI_COMMAND_BUS_MASTER, __PCI_COMMAND_MEMORY);
    if (status != OS_EOK) {
        return status;
    }
    *resultOut = result;
    return OS_EOK;
}

oserr_t
ArmSMMUTestDeviceDisable(
    _InOut_ ArmSMMUTestDevice_t* device)
{
    __PciFunction_t function;
    oserr_t         status;

    if (device == NULL) {
        ERROR("ArmSMMUTestDeviceDisable: device is NULL");
        return OS_EINVALPARAMS;
    }

    if (device->State == ArmSMMUTestDeviceEmpty) {
        return OS_EOK;
    }

    if (device->State == ArmSMMUTestDeviceReady) {
        status = __Disarm(device);
        if (status != OS_EOK) {
            ERROR("ArmSMMUTestDeviceDisable: device %04x did not disarm: %u", device->RequesterId, status);
            return status;
        }
    }

    function = __FunctionFromDevice(device);
    status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, 0);
    if (status != OS_EOK) {
        ERROR("ArmSMMUTestDeviceDisable: device %04x command register was not cleared: %u",
              device->RequesterId, status);
        return status;
    }
    device->State = ArmSMMUTestDeviceIdentified;
    return OS_EOK;
}
