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
 * Checked host boundaries for the mixed SMMUv3/MMU-500 DMA fixture.
 */

#include <component/timer.h>

#define __MIXED_POOL_BASE 0x80000000ULL
#define __MIXED_POOL_SIZE (8U * 1024U * 1024U)
#define __MIXED_PAGE_SIZE 4096U
#define __MIXED_MAPPINGS 16U
#define __MIXED_V3_BASE 0x100000ULL
#define __MIXED_500_BASE 0x400000ULL

enum __MixedBackend {
    __MixedV3,
    __Mixed500,
    __MixedBackendCount
};

// A physical allocation remains registered after its CPU view is unmapped.
// VirtualAddress becomes zero at Unmap. Only physical release clears the range.
// The pool's host storage remains intact so delayed device work can still read
// allocated bytes after the driver's CPU view is gone.
typedef struct __MixedMemory {
    uint64_t PhysicalAddress;
    vaddr_t  VirtualAddress;
    size_t   Length;
} __MixedMemory_t;

static __MixedMemory_t g_mixedMemory[__MIXED_MAPPINGS];

// Kernel views of pages that are already registered, such as the private
// source view of a bounce mapping. They alias registered pool memory and are
// not registered themselves.
typedef struct __MixedAlias {
    vaddr_t VirtualAddress;
    size_t  Length;
} __MixedAlias_t;

static __MixedAlias_t g_mixedAliases[4];
static uint8_t*        g_mixedPool;
static void*          g_mixedStack;
static uint64_t        g_mixedTime;
static DMAIOMMUUnit_t* g_mixedUnits[__MixedBackendCount];
static SMMUv3Unit_t*   g_mixedV3;
static SMMU500Unit_t*  g_mixed500;
static MemorySpace_t  g_mixedSpace = { .GroupGlobalHandleID = UUID_INVALID };

static __MixedMemory_t*
__FindMixedMemory(
    _In_ uint64_t physicalAddress,
    _In_ size_t   length)
{
    unsigned int index;

    for (index = 0; index < __MIXED_MAPPINGS; index++) {
        __MixedMemory_t* memory = &g_mixedMemory[index];

        if (memory->Length == 0 || physicalAddress < memory->PhysicalAddress) continue;
        if (physicalAddress - memory->PhysicalAddress > memory->Length) continue;
        if (length <= memory->Length - (physicalAddress - memory->PhysicalAddress)) return memory;
    }
    return NULL;
}

void*
SMMUTestPhysical(
    _In_ uint64_t physicalAddress,
    _In_ size_t   length)
{
    assert(g_mixedPool != NULL && length != 0);
    assert(__FindMixedMemory(physicalAddress, length) != NULL);
    return g_mixedPool + (physicalAddress - __MIXED_POOL_BASE);
}

void
SMMUTestAssertUnitLocked(
    _In_ uintptr_t registers)
{
    DMAIOMMUUnit_t* unit;

    if (registers == (uintptr_t)&g_smmuv3Model) unit = g_mixedUnits[__MixedV3];
    else {
        assert(registers == (uintptr_t)&g_smmu500Model);
        unit = g_mixedUnits[__Mixed500];
    }
    assert(unit != NULL && unit->Lock.held);
}

uintptr_t
Arm64MapDevice(
    _In_ uint64_t physicalAddress,
    _In_ uint64_t length)
{
    if (physicalAddress == __MIXED_V3_BASE) {
        assert(length == sizeof(g_smmuv3Model.Registers));
        assert(length <= __MIXED_500_BASE - physicalAddress);
        return (uintptr_t)&g_smmuv3Model;
    }
    assert(physicalAddress == __MIXED_500_BASE);
    assert(length != 0 && length <= sizeof(g_smmu500Model.Registers));
    g_smmu500Model.RegisterLength = length;
    return (uintptr_t)&g_smmu500Model;
}

void SystemTimerGetClockFrequency(UInteger64_t* frequency)
{
    frequency->QuadPart = 1000000000ULL;
}

