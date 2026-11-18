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
 * Capability, lifecycle and cleanup tests. Expected fields are literal values,
 * independent of production register constants and encoding helpers.
 */
#include "model.h"
#include <stdio.h>
#include <string.h>

static void __Probe(void)
{
    const uint32_t revisions[] = {0x00, 0x10, 0x20, 0x21, 0x22, 0x24};
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;
    void* space = NULL;
    unsigned int index;

    for (index = 0; index < 6; index++) {
        SMMU500ModelBegin(true);
        g_smmu500Model.Registers[0x3C / 4] = revisions[index];
        if (index & 1) g_smmu500Model.Registers[0x24 / 4] |= 1U << 31;
        description = SMMU500ModelDescription();
        description.RegisterLength *= 2; // The CB base must still come from IDR1.
        assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
        assert(unit->ContextBase == SMMU500ModelBank(0));
        assert(unit->Stage2Count == 0 && unit->ContextCount == 4 && unit->SlotCount == 8);
        assert(unit->SIDMask == 0x7FFF && unit->Revision == revisions[index]);
        assert(SMMU500UnitGetBackend(unit) == unit->Common.Backend);
        assert(SMMU500UnitCreate(&description, &unit) == OS_EBUSY);
        assert(SMMU500UnitPoll(unit) == OS_EBUSY);
        DMAIOMMUUnitLock(unit->Common.Backend);
        assert(SMMU500SpaceCreate(unit, &space) == OS_EBUSY && space == NULL);
        DMAIOMMUUnitUnlock(unit->Common.Backend);
        g_smmu500Model.Clock = false;
        assert(SMMU500UnitDestroy(&unit) == OS_EOK && unit == NULL);
        assert(g_smmu500Model.Writes == 0);
        SMMU500ModelEnd();
    }
}

static void __RejectedProfiles(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;
    unsigned int scenario;

    for (scenario = 0; scenario < 19; scenario++) {
        SMMU500ModelBegin(false);
        description = SMMU500ModelDescription();
        switch (scenario) {
        case 0: description.Kind = ArmSMMUv3; break;
        case 1: description.TableCoherent = false; break;
        case 2: description.RegisterLength = 4096; break;
        case 3: description.RegisterBase++; break;
        case 4: g_smmu500Model.Registers[0x20 / 4] &= ~(1U << 29); break;
        case 5: g_smmu500Model.Registers[0x20 / 4] &= ~(1U << 27); break;
        case 6: g_smmu500Model.Registers[0x20 / 4] |= 1U << 15; break;
        case 7: g_smmu500Model.Registers[0x20 / 4] |= 1U << 8; break;
        case 8: g_smmu500Model.Registers[0x20 / 4] &= ~255U; break;
        case 9: g_smmu500Model.Registers[0x20 / 4] = (g_smmu500Model.Registers[0x20 / 4] & ~255U) | 129; break;
        case 10: g_smmu500Model.Registers[0x24 / 4] &= ~255U; break;
        case 11: g_smmu500Model.Registers[0x24 / 4] = (g_smmu500Model.Registers[0x24 / 4] & ~255U) | 129; break;
        case 12: g_smmu500Model.Registers[0x24 / 4] |= 5U << 16; break;
        case 13: g_smmu500Model.Registers[0x24 / 4] &= ~(7U << 28); break;
        case 14: g_smmu500Model.Registers[0x28 / 4] &= ~(1U << 12); break;
        case 15: g_smmu500Model.Registers[0x28 / 4] |= 15U; break;
        case 16: g_smmu500Model.Registers[0x28 / 4] |= 15U << 4; break;
        case 17: g_smmu500Model.Registers[0x3C / 4] = 0x23; break;
        case 18: description.RegisterBase = UINT64_MAX & ~4095ULL; break;
        }
        assert(SMMU500UnitCreate(&description, &unit) == OS_ENOTSUPPORTED && unit == NULL);
        assert(g_smmu500Model.Writes == 0);
        SMMU500ModelEnd();
    }
}

static void __CreationFailures(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;
    int index;

    for (index = 0; index < 5; index++) {
        SMMU500ModelBegin(false);
        description = SMMU500ModelDescription();
        if (index == 4) g_failRegisters = true;
        else g_failHeapAfter = index;
        assert(SMMU500UnitCreate(&description, &unit) == OS_EOOM && unit == NULL);
        assert(g_smmu500Model.Writes == 0);
        g_failHeapAfter = -1;
        SMMU500ModelEnd();
    }
}

