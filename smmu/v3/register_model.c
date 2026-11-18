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
 * SMMUv3 register model shared by standalone and mixed host fixtures.
 * The model preserves posted work and cached translations independently of
 * CPU descriptor writes. Physical memory and lock checks enter through the
 * two boundaries in model_support.h.
 */

#include "model.h"
#include <string.h>

SMMUv3Model_t g_smmuv3Model;

void
SMMUv3ModelReset(void)
{
    memset(&g_smmuv3Model, 0, sizeof(g_smmuv3Model));
    g_smmuv3Model.Clock = true;
    g_smmuv3Model.TimeStep = 1000;
    // Independently specified baseline: stage 2, AArch64, coherent, two-level,
    // 16 SID bits, 256 commands, 128 events, 4 KiB granules and 48 PA bits.
    g_smmuv3Model.Registers[0] = 1U | (2U << 2) | (1U << 4) | (1U << 27);
    g_smmuv3Model.Registers[1] = 16U | (8U << 21) | (7U << 16);
    g_smmuv3Model.Registers[0x14 / 4] = (1U << 4) | 5U;
}

ArmSMMUDescription_t SMMUv3ModelDescription(void)
{
    return (ArmSMMUDescription_t) { .Kind = ArmSMMUv3, .RegisterBase = 0x100000,
        .RegisterLength = 0x20000, .TableCoherent = true };
}

static void __CommitInvalidations(void)
{
    unsigned int i;

    for (i = 0; i < 1024; i++) {
        if (g_smmuv3Model.InvalidateL1[i]) g_smmuv3Model.L1Present[i] = false;
    }
    for (i = 0; i < SMMUV3_MODEL_CACHE_ENTRIES; i++) {
        if (g_smmuv3Model.Routes[i].Present && g_smmuv3Model.InvalidateSID[g_smmuv3Model.Routes[i].SID]) {
            g_smmuv3Model.Routes[i].Present = false;
        }
        if (g_smmuv3Model.Translations[i].Present && g_smmuv3Model.InvalidateVMID[g_smmuv3Model.Translations[i].VMID]) {
            g_smmuv3Model.Translations[i].Present = false;
        }
    }
    memset(g_smmuv3Model.InvalidateL1, 0, sizeof(g_smmuv3Model.InvalidateL1));
    memset(g_smmuv3Model.InvalidateSID, 0, sizeof(g_smmuv3Model.InvalidateSID));
    memset(g_smmuv3Model.InvalidateVMID, 0, sizeof(g_smmuv3Model.InvalidateVMID));
}

void SMMUv3ModelAdvance(void)
{
    uint32_t* regs = g_smmuv3Model.Registers;
    uint32_t  consumer = regs[0x9C / 4] & 0xFFFFU;
    uint32_t  capacity = 1U << (g_smmuv3Model.CommandBase & 31U);
    uint32_t  sid;
    uint32_t  opcode;
    uint64_t* command;

    if (g_smmuv3Model.PendingAck && !g_smmuv3Model.HoldAck) {
        if (g_smmuv3Model.RemainingAckReads != 0) g_smmuv3Model.RemainingAckReads--;
        else {
            regs[g_smmuv3Model.PendingAckOffset / 4] = g_smmuv3Model.PendingAckValue;
            g_smmuv3Model.PendingAck = false;
        }
    }
    if ((regs[0x24 / 4] & 8U) == 0 || g_smmuv3Model.HoldCommands ||
        consumer == regs[0x98 / 4]) return;
    command = SMMUTestPhysical((g_smmuv3Model.CommandBase & 0x000FFFFFFFFFFFE0ULL) +
                                  (consumer & (capacity - 1U)) * 16U, 16);
    opcode = (uint32_t)command[0] & 0xFFU;
    assert((command[0] & ~0x0000FFFF000000FFULL) == 0 || opcode == 3);
    switch (opcode) {
    case 3:
        sid = (uint32_t)(command[0] >> 32);
        assert(sid < 65536 && (command[1] & ~1ULL) == 0);
        g_smmuv3Model.InvalidateSID[sid] = true;
        if (command[1] == 0) g_smmuv3Model.InvalidateL1[sid / 64] = true;
        break;
    case 4:
        assert(command[1] == 31);
        memset(g_smmuv3Model.InvalidateSID, 1, sizeof(g_smmuv3Model.InvalidateSID));
        memset(g_smmuv3Model.InvalidateL1, 1, sizeof(g_smmuv3Model.InvalidateL1));
        break;
    case 0x28:
        assert(command[1] == 0);
        g_smmuv3Model.InvalidateVMID[(command[0] >> 32) & 65535] = true;
        break;
    case 0x30:
        assert(command[1] == 0);
        memset(g_smmuv3Model.InvalidateVMID, 1, sizeof(g_smmuv3Model.InvalidateVMID));
        break;
    case 0x46:
        assert(command[0] == 0x46 && command[1] == 0);
        if (g_smmuv3Model.HoldSync || g_smmuv3Model.HeldWalks != 0 ||
            (g_smmuv3Model.SyncStopAfter != 0 && g_smmuv3Model.Syncs >= g_smmuv3Model.SyncStopAfter)) return;
        __CommitInvalidations();
        g_smmuv3Model.Syncs++;
        break;
    default:
        assert(!"unsupported command encoding");
    }
    g_smmuv3Model.Consumed++;
    regs[0x9C / 4] = (consumer + 1U) & ((capacity * 2U) - 1U);
}

