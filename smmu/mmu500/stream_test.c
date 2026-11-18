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
 * Exact SID reservations, partial connection cleanup and bank reuse.
 */
#include "model.h"
#include <string.h>

static void __Reservations(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* first = NULL;
    SMMU500Space_t* second = NULL;
    SMMU500Connection_t* a = NULL;
    SMMU500Connection_t* b = NULL;
    uint32_t ids[] = {1, 2, 3, 4, 5, 6, 7, 0x7FFF};
    uint32_t extra[] = {8, 9};
    uint32_t duplicate[] = {3, 3};
    uint32_t tooMany[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    uint32_t invalid = 0x8000;
    uint32_t writes;
    unsigned int slot;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    writes = g_smmu500Model.Writes;
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&first) == OS_EOK);
    assert(SMMU500SpaceCreate(unit, (void**)&second) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(first, duplicate, 2, (void**)&a) == OS_EINVALPARAMS && a == NULL);
    assert(SMMU500HardwareIDsPrepare(first, &invalid, 1, (void**)&a) == OS_ENOTSUPPORTED && a == NULL);
    assert(SMMU500HardwareIDsPrepare(first, tooMany, 9, (void**)&a) == OS_ENOTSUPPORTED && a == NULL);
    g_failHeapAfter = 0;
    assert(SMMU500HardwareIDsPrepare(first, ids, 7, (void**)&a) == OS_EOOM && a == NULL);
    for (slot = 0; slot < 8; slot++) assert(!unit->Slots[slot].Reserved);
    g_failHeapAfter = -1;
    assert(SMMU500HardwareIDsPrepare(first, ids, 7, (void**)&a) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(second, ids, 1, (void**)&b) == OS_EEXISTS && b == NULL);
    assert(SMMU500HardwareIDsPrepare(second, extra, 2, (void**)&b) == OS_ENOTSUPPORTED && b == NULL);
    assert(SMMU500HardwareIDsPrepare(second, &ids[7], 1, (void**)&b) == OS_EOK);
    assert(g_smmu500Model.Writes == writes);
    assert(SMMU500HardwareIDsConnect(second, b, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500HardwareIDsDisconnect(second, b, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmu500Model.Registers[(0xC00 + b->Routes[0].Slot * 4) / 4] == 0x20001);
    SMMU500HardwareIDsDestroy(first, a);
    SMMU500HardwareIDsDestroy(second, b);
    SMMU500SpaceDestroy(unit, first);
    SMMU500SpaceDestroy(unit, second);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __PartialConnect(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space;
    SMMU500Connection_t* connection;
    uint32_t ids[] = {3, 5, 9};
    unsigned int scenario;
    uint32_t writes;
    unsigned int slot;

    for (scenario = 0; scenario < 3; scenario++) {
        SMMU500ModelBegin(false);
        unit = SMMU500ModelStart();
        space = NULL;
        connection = NULL;
        DMAIOMMUUnitLock(unit->Common.Backend);
        assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
        assert(SMMU500HardwareIDsPrepare(space, ids, 3, (void**)&connection) == OS_EOK);
        if (scenario == 0) g_smmu500Model.HoldSync = true;
        if (scenario == 1) g_smmu500Model.SyncStopAfter = g_smmu500Model.Syncs + 1;
        g_smmu500Model.TimeStep = scenario == 2 ? 25000000 : 10000000;
        g_noHeap = true;
        assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
        assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EBUSY);
        if (scenario == 2) assert(connection->NextRoute > 0 && connection->NextRoute < connection->RouteCount);
        g_smmu500Model.HoldSync = false;
        g_smmu500Model.SyncStopAfter = 0;
        g_smmu500Model.TimeStep = 1000;
        assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        writes = g_smmu500Model.Writes;
        assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
        assert(g_smmu500Model.Writes == writes);
        for (slot = 0; slot < 3; slot++) {
            assert(unit->Slots[slot].Reserved);
            assert(g_smmu500Model.Registers[(0xC00 + slot * 4) / 4] == 0x20000);
            assert(g_smmu500Model.Registers[(0x800 + slot * 4) / 4] == 0);
        }
        SMMU500HardwareIDsDestroy(space, connection);
        SMMU500SpaceDestroy(unit, space);
        g_noHeap = false;
        DMAIOMMUUnitUnlock(unit->Common.Backend);
        assert(SMMU500UnitDestroy(&unit) == OS_EOK);
        SMMU500ModelEnd();
    }
}

static void __DisconnectAndReuse(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* first = NULL;
    SMMU500Space_t* second = NULL;
    SMMU500Connection_t* connection = NULL;
    SMMU500Mapping_t* mapping = NULL;
    ArmSMMUMemoryBlock_t spacer = {0};
    uint32_t sid = 7;
    uint32_t writes;
    uint32_t oldBank;
    uint64_t oldRoot;
    uint64_t physical;
    SHMSG_t extent = {.Address = 0x12345000, .Length = 4096};
    DMAIOMMUMappingConfig_t config = {.DeviceAddress = 0x1000, .Length = 4096,
        .Direction = DMABidirectional, .ExtentCount = 1, .Extents = &extent};

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&first) == OS_EOK);
    assert(SMMU500SpaceCreate(unit, (void**)&second) == OS_EOK);
    oldBank = first->Bank;
    oldRoot = first->Table.Root.PhysicalAddress;
    assert(SMMU500MappingPrepare(first, &config, (void**)&mapping) == OS_EOK);
    assert(SMMU500MappingInstall(first, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(first, &sid, 1, (void**)&connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(first, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500ModelAccess(sid, 0x1000, false, &physical) == OS_EOK && physical == extent.Address);
    g_smmu500Model.HeldWalks = 1;
    g_smmu500Model.TimeStep = 10000000;
    assert(SMMU500HardwareIDsDisconnect(first, connection, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(connection->Phase == SMMU500ConnectionDrain && g_smmu500Model.PendingSync);
    assert(unit->Contexts[oldBank].Space == first && unit->Slots[connection->Routes[0].Slot].Reserved);
    // Software routes are blocked, but the cached old route and populated root
    // remain reachable until the held walk and global synchronization finish.
    assert(SMMU500ModelAccess(sid, 0x1000, false, &physical) == OS_EOK);
    writes = g_smmu500Model.SyncWrites;
    g_smmu500Model.HeldWalks = 0;
    g_smmu500Model.TimeStep = 1000;
    assert(SMMU500HardwareIDsDisconnect(first, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmu500Model.SyncWrites == writes + 1); // Only the final bank invalidation is new.
    assert(SMMU500ModelAccess(sid, 0x1000, false, &physical) == OS_EDEVFAULT);
    assert(mapping->Stage2.InstalledBytes == 4096); // No accidental reliance on cleared leaves.
    SMMU500HardwareIDsDestroy(first, connection);
    assert(SMMU500MappingRemove(first, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500MappingDestroy(first, mapping);
    SMMU500SpaceDestroy(unit, first);
    // Consume the old root so the reused bank points at a different root while
    // another bank remains live. Stale bank/VMID state cannot pass by coincidence.
    assert(ArmSMMUMemoryAllocate(&unit->Common.Memory, 4096, 4096, &spacer) == OS_EOK);
    first = NULL;
    connection = NULL;
    mapping = NULL;
    assert(SMMU500SpaceCreate(unit, (void**)&first) == OS_EOK);
    assert(first->Bank == oldBank && first->Table.Root.PhysicalAddress != oldRoot);
    extent.Address = 0x45678000;
    assert(SMMU500MappingPrepare(first, &config, (void**)&mapping) == OS_EOK);
    assert(SMMU500MappingInstall(first, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(first, &sid, 1, (void**)&connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(first, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(SMMU500ModelAccess(sid, 0x1000, true, &physical) == OS_EOK && physical == extent.Address);
    assert(SMMU500ModelAccess(sid + 1, 0x1000, true, &physical) == OS_EDEVFAULT);
    assert(SMMU500HardwareIDsDisconnect(first, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500HardwareIDsDestroy(first, connection);
    assert(SMMU500MappingRemove(first, mapping, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500MappingDestroy(first, mapping);
    SMMU500SpaceDestroy(unit, first);
    SMMU500SpaceDestroy(unit, second);
    assert(ArmSMMUMemoryFree(&unit->Common.Memory, &spacer) == OS_EOK);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __PartialDisconnect(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Connection_t* connection = NULL;
    uint32_t ids[] = {3, 5, 9};
    uint32_t writes;
    uint32_t slot;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(space, ids, 3, (void**)&connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    writes = g_smmu500Model.SyncWrites;
    g_noHeap = true;
    g_smmu500Model.TimeStep = 40000000;
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(connection->Phase == SMMU500ConnectionFaultRoutes);
    assert(connection->NextRoute > 0 && connection->NextRoute < connection->RouteCount);
    assert(space->ContextProgress == SMMU500ContextEnabled && g_smmu500Model.SyncWrites == writes);
    for (slot = 0; slot < connection->RouteCount; slot++) assert(unit->Slots[slot].Reserved);
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_ETIMEOUT);
    assert(connection->Phase == SMMU500ConnectionDisable);
    assert(connection->NextRoute == connection->RouteCount);
    assert(space->ContextProgress == SMMU500ContextEnabled && g_smmu500Model.SyncWrites == writes + 1);
    g_smmu500Model.TimeStep = 1000;
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(connection->NextRoute == connection->RouteCount);
    assert(space->ContextProgress == SMMU500ContextDisabled && g_smmu500Model.SyncWrites == writes + 2);
    SMMU500HardwareIDsDestroy(space, connection);
    SMMU500SpaceDestroy(unit, space);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __DisconnectWriteRejected(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    SMMU500Connection_t* connection = NULL;
    uint32_t sid = 3;

    SMMU500ModelBegin(false);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(SMMU500HardwareIDsPrepare(space, &sid, 1, (void**)&connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    g_smmu500Model.IgnoreFault = true;
    assert(SMMU500HardwareIDsDisconnect(space, connection, ARM_SMMU_TIMEOUT_NS) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable && space->HasConnection && unit->Contexts[0].Space == space);
    assert(unit->Slots[0].Reserved && g_unmaps == 0 && g_releases == 0);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
}

void SMMU500StreamTests(void)
{
    __Reservations();
    __PartialConnect();
    __DisconnectAndReuse();
    __PartialDisconnect();
    SMMU500ModelPinned(__DisconnectWriteRejected);
}
