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
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    void* connection = NULL;
    SMMU500Mapping_t* first = NULL;
    SMMU500Mapping_t* second = NULL;
    uint32_t sid = 65;
    SHMSG_t extent = { .Address = 0x12345000, .Length = 4096 };
    DMAIOMMUMappingConfig_t a = { .DeviceAddress = 0x3000, .Length = 4096,
        .Direction = direction, .Extents = &extent, .ExtentCount = 1 };
    DMAIOMMUMappingConfig_t b = a;
    uint64_t physical;
    uint32_t producers;
    int tables;

    b.DeviceAddress = 0x4000;
    SMMU500ModelBegin(false);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(space, &sid, 1, &connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500MappingPrepare(space, &a, (void**)&first) == OS_EOK);
    assert(SMMU500MappingPrepare(space, &b, (void**)&second) == OS_EOK);
    g_noHeap = true;
    // Install in reverse preparation order. Both must resolve shared paths now.
    assert(SMMU500MappingInstall(space, second, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    tables = space->Table.Pages.count;
    assert(SMMU500MappingInstall(space, first, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(space->Table.Pages.count == tables && tables == 3);
    assert(SMMU500ModelAccess(sid, a.DeviceAddress + 17, false, &physical) ==
           (direction == DMAFromDevice ? OS_EDEVFAULT : OS_EOK));
    assert(SMMU500ModelAccess(sid, a.DeviceAddress + 17, true, &physical) ==
           (direction == DMAToDevice ? OS_EDEVFAULT : OS_EOK));
    assert(physical == extent.Address + 17);
    // SYNC must wait for an older table walk even after clearing the leaf and
    // consuming its invalidation. The model keeps that cached translation live.
    g_smmu500Model.HeldWalks = 1;
    g_smmu500Model.TimeStep = 10000000;
    assert(SMMU500MappingRemove(space, first, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(first->Phase == SMMU500MappingInvalidate && first->Stage2.InstalledBytes == 0);
    assert(SMMU500ModelAccess(sid, a.DeviceAddress, direction == DMAFromDevice, &physical) == OS_EOK);
    producers = g_smmu500Model.SyncWrites;
    g_smmu500Model.HeldWalks = 0;
    g_smmu500Model.TimeStep = 1000;
    assert(SMMU500MappingRemove(space, first, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmu500Model.SyncWrites == producers);
    assert(SMMU500ModelAccess(sid, a.DeviceAddress, false, &physical) == OS_EDEVFAULT);
    SMMU500MappingDestroy(space, first);
    assert(space->Table.Pages.count == tables);
    assert(SMMU500ModelAccess(sid, b.DeviceAddress, direction == DMAFromDevice, &physical) == OS_EOK);
    assert(SMMU500MappingRemove(space, second, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500MappingDestroy(space, second);
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500HardwareIDsDestroy(space, connection);
    SMMU500SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __PreparationAndPartialInstall(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Mapping_t* mapping = NULL;
    SHMSG_t extent = { .Address = 0xFFFFFFF000ULL, .Length = 3 * 4096 };
    DMAIOMMUMappingConfig_t config = { .DeviceAddress = (1ULL << 39) - 4096,
        .Length = 3 * 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1 };
    size_t freePages;
    int i;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    freePages = MemoryStackFreeBlockCount(&unit->Common.Memory.FreePages);
    for (i = 0; i < 7; i++) {
        g_failHeapAfter = i;
        assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOOM);
        assert(mapping == NULL && space->Table.MappingCount == 0);
        assert(MemoryStackFreeBlockCount(&unit->Common.Memory.FreePages) == freePages);
    }
    g_failHeapAfter = -1;
    extent.Length--;
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS);
    extent.Length++;
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
    assert(mapping->Stage2.SparePages.count == 6);
    g_smmu500Model.TimeStep = 40000000;
    assert(SMMU500MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(mapping->Stage2.InstalledBytes != 0 && mapping->Stage2.InstalledBytes < config.Length);
    g_smmu500Model.TimeStep = 1000;
    g_noHeap = true;
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500MappingDestroy(space, mapping);
    SMMU500SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __BankCapacity(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* spaces[5] = {0};
    unsigned int i;

    SMMU500ModelBegin(false);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    for (i = 0; i < 4; i++) {
        assert(SMMU500SpaceCreate(unit, (void**)&spaces[i]) == OS_EOK);
        assert(spaces[i]->Bank == i);
    }
    assert(SMMU500SpaceCreate(unit, (void**)&spaces[4]) == OS_ENOTSUPPORTED);
    for (i = 0; i < 4; i++) SMMU500SpaceDestroy(unit, spaces[i]);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __GenericDMA(void)
{
    SMMU500Unit_t* unit;
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

    SMMU500ModelBegin(false);
    unit = SMMU500ModelStart();
    assert(DMAAddressSpaceCreateTranslated(unit->Common.Backend, &addresses, &space) == OS_EOK);
    assert(SMMU500UnitDestroy(&unit) == OS_EBUSY);
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500HardwareIDsPrepare(space->Translated.BackendSpace, &sid, 1, &connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(space->Translated.BackendSpace, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX, DMABidirectional,
                                    &requirements, &mapping) == OS_EOK);
    assert(DMAAddressMappingInstall(mapping) == OS_EOK);
    segments = DMAAddressMappingSegments(mapping, &count);
    assert(count == 1 && segments != NULL);
    iova = segments[0].Address;
    assert(iova != extent.Address);
    assert(SMMU500ModelAccess(sid, iova, false, &physical) == OS_EOK && physical == extent.Address);
    g_smmu500Model.HoldSync = true;
    g_smmu500Model.TimeStep = 10000000;
    assert(DMAAddressMappingRelease(&mapping) == OS_ETIMEOUT && mapping != NULL);
    g_smmu500Model.HoldSync = false;
    g_smmu500Model.TimeStep = 1000;
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
    assert(SMMU500ModelAccess(sid, iova, true, &physical) == OS_EDEVFAULT);
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500HardwareIDsDisconnect(space->Translated.BackendSpace, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500HardwareIDsDestroy(space->Translated.BackendSpace, connection);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(DMAAddressSpaceDestroy(&space) == OS_EOK && space == NULL);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}


static void __AddressGeometry(void)
{
    const uint32_t widths[] = {32, 36, 40, 42, 44, 48};
    SMMU500Unit_t* unit;
    SMMU500Space_t* space;
    SMMU500Mapping_t* mapping;
    void* connection;
    uint32_t sid = 7;
    uint32_t encoding;
    SHMSG_t extent;
    DMAIOMMUMappingConfig_t config;
    uint64_t physical;
    uint64_t limit;

    for (encoding = 0; encoding < 6; encoding++) {
        SMMU500ModelBegin(false);
        g_smmu500Model.Registers[0x28 / 4] = 0x1000 | encoding | (encoding << 4);
        if (encoding & 1) g_smmu500Model.Registers[0x24 / 4] |= 1U << 31;
        unit = SMMU500ModelStart();
        limit = (1ULL << widths[encoding]) - 1;
        assert(unit->Stage2.AddressLimit == limit && unit->InputBits == widths[encoding]);
        assert(unit->Stage2.RootLevel == (widths[encoding] > 39 ? 0U : 1U));
        space = NULL;
        mapping = NULL;
        connection = NULL;
        extent = (SHMSG_t){ .Address = limit & ~4095ULL, .Length = 4096 };
        config = (DMAIOMMUMappingConfig_t){ .DeviceAddress = limit & ~4095ULL,
            .Length = 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1 };
        DMAIOMMUUnitLock(unit->Common.Backend);
        assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
        assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
        assert(SMMU500MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        assert(SMMU500HardwareIDsPrepare(space, &sid, 1, &connection) == OS_EOK);
        assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        assert(SMMU500ModelAccess(sid, limit, true, &physical) == OS_EOK && physical == limit);
        assert(SMMU500ModelAccess(sid, limit + 1, false, &physical) == OS_EDEVFAULT);
        assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        SMMU500HardwareIDsDestroy(space, connection);
        assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        SMMU500MappingDestroy(space, mapping);
        mapping = NULL;
        config.DeviceAddress = limit + 1;
        assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS && mapping == NULL);
        config.DeviceAddress = 0;
        extent.Address = limit + 1;
        assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS && mapping == NULL);
        SMMU500SpaceDestroy(unit, space);
        DMAIOMMUUnitUnlock(unit->Common.Backend);
        assert(SMMU500UnitDestroy(&unit) == OS_EOK);
        SMMU500ModelEnd();
    }
}

static void __DistinctLimitsAndPendingInstall(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Mapping_t* mapping = NULL;
    SHMSG_t extent = {.Address = 0xFFFFF000, .Length = 4096};
    DMAIOMMUMappingConfig_t config = {.DeviceAddress = 0xFFFFFFFFF000ULL,
        .Length = 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1};
    uint32_t writes;

    SMMU500ModelBegin(true);
    g_smmu500Model.Registers[0x28 / 4] = 0x1005; // 48-bit IOVA and 32-bit physical output.
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    extent.Address += 4096;
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EINVALPARAMS);
    extent.Address -= 4096;
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
    g_smmu500Model.HoldSync = true;
    g_smmu500Model.TimeStep = 10000000;
    assert(SMMU500MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(mapping->Stage2.InstalledBytes == 4096 && mapping->Sync.Ticket != 0);
    writes = g_smmu500Model.SyncWrites;
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(mapping->Stage2.InstalledBytes == 4096 && g_smmu500Model.SyncWrites == writes);
    g_smmu500Model.HoldSync = false;
    g_smmu500Model.TimeStep = 1000;
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmu500Model.SyncWrites == writes + 1);
    SMMU500MappingDestroy(space, mapping);
    SMMU500SpaceDestroy(unit, space);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __PartialClear(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Mapping_t* mapping = NULL;
    SHMSG_t extent = {.Address = 0x12345000, .Length = 3 * 4096};
    DMAIOMMUMappingConfig_t config = {.DeviceAddress = 0x1000,
        .Length = 3 * 4096, .Direction = DMABidirectional, .Extents = &extent, .ExtentCount = 1};
    uint32_t writes;
    uint64_t ticket;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
    assert(SMMU500MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    writes = g_smmu500Model.SyncWrites;
    ticket = mapping->Sync.Ticket;
    g_noHeap = true;
    g_smmu500Model.TimeStep = 40000000;
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(mapping->Phase == SMMU500MappingClearLeaves);
    assert(mapping->Stage2.InstalledBytes > 0 && mapping->Stage2.InstalledBytes < config.Length);
    assert(mapping->Sync.Ticket == ticket && g_smmu500Model.SyncWrites == writes);
    g_smmu500Model.TimeStep = 1000;
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(mapping->Stage2.InstalledBytes == 0 && g_smmu500Model.SyncWrites == writes + 1);
    SMMU500MappingDestroy(space, mapping);
    SMMU500SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __ScatterGather(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Mapping_t* mapping = NULL;
    void* connection = NULL;
    uint32_t sid = 11;
    uint64_t physical;
    SHMSG_t extents[] = {{.Address = 0x123000, .Length = 8192}, {.Address = 0x789000, .Length = 4096}};
    DMAIOMMUMappingConfig_t config = {.DeviceAddress = 0x1FF000, .Length = 12288,
        .Direction = DMABidirectional, .Extents = extents, .ExtentCount = 2};

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMU500MappingPrepare(space, &config, (void**)&mapping) == OS_EOK);
    assert(SMMU500MappingInstall(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(space, &sid, 1, &connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500ModelAccess(sid, 0x1FF123, false, &physical) == OS_EOK && physical == 0x123123);
    assert(SMMU500ModelAccess(sid, 0x200456, true, &physical) == OS_EOK && physical == 0x124456);
    assert(SMMU500ModelAccess(sid, 0x201789, false, &physical) == OS_EOK && physical == 0x789789);
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500HardwareIDsDestroy(space, connection);
    assert(SMMU500MappingRemove(space, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500MappingDestroy(space, mapping);
    SMMU500SpaceDestroy(unit, space);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

void SMMU500TableTests(void)
{
    __Mappings(DMAToDevice);
    __Mappings(DMAFromDevice);
    __Mappings(DMABidirectional);
    __PreparationAndPartialInstall();
    __BankCapacity();
    __AddressGeometry();
    __GenericDMA();
    __ScatterGather();
    __DistinctLimitsAndPendingInstall();
    __PartialClear();
}
