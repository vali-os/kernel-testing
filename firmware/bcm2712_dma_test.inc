/**
 * Copyright 2026, Philip Meulengracht
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
 */

// These tests edit cached records temporarily and restore them before returning.
static void
__Pi5FindMemory(const struct FdtResources* nodes, int depth, void* context)
{
    uint32_t* memory = context;

    if (nodes[depth].IsMemory) {
        *memory = nodes[depth].NodeOffset;
    }
}

static void
__Pi5DmaRejected(struct FdtTree* tree, uint32_t node, oserr_t expected)
{
    FdtDmaMap_t map;
    FdtDmaMap_t before;

    memset(&map, 0xa5, sizeof(map));
    before = map;
    CHECK(FdtPlatformDma(tree, node, &map) == expected);
    CHECK(!memcmp(&map, &before, sizeof(map)));
}

// Board fallback requires the device's own bus directly below the tree root.
static void
__Pi5DmaNestedPath(
    const struct FdtResources* root,
    const struct FdtResources* bus,
    const struct FdtResources* device,
    const struct FdtResources* memory)
{
    struct __FdtEntry entries[5] = {
        { .Resources = *root, .Parent = SIZE_MAX, .End = 5, .Depth = 0 },
        { .Resources = *root, .Parent = 0, .End = 4, .Depth = 1 },
        { .Resources = *bus, .Parent = 1, .End = 4, .Depth = 2 },
        { .Resources = *device, .Parent = 2, .End = 4, .Depth = 3 },
        { .Resources = *memory, .Parent = 0, .End = 5, .Depth = 1 }
    };
    struct FdtTree tree = { .Entries = entries, .Count = 5 };
    struct FdtProperty compatible = { "compatible", (const uint8_t*)"simple-bus", sizeof("simple-bus") };
    FdtDmaBusWindows_t ranges;
    FdtDmaBusWindows_t saved;

    for (uint32_t i = 0; i < 5; i++) {
        entries[i].Resources.NodeOffset = i;
        entries[i].Resources.View.NodeOffset = i;
    }
    memset(&ranges, 0xa5, sizeof(ranges));
    saved = ranges;
    CHECK(FdtBoardDmaRanges(&tree, 3, &ranges) == OS_ENOTSUPPORTED);
    CHECK(!memcmp(&ranges, &saved, sizeof(ranges)));

    entries[1].Resources.View.Properties = &compatible;
    entries[1].Resources.View.PropertyCount = 1;
    entries[1].Resources.DmaRanges = (const uint8_t*)"";
    entries[1].Resources.DmaRangesLength = 0;
    __Pi5DmaRejected(&tree, 3, OS_ENOTSUPPORTED);

    // A missing outer route cannot be filled by the device's board route.
    entries[1].Resources.DmaRanges = NULL;
    entries[2].Resources.DmaRanges = (const uint8_t*)"";
    entries[2].Resources.DmaRangesLength = 0;
    __Pi5DmaRejected(&tree, 3, OS_ENOTSUPPORTED);
}

