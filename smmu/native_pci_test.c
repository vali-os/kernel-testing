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
 * Host checks of the native SMMU test-device adapter (native_pci.c).
 *
 * The adapter's three boundaries are replaced here: AcpiPciQuery returns a
 * configurable root and window list, AcpiPciConfig serves a modeled bus 0,
 * Arm64MapDevice returns a fake mapping, and the register hooks run a model
 * of QEMU's iommu-testdev protocol. Every boundary call is appended to an
 * event log, so the tests can check the order of accesses, their widths and
 * that failures stop before later steps.
 */

#include "native_private.h"
#include "../../kernel/acpi/pci.h"
#include <assert.h>
#include <log.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Register offsets and values, written out again so that the tests do not
// depend on the adapter's own constants.
#define REG_TRIGGER       0x00U
#define REG_IOVA_LOW      0x04U
#define REG_IOVA_HIGH     0x08U
#define REG_LENGTH        0x0CU
#define REG_RESULT        0x10U
#define REG_DOORBELL      0x14U
#define REG_ATTRIBUTES    0x18U
#define REG_PHYSICAL_LOW  0x1CU
#define REG_PHYSICAL_HIGH 0x20U
#define ATTR_RESET        0x2U // NonSecure space, valid bit clear.
#define ATTR_NONSECURE    0xAU // NonSecure space with the valid bit set.

#define COMMAND_IO         0x1U
#define COMMAND_MEMORY     0x2U
#define COMMAND_BUS_MASTER 0x4U

// Fake CPU mapping: Arm64MapDevice returns physical + MAP_OFFSET.
#define MAP_OFFSET 0x700000000000ULL

// Window used by the default fixture. CPU addresses differ from bus
// addresses, so a missing translation is visible.
#define WINDOW_BUS      0x10000000ULL
#define WINDOW_CPU      0x3EFF0000ULL
#define WINDOW_LENGTH   0x2EFF0000ULL

enum EventType {
    EventConfigRead,
    EventConfigWrite,
    EventMap,
    EventRead,
    EventWrite,
    EventBarrier
};

typedef struct Event {
    enum EventType Type;
    int            Device;  // Slot for config events, device index for MMIO.
    uint32_t       Offset;
    uint32_t       Width;
    uint64_t       Value;
} Event_t;

typedef struct Function {
    bool     Present;
    uint32_t VendorDevice;
    uint32_t ClassRevision;
    uint32_t Header;
    uint32_t BarMask[6];  // Size mask with type bits. 0 means absent.
    uint32_t Bar[6];
    uint32_t Command;
    uint32_t StuckCommand; // Command bits that ignore writes.
    bool     BrokenBar;    // BAR0 ignores a new address. Sizing and restoring still work.
    uint32_t SizingSaved;  // BAR0 value before the last all-ones write.
} Function_t;

typedef struct Device {
    uint64_t Iova;
    uint64_t Physical;
    uint32_t Length;
    uint32_t Result;
    uint32_t Attributes;
    bool     Armed;
    uint32_t Triggers;
    uint32_t NextResult;   // Result a trigger produces when armed.
    bool     IgnoreArm;    // Doorbell arm has no effect.
    bool     IgnoreDisarm; // Doorbell disarm leaves RESULT unchanged.
    bool     MasterAtTrigger;
} Device_t;

static struct {
    // ACPI root list.
    uint32_t                RootCount;
    struct AcpiPciRoot      Root;
    struct PciAddressWindow Windows[4];
    uint32_t                WindowCount;

    // Bus 0, function 0..7 of slots 0..31.
    Function_t Functions[32][8];
    Device_t   Devices[2];
    bool       MapFails;
    uint32_t   MapCount;
    uintptr_t  Mapped[2]; // CPU mapping returned for each device, in call order.

    Event_t  Events[4096];
    uint32_t EventCount;
} g_fixture;

