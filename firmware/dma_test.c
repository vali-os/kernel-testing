#include <firmware/bindings/dma.h>
#include <firmware/tree/resources.h>
#include <ddk/utils.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

/** Actionable diagnostics are captured separately from optional trace output. */
static struct __DiagnosticCapture {
    unsigned int Warnings;
    unsigned int Errors;
    char         Message[512];
} g_Diagnostics;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    va_list arguments;

    if (level == OSSYSLOGLEVEL_WARNING) {
        g_Diagnostics.Warnings++;
    } else if (level == OSSYSLOGLEVEL_ERROR) {
        g_Diagnostics.Errors++;
    } else {
        return;
    }

    va_start(arguments, format);
    vsnprintf(g_Diagnostics.Message, sizeof(g_Diagnostics.Message), format, arguments);
    va_end(arguments);
}

static void
__StoreCells(
    _Out_ uint8_t*        bytes,
    _In_  const uint32_t* cells,
    _In_  uint32_t        count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        bytes[i * 4] = cells[i] >> 24;
        bytes[i * 4 + 1] = cells[i] >> 16;
        bytes[i * 4 + 2] = cells[i] >> 8;
        bytes[i * 4 + 3] = cells[i];
    }
}

static oserr_t
__DecodeBus(
    _In_  const struct FdtResources* bus,
    _In_  enum FdtDmaAddressFormat   child,
    _In_  enum FdtDmaAddressFormat   parent,
    _Out_ FdtDmaBusWindows_t*        windows)
{
    return FdtDecodeDmaRanges(bus, child, parent, windows);
}

