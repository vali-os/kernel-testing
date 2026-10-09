#include <firmware/dma.h>
#include <firmware/resources.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

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

static void
__DecodeFailure(
    _In_ const struct FdtResources* bus,
    _In_ enum FdtDmaAddressFormat   child,
    _In_ enum FdtDmaAddressFormat   parent,
    _In_ oserr_t                   expected)
{
    struct FdtDmaRanges result;
    struct FdtDmaRanges saved;

    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    CHECK(FdtDecodeDmaRanges(bus, child, parent, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
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
    struct FdtDmaRanges result;
    uint32_t badFlags[] = { 0, 0x01000000, 0x23000000, 0x03000100, 0x07000000 };
    uint32_t i;

    __StoreCells(bytes, cells, 14);
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
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
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
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
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.State == FdtDmaPropertyAbsent && result.Count == 0);
    bus.DmaRanges = bytes;
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressPci, &result) == OS_EOK);
    CHECK(result.State == FdtDmaPropertyIdentity && result.Count == 0);
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
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, &result) == OS_EOK);
    CHECK(result.ChildLimit == UINT32_MAX && result.Windows[0].ParentBase == 0x200002000ULL);
    simple[0] = UINT32_MAX;
    __StoreCells(bytes, simple, 4);
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    simple[3] = 1;
    __StoreCells(bytes, simple, 4);
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, &result) == OS_EOK);
    bus.Malformed = 1;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    bus.Malformed = 0;
    bus.AncestorMalformed = 1;
    __DecodeFailure(&bus, FdtDmaAddressSimple, FdtDmaAddressSimple, OS_EINVALPARAMS);
    bus.AncestorMalformed = 0;
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
    CHECK(FdtDecodeDmaRanges(&bus, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EOK);
    CHECK(result.Windows[0].ChildBase == 0x1000000000ULL);
    CHECK(result.Windows[0].ChildAttributes == 0x43000000);
    CHECK(result.Windows[0].ParentBase == 0x8000);
    CHECK(FdtDecodeDmaRanges(NULL, FdtDmaAddressPci, FdtDmaAddressSimple, &result) == OS_EINVALPARAMS);
}

static void
__ComposeFailure(
    _In_ const struct FdtDmaRanges* child,
    _In_ const struct FdtDmaMap* parent,
    _In_ oserr_t expected)
{
    struct FdtDmaMap result;
    struct FdtDmaMap saved;

    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    CHECK(FdtComposeDmaRanges(child, parent, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
}

static void
__Compose(void)
{
    struct FdtDmaRanges child = {
        .State = FdtDmaPropertyWindows,
        .ChildLimit = UINT64_MAX,
        .Count = 2,
        .Windows = {
            { .ChildBase = 0x100000, .ParentBase = 0x1800, .Length = 0x3000 },
            { .ChildBase = 0x10000, .ParentBase = 0x1800, .Length = 0x3000 }
        }
    };
    struct FdtDmaMap parent = {
        .Count = 3,
        .Ranges = {
            { .DeviceBase = 0x4000, .PhysicalBase = 0xd000, .Length = 0x1000 },
            { .DeviceBase = 0x1000, .PhysicalBase = 0x8000, .Length = 0x1000 },
            { .DeviceBase = 0x2000, .PhysicalBase = 0xb000, .Length = 0x800 }
        }
    };
    struct FdtDmaMap result;
    uint32_t i;

    CHECK(FdtComposeDmaRanges(&child, &parent, &result) == OS_EOK);
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
    CHECK(FdtComposeDmaRanges(&child, &parent, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Ranges[0].Length == 1);

    child.State = FdtDmaPropertyIdentity;
    child.Count = 0;
    CHECK(FdtComposeDmaRanges(&child, &parent, &parent) == OS_EOK);
    CHECK(parent.Count == 3 && parent.Ranges[0].DeviceBase == 0x1000);
    child.State = FdtDmaPropertyAbsent;
    __ComposeFailure(&child, &parent, OS_ENOTSUPPORTED);
    child.State = FdtDmaPropertyIdentity;
    parent.Ranges[2].DeviceBase = parent.Ranges[0].DeviceBase;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Count = 1;
    parent.Ranges[0].Length = 0;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0] = (struct FdtDmaRange) {
        .DeviceBase = UINT64_MAX, .PhysicalBase = UINT64_MAX, .Length = 1
    };
    CHECK(FdtComposeDmaRanges(&child, &parent, &result) == OS_EOK);
    CHECK(result.Ranges[0].DeviceBase == UINT64_MAX && result.Ranges[0].Length == 1);
    parent.Ranges[0].Length = 2;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0].DeviceBase = 0;
    __ComposeFailure(&child, &parent, OS_EINVALPARAMS);
    parent.Ranges[0] = (struct FdtDmaRange) {
        .DeviceBase = 0xfffff000, .PhysicalBase = 0x8000, .Length = 0x2000
    };
    child.ChildLimit = UINT32_MAX;
    CHECK(FdtComposeDmaRanges(&child, &parent, &result) == OS_EOK);
    CHECK(result.Ranges[0].Length == 0x1000);
    parent.Ranges[0].DeviceBase = 0x100000000ULL;
    __ComposeFailure(&child, &parent, OS_ENOENT);

    // A legal set of aliases can require more output fragments than capacity.
    child.State = FdtDmaPropertyWindows;
    child.ChildLimit = UINT64_MAX;
    child.Count = FDT_DMA_MAX_WINDOWS;
    for (i = 0; i < child.Count; i++) {
        child.Windows[i] = (struct FdtDmaWindow) {
            .ChildBase = i * 0x10000, .ParentBase = 0, .Length = 0x10000
        };
    }
    parent.Count = 5;
    for (i = 0; i < parent.Count; i++) {
        parent.Ranges[i] = (struct FdtDmaRange) {
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

int
main(void)
{
    __Decode();
    __Compose();
    puts("DMA ranges: encodings, aliases, intersections, holes, overflow and atomic failure passed");
    return 0;
}