void
LogAppendMessage(
    enum OSSysLogLevel level,
    const char*        format, ...)
{
    va_list arguments;

    (void)level;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

static void
AddEvent(
    enum EventType type,
    int            device,
    uint32_t       offset,
    uint32_t       width,
    uint64_t       value)
{
    assert(g_fixture.EventCount < sizeof(g_fixture.Events) / sizeof(g_fixture.Events[0]));
    g_fixture.Events[g_fixture.EventCount++] = (Event_t) { type, device, offset, width, value };
}

oserr_t
AcpiPciQuery(
    const struct AcpiPciQuery* query,
    union AcpiPciResult*       result)
{
    if (query->Type == AcpiPciQueryRoot) {
        if (query->Index >= g_fixture.RootCount) {
            return OS_ENOENT;
        }
        result->Root = g_fixture.Root;
        result->Root.Id = query->Index + 1;
        return OS_EOK;
    }

    assert(query->Type == AcpiPciQueryWindow);
    assert(query->RootId == 1);
    if (query->Index >= g_fixture.WindowCount) {
        return OS_ENOENT;
    }
    result->Window = g_fixture.Windows[query->Index];
    return OS_EOK;
}

static uint32_t
ReadFunction(
    Function_t* function,
    uint32_t    offset)
{
    if (offset == 0x00) {
        return function->VendorDevice;
    }
    if (offset == 0x04) {
        return function->Command;
    }
    if (offset == 0x08) {
        return function->ClassRevision;
    }
    if (offset == 0x0E) {
        return function->Header;
    }
    if (offset >= 0x10 && offset < 0x28) {
        uint32_t index = (offset - 0x10) / 4;

        uint32_t value;

        // Type bits are fixed and always read back. An absent BAR reads 0.
        value = function->BarMask[index] == 0 ? 0 : function->Bar[index] | (function->BarMask[index] & 0xFU);
        return value;
    }
    return 0;
}

ACPI_STATUS
AcpiPciConfig(
    ACPI_PCI_ID* id,
    UINT32       reg,
    UINT64*      value,
    UINT32       width,
    int          write)
{
    Function_t* function;

    // The adapter only uses the root bus of segment 0.
    assert(id->Segment == 0 && id->Bus == 0 && id->Device < 32 && id->Function < 8);
    function = &g_fixture.Functions[id->Device][id->Function];

    // The command register is only reached through 16-bit accesses, and the
    // status register at 0x06 is never written.
    if (reg == 0x04 || reg == 0x06) {
        assert(width == 16 && reg == 0x04);
    }
    assert(!(write && reg == 0x06));

    if (!function->Present) {
        assert(!write);
        *value = width == 32 ? 0xFFFFFFFFU : width == 16 ? 0xFFFFU : 0xFFU;
        AddEvent(EventConfigRead, id->Device * 8 + id->Function, reg, width, *value);
        return AE_OK;
    }

    if (!write) {
        *value = ReadFunction(function, reg);
        AddEvent(EventConfigRead, id->Device * 8 + id->Function, reg, width, *value);
        return AE_OK;
    }

    AddEvent(EventConfigWrite, id->Device * 8 + id->Function, reg, width, *value);
    if (reg == 0x04) {
        function->Command = ((uint32_t)*value & ~function->StuckCommand) |
            (function->Command & function->StuckCommand);
    } else if (reg >= 0x10 && reg < 0x28) {
        uint32_t index = (reg - 0x10) / 4;
        uint32_t mask = function->BarMask[index];

        // Bar holds only the writable address bits. Bits below the size
        // read as zero.
        if (index == 0 && function->BrokenBar && (uint32_t)*value != 0xFFFFFFFFU &&
            ((uint32_t)*value & mask & 0xFFFFFFF0U) != function->SizingSaved) {
            return AE_OK;
        }
        if (index == 0 && (uint32_t)*value == 0xFFFFFFFFU) {
            function->SizingSaved = function->Bar[0];
        }
        function->Bar[index] = (uint32_t)*value & mask & 0xFFFFFFF0U;
    } else {
        assert(!"unexpected configuration write");
    }
    return AE_OK;
}

uintptr_t
Arm64MapDevice(
    uint64_t physical,
    uint64_t length)
{
    AddEvent(EventMap, -1, 0, (uint32_t)length, physical);
    assert(length == 0x1000);
    if (g_fixture.MapFails) {
        return 0;
    }
    assert(g_fixture.MapCount < 2);
    g_fixture.Mapped[g_fixture.MapCount++] = (uintptr_t)(physical + MAP_OFFSET);
    return (uintptr_t)(physical + MAP_OFFSET);
}

// Finds the modeled device whose BAR0 maps the address.
static int
DeviceAt(
    uintptr_t address,
    uint32_t* offsetOut)
{
    for (int i = 0; i < (int)g_fixture.MapCount; i++) {
        uintptr_t base = g_fixture.Mapped[i];

        if (address >= base && address < base + 0x1000) {
            *offsetOut = (uint32_t)(address - base);
            assert((*offsetOut & 3U) == 0);
            // Register access needs memory decode, as on real PCI.
            assert(g_fixture.Functions[1 + i][0].Command & COMMAND_MEMORY);
            return i;
        }
    }
    assert(!"register access outside a test BAR");
    return -1;
}

uint32_t
ArmSMMUTestHostRead32(
    uintptr_t address)
{
    uint32_t  offset;
    int       index = DeviceAt(address, &offset);
    Device_t* device = &g_fixture.Devices[index];
    uint32_t  value = 0;

    switch (offset) {
        case REG_TRIGGER:
            device->Triggers++;
            device->MasterAtTrigger = (g_fixture.Functions[1 + index][0].Command & COMMAND_BUS_MASTER) != 0;
            device->Result = device->Armed ? device->NextResult : 0xDEAD0005U;
            device->Armed = false;
            break;
        case REG_IOVA_LOW: value = (uint32_t)device->Iova; break;
        case REG_IOVA_HIGH: value = (uint32_t)(device->Iova >> 32); break;
        case REG_PHYSICAL_LOW: value = (uint32_t)device->Physical; break;
        case REG_PHYSICAL_HIGH: value = (uint32_t)(device->Physical >> 32); break;
        case REG_LENGTH: value = device->Length; break;
        case REG_RESULT: value = device->Result; break;
        case REG_ATTRIBUTES: value = device->Attributes; break;
        default: break;
    }
    AddEvent(EventRead, index, offset, 32, value);
    return value;
}

void
ArmSMMUTestHostWrite32(
    uintptr_t address,
    uint32_t  value)
{
    uint32_t  offset;
    int       index = DeviceAt(address, &offset);
    Device_t* device = &g_fixture.Devices[index];

    AddEvent(EventWrite, index, offset, 32, value);
    switch (offset) {
        case REG_IOVA_LOW: device->Iova = (device->Iova & ~0xFFFFFFFFULL) | value; break;
        case REG_IOVA_HIGH: device->Iova = (device->Iova & 0xFFFFFFFFULL) | ((uint64_t)value << 32); break;
        case REG_PHYSICAL_LOW: device->Physical = (device->Physical & ~0xFFFFFFFFULL) | value; break;
        case REG_PHYSICAL_HIGH: device->Physical = (device->Physical & 0xFFFFFFFFULL) | ((uint64_t)value << 32); break;
        case REG_LENGTH: device->Length = value; break;
        case REG_RESULT: device->Result = value; break;
        case REG_ATTRIBUTES: device->Attributes = value; break;
        case REG_DOORBELL:
            if (value & 1U) {
                if (!device->IgnoreArm) {
                    device->Armed = true;
                    device->Result = 0xFFFFFFFEU;
                }
            } else if (!device->IgnoreDisarm) {
                device->Armed = false;
                device->Result = 0xFFFFFFFFU;
            }
            break;
        default:
            break;
    }
}

void
ArmSMMUTestHostBarrier(void)
{
    AddEvent(EventBarrier, -1, 0, 0, 0);
}

static void
SetEndpoint(
    uint32_t slot,
    uint32_t vendorDevice,
    uint32_t classRevision)
{
    Function_t* function = &g_fixture.Functions[slot][0];

    memset(function, 0, sizeof(*function));
    function->Present = true;
    function->VendorDevice = vendorDevice;
    function->ClassRevision = classRevision;
    function->BarMask[0] = 0xFFFFF000U;
    // Firmware left an old BAR and decode enabled. The adapter must stop it.
    function->Bar[0] = 0x10008000U;
    function->Command = COMMAND_MEMORY | COMMAND_BUS_MASTER | 0x400U;
}

// Builds the QEMU fixture: one root, one 32-bit window, the host bridge and
// two iommu-testdev endpoints in their reset state.
static void
ResetFixture(void)
{
    memset(&g_fixture, 0, sizeof(g_fixture));
    g_fixture.RootCount = 1;
    g_fixture.Root.Segment = 0;
    g_fixture.Root.BusStart = 0;
    g_fixture.Root.BusEnd = 255;
    g_fixture.Windows[0] = (struct PciAddressWindow) { PciAddressIo, 0, 0, 0x3EFF0000ULL, 0x10000 };
    g_fixture.Windows[1] = (struct PciAddressWindow) { PciAddressMemory32, 0, WINDOW_BUS, WINDOW_CPU, WINDOW_LENGTH };
    g_fixture.Windows[2] = (struct PciAddressWindow) { PciAddressMemory64, 0, 0x8000000000ULL, 0x8000000000ULL,
                                                       0x8000000000ULL };
    g_fixture.WindowCount = 3;

    g_fixture.Functions[0][0] = (Function_t) { .Present = true, .VendorDevice = 0x00081B36U,
                                               .ClassRevision = 0x06000000U };
    SetEndpoint(1, 0x00051B36U, 0x00FF0000U);
    SetEndpoint(2, 0x00051B36U, 0x00FF0000U);
    for (int i = 0; i < 2; i++) {
        g_fixture.Devices[i].Result = 0xFFFFFFFFU;
        g_fixture.Devices[i].Attributes = ATTR_RESET;
    }
}

static uint32_t
CountEvents(
    enum EventType type)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        count += g_fixture.Events[i].Type == type;
    }
    return count;
}