void SystemTimerGetTimestamp(tick_t* tick)
{
    // Shared monotonic nanoseconds. A 1 ms step bounds intentionally held
    // completions without introducing wall-clock waits into the host suite.
    g_mixedTime += 1000000;
    *tick = (tick_t)g_mixedTime;
}

MemorySpace_t* __wrap_GetCurrentMemorySpace(void) { return &g_mixedSpace; }
MemorySpace_t* __wrap_GetDomainMemorySpace(void) { return &g_mixedSpace; }

oserr_t
__wrap_MemorySpaceMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      addressOut)
{
    uint64_t physical;
    unsigned int index;
    __MixedMemory_t* available = NULL;

    assert(space == &g_mixedSpace && options->Length != 0);
    assert(options->Length % __MIXED_PAGE_SIZE == 0);
    assert(options->Flags & MAPPING_COMMIT);
    assert(options->Flags & MAPPING_PERSISTENT);
    assert(((options->Flags & MAPPING_NOCACHE) != 0) == !g_pool.Cached);
    if (options->PlacementFlags & MAPPING_PHYSICAL_CONTIGUOUS) {
        assert(options->PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_GLOBAL));
        assert((g_mixedUnits[0] != NULL && g_mixedUnits[0]->Lock.held) ||
               (g_mixedUnits[1] != NULL && g_mixedUnits[1]->Lock.held));
        physical = options->PhysicalStart;
    } else if (options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_GLOBAL)) {
        // A kernel view made by SHM device code. Translated bounce storage
        // here is one unit, so both kinds of view are physically contiguous.
        physical = options->Pages[0];
        for (index = 0; index < options->Length / __MIXED_PAGE_SIZE; index++) {
            assert(options->Pages[index] == physical + index * __MIXED_PAGE_SIZE);
        }
        if (__FindMixedMemory(physical, options->Length) != NULL) {
            for (index = 0; index < sizeof(g_mixedAliases) / sizeof(g_mixedAliases[0]); index++) {
                if (g_mixedAliases[index].VirtualAddress == 0) {
                    g_mixedAliases[index].VirtualAddress = (vaddr_t)(g_mixedPool + (physical - __MIXED_POOL_BASE));
                    g_mixedAliases[index].Length = options->Length;
                    *addressOut = g_mixedAliases[index].VirtualAddress;
                    return OS_EOK;
                }
            }
            assert(!"too many kernel alias views");
        }
    } else {
        struct SHMBuffer* buffer = LookupHandle(options->SHMTag, HandleTypeSHM);

        assert(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_PROCESS));
        assert(buffer != NULL && buffer->Allocation.Owner == &g_machine.PhysicalMemory);
        assert(options->Length == buffer->Allocation.Length && options->Pages == buffer->Pages);
        physical = buffer->Allocation.PhysicalBase;
        for (index = 0; index < options->Length / __MIXED_PAGE_SIZE; index++) {
            assert(options->Pages[index] == physical + index * __MIXED_PAGE_SIZE);
        }
    }
    assert(physical >= __MIXED_POOL_BASE && physical - __MIXED_POOL_BASE <= __MIXED_POOL_SIZE);
    assert(options->Length <= __MIXED_POOL_SIZE - (physical - __MIXED_POOL_BASE));
    for (index = 0; index < __MIXED_MAPPINGS; index++) {
        __MixedMemory_t* memory = &g_mixedMemory[index];

        if (memory->Length == 0) {
            if (available == NULL) available = memory;
            continue;
        }
        assert(physical + options->Length <= memory->PhysicalAddress ||
               memory->PhysicalAddress + memory->Length <= physical);
    }
    assert(available != NULL);
    available->PhysicalAddress = physical;
    available->Length = options->Length;
    available->VirtualAddress = (vaddr_t)(g_mixedPool + (physical - __MIXED_POOL_BASE));
    memset((void*)available->VirtualAddress, 0xa5, options->Length);
    if (options->Flags & MAPPING_CLEAN) memset((void*)available->VirtualAddress, 0, options->Length);
    *addressOut = available->VirtualAddress;
    return OS_EOK;
}