static void __StartupAndCleanup(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;
    unsigned int scenario;
    uint32_t writes;
    unsigned int bank;

    for (scenario = 0; scenario < 7; scenario++) {
        SMMU500ModelBegin(scenario & 1);
        g_smmu500Model.Registers[0x20 / 4] &= ~(1U << 14); // Firmware coherency overrides CTTW.
        description = SMMU500ModelDescription();
        assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
        if (scenario == 0) g_smmu500Model.Clock = false;
        if (scenario == 1) g_failMap = true;
        if (scenario == 2) {
            g_smmu500Model.PendingSync = g_smmu500Model.HoldSync = true;
            g_smmu500Model.Registers[0x74 / 4] = 1;
            g_smmu500Model.TimeStep = 10000000;
        }
        if (scenario < 3) {
            assert(SMMU500UnitStart(unit) != OS_EOK);
            assert(SMMU500UnitPoll(unit) == OS_EBUSY);
            assert(SMMU500UnitStart(unit) == OS_ECANCELLED);
            assert(g_smmu500Model.Writes == 0);
            g_smmu500Model.Clock = false;
            assert(SMMU500UnitDestroy(&unit) == OS_EOK);
            SMMU500ModelEnd();
            continue;
        }
        if (scenario == 3) {
            g_smmu500Model.HoldSync = true;
            g_smmu500Model.TimeStep = 1000000;
            assert(SMMU500UnitStart(unit) == OS_ETIMEOUT);
            assert(unit->StartProgress == SMMU500StartupSync);
            assert(SMMU500UnitDestroy(&unit) == OS_ETIMEOUT && unit != NULL);
            g_smmu500Model.HoldSync = false;
            g_smmu500Model.TimeStep = 1000;
        } else {
            assert(SMMU500UnitStart(unit) == OS_EOK);
            writes = g_smmu500Model.Writes;
            assert(SMMU500UnitStart(unit) == OS_EOK && g_smmu500Model.Writes == writes);
        }
        assert((g_smmu500Model.Registers[0] & 0x8000FC3F) == 0x1C12);
        assert(g_smmu500Model.Registers[0x10 / 4] == 0x10);
        for (bank = 0; bank < 4; bank++) {
            assert(g_smmu500Model.Registers[(SMMU500ModelBank(bank) + 4) / 4] == 1);
        }
        if (scenario == 4) g_failUnmap = true;
        if (scenario == 5) g_failRelease = true;
        if (scenario == 6) DMAIOMMUUnitFail(unit->Common.Backend, OS_EDEVFAULT);
        if (scenario == 4 || scenario == 5) {
            assert(SMMU500UnitDestroy(&unit) != OS_EOK && unit != NULL);
            writes = g_smmu500Model.Writes;
            g_failUnmap = g_failRelease = false;
            g_smmu500Model.Clock = false; // Hardware is already safely closed.
            assert(SMMU500UnitDestroy(&unit) == OS_EOK && unit == NULL);
            assert(g_smmu500Model.Writes == writes);
        } else assert(SMMU500UnitDestroy(&unit) == OS_EOK);
        SMMU500ModelEnd();
    }
}

static void __RejectedFaultWrite(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;

    SMMU500ModelBegin(true);
    description = SMMU500ModelDescription();
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    g_smmu500Model.IgnoreFault = true;
    assert(SMMU500UnitStart(unit) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable && SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
    assert(g_unmaps == 0 && g_releases == 0);
}

static void __LockedCache(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;

    SMMU500ModelBegin(true);
    description = SMMU500ModelDescription();
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    g_smmu500Model.LockACR = true;
    assert(SMMU500UnitStart(unit) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable && SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
}

static void __TruncatedSID(void)
{
    ArmSMMUDescription_t description;
    SMMU500Unit_t* unit = NULL;

    SMMU500ModelBegin(true);
    description = SMMU500ModelDescription();
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    g_smmu500Model.SIDMask = 0x3FFF;
    assert(SMMU500UnitStart(unit) == OS_ECANCELLED);
    assert(unit->Common.CompletionUnreliable && SMMU500UnitDestroy(&unit) == OS_ECANCELLED);
}

void SMMU500UnitTests(void)
{
    __Probe();
    __RejectedProfiles();
    __CreationFailures();
    __StartupAndCleanup();
    SMMU500ModelPinned(__RejectedFaultWrite);
    SMMU500ModelPinned(__LockedCache);
    SMMU500ModelPinned(__TruncatedSID);
}

int main(void)
{
    SMMU500UnitTests();
    SMMU500ContextTests();
    SMMU500TableTests();
    SMMU500StreamTests();
    puts("MMU-500: capability, context, table, stream and generic DMA tests passed");
    return 0;
}