// Index of the first event of the given type, or EventCount if none.
static uint32_t
FirstEvent(
    enum EventType type)
{
    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        if (g_fixture.Events[i].Type == type) {
            return i;
        }
    }
    return g_fixture.EventCount;
}

static void
AssertZeroed(
    const ArmSMMUTestDevice_t* devices)
{
    static const ArmSMMUTestDevice_t zero = { 0 };

    assert(memcmp(&devices[0], &zero, sizeof(zero)) == 0 && memcmp(&devices[1], &zero, sizeof(zero)) == 0);
}

static void
PrepareReady(
    ArmSMMUTestDevice_t devices[2])
{
    ResetFixture();
    memset(devices, 0, sizeof(ArmSMMUTestDevice_t) * 2);
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EOK);
    g_fixture.EventCount = 0;
}

static void
TestPrepare(void)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };
    uint32_t            firstWrite;

    ResetFixture();
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EOK);
    for (int i = 0; i < 2; i++) {
        uint64_t bus = WINDOW_BUS + (uint64_t)i * 0x1000U;

        assert(devices[i].State == ArmSMMUTestDeviceReady);
        assert(devices[i].Segment == 0 && devices[i].RequesterId == (uint16_t)((1 + i) << 3));
        assert(devices[i].BarBusAddress == bus);
        assert(devices[i].BarPhysicalAddress == bus - WINDOW_BUS + WINDOW_CPU);
        assert(devices[i].Registers == (uintptr_t)(devices[i].BarPhysicalAddress + MAP_OFFSET));
        assert(g_fixture.Functions[1 + i][0].Bar[0] == (uint32_t)bus);
        // Memory decode on, bus mastering and I/O off, other bits kept.
        assert(g_fixture.Functions[1 + i][0].Command == (COMMAND_MEMORY | 0x400U));
        assert(g_fixture.Devices[i].Triggers == 0 && !g_fixture.Devices[i].Armed);
    }

    // No configuration write happens before every slot and function was read.
    firstWrite = FirstEvent(EventConfigWrite);
    assert(firstWrite < g_fixture.EventCount);
    for (uint32_t slot = 0; slot < 32; slot++) {
        bool read = false;

        for (uint32_t i = 0; i < firstWrite; i++) {
            read |= g_fixture.Events[i].Type == EventConfigRead && g_fixture.Events[i].Device == (int)(slot * 8) &&
                g_fixture.Events[i].Offset == 0;
        }
        assert(read);
    }

    // Both endpoints lose decode and bus mastering before any BAR write,
    // and memory decode returns only after both BARs hold their new value.
    {
        uint32_t disabled = 0;
        uint32_t firstBarWrite = g_fixture.EventCount;
        uint32_t lastBarAssign = 0;
        uint32_t firstEnable = g_fixture.EventCount;

        for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
            const Event_t* event = &g_fixture.Events[i];

            if (event->Type != EventConfigWrite) {
                continue;
            }
            if (event->Offset == 0x04 && (event->Value & 7U) == 0 && firstBarWrite == g_fixture.EventCount) {
                disabled++;
            }
            if (event->Offset >= 0x10 && event->Offset < 0x28 && firstBarWrite == g_fixture.EventCount) {
                firstBarWrite = i;
            }
            if (event->Offset == 0x10 && (event->Value == WINDOW_BUS || event->Value == WINDOW_BUS + 0x1000U)) {
                lastBarAssign = i;
            }
            if (event->Offset == 0x04 && (event->Value & COMMAND_MEMORY) && firstEnable == g_fixture.EventCount) {
                firstEnable = i;
            }
            // Bus mastering is never enabled during Prepare.
            assert(!(event->Offset == 0x04 && (event->Value & COMMAND_BUS_MASTER)));
        }
        assert(disabled == 2);
        assert(lastBarAssign < firstEnable && firstEnable < g_fixture.EventCount);
        assert(FirstEvent(EventMap) < firstEnable);
    }

    // The register reset values were read and both devices disarmed.
    assert(CountEvents(EventRead) >= 2 * 8);
    assert(CountEvents(EventWrite) == 2);
    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        if (g_fixture.Events[i].Type == EventWrite) {
            assert(g_fixture.Events[i].Offset == REG_DOORBELL && g_fixture.Events[i].Value == 0);
        }
    }

    // After the last configuration write, the endpoint report reads the
    // identity, BAR0 and the 16-bit command register of both endpoints again.
    {
        uint32_t lastWrite = 0;

        for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
            if (g_fixture.Events[i].Type == EventConfigWrite) {
                lastWrite = i;
            }
        }
        for (int slot = 1; slot <= 2; slot++) {
            bool identity = false;
            bool bar = false;
            bool command = false;

            for (uint32_t i = lastWrite + 1; i < g_fixture.EventCount; i++) {
                const Event_t* event = &g_fixture.Events[i];

                if (event->Type != EventConfigRead || event->Device != slot * 8) {
                    continue;
                }
                identity |= event->Offset == 0x00;
                bar |= event->Offset == 0x10;
                command |= event->Offset == 0x04 && event->Width == 16;
            }
            assert(identity && bar && command);
        }
    }
    puts("prepare: topology, BAR assignment, reset and report checks passed");
}