oserr_t
__wrap_MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    unsigned int index;

    assert(space == &g_mixedSpace && address != 0);
    for (index = 0; index < sizeof(g_mixedAliases) / sizeof(g_mixedAliases[0]); index++) {
        if (g_mixedAliases[index].VirtualAddress != address) continue;
        assert(length == g_mixedAliases[index].Length);
        g_mixedAliases[index].VirtualAddress = 0;
        return OS_EOK;
    }
    for (index = 0; index < __MIXED_MAPPINGS; index++) {
        __MixedMemory_t* memory = &g_mixedMemory[index];

        if (memory->VirtualAddress != address) continue;
        assert(length == memory->Length);
        memory->VirtualAddress = 0;
        return OS_EOK;
    }
    assert(!"unmapping an unknown or already unmapped CPU view");
    return OS_EUNKNOWN;
}

static bool
__MixedRangeContains(
    _In_ const __MixedMemory_t* memory,
    _In_ uint64_t               physical)
{
    return physical >= memory->PhysicalAddress && physical - memory->PhysicalAddress < memory->Length;
}

oserr_t __real_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation);

oserr_t
__wrap_SystemMemoryReleaseAllocation(
    _InOut_ struct SystemMemoryAllocation* allocation)
{
    __MixedMemory_t* memory;
    unsigned int index;
    oserr_t status;

    // Coherent mappings have no bounce allocation. Their common cleanup still
    // calls the allocator's documented empty-record release.
    if (allocation->Owner == NULL) {
        assert(allocation->Length == 0 && allocation->PhysicalBase == 0);
        status = __real_SystemMemoryReleaseAllocation(allocation);
        assert(status == OS_EOK);
        return status;
    }
    assert(allocation->Owner == &g_machine.PhysicalMemory);
    memory = __FindMixedMemory(allocation->PhysicalBase, allocation->Length);
    assert(memory != NULL && memory->PhysicalAddress == allocation->PhysicalBase);
    assert(memory->Length == allocation->Length && memory->VirtualAddress == 0);
    // A source buffer must remain allocated while either unit still caches a
    // translation to it. A control arena can be released only after its unit
    // has stopped reading queues, stream tables and context roots.
    // Check pending work only for the arena being returned. A held walk on
    // the other unit must not prevent independent cleanup here.
    if (g_mixedV3 != NULL && allocation == &g_mixedV3->Common.Memory.Allocation) {
        assert(g_smmuv3Model.HeldWalks == 0 && !g_smmuv3Model.PendingAck);
    }
    if (g_mixed500 != NULL && allocation == &g_mixed500->Common.Memory.Allocation) {
        assert(g_smmu500Model.HeldWalks == 0 && !g_smmu500Model.PendingSync);
        assert(g_smmu500Model.PostedCount == 0);
    }
    if (g_smmuv3Model.Registers[SMMUV3_CR0ACK / 4] &
        (SMMUV3_CR0_ENABLE | SMMUV3_CR0_EVENTS | SMMUV3_CR0_COMMANDS)) {
        // CMDQ_BASE.ADDR is bits [51:5]. The low bits encode queue size.
        assert(!__MixedRangeContains(memory, g_smmuv3Model.CommandBase & 0x000FFFFFFFFFFFE0ULL));
        for (index = 0; index < SMMUV3_MODEL_CACHE_ENTRIES; index++) {
            if (g_smmuv3Model.Translations[index].Present) {
                assert(!__MixedRangeContains(memory, g_smmuv3Model.Translations[index].Descriptor & ARM_SMMU_S2_ADDRESS_MASK));
            }
        }
    }
    for (index = 0; index < SMMU500_MODEL_CACHE_ENTRIES; index++) {
        if (g_smmu500Model.Translations[index].Present) {
            assert(!__MixedRangeContains(memory, g_smmu500Model.Translations[index].Descriptor & ARM_SMMU_S2_ADDRESS_MASK));
        }
    }
    if (g_mixed500 != NULL) {
        for (index = 0; index < g_mixed500->ContextCount; index++) {
            uint32_t bank = SMMU500ModelBank(index);
            uint64_t root = g_smmu500Model.Registers[(bank + SMMU500_CB_TTBR0) / 4];

            root |= (uint64_t)g_smmu500Model.Registers[(bank + SMMU500_CB_TTBR0) / 4 + 1] << 32;
            if (g_smmu500Model.Registers[(bank + SMMU500_CB_SCTLR) / 4] & SMMU500_SCTLR_ENABLE) {
                assert(!__MixedRangeContains(memory, root & ARM_SMMU_S2_ADDRESS_MASK));
            }
        }
    }
    status = __real_SystemMemoryReleaseAllocation(allocation);
    assert(status == OS_EOK);
    memset(memory, 0, sizeof(*memory));
    return status;
}

