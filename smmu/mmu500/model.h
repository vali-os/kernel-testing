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
 * Host MMU-500 model and checked kernel boundaries. Test-only interfaces.
 */
#ifndef __SMMU500_TEST_MODEL_H__
#define __SMMU500_TEST_MODEL_H__

#include "../../../kernel/arch/aarch64/smmu/mmu500/private.h"
#include "../model_support.h"
#include <assert.h>

#define SMMU500_MODEL_POOL_BASE 0x80000000ULL
#define SMMU500_MODEL_POOL_SIZE (4U * 1024U * 1024U)
#define SMMU500_MODEL_CACHE_ENTRIES 32U
#define SMMU500_MODEL_REGISTER_SIZE 0x200000U

typedef struct SMMU500ModelRoute {
    bool     Present;
    uint32_t SID;
    uint32_t Bank;
    bool     Fault;
} SMMU500ModelRoute_t;

typedef struct SMMU500ModelTranslation {
    bool     Present;
    uint32_t VMID;
    uint64_t Address;
    uint64_t Descriptor;
} SMMU500ModelTranslation_t;

// MMIO writes are posted until a barrier. Accepted routes and cached TLB
// entries have separate lifetimes. Holding old walks prevents SYNC completion.
typedef struct SMMU500Model {
    uint32_t Registers[SMMU500_MODEL_REGISTER_SIZE / 4U];
    // Length supplied by the mapping mock. IDR1 may describe a smaller unit.
    uint64_t RegisterLength;
    // CPU cache policy supplied by fixture setup. Context TCR must agree.
    bool     TableCached;
    uint64_t Now;
    uint64_t TimeStep;
    bool     Clock;
    bool     HoldSync;
    bool     PendingSync;
    bool     IgnoreFault;
    bool     LockACR;
    uint32_t RejectOffset;
    uint32_t SIDMask;
    uint32_t HeldWalks;
    uint32_t SyncDelay;
    uint32_t RemainingSyncReads;
    uint32_t SyncStopAfter;
    uint32_t Writes;
    uint32_t SyncWrites;
    uint32_t Syncs;
    uint32_t Barriers;
    uint32_t CompletionFault;
    uint32_t CompletionBankFault;
    uint32_t GlobalRace;
    uint32_t BankRace;
    uint32_t InvalidationVMID;
    bool     InvalidateAll;
    bool     InvalidationPending;
    uint32_t PostedCount;
    struct {
        uint32_t Offset;
        uint64_t Value;
        bool     Wide;
    } Posted[256];
    SMMU500ModelRoute_t Routes[SMMU500_MODEL_CACHE_ENTRIES];
    SMMU500ModelTranslation_t Translations[SMMU500_MODEL_CACHE_ENTRIES];
} SMMU500Model_t;

extern SMMU500Model_t g_smmu500Model;
extern char           g_lastError[512];
extern char           g_firstError[512];
extern int            g_failHeapAfter;
extern bool           g_noHeap;
extern bool           g_failMap;
extern bool           g_failUnmap;
extern bool           g_failRelease;
extern bool           g_failRegisters;
extern size_t         g_heapCount;
extern size_t         g_unmaps;
extern size_t         g_releases;

/**
 * @brief Reset register state, pending work and caches to the baseline profile.
 * Call only after the previous modeled unit has stopped using this record.
 * This does not initialize fixture memory, locks or allocation-failure controls.
 */
void SMMU500ModelReset(void);

// Standalone fixture setup and teardown. Mixed tests supply their own runtime.
void SMMU500ModelBegin(bool cached);
void SMMU500ModelEnd(void);
ArmSMMUDescription_t SMMU500ModelDescription(void);
SMMU500Unit_t* SMMU500ModelStart(void);
uint32_t SMMU500ModelBank(uint32_t bank);
void SMMU500ModelAdvance(void);
oserr_t SMMU500ModelAccess(uint32_t sid, uint64_t address, bool write, uint64_t* physicalOut);

/**
 * @brief Record a context bank fault the way hardware does.
 * If the bank's FSR already holds a fault, only MULTI is set and the address
 * and syndrome registers keep the earlier fault (IHI 0062D.c section 3.7).
 */
void SMMU500ModelContextFault(uint32_t bank, uint32_t status, uint32_t sid, uint64_t address, bool write);

/**
 * @brief Record a global fault the way hardware does.
 * If GFSR already holds a fault, only MULTI is set (IHI 0062D.c section 3.8.1).
 */
void SMMU500ModelGlobalFault(uint32_t status, uint32_t sid, uint64_t address, bool write);

void SMMU500ModelPinned(void (*test)(void));
void SMMU500UnitTests(void);
void SMMU500ContextTests(void);
void SMMU500TableTests(void);
void SMMU500StreamTests(void);
void SMMU500FaultTests(void);
#endif