static void
__Pi5Dma(struct PlatformBus* bus, struct PlatformEntry* entry)
{
    struct FdtTree* tree = bus->Tree;
    struct FdtResources* device = (void*)FdtTreeNode(tree, entry->Firmware.FirmwareNode);
    struct FdtResources* parent = (void*)FdtTreeParent(tree, device->NodeOffset);
    struct FdtResources* root = (void*)FdtTreeParent(tree, parent->NodeOffset);
    struct FdtResources* memory;
    struct FdtResources savedDevice = *device;
    struct FdtResources savedParent = *parent;
    struct FdtResources savedRoot = *root;
    struct FdtResources savedMemory;
    struct FdtResources* path[] = { device, parent, root };
    const char* unsupported[] = { "iommus", "iommu-map", "dma-parent", "memory-region" };
    struct FdtProperty property = { "iommus", (const uint8_t*)"", 0 };
    FdtDmaMap_t map;
    DmDmaDescription_t description;
    uint32_t memoryNode = UINT32_MAX;
    // Low RAM, a range crossing 4 GiB, and RAM wholly above 4 GiB.
    const uint8_t memoryRanges[] = {
        0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0,
        0, 0, 0, 0, 0xff, 0xff, 0xf0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0,
        0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0
    };
    const uint8_t firmwareRange[] = {
        0x40, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0x10, 0
    };

    CHECK(device->PhysicalBase == 0x1000fff000ULL && parent->DmaRanges == NULL);
    CHECK(FdtPlatformDma(tree, device->NodeOffset, &map) == OS_EOK);
    CHECK(map.Count == 1 && map.Ranges[0].PhysicalBase == 0);
    CHECK(map.Ranges[0].DeviceBase == 0 && map.Ranges[0].Length == 0x28000000);
    CHECK(PlatformDescribeDma(entry, &description) == OS_EOK);
    CHECK(description.CachePolicy == DmDmaCacheNonCoherent);
    CHECK(!memcmp(&description.Map, &map, sizeof(map)) && entry->DmaReferences == 0);

    // Board identification and the physical register block restrict the exception.
    root->View.PropertyCount = 0;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *root = savedRoot;
    property = (struct FdtProperty) { "compatible", (const uint8_t*)"brcm,bcm2712", 13 };
    root->View.Properties = &property;
    root->View.PropertyCount = 1;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *root = savedRoot;
    property = (struct FdtProperty) { "iommus", (const uint8_t*)"", 0 };
    device->View.PropertyCount = 0;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *device = savedDevice;
    device->PhysicalBase = 0x1001100000ULL;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *device = savedDevice;
    device->PhysicalLength--;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *device = savedDevice;
    device->RegisterStatus = OS_EINVALPARAMS;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *device = savedDevice;
    property = (struct FdtProperty) { "compatible", (const uint8_t*)"simple-mfd", 11 };
    parent->View.Properties = &property;
    parent->View.PropertyCount = 1;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *parent = savedParent;
    property = (struct FdtProperty) { "iommus", (const uint8_t*)"", 0 };
    parent->AddressCells = 2;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
    *parent = savedParent;

    // Reject routing or unusable records at every level before using the exception.
    for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
        struct FdtResources saved = *path[i];
        struct FdtProperty* properties = malloc((saved.View.PropertyCount + 1) * sizeof(*properties));

        CHECK(properties != NULL);
        memcpy(properties, saved.View.Properties, saved.View.PropertyCount * sizeof(*properties));
        for (size_t j = 0; j < sizeof(unsupported) / sizeof(unsupported[0]); j++) {
            property.Name = unsupported[j];
            properties[saved.View.PropertyCount] = property;
            path[i]->View.Properties = properties;
            path[i]->View.PropertyCount = saved.View.PropertyCount + 1;
            __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
            *path[i] = saved;
        }
        free(properties);
        path[i]->Disabled = 1;
        __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOENT);
        *path[i] = saved;
        path[i]->Malformed = 1;
        __Pi5DmaRejected(tree, device->NodeOffset, OS_EINVALPARAMS);
        *path[i] = saved;
        path[i]->IsPci = 1;
        __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOTSUPPORTED);
        *path[i] = saved;
    }

    // Explicit firmware routes remain authoritative, including malformed ones.
    parent->DmaRanges = firmwareRange;
    parent->DmaRangesLength = sizeof(firmwareRange);
    CHECK(FdtPlatformDma(tree, device->NodeOffset, &map) == OS_EOK);
    CHECK(map.Count == 1 && map.Ranges[0].PhysicalBase == 0x10000000);
    CHECK(map.Ranges[0].DeviceBase == 0x40000000 && map.Ranges[0].Length == 0x1000);
    parent->DmaRangesLength--;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_EINVALPARAMS);
    parent->DmaRangesLength = 0;
    {
        FdtDmaBusWindows_t ranges;
        CHECK(FdtBoardDmaRanges(tree, device->NodeOffset, &ranges) == OS_ENOTSUPPORTED);
    }
    CHECK(FdtPlatformDma(tree, device->NodeOffset, &map) == OS_EOK);
    CHECK(map.Count == 1 && map.Ranges[0].DeviceBase == 0);
    *parent = savedParent;

    CHECK(FdtWalkResources(tree, __Pi5FindMemory, &memoryNode) == OS_EOK);
    CHECK(memoryNode != UINT32_MAX);
    memory = (void*)FdtTreeNode(tree, memoryNode);
    savedMemory = *memory;
    memory->Reg = memoryRanges;
    memory->RegLength = sizeof(memoryRanges);
    CHECK(FdtPlatformDma(tree, device->NodeOffset, &map) == OS_EOK);
    CHECK(map.Count == 2);
    CHECK(map.Ranges[0].PhysicalBase == 0x100000 && map.Ranges[0].DeviceBase == 0x100000);
    CHECK(map.Ranges[0].Length == 0x200000);
    CHECK(map.Ranges[1].PhysicalBase == 0xfffff000 && map.Ranges[1].DeviceBase == 0xfffff000);
    CHECK(map.Ranges[1].Length == 0x1000);
    memory->Reg = memoryRanges + 32;
    memory->RegLength = 16;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_ENOENT);
    memory->RegLength--;
    __Pi5DmaRejected(tree, device->NodeOffset, OS_EINVALPARAMS);
    *memory = savedMemory;
    __Pi5DmaNestedPath(root, parent, device, memory);
}