// Runs Prepare on a changed fixture and expects a failure before any
// configuration write.
static void
ExpectTopologyFailure(
    oserr_t expected)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };

    assert(ArmSMMUTestDevicesPrepare(devices) == expected);
    assert(CountEvents(EventConfigWrite) == 0);
    AssertZeroed(devices);
}

static void
TestTopology(void)
{
    ResetFixture();
    g_fixture.Functions[3][0] = g_fixture.Functions[1][0];
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    ResetFixture();
    g_fixture.Functions[2][0].Present = false;
    ExpectTopologyFailure(OS_ENOENT);

    ResetFixture();
    g_fixture.Functions[0][0].Present = false;
    ExpectTopologyFailure(OS_ENOENT);

    // A PCI-to-PCI bridge in a test slot.
    ResetFixture();
    g_fixture.Functions[2][0].ClassRevision = 0x06040000U;
    g_fixture.Functions[2][0].Header = 0x01U;
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    // pci-testdev shares the IDs but would fail the BAR check. Another class
    // fails here already.
    ResetFixture();
    g_fixture.Functions[1][0].ClassRevision = 0x00FF0001U;
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    ResetFixture();
    g_fixture.Functions[1][0].Header = 0x80U;
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    // A second function on a test slot would share the SMMU route.
    ResetFixture();
    g_fixture.Functions[2][3] = g_fixture.Functions[2][0];
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    ResetFixture();
    g_fixture.Functions[0][1].Present = true;
    g_fixture.Functions[0][1].VendorDevice = 0x12341234U;
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    ResetFixture();
    g_fixture.RootCount = 2;
    ExpectTopologyFailure(OS_ENOTSUPPORTED);

    ResetFixture();
    g_fixture.RootCount = 0;
    ExpectTopologyFailure(OS_ENOENT);
    puts("topology: unexpected functions, bridges and roots rejected");
}