// A rejected property leaves the output untouched and logs exactly one message
// that names the node and, when a window is at fault, its index.
static void
__DecodeFailure(
    _In_ const struct FdtResources* bus,
    _In_ enum FdtDmaAddressFormat   child,
    _In_ enum FdtDmaAddressFormat   parent,
    _In_ oserr_t                   expected)
{
    FdtDmaBusWindows_t result;
    FdtDmaBusWindows_t saved;

    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    memset(&g_Diagnostics, 0, sizeof(g_Diagnostics));
    CHECK(__DecodeBus(bus, child, parent, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
    CHECK(g_Diagnostics.Warnings + g_Diagnostics.Errors == 1);
    CHECK(strstr(g_Diagnostics.Message, "FdtDecodeDmaRanges: node ") == g_Diagnostics.Message);
}

static void
__Decode(void)
{
    uint32_t cells[] = {
        0x10, 0, 0x43000000, 0x10, 0, 0x10, 0,
        0, 0, 0x02000000, 0x10, 0, 0x10, 0
    };
    uint32_t simple[] = { 0x1000, 2, 0x2000, 0x100 };
    uint8_t bytes[(FDT_DMA_MAX_WINDOWS + 1) * 28];
    struct FdtResources bus = {
        .AddressCells = 2,
        .ParentAddressCells = 3,
        .SizeCells = 2,
        .DmaRanges = bytes,
        .DmaRangesLength = sizeof(cells)
    };
    FdtDmaBusWindows_t result;
    uint32_t badFlags[] = { 0, 0x01000000, 0x23000000, 0x03000100, 0x07000000 };
    uint32_t i;

    __StoreCells(bytes, cells, 14);
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.State == FdtDmaPropertyWindows && result.Count == 2);
    CHECK(result.Windows[0].ParentBase == 0x1000000000ULL);
    CHECK(result.Windows[0].ParentAttributes == 0x43000000);
    CHECK(result.Windows[1].ChildBase == 0 && result.Windows[1].Length == 0x1000000000ULL);
    CHECK(result.Windows[1].ParentAttributes == 0x02000000);
    for (i = 0; i < sizeof(badFlags) / sizeof(badFlags[0]); i++) {
        cells[2] = badFlags[i];
        __StoreCells(bytes, cells, 14);
        __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_ENOTSUPPORTED);
    }
    cells[2] = 0xc3000000;
    __StoreCells(bytes, cells, 14);
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.Windows[0].ParentAttributes == 0xc3000000);

    bus.DmaRangesLength--;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);
    bus.DmaRangesLength = sizeof(bytes);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_ENOTSUPPORTED);
    bus.DmaRangesLength = sizeof(cells);
    cells[5] = 0;
    __StoreCells(bytes, cells, 14);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);
    cells[5] = 0x10;
    cells[0] = 0;
    __StoreCells(bytes, cells, 14);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);
    cells[0] = UINT32_MAX;
    cells[1] = UINT32_MAX;
    __StoreCells(bytes, cells, 14);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);
    cells[0] = 0x10;
    cells[1] = 0;
    cells[3] = UINT32_MAX;
    cells[4] = UINT32_MAX;
    __StoreCells(bytes, cells, 14);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);

    bus.DmaRanges = NULL;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_EINVALPARAMS);
    bus.DmaRangesLength = 0;
    memset(&g_Diagnostics, 0, sizeof(g_Diagnostics));
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.State == FdtDmaPropertyAbsent && result.Count == 0);
    CHECK(g_Diagnostics.Warnings == 0 && g_Diagnostics.Errors == 0);
    bus.DmaRanges = bytes;
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.State == FdtDmaPropertyIdentity && result.Count == 0);
    CHECK(g_Diagnostics.Warnings == 0 && g_Diagnostics.Errors == 0);
    bus.SizeCells = 0;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_ENOTSUPPORTED);
    bus.SizeCells = 2;
    bus.AddressCells = 3;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, OS_ENOTSUPPORTED);

    bus.AddressCells = 1;
    bus.ParentAddressCells = 2;
    bus.SizeCells = 1;
    bus.DmaRangesLength = sizeof(simple);
    __StoreCells(bytes, simple, 4);
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, &result) == OS_EOK);
    CHECK(result.ChildLimit == UINT32_MAX && result.Windows[0].ParentBase == 0x200002000ULL);
    simple[0] = UINT32_MAX;
    __StoreCells(bytes, simple, 4);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    simple[3] = 1;
    __StoreCells(bytes, simple, 4);
    CHECK(__DecodeBus(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, &result) == OS_EOK);
    bus.Malformed = 1;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    bus.Malformed = 0;
    bus.ParentMalformed = 1;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    bus.ParentMalformed = 0;
    bus.ParentAddressCells = 4;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_ENOTSUPPORTED);

    // Exercise the PCI child encoding independently of the RP1 parent case.
    cells[0] = 0x43000000;
    cells[1] = 0x10;
    cells[2] = 0;
    cells[3] = 0;
    cells[4] = 0x8000;
    cells[5] = 0;
    cells[6] = 0x1000;
    __StoreCells(bytes, cells, 7);
    bus.AddressCells = 3;
    bus.ParentAddressCells = 2;
    bus.SizeCells = 2;
    bus.DmaRangesLength = 28;
    CHECK(__DecodeBus(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EOK);
    CHECK(result.Windows[0].ChildBase == 0x1000000000ULL);
    CHECK(result.Windows[0].ChildAttributes == 0x43000000);
    CHECK(result.Windows[0].ParentBase == 0x8000);
    CHECK(__DecodeBus(NULL, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EINVALPARAMS);

    // The 32-bit memory tag does not bound a window. Only the 64-bit PCI
    // address cells do, so a tagged window may cross or sit above 4 GiB.
    cells[0] = 0x02000000;
    cells[1] = 0;
    cells[2] = 0xfffff000;
    cells[6] = 0x2000;
    __StoreCells(bytes, cells, 7);
    CHECK(__DecodeBus(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EOK);
    cells[0] = 0x03000000;
    __StoreCells(bytes, cells, 7);
    CHECK(__DecodeBus(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EOK);

    // Raspberry Pi 4 firmware window: PCI 0x4_00000000 reaches the first
    // 4 GiB of CPU RAM, and still carries the 32-bit memory tag.
    cells[0] = 0x02000000;
    cells[1] = 0x4;
    cells[2] = 0;
    cells[3] = 0;
    cells[4] = 0;
    cells[5] = 0x1;
    cells[6] = 0;
    __StoreCells(bytes, cells, 7);
    CHECK(__DecodeBus(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EOK);
    CHECK(result.Windows[0].ChildBase == 0x400000000ULL);
    CHECK(result.Windows[0].ChildAttributes == 0x02000000);
    CHECK(result.Windows[0].ParentBase == 0);
    CHECK(result.Windows[0].Length == 0x100000000ULL);

    // A window past the end of the 64-bit PCI address space is still rejected.
    cells[1] = UINT32_MAX;
    cells[2] = 0xfffff000;
    cells[5] = 0;
    cells[6] = 0x2000;
    __StoreCells(bytes, cells, 7);
    __DecodeFailure(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, OS_EINVALPARAMS);
}

static void
__ComposeFailure(
    _In_ const FdtDmaBusWindows_t* child,
    _In_ const FdtDmaMap_t* parent,
    _In_ oserr_t expected)
{
    FdtDmaMap_t result;
    FdtDmaMap_t saved;

    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    memset(&g_Diagnostics, 0, sizeof(g_Diagnostics));
    CHECK(FdtDmaCompose(child, parent, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
    CHECK(g_Diagnostics.Warnings == 0 && g_Diagnostics.Errors == 0);
}

// Two overlapping child windows and a truncated property must each log the
// failing windows, which the status code alone cannot tell apart.
static void
__DecodeFaultDetail(void)
{
    uint32_t windows[] = {
        0x1000, 0x8000, 0x2000,
        0x2000, 0xa000, 0x2000
    };
    uint8_t bytes[sizeof(windows)];
    struct FdtResources bus = {
        .AddressCells = 1,
        .ParentAddressCells = 1,
        .SizeCells = 1,
        .DmaRanges = bytes,
        .DmaRangesLength = sizeof(windows)
    };
    FdtDmaBusWindows_t result;
    FdtDmaProperty_t   property;
    FdtDmaFault_t      fault = { FdtDmaFaultNone };

    __StoreCells(bytes, windows, 6);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    CHECK(strstr(g_Diagnostics.Message, "windows 1 and 0 overlap") != NULL);

    bus.DmaRangesLength = sizeof(windows) - 4;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    CHECK(strstr(g_Diagnostics.Message, "truncated DMA window") != NULL);

    property = (FdtDmaProperty_t) {
        .Bytes = bytes,
        .Length = sizeof(windows),
        .ChildCells = 1,
        .ParentCells = 1,
        .SizeCells = 1,
        .ChildFormat = FdtDmaAddressSimple,
        .ParentFormat = FdtDmaAddressSimple
    };
    CHECK(FdtDmaDecode(&property, &result, &fault) == OS_EINVALPARAMS);
    CHECK(fault.Kind == FdtDmaFaultOverlap && fault.Window == 1 && fault.OtherWindow == 0);
    fault.Kind = FdtDmaFaultNone;
    CHECK(FdtDmaDecode(&property, &result, NULL) == OS_EINVALPARAMS);
    CHECK(fault.Kind == FdtDmaFaultNone);

    // Parent overflow and child overflow must report different extents.
    property.Length = 16;
    property.ParentCells = 2;
    __StoreCells(bytes, (uint32_t[]) { 0, UINT32_MAX, UINT32_MAX, 2 }, 4);
    CHECK(FdtDmaDecode(&property, &result, &fault) == OS_EINVALPARAMS);
    CHECK(fault.Kind == FdtDmaFaultParentExtent && fault.Window == 0);
    __StoreCells(bytes, (uint32_t[]) { UINT32_MAX, 0, 0, 2 }, 4);
    CHECK(FdtDmaDecode(&property, &result, &fault) == OS_EINVALPARAMS);
    CHECK(fault.Kind == FdtDmaFaultChildExtent && fault.Window == 0);

}

static void
__Compose(void)
{
    FdtDmaBusWindows_t child = {
        .State = FdtDmaPropertyWindows,
        .ChildLimit = UINT64_MAX,
        .Count = 2,
        .Windows = {
            { .ChildBase = 0x100000, .ParentBase = 0x1800, .Length = 0x3000 },
            { .ChildBase = 0x10000, .ParentBase = 0x1800, .Length = 0x3000 }
        }
    };
    FdtDmaMap_t parent = {
        .Count = 3,
        .Ranges = {
            { .DeviceBase = 0x4000, .PhysicalBase = 0xd000, .Length = 0x1000 },
            { .DeviceBase = 0x1000, .PhysicalBase = 0x8000, .Length = 0x1000 },
            { .DeviceBase = 0x2000, .PhysicalBase = 0xb000, .Length = 0x800 }
        }
    };
    FdtDmaMap_t result;
    uint32_t i;

    CHECK(FdtDmaCompose(&child, &parent, &result) == OS_EOK);
    CHECK(result.Count == 6);
    CHECK(result.Ranges[0].DeviceBase == 0x10000 && result.Ranges[0].PhysicalBase == 0x8800);
    CHECK(result.Ranges[1].DeviceBase == 0x10800 && result.Ranges[1].PhysicalBase == 0xb000);
    CHECK(result.Ranges[2].DeviceBase == 0x12800 && result.Ranges[2].PhysicalBase == 0xd000);
    CHECK(result.Ranges[3].DeviceBase == 0x100000 && result.Ranges[3].PhysicalBase == 0x8800);
    for (i = 0; i < result.Count; i++) {
        CHECK(result.Ranges[i].Length == 0x800);
    }
    CHECK(!FdtContainsRange(result.Ranges[0].DeviceBase, result.Ranges[0].Length, 0x107ff, 2));
    child.Windows[0].ParentBase = 0x5000;
    child.Count = 1;
    __ComposeFailure(&child, &parent, OS_ENOENT);
    child.Windows[0].ParentBase = 0x4fff;
    CHECK(FdtDmaCompose(&child, &parent, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Ranges[0].Length == 1);

    child.State = FdtDmaPropertyIdentity;
    child.Count = 0;
    CHECK(FdtDmaCompose(&child, &parent, &parent) == OS_EOK);
    CHECK(parent.Count == 3 && parent.Ranges[0].DeviceBase == 0x1000);
    child.State = FdtDmaPropertyAbsent;
    __ComposeFailure(&child, &parent, OS_ENOTSUPPORTED);
    child.State = FdtDmaPropertyIdentity;
    parent.Ranges[2].DeviceBase = parent.Ranges[0].DeviceBase;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Count = 1;
    parent.Ranges[0].Length = 0;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0] = (FdtDmaRange_t) {
        .DeviceBase = UINT64_MAX, .PhysicalBase = UINT64_MAX, .Length = 1
    };
    CHECK(FdtDmaCompose(&child, &parent, &result) == OS_EOK);
    CHECK(result.Ranges[0].DeviceBase == UINT64_MAX && result.Ranges[0].Length == 1);
    parent.Ranges[0].Length = 2;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0].DeviceBase = 0;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0] = (FdtDmaRange_t) {
        .DeviceBase = 0xfffff000, .PhysicalBase = 0x8000, .Length = 0x2000
    };
    child.ChildLimit = UINT32_MAX;
    CHECK(FdtDmaCompose(&child, &parent, &result) == OS_EOK);
    CHECK(result.Ranges[0].Length == 0x1000);
    parent.Ranges[0].DeviceBase = 0x100000000ULL;
    __ComposeFailure(&child, &parent, OS_ENOENT);

    // A legal set of aliases can require more output fragments than capacity.
    child.State = FdtDmaPropertyWindows;
    child.ChildLimit = UINT64_MAX;
    child.Count = FDT_DMA_MAX_WINDOWS;
    for (i = 0; i < child.Count; i++) {
        child.Windows[i] = (FdtDmaWindow_t) {
            .ChildBase = i * 0x10000, .ParentBase = 0, .Length = 0x10000
        };
    }
    parent.Count = 5;
    for (i = 0; i < parent.Count; i++) {
        parent.Ranges[i] = (FdtDmaRange_t) {
            .DeviceBase = i * 0x2000, .PhysicalBase = i * 0x3000, .Length = 0x1000
        };
    }
    __ComposeFailure(&child, &parent, OS_ENOTSUPPORTED);
    child.Count = 1;
    child.Windows[0].Length = 0;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    child.Windows[0].Length = 0x10000;
    child.Count = FDT_DMA_MAX_WINDOWS + 1;
    __ComposeFailure(&child, &parent, OS_ENOTSUPPORTED);
    child.Count = 1;
    parent.Count = FDT_DMA_MAX_RANGES + 1;
    __ComposeFailure(&child, &parent, OS_ENOTSUPPORTED);
}

static void
__NodeIsDirect(void)
{
    const char simpleBus[] = "simple-bus";
    const char firmwareBus[] = "simple-mfd";
    FdtDmaNode_t node = { .Compatible = simpleBus, .CompatibleLength = sizeof(simpleBus) };
    enum FdtDmaIndirectReason reason = FdtDmaIndirectIommu;

    CHECK(FdtDmaNodeIsDirect(&node, NULL));
    node.HasDmaRanges = true;
    CHECK(FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);

    node.Disabled = true;
    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectDisabled);
    node.Disabled = false;
    node.HasIommus = true;
    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);
    node.HasIommus = false;
    node.HasIommuMap = true;
    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);
    node.HasIommuMap = false;
    node.HasDmaParent = true;
    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectDmaParent);
    node.HasDmaParent = false;

    // Only buses that carry DMA are accepted: simple-bus, PCI hosts, buses
    // below a PCI host and the root. Other buses hold ranges for firmware.
    node.Compatible = firmwareBus;
    node.CompatibleLength = sizeof(firmwareBus);
    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectFirmwareBus);
    node.IsPci = true;
    CHECK(FdtDmaNodeIsDirect(&node, NULL));
    node.IsPci = false;
    node.ParentIsPci = true;
    CHECK(FdtDmaNodeIsDirect(&node, NULL));
    node.ParentIsPci = false;
    node.IsRoot = true;
    CHECK(FdtDmaNodeIsDirect(&node, NULL));
    node.IsRoot = false;
    node.HasDmaRanges = false;
    CHECK(FdtDmaNodeIsDirect(&node, NULL));
    node.HasDmaRanges = true;
    node.Compatible = NULL;
    node.CompatibleLength = 0;
    CHECK(FdtDmaNodeIsDirect(&node, NULL));
}

