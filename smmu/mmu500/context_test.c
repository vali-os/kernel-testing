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
 * Shared maintenance, clock budgets, context fields and completion faults.
 * Fault sorting and Poll records are tested in fault_test.c.
 */
#include "model.h"
#include <string.h>

static void __Tickets(void)
{
    SMMU500Unit_t* unit;
    ArmSMMUDeadline_t deadline;
    SMMU500Sync_t first = {.Scope = SMMU500SyncAll};
    SMMU500Sync_t second = {.Scope = SMMU500SyncVMID, .VMID = 1};
    uint32_t writes;
    uint64_t ticket;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    g_noHeap = true;
    g_smmu500Model.HoldSync = true;
    g_smmu500Model.TimeStep = 10000000;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &first, &deadline) == OS_ETIMEOUT);
    ticket = first.Ticket;
    writes = g_smmu500Model.SyncWrites;
    assert(ticket == 2 && unit->SyncsSubmitted == 2 && unit->SyncsCompleted == 1);
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &second, &deadline) == OS_ETIMEOUT);
    assert(second.Ticket == 0 && g_smmu500Model.SyncWrites == writes);
    g_smmu500Model.HoldSync = false;
    g_smmu500Model.SyncStopAfter = 2; // Complete first, then hold the second ticket.
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &second, &deadline) == OS_ETIMEOUT);
    assert(second.Ticket == 3 && unit->SyncsCompleted == ticket && g_smmu500Model.SyncWrites == writes + 1);
    assert(SMMU500SyncRun(unit, &first, &deadline) == OS_EOK);
    assert(first.Ticket == ticket && g_smmu500Model.PendingSync && g_smmu500Model.SyncWrites == writes + 1);
    g_smmu500Model.SyncStopAfter = 0;
    g_smmu500Model.TimeStep = 1000;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &second, &deadline) == OS_EOK);
    assert(unit->SyncsCompleted == 3 && g_smmu500Model.SyncWrites == writes + 1);
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __Budgets(void)
{
    ArmSMMUDeadline_t deadline;

    SMMU500ModelBegin(false);
    assert(ArmSMMUDeadlineBegin(0, &deadline) == OS_EINVALPARAMS);
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS + 1, &deadline) == OS_EINVALPARAMS);
    g_smmu500Model.Now = UINT64_MAX - 20;
    g_smmu500Model.TimeStep = 10;
    assert(ArmSMMUDeadlineBegin(30, &deadline) == OS_EOK);
    assert(!ArmSMMUDeadlineExpired(&deadline));
    assert(!ArmSMMUDeadlineExpired(&deadline));
    assert(ArmSMMUDeadlineExpired(&deadline));
    g_smmu500Model.Clock = false;
    assert(ArmSMMUDeadlineBegin(30, &deadline) == OS_ENOTSUPPORTED);
    SMMU500ModelEnd();
}

static void __ContextFields(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    ArmSMMUDeadline_t deadline;
    uint32_t bank;
    uint32_t writes;

    SMMU500ModelBegin(true);
    g_smmu500Model.Registers[0x28 / 4] = 0x1015; // 48-bit input, 36-bit output.
    unit = SMMU500ModelStart();
    assert(unit->Stage2.AddressLimit == 0xFFFFFFFFFFFFULL && unit->Stage2.PhysicalLimit == 0xFFFFFFFFFULL);
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    bank = SMMU500ModelBank(space->Bank);
    g_smmu500Model.SyncDelay = 3;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500ContextEnable(space, &deadline) == OS_EOK);
    assert(g_smmu500Model.Registers[bank / 4] == 0x21);
    assert(g_smmu500Model.Registers[(bank + 0x30) / 4] == 0x12590);
    assert(g_smmu500Model.Registers[(bank + 0x20) / 4] == space->Table.Root.PhysicalAddress);
    assert(g_smmu500Model.Registers[(0x1000 + space->Bank * 4) / 4] == space->Bank + 1);
    g_smmu500Model.HoldSync = true;
    g_smmu500Model.TimeStep = 10000000;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500ContextDisable(space, &deadline) == OS_ETIMEOUT);
    assert(space->ContextProgress == SMMU500ContextDisableSync);
    writes = g_smmu500Model.SyncWrites;
    g_smmu500Model.HoldSync = false;
    g_smmu500Model.TimeStep = 1000;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500ContextDisable(space, &deadline) == OS_EOK);
    assert(g_smmu500Model.SyncWrites == writes && space->ContextProgress == SMMU500ContextDisabled);
    assert((g_smmu500Model.Registers[bank / 4] & 1U) == 0);
    SMMU500SpaceDestroy(unit, space);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