static void
TestRecordsMustBeZeroed(void)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };

    ResetFixture();
    devices[1].BarBusAddress = 1;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EINVALPARAMS);
    memset(devices, 0, sizeof(devices));
    devices[0].State = ArmSMMUTestDeviceIdentified;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDevicesPrepare(NULL) == OS_EINVALPARAMS);
    assert(g_fixture.EventCount == 0);
    puts("prepare: used records rejected");
}

// Prepare with a changed window list. Returns the status, and on success
// the bus address of BAR0 of the first device.
static oserr_t
PrepareWithWindows(
    const struct PciAddressWindow* windows,
    uint32_t                       count,
    uint64_t*                      busOut)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };
    oserr_t             status;

    ResetFixture();
    memcpy(g_fixture.Windows, windows, sizeof(*windows) * count);
    g_fixture.WindowCount = count;
    status = ArmSMMUTestDevicesPrepare(devices);
    if (status == OS_EOK) {
        assert(devices[1].BarBusAddress == devices[0].BarBusAddress + 0x1000U);
        assert(devices[0].BarPhysicalAddress - devices[0].BarBusAddress ==
               windows[0].PhysicalBase - windows[0].BusBase);
        *busOut = devices[0].BarBusAddress;
    } else {
        // A failed selection leaves both records Identified with no BAR.
        assert(devices[0].State == ArmSMMUTestDeviceIdentified && devices[0].BarBusAddress == 0);
        assert(CountEvents(EventMap) == 0);
    }
    return status;
}

