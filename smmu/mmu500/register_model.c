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
 * SMMU500 register model shared by standalone and mixed host fixtures.
 * The model preserves posted work and cached translations independently of
 * CPU descriptor writes. Physical memory and lock checks enter through the
 * two boundaries in model_support.h.
 */

#include "model.h"
#include <string.h>

SMMU500Model_t g_smmu500Model;

void
SMMU500ModelReset(void)
{
    unsigned int bank;

    memset(&g_smmu500Model, 0, sizeof(g_smmu500Model));
    g_smmu500Model.Clock = true;
    g_smmu500Model.TimeStep = 1000;
    // Literal architectural encodings: stage 2, matching, coherent, 15-bit
    // SIDs, eight slots, four general banks, 4 KiB register pages and granules.
    g_smmu500Model.Registers[0x20 / 4] = (1U << 29) | (1U << 27) | (1U << 14) | (15U << 9) | 8;
    g_smmu500Model.Registers[0x24 / 4] = (2U << 28) | 4;
    g_smmu500Model.Registers[0x28 / 4] = (1U << 12) | 0x55;
    g_smmu500Model.Registers[0x3C / 4] = 0x24;
    g_smmu500Model.Registers[0] = 1; // Firmware bypass. Start must clear CLIENTPD.
    g_smmu500Model.Registers[0x10 / 4] = (1U << 26) | (7U << 8) | (1U << 4);
    g_smmu500Model.RejectOffset = UINT32_MAX;
    g_smmu500Model.SIDMask = 0x7FFF;
    for (bank = 0; bank < 4; bank++) {
        g_smmu500Model.Registers[(SMMU500ModelBank(bank) + 4) / 4] = 3;
    }
}

ArmSMMUDescription_t SMMU500ModelDescription(void)
{
    return (ArmSMMUDescription_t) { .Kind = ArmSMMU500, .RegisterBase = 0x100000,
        .RegisterLength = 2ULL << (((g_smmu500Model.Registers[0x24 / 4] >> 31) ? 16 : 12) +
                                  ((g_smmu500Model.Registers[0x24 / 4] >> 28) & 7) + 1), .TableCoherent = true };
}

uint32_t SMMU500ModelBank(uint32_t bank)
{
    uint32_t id1 = g_smmu500Model.Registers[0x24 / 4];
    uint32_t page = (id1 >> 31) ? 16 : 12;

    return (1U << (page + ((id1 >> 28) & 7) + 1)) + (bank << page);
}

void SMMU500ModelAdvance(void)
{
    unsigned int index;

    if (!g_smmu500Model.PendingSync || g_smmu500Model.HoldSync || g_smmu500Model.HeldWalks != 0 ||
        (g_smmu500Model.SyncStopAfter != 0 && g_smmu500Model.Syncs >= g_smmu500Model.SyncStopAfter)) return;
    if (g_smmu500Model.RemainingSyncReads != 0) {
        g_smmu500Model.RemainingSyncReads--;
        return;
    }
    for (index = 0; index < SMMU500_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmu500Model.InvalidateAll) g_smmu500Model.Routes[index].Present = false;
        if (g_smmu500Model.Translations[index].Present &&
            (g_smmu500Model.InvalidateAll || g_smmu500Model.Translations[index].VMID == g_smmu500Model.InvalidationVMID)) {
            g_smmu500Model.Translations[index].Present = false;
        }
    }
    g_smmu500Model.InvalidateAll = false;
    g_smmu500Model.InvalidationPending = false;
    g_smmu500Model.PendingSync = false;
    g_smmu500Model.Registers[0x74 / 4] = 0;
    g_smmu500Model.Syncs++;
    g_smmu500Model.Registers[0x48 / 4] |= g_smmu500Model.CompletionFault;
    g_smmu500Model.CompletionFault = 0;
    g_smmu500Model.Registers[(SMMU500ModelBank(0) + 0x58) / 4] |= g_smmu500Model.CompletionBankFault;
    g_smmu500Model.CompletionBankFault = 0;
}

uint32_t SMMU500TestRead32(uintptr_t base, unsigned int offset)
{
    assert(base == (uintptr_t)&g_smmu500Model && offset < g_smmu500Model.RegisterLength && offset % 4 == 0);
    if (offset < 0x20 || offset > 0x3C) SMMUTestAssertUnitLocked(base);
    SMMU500ModelAdvance();
    return g_smmu500Model.Registers[offset / 4];
}

