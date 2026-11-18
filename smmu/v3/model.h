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
 * Host SMMUv3 model and checked kernel boundaries. This interface is test-only.
 */
#ifndef __SMMUV3_TEST_MODEL_H__
#define __SMMUV3_TEST_MODEL_H__

#include "../../../kernel/arch/aarch64/smmu/v3/private.h"
#include "../model_support.h"
#include <assert.h>

#define SMMUV3_MODEL_POOL_BASE 0x80000000ULL
#define SMMUV3_MODEL_POOL_SIZE (4U * 1024U * 1024U)
#define SMMUV3_MODEL_CACHE_ENTRIES 32U

// Caches outlive CPU descriptor updates. Invalidations become effective only
// at SYNC retirement, which can be held independently of command consumption.
typedef struct SMMUv3ModelRoute {
    bool     Present;
    uint32_t SID;
    uint64_t Words[8];
} SMMUv3ModelRoute_t;

typedef struct SMMUv3ModelTranslation {
    bool     Present;
    uint32_t VMID;
    uint64_t Address;
    uint64_t Descriptor;
} SMMUv3ModelTranslation_t;

typedef struct SMMUv3Model {
    uint32_t Registers[0x20000U / 4U];
    uint64_t CommandBase;
    uint64_t EventBase;
    uint64_t StreamBase;
    uint64_t Now;
    uint64_t TimeStep;
    bool     Clock;
    bool     HoldAck;
    uint32_t HoldAckOffset;
    uint32_t HoldAckValue;
    bool     HoldCommands;
    bool     HoldSync;
    uint32_t SyncStopAfter;
    uint32_t HeldWalks;
    uint32_t StaleError;
    uint32_t Writes;
    uint32_t ProducerWrites;
    uint32_t Consumed;
    uint32_t Syncs;
    uint32_t Barriers;
    uint32_t LastProducerBarrier;
    uint32_t RemainingAckReads;
    uint32_t AckDelay;
    uint32_t PendingAckOffset;
    uint32_t PendingAckValue;
    bool     PendingAck;
    uint64_t L1[1024];
    bool     L1Present[1024];
    bool     InvalidateL1[1024];
    bool     InvalidateSID[65536];
    bool     InvalidateVMID[65536];
    SMMUv3ModelRoute_t Routes[SMMUV3_MODEL_CACHE_ENTRIES];
    SMMUv3ModelTranslation_t Translations[SMMUV3_MODEL_CACHE_ENTRIES];
} SMMUv3Model_t;

extern SMMUv3Model_t g_smmuv3Model;
extern int           g_failHeapAfter;
extern bool          g_noHeap;
extern bool          g_failMap;
extern bool          g_failUnmap;
extern bool          g_failRelease;
extern bool          g_failRegisters;
extern size_t        g_heapCount;
extern size_t        g_unmaps;
extern size_t        g_releases;
extern char          g_lastError[512];

/**
 * @brief Reset register state, pending work and caches to the baseline profile.
 * Call only after the previous modeled unit has stopped using this record.
 * This does not initialize fixture memory, locks or allocation-failure controls.
 */
void SMMUv3ModelReset(void);

// Standalone fixture setup and teardown. Mixed tests supply their own runtime.
void SMMUv3ModelBegin(bool cached);
void SMMUv3ModelEnd(void);
ArmSMMUDescription_t SMMUv3ModelDescription(void);
SMMUv3Unit_t* SMMUv3ModelStart(void);
void SMMUv3ModelAdvance(void);
oserr_t SMMUv3ModelAccess(uint32_t sid, uint64_t address, bool write, uint64_t* physicalOut);
void SMMUv3ModelEvent(uint32_t sid, uint32_t type, uint64_t address);
void SMMUv3ModelPinned(void (*test)(void));
void SMMUv3UnitTests(void);
void SMMUv3QueueTests(void);
void SMMUv3TableTests(void);
void SMMUv3StreamTests(void);
#endif