static void
TestBarArithmetic(void)
{
    struct PciAddressWindow windows[2];
    uint64_t                bus = 0;

    // An unaligned window start is rounded up to the next 4 KiB.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000800ULL, 0x20000800ULL, 0x10000 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_EOK && bus == 0x10001000ULL);

    // Exactly two pages after rounding fit. One byte less does not.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000800ULL, 0x20000800ULL, 0x2800 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_EOK && bus == 0x10001000ULL);
    windows[0].Length = 0x27FF;
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);

    // A window that crosses 4 GiB is cut at the 32-bit BAR limit.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory64, 0, 0xFFFFE000ULL, 0x1FFFFE000ULL, 0x10000 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_EOK && bus == 0xFFFFE000ULL);
    windows[0].BusBase = 0xFFFFF000ULL;
    windows[0].PhysicalBase = 0x1FFFFF000ULL;
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);

    // Windows above 4 GiB, prefetchable windows and I/O windows are unusable.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory64, 0, 0x100000000ULL, 0x100000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, PCI_WINDOW_PREFETCHABLE, 0x10000000ULL,
                                             0x10000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);
    windows[0] = (struct PciAddressWindow) { PciAddressIo, 0, 0x10000000ULL, 0x10000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);

    // A separate prefetchable window is ignored, so the choice stays unique.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000000ULL, 0x10000000ULL, 0x100000 };
    windows[1] = (struct PciAddressWindow) { PciAddressMemory32, PCI_WINDOW_PREFETCHABLE, 0x20000000ULL,
                                             0x20000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 2, &bus) == OS_EOK && bus == 0x10000000ULL);

    // A window that starts above 4 GiB is skipped before its start is
    // rounded up, which would wrap to 0 at the top of the address space.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000000ULL, 0x10000000ULL, 0x100000 };
    windows[1] = (struct PciAddressWindow) { PciAddressMemory64, 0, 0xFFFFFFFFFFFFF001ULL, 0x1000ULL, 0xFFF };
    assert(PrepareWithWindows(windows, 2, &bus) == OS_EOK && bus == 0x10000000ULL);

    // A window whose end overflows 64 bits is skipped.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000000ULL, 0x10000000ULL, UINT64_MAX };
    assert(PrepareWithWindows(windows, 1, &bus) == OS_ENOTSUPPORTED);

    // Two usable windows are ambiguous.
    windows[0] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x10000000ULL, 0x10000000ULL, 0x100000 };
    windows[1] = (struct PciAddressWindow) { PciAddressMemory32, 0, 0x20000000ULL, 0x20000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 2, &bus) == OS_ENOTSUPPORTED);

    // A usable window plus an overlapping prefetchable one: the selected range
    // is covered twice, which __PciTranslateWindow refuses.
    windows[1] = (struct PciAddressWindow) { PciAddressMemory32, PCI_WINDOW_PREFETCHABLE, 0x10000000ULL,
                                             0x10000000ULL, 0x100000 };
    assert(PrepareWithWindows(windows, 2, &bus) == OS_ENOTSUPPORTED);
    puts("bar arithmetic: alignment, size, 4 GiB limit and window selection passed");
}

static void
TestBarLayout(void)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };

    // pci-testdev has an I/O BAR1, which iommu-testdev lacks.
    ResetFixture();
    g_fixture.Functions[2][0].BarMask[1] = 0xFFFFFF01U;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_ENOTSUPPORTED);
    assert(devices[0].State == ArmSMMUTestDeviceIdentified && devices[1].State == ArmSMMUTestDeviceIdentified);
    // Probing happened only after decode was off on both endpoints.
    assert((g_fixture.Functions[1][0].Command & 7U) == 0 && (g_fixture.Functions[2][0].Command & 7U) == 0);

    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Functions[1][0].BarMask[0] = 0xFFFFE000U;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_ENOTSUPPORTED);

    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Functions[1][0].BarMask[0] = 0xFFFFF008U;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_ENOTSUPPORTED);

    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Functions[2][0].BarMask[0] = 0xFFFFF004U;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_ENOTSUPPORTED);
    puts("bar layout: wrong size, type and extra BARs rejected");
}

static void
TestPartialFailures(void)
{
    ArmSMMUTestDevice_t devices[2] = { 0 };

    // Command bits that do not clear fail before any BAR is probed.
    ResetFixture();
    g_fixture.Functions[2][0].StuckCommand = COMMAND_BUS_MASTER;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EDEVFAULT);
    assert(devices[1].State == ArmSMMUTestDeviceIdentified);
    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        assert(!(g_fixture.Events[i].Type == EventConfigWrite && g_fixture.Events[i].Offset >= 0x10));
    }

    // A BAR that does not hold its new address.
    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Functions[2][0].BrokenBar = true;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EDEVFAULT);
    assert(devices[0].State == ArmSMMUTestDeviceIdentified && devices[1].State == ArmSMMUTestDeviceIdentified);

    // A failed mapping.
    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.MapFails = true;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EOOM);
    assert(devices[0].Registers == 0 && devices[0].State == ArmSMMUTestDeviceIdentified);
    assert((g_fixture.Functions[1][0].Command & COMMAND_MEMORY) == 0);

    // Wrong register reset values: the first device becomes Ready only if
    // its own checks pass. Disable then clears both.
    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Devices[1].Attributes = ATTR_NONSECURE;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EDEVFAULT);
    assert(devices[0].State == ArmSMMUTestDeviceReady && devices[1].State == ArmSMMUTestDeviceIdentified);
    assert(ArmSMMUTestDeviceDisable(&devices[0]) == OS_EOK);
    assert(ArmSMMUTestDeviceDisable(&devices[1]) == OS_EOK);
    assert(devices[0].State == ArmSMMUTestDeviceIdentified && devices[1].State == ArmSMMUTestDeviceIdentified);
    assert((g_fixture.Functions[1][0].Command & 7U) == 0 && (g_fixture.Functions[2][0].Command & 7U) == 0);

    ResetFixture();
    memset(devices, 0, sizeof(devices));
    g_fixture.Devices[0].Result = 0xFFFFFFFEU;
    assert(ArmSMMUTestDevicesPrepare(devices) == OS_EDEVFAULT);
    assert(devices[0].State == ArmSMMUTestDeviceIdentified);
    puts("prepare: partial failures keep records for Disable");
}