uint32_t SMMUv3TestRead32(uintptr_t base, unsigned int offset)
{
    assert(base == (uintptr_t)&g_smmuv3Model && offset < 0x20000 && offset % 4 == 0);
    // Probe can read IDs without a backend lock. All mutable registers require it.
    if (offset >= 0x20) SMMUTestAssertUnitLocked(base);
    SMMUv3ModelAdvance();
    return g_smmuv3Model.Registers[offset / 4] | (offset == 0x9C ? g_smmuv3Model.StaleError : 0);
}

static void __Acknowledge(uint32_t offset, uint32_t value)
{
    assert(!g_smmuv3Model.PendingAck);
    if (g_smmuv3Model.Registers[offset / 4] == value) return;
    g_smmuv3Model.PendingAck = true;
    g_smmuv3Model.PendingAckOffset = offset;
    g_smmuv3Model.PendingAckValue = value;
    g_smmuv3Model.RemainingAckReads = g_smmuv3Model.AckDelay;
    if (g_smmuv3Model.HoldAckOffset == offset && g_smmuv3Model.HoldAckValue == value) g_smmuv3Model.HoldAck = true;
}

void SMMUv3TestWrite32(uintptr_t base, unsigned int offset, uint32_t value)
{
    // The shared fixture can recognize both units. This hook must still reject
    // the other model's register base, even when that unit's mutex is held.
    assert(base == (uintptr_t)&g_smmuv3Model);
    SMMUTestAssertUnitLocked(base);
    assert(offset < 0x20000 && offset % 4 == 0);
    g_smmuv3Model.Writes++;
    g_smmuv3Model.Registers[offset / 4] = value;
    switch (offset) {
    case 0x20:
        // RES0 ATSCHK cannot be echoed when ATS is absent.
        __Acknowledge(0x24, value & ((g_smmuv3Model.Registers[0] & (1U << 10)) ? 31U : 15U));
        break;
    case 0x44:
        assert(value & (1U << 31));
        __Acknowledge(0x44, value & ~(1U << 31));
        break;
    case 0x50:
        assert(value == 0);
        __Acknowledge(0x54, value);
        break;
    case 0x28:
    case 0x2C:
    case 0x88:
        assert(g_smmuv3Model.Registers[0x24 / 4] == 0);
        break;
    case 0x98:
        if ((g_smmuv3Model.Registers[0x24 / 4] & 8U) != 0) {
            assert(g_smmuv3Model.Barriers > g_smmuv3Model.LastProducerBarrier);
            g_smmuv3Model.LastProducerBarrier = g_smmuv3Model.Barriers;
            g_smmuv3Model.ProducerWrites++;
        }
        break;
    case 0x9C:
        assert((g_smmuv3Model.Registers[0x24 / 4] & 8U) == 0);
        break;
    case 0x100A8:
        assert((g_smmuv3Model.Registers[0x24 / 4] & 4U) == 0);
        break;
    case 0x100AC:
        break;
    default:
        assert(!"unexpected register write");
    }
}

void SMMUv3TestWrite64(uintptr_t base, unsigned int offset, uint64_t value)
{
    assert(base == (uintptr_t)&g_smmuv3Model);
    SMMUTestAssertUnitLocked(base);
    assert(g_smmuv3Model.Registers[0x24 / 4] == 0 && offset % 8 == 0);
    g_smmuv3Model.Writes++;
    switch (offset) {
    case 0x80: g_smmuv3Model.StreamBase = value; break;
    case 0x90: g_smmuv3Model.CommandBase = value; break;
    case 0xA0: g_smmuv3Model.EventBase = value; break;
    default: assert(!"unexpected wide register write");
    }
}

