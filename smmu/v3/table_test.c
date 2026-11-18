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
 * Exact permissions, shared page paths, delayed walks and generic DMA lifetimes.
 */
#include "model.h"
#include "../../../kernel/memory/dma/private.h"
#include <string.h>

static void __Mappings(enum DMADirection direction)
{
    SMMUv3Unit_t* unit;
    SMMUv3Space_t* space = NULL;
    void* connection = NULL;
    SMMUv3Mapping_t* first = NULL;
    SMMUv3Mapping_t* second = NULL;
    uint32_t sid = 65;
    SHMSG_t extent = { .Address = 0x12345000, .Length = 4096 };
    DMAIOMMUMappingConfig_t a = { .DeviceAddress = 0x3000, .Length = 4096,
        .Direction = direction, .Extents = &extent, .ExtentCount = 1 };
    DMAIOMMUMappingConfig_t b = a;
    uint64_t physical;
    uint32_t producers;
    int tables;

    b.DeviceAddress = 0x4000;
    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMUv3SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMUv3HardwareIDsPrepare(space, &sid, 1, &connection) == OS_EOK);
    assert(SMMUv3HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMUv3MappingPrepare(space, &a, (void**)&first) == OS_EOK);
    assert(SMMUv3MappingPrepare(space, &b, (void**)&second) == OS_EOK);
    g_noHeap = true;
    // Install in reverse preparation order. Both must resolve shared paths now.
    assert(SMMUv3MappingInstall(space, second, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    tables = space->Table.Pages.count;
    assert(SMMUv3MappingInstall(space, first, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(space->Table.Pages.count == tables && tables == 3);
    assert(SMMUv3ModelAccess(sid, a.DeviceAddress + 17, false, &physical) ==
           (direction == DMAFromDevice ? OS_EDEVFAULT : OS_EOK));
    assert(SMMUv3ModelAccess(sid, a.DeviceAddress + 17, true, &physical) ==
           (direction == DMAToDevice ? OS_EDEVFAULT : OS_EOK));
    assert(physical == extent.Address + 17);
    // SYNC must wait for an older table walk even after clearing the leaf and
    // consuming its invalidation. The model keeps that cached translation live.
    g_smmuv3Model.HeldWalks = 1;
    g_smmuv3Model.TimeStep = 10000000;
    assert(SMMUv3MappingRemove(space, first, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(first->Phase == SMMUv3MappingInvalidate && first->Stage2.InstalledBytes == 0);
    assert(SMMUv3ModelAccess(sid, a.DeviceAddress, direction == DMAFromDevice, &physical) == OS_EOK);
    producers = g_smmuv3Model.ProducerWrites;
    g_smmuv3Model.HeldWalks = 0;
    g_smmuv3Model.TimeStep = 1000;
    assert(SMMUv3MappingRemove(space, first, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmuv3Model.ProducerWrites == producers);
    assert(SMMUv3ModelAccess(sid, a.DeviceAddress, false, &physical) == OS_EDEVFAULT);
    SMMUv3MappingDestroy(space, first);
    assert(space->Table.Pages.count == tables);
    assert(SMMUv3ModelAccess(sid, b.DeviceAddress, direction == DMAFromDevice, &physical) == OS_EOK);
    assert(SMMUv3MappingRemove(space, second, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMUv3MappingDestroy(space, second);
    assert(SMMUv3HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMUv3HardwareIDsDestroy(space, connection);
    SMMUv3SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __PreparationAndPartialInstall(void)
{
    SMMUv3Unit_t* unit;
    SMMUv3Space_t* space = NULL;
    SMMUv3Mapping_t* mapping = NULL;
    SHMSG_t extent = { .Address = 0xFFFFFFF000ULL, .Length = 3 * 4096 };
    DMAIOMMUMappingConfig_t config = { .DeviceAddress = (1ULL << 39) - 4096,
        .Length = 3 * 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1 };
    size_t freePages;
    int i;

    SMMUv3ModelBegin(true);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMUv3SpaceCreate(unit, (void**)&space) == OS_EOK);
    freePages = MemoryStackFreeBlockCount(&unit->Common.Memory.FreePages);
    for (i = 0; i < 7; i++) {
        g_failHeapAfter = i;
        assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EOOM);
        assert(mapping == NULL && space->Table.MappingCount == 0);
        assert(MemoryStackFreeBlockCount(&unit->Common.Memory.FreePages) == freePages);
    }
    g_failHeapAfter = -1;
    extent.Length--;
    assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS);
    extent.Length++;
    assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
    assert(mapping->Stage2.SparePages.count == 6);
    g_smmuv3Model.TimeStep = 40000000;
    assert(SMMUv3MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(mapping->Stage2.InstalledBytes != 0 && mapping->Stage2.InstalledBytes < config.Length);
    g_smmuv3Model.TimeStep = 1000;
    g_noHeap = true;
    assert(SMMUv3MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMUv3MappingDestroy(space, mapping);
    SMMUv3SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __VMIDCapacity(void)
{
    SMMUv3Unit_t* unit;
    SMMUv3Space_t* spaces[256] = {0};
    unsigned int i;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    for (i = 0; i < 255; i++) {
        assert(SMMUv3SpaceCreate(unit, (void**)&spaces[i]) == OS_EOK);
        assert(spaces[i]->VMID == i + 1);
    }
    assert(SMMUv3SpaceCreate(unit, (void**)&spaces[255]) == OS_ENOTSUPPORTED);
    for (i = 0; i < 255; i++) SMMUv3SpaceDestroy(unit, spaces[i]);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __GenericDMA(void)
{
    SMMUv3Unit_t* unit;
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressMapping* mapping = NULL;
    DMAIOVAConfig_t addresses = { .Granularity = 4096, .Bounds = {0x1000, 0xFFFFF} };
    struct DMAMapRequirements requirements = { .AddressAlignment = 4096 };
    SHMSG_t extent = { .Address = 0x200000, .Length = 8192 };
    void* connection = NULL;
    uint32_t sid = 7;
    uint64_t physical;
    uint64_t iova;
    uint32_t count;
    const struct DMASegment* segments;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    assert(DMAAddressSpaceCreateTranslated(unit->Common.Backend, &addresses, &space) == OS_EOK);
    assert(SMMUv3UnitDestroy(&unit) == OS_EBUSY);
    assert(ArmSMMUUnitIsReady(&unit->Common));
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMUv3HardwareIDsPrepare(space->Translated.BackendSpace, &sid, 1, &connection) == OS_EOK);
    assert(SMMUv3HardwareIDsConnect(space->Translated.BackendSpace, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX, DMABidirectional,
                                    &requirements, &mapping) == OS_EOK);
    assert(DMAAddressMappingInstall(mapping) == OS_EOK);
    segments = DMAAddressMappingSegments(mapping, &count);
    assert(count == 1 && segments != NULL);
    iova = segments[0].Address;
    assert(iova != extent.Address);
    assert(SMMUv3ModelAccess(sid, iova, false, &physical) == OS_EOK && physical == extent.Address);
    g_smmuv3Model.HoldSync = true;
    g_smmuv3Model.TimeStep = 10000000;
    assert(DMAAddressMappingRelease(&mapping) == OS_ETIMEOUT && mapping != NULL);
    g_smmuv3Model.HoldSync = false;
    g_smmuv3Model.TimeStep = 1000;
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
    assert(SMMUv3ModelAccess(sid, iova, true, &physical) == OS_EDEVFAULT);
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMUv3HardwareIDsDisconnect(space->Translated.BackendSpace, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMUv3HardwareIDsDestroy(space->Translated.BackendSpace, connection);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(DMAAddressSpaceDestroy(&space) == OS_EOK && space == NULL);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}


static void __AddressGeometry(void)
{
    const uint32_t widths[] = {32, 36, 40, 42, 44, 48, 48};
    SMMUv3Unit_t* unit;
    SMMUv3Space_t* space;
    SMMUv3Mapping_t* mapping;
    void* connection;
    uint32_t sid = 7;
    uint32_t encoding;
    SHMSG_t extent;
    DMAIOMMUMappingConfig_t config;
    uint64_t physical;
    uint64_t limit;

    for (encoding = 0; encoding < 7; encoding++) {
        SMMUv3ModelBegin(false);
        g_smmuv3Model.Registers[0x14 / 4] = 16 | encoding;
        if (encoding == 6) g_smmuv3Model.Registers[0] |= 1U << 18;
        // Also exercise a single-slot stream root and the minimum SID width.
        g_smmuv3Model.Registers[1] = (g_smmuv3Model.Registers[1] & ~63U) | 6U;
        unit = SMMUv3ModelStart();
        limit = (1ULL << widths[encoding]) - 1;
        assert(unit->Stage2.AddressLimit == limit && unit->AddressBits == widths[encoding]);
        assert(unit->Stage2.RootLevel == (widths[encoding] > 39 ? 0U : 1U));
        assert(unit->VMIDCount == (encoding == 6 ? 65536U : 256U));
        space = NULL;
        mapping = NULL;
        connection = NULL;
        extent = (SHMSG_t){ .Address = limit & ~4095ULL, .Length = 4096 };
        config = (DMAIOMMUMappingConfig_t){ .DeviceAddress = limit & ~4095ULL,
            .Length = 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1 };
        DMAIOMMUUnitLock(unit->Common.Backend);
        assert(SMMUv3SpaceCreate(unit, (void**)&space) == OS_EOK);
        assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
        assert(SMMUv3MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        assert(SMMUv3HardwareIDsPrepare(space, &sid, 1, &connection) == OS_EOK);
        assert(SMMUv3HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        assert(SMMUv3ModelAccess(sid, limit, true, &physical) == OS_EOK && physical == limit);
        assert(SMMUv3ModelAccess(sid, limit + 1, false, &physical) == OS_EDEVFAULT);
        assert(SMMUv3HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        SMMUv3HardwareIDsDestroy(space, connection);
        assert(SMMUv3MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        SMMUv3MappingDestroy(space, mapping);
        mapping = NULL;
        config.DeviceAddress = limit + 1;
        assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS && mapping == NULL);
        config.DeviceAddress = 0;
        extent.Address = limit + 1;
        assert(SMMUv3MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS && mapping == NULL);
        SMMUv3SpaceDestroy(unit, space);
        DMAIOMMUUnitUnlock(unit->Common.Backend);
        assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
        SMMUv3ModelEnd();
    }
}

void SMMUv3TableTests(void)
{
    __Mappings(DMAToDevice);
    __Mappings(DMAFromDevice);
    __Mappings(DMABidirectional);
    __PreparationAndPartialInstall();
    __VMIDCapacity();
    __AddressGeometry();
    __GenericDMA();
}