// Expected register sequence of one Write call, after the configuration
// accesses are filtered out.
static void
AssertWriteSequence(
    int      device,
    uint64_t iova,
    uint64_t physical,
    uint32_t length)
{
    const struct {
        enum EventType Type;
        uint32_t       Offset;
        uint64_t       Value;
    } expected[] = {
        { EventWrite, REG_IOVA_LOW, (uint32_t)iova }, { EventRead, REG_IOVA_LOW, (uint32_t)iova },
        { EventWrite, REG_IOVA_HIGH, iova >> 32 }, { EventRead, REG_IOVA_HIGH, iova >> 32 },
        { EventWrite, REG_PHYSICAL_LOW, (uint32_t)physical }, { EventRead, REG_PHYSICAL_LOW, (uint32_t)physical },
        { EventWrite, REG_PHYSICAL_HIGH, physical >> 32 }, { EventRead, REG_PHYSICAL_HIGH, physical >> 32 },
        { EventWrite, REG_LENGTH, length }, { EventRead, REG_LENGTH, length },
        { EventWrite, REG_ATTRIBUTES, ATTR_NONSECURE }, { EventRead, REG_ATTRIBUTES, ATTR_NONSECURE },
        { EventWrite, REG_DOORBELL, 1 }, { EventRead, REG_RESULT, 0xFFFFFFFEU },
        { EventRead, REG_TRIGGER, 0 }, { EventRead, REG_RESULT, UINT64_MAX },
        { EventWrite, REG_DOORBELL, 0 }, { EventRead, REG_RESULT, 0xFFFFFFFFU }
    };
    uint32_t next = 0;
    uint32_t masterOn = 0;
    uint32_t masterOff = 0;
    uint32_t arm = 0;
    uint32_t disarm = 0;

    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        const Event_t* event = &g_fixture.Events[i];

        if (event->Type == EventConfigWrite) {
            assert(event->Offset == 0x04 && event->Width == 16);
            if (event->Value & COMMAND_BUS_MASTER) {
                masterOn = i;
            } else {
                masterOff = i;
            }
            continue;
        }

        if (event->Type != EventRead && event->Type != EventWrite) {
            continue;
        }
        assert(event->Device == device && event->Width == 32);
        assert(next < sizeof(expected) / sizeof(expected[0]));
        assert(event->Type == expected[next].Type && event->Offset == expected[next].Offset);
        if (expected[next].Value != UINT64_MAX) {
            assert(event->Value == expected[next].Value);
        }
        if (event->Type == EventWrite && event->Offset == REG_DOORBELL) {
            if (event->Value) {
                arm = i;
            } else {
                disarm = i;
            }
        }
        next++;
    }
    assert(next == sizeof(expected) / sizeof(expected[0]));
    // Bus mastering is enabled after the request is loaded and before arming,
    // and cleared after the disarm was checked.
    assert(masterOn != 0 && masterOn < arm && masterOff > disarm);
}

static void
TestWrite(void)
{
    ArmSMMUTestDevice_t devices[2];
    uint32_t            result = 0x55U;

    PrepareReady(devices);
    assert(ArmSMMUTestDeviceWrite(&devices[1], 0x12345678000ULL, 0x140001000ULL, 256, &result) == OS_EOK);
    assert(result == 0);
    AssertWriteSequence(1, 0x12345678000ULL, 0x140001000ULL, 256);
    assert(g_fixture.Devices[1].Triggers == 1 && g_fixture.Devices[1].MasterAtTrigger);
    assert(g_fixture.Devices[0].Triggers == 0);
    assert(g_fixture.Functions[2][0].Command == (COMMAND_MEMORY | 0x400U));
    assert(devices[1].State == ArmSMMUTestDeviceReady);

    // A refused DMA is still a completed protocol transaction.
    g_fixture.EventCount = 0;
    g_fixture.Devices[0].NextResult = 0xDEAD0002U;
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x7FC00100ULL, 0x7FC03100ULL, 4, &result) == OS_EOK);
    assert(result == 0xDEAD0002U);
    AssertWriteSequence(0, 0x7FC00100ULL, 0x7FC03100ULL, 4);

    // Invalid requests touch nothing.
    g_fixture.EventCount = 0;
    result = 0x55U;
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 0, &result) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 260, &result) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 6, &result) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1002, 0x2000, 8, &result) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2002, 8, &result) == OS_EINVALPARAMS);
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 8, NULL) == OS_EINVALPARAMS);
    devices[0].State = ArmSMMUTestDeviceIdentified;
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 8, &result) == OS_EINVALPARAMS);
    assert(g_fixture.EventCount == 0 && result == 0x55U);
    puts("write: register order, widths, bus mastering and argument checks passed");
}