static void __BankFaultOnCleanup(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    ArmSMMUDeadline_t deadline;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500ContextEnable(space, &deadline) == OS_EOK);
    g_smmu500Model.Registers[(SMMU500ModelBank(0) + 0x58) / 4] = 0x40000402; // Stalled translation fault.
    assert(SMMU500ContextDisable(space, &deadline) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable && unit->Contexts[0].Space == space);
    assert(g_unmaps == 0 && g_releases == 0);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
}

static void __CompletionFault(void)
{
    SMMU500Unit_t* unit;
    ArmSMMUDeadline_t deadline;
    SMMU500Sync_t sync = {.Scope = SMMU500SyncAll};

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    // Advance runs on each read. One delayed read makes CAF arrive with the
    // idle status, after the wait loop's first global-fault sample.
    g_smmu500Model.SyncDelay = 1;
    g_smmu500Model.CompletionFault = 1U << 5;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &sync, &deadline) == OS_ECANCELLED);
    assert(unit->SyncsCompleted == 1 && unit->Common.CompletionUnreliable);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
}

static void __CompletionBankFault(void)
{
    SMMU500Unit_t* unit;
    ArmSMMUDeadline_t deadline;
    SMMU500Sync_t sync = {.Scope = SMMU500SyncAll};

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    g_smmu500Model.SyncDelay = 1;
    // An external fault (EF) in a bank points at the unit or its table walks
    // and fails the unit. Device faults stay latched and do not stop a sync.
    g_smmu500Model.CompletionBankFault = 0x410;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &sync, &deadline) == OS_ECANCELLED);
    assert(unit->SyncsCompleted == 1 && unit->Common.CompletionUnreliable);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
}

static void __CounterOverflow(void)
{
    SMMU500Unit_t* unit;
    ArmSMMUDeadline_t deadline;
    SMMU500Sync_t sync = {.Scope = SMMU500SyncAll};
    uint32_t writes;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    unit->SyncsSubmitted = unit->SyncsCompleted = UINT64_MAX;
    writes = g_smmu500Model.Writes;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500SyncRun(unit, &sync, &deadline) == OS_ECANCELLED);
    assert(sync.Ticket == 0 && unit->Common.CompletionUnreliable && g_smmu500Model.Writes == writes);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
}

static void __MaximumCapacity(void)
{
    SMMU500Unit_t* unit;
    SMMU500Space_t* spaces[129] = {0};
    SMMU500Connection_t* connection = NULL;
    uint32_t ids[128];
    unsigned int index;

    SMMU500ModelBegin(true);
    g_smmu500Model.Registers[0x24 / 4] = (6U << 28) | (16U << 16) | 128;
    g_smmu500Model.Registers[0x20 / 4] = (g_smmu500Model.Registers[0x20 / 4] & ~255U) | 128;
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    for (index = 0; index < 128; index++) {
        ids[index] = index;
        assert(SMMU500SpaceCreate(unit, (void**)&spaces[index]) == OS_EOK);
        assert(spaces[index]->Bank == index);
    }
    assert(SMMU500SpaceCreate(unit, (void**)&spaces[128]) == OS_ENOTSUPPORTED);
    assert(SMMU500HardwareIDsPrepare(spaces[127], ids, 128, (void**)&connection) == OS_EOK);
    assert(SMMU500HardwareIDsConnect(spaces[127], connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    assert(g_smmu500Model.Registers[(0x1000 + 127 * 4) / 4] == 128);
    assert(SMMU500HardwareIDsDisconnect(spaces[127], connection, ARM_SMMU_TIMEOUT_NS) == OS_EOK);
    SMMU500HardwareIDsDestroy(spaces[127], connection);
    for (index = 0; index < 128; index++) SMMU500SpaceDestroy(unit, spaces[index]);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

void SMMU500ContextTests(void)
{
    __MaximumCapacity();
    __Tickets();
    __Budgets();
    __ContextFields();
    SMMU500ModelPinned(__BankFaultOnCleanup);
    SMMU500ModelPinned(__CompletionFault);
    SMMU500ModelPinned(__CompletionBankFault);
    SMMU500ModelPinned(__CounterOverflow);
}
