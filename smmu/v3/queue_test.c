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
 * Command retirement must remain distinct from command fetch/consumption.
 */
#include "model.h"
#include <string.h>

static SMMUv3CommandBatch_t __Batch(void)
{
    SMMUv3CommandBatch_t batch = {0};
    batch.Count = 2;
    batch.Commands[0].Words[0] = 0x28 | (1ULL << 32);
    batch.Commands[1].Words[0] = 0x46;
    return batch;
}

static void __TimeoutAndWrap(void)
{
    SMMUv3Unit_t* unit;
    SMMUv3CommandBatch_t first = __Batch();
    SMMUv3CommandBatch_t second = __Batch();
    ArmSMMUDeadline_t deadline;
    uint64_t end;
    uint32_t producers;
    unsigned int i;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    g_noHeap = true;
    g_smmuv3Model.HoldSync = true;
    g_smmuv3Model.TimeStep = 10000000;
    assert(ArmSMMUDeadlineBegin(0, &deadline) == OS_EINVALPARAMS);
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &first, &deadline) == OS_ETIMEOUT);
    assert(first.State == SMMUv3BatchSubmitted && !unit->Common.CompletionUnreliable);
    assert(unit->CommandsCompleted == first.EndPosition - 1);
    end = first.EndPosition;
    producers = g_smmuv3Model.ProducerWrites;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &first, &deadline) == OS_ETIMEOUT);
    assert(g_smmuv3Model.ProducerWrites == producers && first.EndPosition == end);
    g_smmuv3Model.HoldSync = false;
    g_smmuv3Model.TimeStep = 1000;
    // Another operation can drain and wrap the ring before the original retries.
    for (i = 0; i < 600; i++) {
        second = __Batch();
        assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
        assert(SMMUv3CommandBatchRun(unit, &second, &deadline) == OS_EOK);
    }
    producers = g_smmuv3Model.ProducerWrites;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &first, &deadline) == OS_EOK);
    assert(first.State == SMMUv3BatchComplete && g_smmuv3Model.ProducerWrites == producers);
    // Unsigned elapsed time remains valid across the 64-bit timestamp wrap.
    g_smmuv3Model.Now = UINT64_MAX - 100;
    g_smmuv3Model.TimeStep = 100;
    assert(ArmSMMUDeadlineBegin(1000, &deadline) == OS_EOK);
    assert(!ArmSMMUDeadlineExpired(&deadline));
    g_smmuv3Model.Now += 1000;
    assert(ArmSMMUDeadlineExpired(&deadline));
    g_noHeap = false;
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __QueueSpaceBudget(void)
{
    SMMUv3Unit_t* unit;
    SMMUv3CommandBatch_t batch;
    ArmSMMUDeadline_t deadline;
    unsigned int i;
    uint64_t before;

    SMMUv3ModelBegin(false);
    g_smmuv3Model.Registers[1] = 16U | (2U << 21) | (1U << 16);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    g_smmuv3Model.HoldCommands = true;
    g_smmuv3Model.TimeStep = 10000000;
    for (i = 0; i < 2; i++) {
        batch = __Batch();
        assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
        assert(SMMUv3CommandBatchRun(unit, &batch, &deadline) == OS_ETIMEOUT);
        assert(batch.State == SMMUv3BatchSubmitted);
    }
    batch = __Batch();
    before = g_smmuv3Model.Now;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &batch, &deadline) == OS_ETIMEOUT);
    assert(batch.State == SMMUv3BatchPrepared);
    assert(g_smmuv3Model.Now - before <= ARM_SMMU_TIMEOUT_NS + g_smmuv3Model.TimeStep);
    g_smmuv3Model.HoldCommands = false;
    g_smmuv3Model.TimeStep = 1000;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &batch, &deadline) == OS_EOK);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __BadConsumer(void)
{
    SMMUv3Unit_t* unit;
    SMMUv3CommandBatch_t batch = __Batch();
    ArmSMMUDeadline_t deadline;
    uint32_t producers;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    g_smmuv3Model.HoldCommands = true;
    g_smmuv3Model.Registers[0x9C / 4] += 10;
    producers = g_smmuv3Model.ProducerWrites;
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMUv3CommandBatchRun(unit, &batch, &deadline) == OS_ECANCELLED);
    assert(g_smmuv3Model.ProducerWrites == producers);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(SMMUv3UnitDestroy(&unit) == OS_ECANCELLED);
}

void SMMUv3QueueTests(void)
{
    __TimeoutAndWrap();
    __QueueSpaceBudget();
    SMMUv3ModelPinned(__BadConsumer);
}