static void
__DeviceIsDirect(void)
{
    const char compatible[] = "vendor,device";
    FdtDmaNode_t node = { .Compatible = compatible, .CompatibleLength = sizeof(compatible) };
    enum FdtDmaIndirectReason reason = FdtDmaIndirectIommu;

    CHECK(FdtDmaDeviceIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);
    node.IsPci = true;
    CHECK(!FdtDmaDeviceIsDirect(&node, &reason) && reason == FdtDmaIndirectPci);
    node.IsPci = false;
    node.ParentIsPci = true;
    CHECK(FdtDmaDeviceIsDirect(&node, NULL));
    node.Disabled = true;
    CHECK(!FdtDmaDeviceIsDirect(&node, &reason) && reason == FdtDmaIndirectDisabled);
}

// Reserved memory can carry DMA restrictions that no direct resolver supports.
static void
__MemoryRegionIsUnsupported(void)
{
    FdtDmaNode_t node = { .IsRoot = true, .HasMemoryRegion = true };
    enum FdtDmaIndirectReason reason;

    CHECK(!FdtDmaNodeIsDirect(&node, &reason) && reason == FdtDmaIndirectMemoryRegion);
    CHECK(!FdtDmaDeviceIsDirect(&node, &reason) && reason == FdtDmaIndirectMemoryRegion);
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectMemoryRegion);
}