uint64_t SMMU500TestRead64(uintptr_t base, unsigned int offset)
{
    uint64_t low = SMMU500TestRead32(base, offset);
    uint64_t high = SMMU500TestRead32(base, offset + 4);

    assert(offset % 8 == 0);
    return low | (high << 32);
}

static void __Post(uintptr_t base, uint32_t offset, uint64_t value, bool wide)
{
    unsigned int index;

    // A mixed fixture may accept the other unit's lock. Check model identity
    // before changing the posted-write queue or asking it to check the mutex.
    assert(base == (uintptr_t)&g_smmu500Model);
    SMMUTestAssertUnitLocked(base);
    assert(offset < g_smmu500Model.RegisterLength && offset % (wide ? 8 : 4) == 0);
    index = g_smmu500Model.PostedCount;
    assert(index < 256);
    g_smmu500Model.PostedCount++;
    g_smmu500Model.Posted[index].Offset = offset;
    g_smmu500Model.Posted[index].Value = value;
    g_smmu500Model.Posted[index].Wide = wide;
    g_smmu500Model.Writes++;
}

void SMMU500TestWrite32(uintptr_t base, unsigned int offset, uint32_t value)
{
    __Post(base, offset, value, false);
}

void SMMU500TestWrite64(uintptr_t base, unsigned int offset, uint64_t value)
{
    __Post(base, offset, value, true);
}

static void __Accept(uint32_t offset, uint64_t value, bool wide)
{
    uint32_t* regs = g_smmu500Model.Registers;
    unsigned int bank;

    if (offset == g_smmu500Model.RejectOffset) return;
    if (wide) {
        assert(offset >= SMMU500ModelBank(0));
        regs[offset / 4] = (uint32_t)value;
        regs[offset / 4 + 1] = (uint32_t)(value >> 32);
        return;
    }
    if (offset == 0x48) {
        regs[offset / 4] |= g_smmu500Model.GlobalRace;
        g_smmu500Model.GlobalRace = 0;
        regs[offset / 4] &= ~(uint32_t)value;
        return;
    }
    for (bank = 0; bank < (regs[0x24 / 4] & 255); bank++) {
        if (offset == SMMU500ModelBank(bank) + 0x58) {
            regs[offset / 4] |= g_smmu500Model.BankRace;
            g_smmu500Model.BankRace = 0;
            regs[offset / 4] &= ~(uint32_t)value;
            return;
        }
    }
    if (offset == 0x10 && g_smmu500Model.LockACR) return;
    if (offset >= 0xC00 && offset < 0xE00 && g_smmu500Model.IgnoreFault && ((value >> 16) & 3) == 2) return;
    // CBNDX/PRIVCFG are reserved in fault format and may preserve old values.
    if (offset >= 0xC00 && offset < 0xE00 && ((value >> 16) & 3) == 2) {
        value |= regs[offset / 4] & 0x030000FFU;
    }
    if (offset >= 0x800 && offset < 0xA00) value &= 0xFFFF8000U | g_smmu500Model.SIDMask;
    if (offset == 0) assert((value & 1U) == 0); // No bypass write, even during cleanup.
    if (offset == 0x64 || offset == 0x68) {
        assert(!g_smmu500Model.InvalidationPending && !g_smmu500Model.PendingSync);
        g_smmu500Model.InvalidationPending = true;
        g_smmu500Model.InvalidateAll = offset == 0x68;
        g_smmu500Model.InvalidationVMID = value;
        assert(offset != 0x64 || (value != 0 && value <= 128));
    }
    if (offset == 0x70) {
        assert(g_smmu500Model.InvalidationPending && !g_smmu500Model.PendingSync);
        assert((regs[0] & 0x3800) == 0x1800); // Private VMIDs, no broadcast.
        g_smmu500Model.PendingSync = true;
        g_smmu500Model.RemainingSyncReads = g_smmu500Model.SyncDelay;
        regs[0x74 / 4] = 1;
        g_smmu500Model.SyncWrites++;
    }
    regs[offset / 4] = value;
}

void SMMU500TestBarrier(void)
{
    unsigned int index;

    SMMUTestAssertUnitLocked((uintptr_t)&g_smmu500Model);
    g_smmu500Model.Barriers++;
    for (index = 0; index < g_smmu500Model.PostedCount; index++) {
        __Accept(g_smmu500Model.Posted[index].Offset, g_smmu500Model.Posted[index].Value, g_smmu500Model.Posted[index].Wide);
    }
    g_smmu500Model.PostedCount = 0;
}