void SMMUv3TestBarrier(void)
{
    SMMUTestAssertUnitLocked((uintptr_t)&g_smmuv3Model);
    g_smmuv3Model.Barriers++;
}

static SMMUv3ModelRoute_t* __Route(uint32_t sid)
{
    SMMUv3ModelRoute_t* cached = NULL;
    uint64_t* root;
    uint64_t* entry;
    unsigned int index;

    for (index = 0; index < SMMUV3_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmuv3Model.Routes[index].Present && g_smmuv3Model.Routes[index].SID == sid) return &g_smmuv3Model.Routes[index];
        if (!g_smmuv3Model.Routes[index].Present) cached = &g_smmuv3Model.Routes[index];
    }
    assert(cached != NULL);
    if (!g_smmuv3Model.L1Present[sid / 64]) {
        root = SMMUTestPhysical(g_smmuv3Model.StreamBase & 0x000FFFFFFFFFFFC0ULL, 8192);
        g_smmuv3Model.L1[sid / 64] = root[sid / 64];
        g_smmuv3Model.L1Present[sid / 64] = true;
    }
    memset(cached->Words, 0, sizeof(cached->Words));
    if ((g_smmuv3Model.L1[sid / 64] & 31) != 0) {
        assert((g_smmuv3Model.L1[sid / 64] & 31) == 7);
        entry = SMMUTestPhysical((g_smmuv3Model.L1[sid / 64] & 0x000FFFFFFFFFFFC0ULL) + (sid % 64) * 64, 64);
        memcpy(cached->Words, entry, 64);
    }
    cached->SID = sid;
    cached->Present = true;
    return cached;
}

oserr_t SMMUv3ModelAccess(uint32_t sid, uint64_t address, bool write, uint64_t* physicalOut)
{
    SMMUv3ModelRoute_t* route;
    SMMUv3ModelTranslation_t* cached = NULL;
    uint64_t* table;
    uint64_t descriptor;
    uint64_t root;
    uint32_t control;
    uint32_t vmid;
    uint32_t level;
    unsigned int index;

    if ((g_smmuv3Model.Registers[0x24 / 4] & 1U) == 0 || sid >= (1U << (g_smmuv3Model.Registers[0x88 / 4] & 63))) return OS_EDEVFAULT;
    route = __Route(sid);
    if ((route->Words[0] & 15ULL) != 13ULL) return OS_EDEVFAULT;
    vmid = route->Words[2] & 65535;
    assert(vmid != 0 && (route->Words[2] & (1ULL << 51)) != 0);
    assert((route->Words[2] & ((1ULL << 52) | (1ULL << 57))) == 0);
    control = (uint32_t)(route->Words[2] >> 32) & 0x7FFFFU;
    if ((address >> (64 - (control & 63))) != 0) return OS_EDEVFAULT;
    for (index = 0; index < SMMUV3_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmuv3Model.Translations[index].Present && g_smmuv3Model.Translations[index].VMID == vmid &&
            g_smmuv3Model.Translations[index].Address == (address & ~4095ULL)) {
            cached = &g_smmuv3Model.Translations[index];
            break;
        }
        if (!g_smmuv3Model.Translations[index].Present) cached = &g_smmuv3Model.Translations[index];
    }
    assert(cached != NULL);
    if (!cached->Present) {
        level = 2 - ((control >> 6) & 3);
        root = route->Words[3];
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

void SMMUv3ModelEvent(uint32_t sid, uint32_t type, uint64_t address)
{
    uint32_t capacity = 1U << (g_smmuv3Model.EventBase & 31);
    uint32_t producer = g_smmuv3Model.Registers[0x100A8 / 4];
    uint64_t* entry = SMMUTestPhysical((g_smmuv3Model.EventBase & 0x000FFFFFFFFFFFE0ULL) +
                                         (producer & (capacity - 1)) * 32, 32);
    entry[0] = type | ((uint64_t)sid << 32);
    entry[1] = 0;
    entry[2] = address;
    entry[3] = 0;
    g_smmuv3Model.Registers[0x100A8 / 4] = (producer & (1U << 31)) | ((producer + 1) & (capacity * 2 - 1));
}