// The bus rule adds simple-bus and PCI checks on top of the node rule. The root
// is only held to the node rule.
static void
__BusIsDirect(void)
{
    const char simpleBus[] = "simple-bus";
    const char firmwareBus[] = "simple-mfd";
    const char plainBus[] = "vendor,bus";
    FdtDmaNode_t node = { .Compatible = simpleBus, .CompatibleLength = sizeof(simpleBus) };
    enum FdtDmaIndirectReason reason = FdtDmaIndirectIommu;

    CHECK(FdtDmaBusIsDirect(&node, NULL));
    CHECK(FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);

    node.Disabled = true;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectDisabled);
    node.Disabled = false;
    node.HasIommus = true;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectIommu);
    node.HasIommus = false;

    node.IsPci = true;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectPci);
    node.IsPci = false;

    node.Compatible = plainBus;
    node.CompatibleLength = sizeof(plainBus);
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectNotSimpleBus);
    node.Compatible = NULL;
    node.CompatibleLength = 0;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectNotSimpleBus);

    // A firmware bus is reported by the node rule before the bus rule runs.
    node.Compatible = firmwareBus;
    node.CompatibleLength = sizeof(firmwareBus);
    node.HasDmaRanges = true;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectFirmwareBus);
    node.HasDmaRanges = false;

    node.IsRoot = true;
    node.IsPci = true;
    CHECK(!FdtDmaBusIsDirect(&node, &reason) && reason == FdtDmaIndirectPci);
    node.IsPci = false;
    CHECK(FdtDmaBusIsDirect(&node, NULL));
}

int
main(void)
{
    __Decode();
    __DecodeFaultDetail();
    __Compose();
    __NodeIsDirect();
    __BusIsDirect();
    __DeviceIsDirect();
    __MemoryRegionIsUnsupported();
    puts("DMA ranges: encodings, aliases, intersections, holes, overflow and atomic failure passed");
    return 0;
}