oserr_t MSUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    // Only DMA grants and direct CPU views are supported by this fixture.
    (void)space; (void)address; (void)length;
    assert(false);
    return OS_EUNKNOWN;
}

void FreePhysicalMemory(int count, const uintptr_t* pages)
{
    (void)count; (void)pages;
    assert(!"pool-backed buffers must use SystemMemoryReleaseAllocation");
}

static void
__MixedStart(void)
{
    ArmSMMUDescription_t description;
    size_t storageLength = MemoryStackCalculateSize(__MIXED_POOL_SIZE / __MIXED_PAGE_SIZE);

    assert(g_mixedPool == NULL && g_mixedV3 == NULL && g_mixed500 == NULL);
    g_mixedPool = aligned_alloc(__MIXED_PAGE_SIZE, __MIXED_POOL_SIZE);
    g_mixedStack = calloc(1, storageLength);
    assert(g_mixedPool != NULL && g_mixedStack != NULL);
    memset(&g_machine, 0, sizeof(g_machine));
    SystemMemoryConstruct(&g_machine.PhysicalMemory, __MIXED_POOL_BASE, __MIXED_POOL_SIZE, __MIXED_PAGE_SIZE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory,
        __MIXED_POOL_BASE, __MIXED_POOL_SIZE, true, g_mixedStack, storageLength) == OS_EOK);
    SMMUv3ModelReset();
    SMMU500ModelReset();
    g_smmu500Model.TableCached = true;
    g_mixedTime = 0;
    description = SMMUv3ModelDescription();
    description.RegisterBase = __MIXED_V3_BASE;
    assert(SMMUv3UnitCreate(&description, &g_mixedV3) == OS_EOK);
    g_mixedUnits[__MixedV3] = SMMUv3UnitGetBackend(g_mixedV3);
    assert(SMMUv3UnitStart(g_mixedV3) == OS_EOK);
    description = SMMU500ModelDescription();
    description.RegisterBase = __MIXED_500_BASE;
    assert(SMMU500UnitCreate(&description, &g_mixed500) == OS_EOK);
    g_mixedUnits[__Mixed500] = SMMU500UnitGetBackend(g_mixed500);
    assert(SMMU500UnitStart(g_mixed500) == OS_EOK);
}

static void
__MixedFinish(void)
{
    unsigned int index;

    assert(SMMUv3UnitDestroy(&g_mixedV3) == OS_EOK && g_mixedV3 == NULL);
    g_mixedUnits[__MixedV3] = NULL;
    assert(SMMU500UnitDestroy(&g_mixed500) == OS_EOK && g_mixed500 == NULL);
    g_mixedUnits[__Mixed500] = NULL;
    for (index = 0; index < __MIXED_MAPPINGS; index++) assert(g_mixedMemory[index].Length == 0);
    assert(__PoolAllFree());
    free(g_mixedStack);
    free(g_mixedPool);
    g_mixedStack = NULL;
    g_mixedPool = NULL;
}
