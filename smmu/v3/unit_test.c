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
 * Unit capability, startup and teardown regression cases.
 */
#include "model.h"
#include <stdio.h>
#include <string.h>

static void __ProbeRejections(void)
{
    const struct { unsigned int Register; uint32_t Clear; uint32_t Set; } cases[] = {
        {0, 1, 0}, {0, 8, 0}, {0, 16, 0}, {0, 3U << 21, 3U << 21},
        {0, 3U << 24, 2U << 24}, {0, 3U << 27, 0},
        {4, 0, 1U << 30}, {4, 0, 1U << 29}, {4, 0, 1U << 28},
        {4, 63, 5}, {4, 63, 33}, {4, 31U << 21, 1U << 21},
        {4, 31U << 21, 20U << 21}, {4, 31U << 16, 0},
        {4, 31U << 16, 31U << 16}, {0x14, 16, 0},
        {0x14, 7, 7}, {0x1C, 0, 0x10}
    };
    ArmSMMUDescription_t description;
    SMMUv3Unit_t* unit;
    unsigned int i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SMMUv3ModelBegin(false);
        description = SMMUv3ModelDescription();
        g_smmuv3Model.Registers[cases[i].Register / 4] &= ~cases[i].Clear;
        g_smmuv3Model.Registers[cases[i].Register / 4] |= cases[i].Set;
        unit = NULL;
        assert(SMMUv3UnitCreate(&description, &unit) == OS_ENOTSUPPORTED);
        assert(unit == NULL && g_smmuv3Model.Writes == 0);
        SMMUv3ModelEnd();
    }
}

static void __CreateAndNoWriteCleanup(void)
{
    ArmSMMUDescription_t description;
    SMMUv3Unit_t* unit;
    int i;

    for (i = 0; i < 4; i++) {
        SMMUv3ModelBegin(false);
        description = SMMUv3ModelDescription();
        g_failHeapAfter = i;
        unit = NULL;
        assert(SMMUv3UnitCreate(&description, &unit) == OS_EOOM);
        assert(unit == NULL && g_smmuv3Model.Writes == 0);
        SMMUv3ModelEnd();
    }
    SMMUv3ModelBegin(false);
    description = SMMUv3ModelDescription();
    unit = NULL;
    assert(SMMUv3UnitCreate(NULL, &unit) == OS_EINVALPARAMS);
    assert(SMMUv3UnitCreate(&description, NULL) == OS_EINVALPARAMS);
    assert(DMAIOMMUUnitCheck(NULL) == OS_EINVALPARAMS);
    description.TableCoherent = false;
    assert(SMMUv3UnitCreate(&description, &unit) == OS_ENOTSUPPORTED);
    description.TableCoherent = true;
    g_failRegisters = true;
    assert(SMMUv3UnitCreate(&description, &unit) == OS_EOOM);
    g_failRegisters = false;
    assert(SMMUv3UnitCreate(&description, &unit) == OS_EOK);
    assert(SMMUv3UnitCreate(&description, &unit) == OS_EBUSY);
    assert(SMMUv3UnitPoll(unit) == OS_EBUSY);
    // A read-only probe may observe firmware-active hardware. Destroy must
    // leave that state alone if Start never wrote a register.
    g_smmuv3Model.Registers[0x20 / 4] = g_smmuv3Model.Registers[0x24 / 4] = 13;
    g_smmuv3Model.Clock = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
    assert(g_smmuv3Model.Writes == 0 && g_smmuv3Model.Registers[0x24 / 4] == 13);
    SMMUv3ModelEnd();
}

static void __PreWriteFailures(void)
{
    ArmSMMUDescription_t description;
    SMMUv3Unit_t* unit;
    unsigned int i;

    for (i = 0; i < 3; i++) {
        SMMUv3ModelBegin(false);
        description = SMMUv3ModelDescription();
        unit = NULL;
        assert(SMMUv3UnitCreate(&description, &unit) == OS_EOK);
        if (i == 0) g_smmuv3Model.Clock = false;
        if (i == 1) g_failHeapAfter = 0;
        if (i == 2) g_failMap = true;
        assert(SMMUv3UnitStart(unit) != OS_EOK);
        assert(SMMUv3UnitPoll(unit) == OS_EBUSY);
        assert(SMMUv3UnitStart(unit) == OS_ECANCELLED);
        g_smmuv3Model.Clock = false;
        assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
        assert(g_smmuv3Model.Writes == 0);
        SMMUv3ModelEnd();
    }
}