static SMMU500ModelRoute_t* __Route(uint32_t sid)
{
    SMMU500ModelRoute_t* cached = NULL;
    unsigned int index;
    uint32_t match;
    uint32_t route;

    for (index = 0; index < SMMU500_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmu500Model.Routes[index].Present && g_smmu500Model.Routes[index].SID == sid) return &g_smmu500Model.Routes[index];
        if (!g_smmu500Model.Routes[index].Present) cached = &g_smmu500Model.Routes[index];
    }
    assert(cached != NULL);
    cached->SID = sid;
    cached->Present = true;
    cached->Fault = true;
    for (index = 0; index < (g_smmu500Model.Registers[0x20 / 4] & 255); index++) {
        match = g_smmu500Model.Registers[(0x800 + 4 * index) / 4];
        if ((match & 0x80000000) == 0) continue;
        assert((match & 0x7FFF8000) == 0); // Exact ordinary SID, no masks.
        if ((match & 0x7FFF) != sid) continue;
        route = g_smmu500Model.Registers[(0xC00 + 4 * index) / 4];
        assert(((route >> 16) & 3) != 1); // Bypass never installed by this backend.
        cached->Bank = route & 255;
        cached->Fault = ((route >> 16) & 3) != 0;
        break;
    }
    return cached;
}

oserr_t SMMU500ModelAccess(uint32_t sid, uint64_t address, bool write, uint64_t* physicalOut)
{
    SMMU500ModelRoute_t* route;
    SMMU500ModelTranslation_t* cached = NULL;
    uint64_t* table;
    uint64_t descriptor;
    uint64_t root;
    uint32_t bank;
    uint32_t gr1;
    uint32_t control;
    uint32_t vmid;
    uint32_t level;
    unsigned int index;

    // CLIENTPD is deliberately modeled as bypass, not as a blocked client.
    if ((g_smmu500Model.Registers[0] & 1) != 0) {
        *physicalOut = address;
        return OS_EOK;
    }
    route = __Route(sid);
    if (route->Fault) return OS_EDEVFAULT;
    bank = SMMU500ModelBank(route->Bank);
    gr1 = 1U << ((g_smmu500Model.Registers[0x24 / 4] >> 31) ? 16 : 12);
    if ((g_smmu500Model.Registers[bank / 4] & 1) == 0) return OS_EDEVFAULT;
    assert((g_smmu500Model.Registers[bank / 4] & 0xF0) == 0x20);
    assert(g_smmu500Model.Registers[(gr1 + 0x800 + route->Bank * 4) / 4] == 1);
    vmid = g_smmu500Model.Registers[(gr1 + route->Bank * 4) / 4];
    assert(vmid == route->Bank + 1 && vmid <= 128);
    control = g_smmu500Model.Registers[(bank + 0x30) / 4];
    assert((control & 0x80003F00) == (g_smmu500Model.TableCached ? 0x2500U : 0x2000U));
    if ((address >> (64 - (control & 63))) != 0) return OS_EDEVFAULT;
    for (index = 0; index < SMMU500_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmu500Model.Translations[index].Present && g_smmu500Model.Translations[index].VMID == vmid &&
            g_smmu500Model.Translations[index].Address == (address & ~4095ULL)) {
            cached = &g_smmu500Model.Translations[index];
            break;
        }
        if (!g_smmu500Model.Translations[index].Present) cached = &g_smmu500Model.Translations[index];
    }
    assert(cached != NULL);
    if (!cached->Present) {
        level = 2 - ((control >> 6) & 3);
        root = g_smmu500Model.Registers[(bank + 0x20) / 4] | ((uint64_t)g_smmu500Model.Registers[(bank + 0x24) / 4] << 32);
        for (;;) {
            table = SMMUTestPhysical(root, 4096);
            descriptor = table[(address >> (39 - level * 9)) & 511];
            if ((descriptor & 3) != 3) return OS_EDEVFAULT;
            if (level == 3) break;
            root = descriptor & 0x0000FFFFFFFFF000ULL;
            level++;
        }
        assert((descriptor & (1ULL << 10)) != 0);
        assert((descriptor & (3ULL << 53)) == (2ULL << 53));
        assert((descriptor & (0xFULL << 2)) == (0xFULL << 2));
        cached->Present = true;
        cached->VMID = vmid;
        cached->Address = address & ~4095ULL;
        cached->Descriptor = descriptor;
    }
    descriptor = cached->Descriptor;
    if ((descriptor & (1ULL << (write ? 7 : 6))) == 0) return OS_EDEVFAULT;
    *physicalOut = (descriptor & 0x0000FFFFFFFFF000ULL) | (address & 4095);
    return OS_EOK;
}