static void
TestWriteFailures(void)
{
    ArmSMMUTestDevice_t devices[2];
    uint32_t            result = 0x55U;

    // The doorbell did not arm: no trigger read follows.
    PrepareReady(devices);
    g_fixture.Devices[0].IgnoreArm = true;
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 8, &result) == OS_EDEVFAULT);
    assert(result == 0x55U && g_fixture.Devices[0].Triggers == 0);
    assert(devices[0].State == ArmSMMUTestDeviceReady);
    g_fixture.Devices[0].IgnoreArm = false;
    assert(ArmSMMUTestDeviceDisable(&devices[0]) == OS_EOK);
    assert((g_fixture.Functions[1][0].Command & 7U) == 0);

    // The disarm did not take effect: the result stays unreported.
    PrepareReady(devices);
    g_fixture.Devices[1].IgnoreDisarm = true;
    assert(ArmSMMUTestDeviceWrite(&devices[1], 0x1000, 0x2000, 8, &result) == OS_EDEVFAULT);
    assert(result == 0x55U && g_fixture.Devices[1].Triggers == 1);
    // Disable must also refuse while the device does not disarm.
    assert(ArmSMMUTestDeviceDisable(&devices[1]) == OS_EDEVFAULT);
    assert(devices[1].State == ArmSMMUTestDeviceReady);
    g_fixture.Devices[1].IgnoreDisarm = false;
    assert(ArmSMMUTestDeviceDisable(&devices[1]) == OS_EOK);
    assert(devices[1].State == ArmSMMUTestDeviceIdentified);
    assert((g_fixture.Functions[2][0].Command & 7U) == 0);

    // A register that does not hold its value stops before arming.
    PrepareReady(devices);
    g_fixture.Functions[1][0].StuckCommand = COMMAND_BUS_MASTER;
    assert(ArmSMMUTestDeviceWrite(&devices[0], 0x1000, 0x2000, 8, &result) == OS_EDEVFAULT);
    assert(result == 0x55U);
    for (uint32_t i = 0; i < g_fixture.EventCount; i++) {
        assert(!(g_fixture.Events[i].Type == EventWrite && g_fixture.Events[i].Offset == REG_DOORBELL));
    }
    puts("write: failed arm, disarm and command readback keep the result unchanged");
}

static void
TestDisable(void)
{
    ArmSMMUTestDevice_t devices[2];
    ArmSMMUTestDevice_t empty = { 0 };

    ResetFixture();
    assert(ArmSMMUTestDeviceDisable(&empty) == OS_EOK);
    assert(g_fixture.EventCount == 0);
    assert(ArmSMMUTestDeviceDisable(NULL) == OS_EINVALPARAMS);

    // Ready: disarm first, then clear the command bits.
    PrepareReady(devices);
    assert(ArmSMMUTestDeviceDisable(&devices[0]) == OS_EOK);
    assert(g_fixture.Events[0].Type == EventWrite && g_fixture.Events[0].Offset == REG_DOORBELL &&
           g_fixture.Events[0].Value == 0);
    assert(g_fixture.Events[1].Type == EventRead && g_fixture.Events[1].Offset == REG_RESULT);
    assert(devices[0].State == ArmSMMUTestDeviceIdentified);
    assert(g_fixture.Functions[1][0].Command == 0x400U);

    // Identified: only the command bits. Calling twice is harmless.
    g_fixture.EventCount = 0;
    assert(ArmSMMUTestDeviceDisable(&devices[0]) == OS_EOK);
    assert(CountEvents(EventRead) == 0 && CountEvents(EventWrite) == 0);
    assert(CountEvents(EventConfigWrite) == 1);

    // A command bit that does not clear keeps the record.
    g_fixture.Functions[2][0].StuckCommand = COMMAND_MEMORY;
    assert(ArmSMMUTestDeviceDisable(&devices[1]) == OS_EDEVFAULT);
    assert(devices[1].State == ArmSMMUTestDeviceReady);
    puts("disable: empty, ready, identified and failed readback passed");
}

int
main(void)
{
    TestPrepare();
    TestTopology();
    TestRecordsMustBeZeroed();
    TestBarArithmetic();
    TestBarLayout();
    TestPartialFailures();
    TestWrite();
    TestWriteFailures();
    TestDisable();
    puts("native_pci_test: all checks passed");
    return 0;
}