static void __StartAndDestroy(bool cached, bool ats)
{
    SMMUv3Unit_t* unit;
    uint32_t writes;

    SMMUv3ModelBegin(cached);
    if (ats) g_smmuv3Model.Registers[0] |= 1U << 10;
    g_smmuv3Model.AckDelay = 2;
    g_smmuv3Model.StaleError = 3U << 24;
    unit = SMMUv3ModelStart();
    assert(unit->Stage2.RootLevel == 0 && unit->Stage2.AddressLimit == 0xFFFFFFFFFFFFULL);
    assert((g_smmuv3Model.Registers[0x24 / 4] & 16U) == (ats ? 16U : 0));
    assert(g_smmuv3Model.Registers[0x28 / 4] == (cached ? 0x965U : 0x820U));
    assert(g_smmuv3Model.Syncs == 1);
    writes = g_smmuv3Model.Writes;
    assert(SMMUv3UnitStart(unit) == OS_EOK && g_smmuv3Model.Writes == writes);
    assert(SMMUv3UnitPoll(unit) == OS_EOK);
    DMAIOMMUUnitFail(unit->Common.Backend, OS_EDEVFAULT);
    assert(DMAIOMMUUnitCheck(unit->Common.Backend) == OS_ECANCELLED);
    assert(SMMUv3UnitStart(unit) == OS_ECANCELLED);
    assert(SMMUv3UnitPoll(unit) == OS_ECANCELLED);
    assert(!unit->Common.CompletionUnreliable);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
    assert(g_smmuv3Model.Syncs == 2 && g_releases == 1 && g_unmaps == 1);
    SMMUv3ModelEnd();
}

static void __CleanupRetries(void)
{
    SMMUv3Unit_t* unit;
    uint32_t producers;
    uint32_t writes;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    g_smmuv3Model.TimeStep = 10000000;
    g_smmuv3Model.HoldAckOffset = 0x24;
    g_smmuv3Model.HoldAckValue = 12;
    assert(SMMUv3UnitDestroy(&unit) == OS_ETIMEOUT && unit != NULL);
    assert(g_unmaps == 0 && g_releases == 0);
    writes = g_smmuv3Model.Writes;
    g_smmuv3Model.HoldAck = false;
    g_smmuv3Model.HoldAckOffset = 0;
    g_smmuv3Model.HoldSync = true;
    assert(SMMUv3UnitDestroy(&unit) == OS_ETIMEOUT);
    assert(g_smmuv3Model.Writes == writes + 1);
    producers = g_smmuv3Model.ProducerWrites;
    g_smmuv3Model.HoldSync = false;
    g_failUnmap = true;
    assert(SMMUv3UnitDestroy(&unit) == OS_ETIMEOUT);
    assert(g_smmuv3Model.ProducerWrites == producers && g_releases == 0);
    writes = g_smmuv3Model.Writes;
    g_failUnmap = false;
    g_failRelease = true;
    assert(SMMUv3UnitDestroy(&unit) == OS_EBUSY);
    assert(g_smmuv3Model.Writes == writes && g_unmaps == 2);
    g_failRelease = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
    assert(g_unmaps == 2 && g_releases == 2);
    SMMUv3ModelEnd();
}

static void __FailedStartRetry(void)
{
    const uint32_t controls[] = {0, 8, 12, 13};
    ArmSMMUDescription_t description;
    SMMUv3Unit_t* unit;
    unsigned int i;

    for (i = 0; i < 4; i++) {
        SMMUv3ModelBegin(false);
        description = SMMUv3ModelDescription();
        unit = NULL;
        assert(SMMUv3UnitCreate(&description, &unit) == OS_EOK);
        if (i == 0) g_smmuv3Model.Registers[0x20 / 4] = g_smmuv3Model.Registers[0x24 / 4] = 13;
        g_smmuv3Model.HoldAckOffset = 0x24;
        g_smmuv3Model.HoldAckValue = controls[i];
        g_smmuv3Model.TimeStep = 10000000;
        assert(SMMUv3UnitStart(unit) == OS_ETIMEOUT);
        assert(SMMUv3UnitStart(unit) == OS_ECANCELLED);
        g_smmuv3Model.HoldAck = false;
        g_smmuv3Model.HoldAckOffset = 0;
        g_smmuv3Model.TimeStep = 1000;
        assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
        SMMUv3ModelEnd();
    }
}

static void __PinnedGlobalError(void)
{
    SMMUv3Unit_t* unit;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    g_smmuv3Model.Registers[0x60 / 4] = 1;
    assert(SMMUv3UnitPoll(unit) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable);
    g_smmuv3Model.Clock = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_ECANCELLED && unit != NULL);
    assert(g_unmaps == 0 && g_releases == 0);
}

void SMMUv3UnitTests(void)
{
    __ProbeRejections();
    __CreateAndNoWriteCleanup();
    __PreWriteFailures();
    __StartAndDestroy(false, false);
    __StartAndDestroy(true, true);
    __CleanupRetries();
    __FailedStartRetry();
    SMMUv3ModelPinned(__PinnedGlobalError);
}

int main(void)
{
    SMMUv3UnitTests();
    SMMUv3QueueTests();
    SMMUv3TableTests();
    SMMUv3StreamTests();
    puts("SMMUv3: unit, queue, table, stream and generic DMA tests passed");
    return 0;
}
