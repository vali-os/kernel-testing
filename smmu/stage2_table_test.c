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
 * ARM SMMU Stage-2 Table Tests
 * Checks the table code shared by SMMUv3 and MMU-500 against the real control
 * arena, with a fake unit that counts barriers and failures.
 */

#include "../../kernel/arch/aarch64/smmu/stage2-table.h"
#include <assert.h>
#include <limits.h>
#include <log.h>
#include <machine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __PAGE 4096U
#define __POOL (4U * 1024U * 1024U)
#define __POOL_BASE 0x40000000ULL
#define __LIMIT_48 0xFFFFFFFFFFFFULL
#define __LIMIT_39 0x7FFFFFFFFFULL
#define __LIMIT_36 0xFFFFFFFFFULL

struct __Mapping {
    paddr_t Physical;
    void*   Address;
    size_t  Length;
};

static SystemMachine_t  g_machine;
static struct __Mapping g_mapping;
static void*            g_poolStorage;
static size_t           g_poolPages;
static size_t           g_heapCount;
static size_t           g_heapBaseline;
static size_t           g_heapCalls;
static unsigned int     g_heapBudget;
static bool             g_limitHeap;

static ArmSMMUDescription_t g_description = { .Kind = ArmSMMUv3, .RegisterBase = 0x9000000 };
static ArmSMMUMemory_t      g_memory;
static ArmSMMUStage2Unit_t  g_unit;
static ArmSMMUStage2Table_t g_table;
static unsigned int         g_barriers;
static unsigned int         g_failures;
static oserr_t              g_lastFailure;

SystemMachine_t* GetMachine(void) { return &g_machine; }
MemorySpace_t* GetDomainMemorySpace(void) { return &g_machine.SystemSpace; }
size_t GetMemorySpacePageSize(void) { return __PAGE; }

void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) { assert(atomic_exchange(&lock->Current, 1) == 0); }
void SpinlockReleaseIrq(Spinlock_t* lock) { assert(atomic_exchange(&lock->Current, 0) == 1); }

void* kmalloc(size_t size)
{
    void* result;

    g_heapCalls++;
    if (g_limitHeap) {
        if (g_heapBudget == 0) return NULL;
        g_heapBudget--;
    }
    result = malloc(size);
    assert(result != NULL);
    g_heapCount++;
    return result;
}

void kfree(void* pointer)
{
    if (pointer != NULL) {
        assert(g_heapCount != 0);
        g_heapCount--;
        free(pointer);
    }
}

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

oserr_t
MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* output)
{
    assert(space == &g_machine.SystemSpace && g_mapping.Address == NULL);
    g_mapping.Address = aligned_alloc(__PAGE, options->Length);
    assert(g_mapping.Address != NULL);
    g_mapping.Physical = options->PhysicalStart;
    g_mapping.Length = options->Length;
    memset(g_mapping.Address, 0xa5, options->Length);
    *output = (vaddr_t)g_mapping.Address;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    assert(space == &g_machine.SystemSpace);
    assert((vaddr_t)g_mapping.Address == address && g_mapping.Length == length);
    free(g_mapping.Address);
    memset(&g_mapping, 0, sizeof(g_mapping));
    return OS_EOK;
}

oserr_t __real_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation);
oserr_t
__wrap_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation)
{
    return __real_SystemMemoryReleaseAllocation(allocation);
}

static void __Barrier(void) { g_barriers++; }

static void
__Fail(void* context, oserr_t status)
{
    assert(context == &g_unit);
    g_failures++;
    g_lastFailure = status;
}

// A poll that lets the walk continue for budget pages and then times out.
static oserr_t
__PollBudget(void* context)
{
    unsigned int* budget = context;

    if (*budget == 0) {
        return OS_ETIMEOUT;
    }
    (*budget)--;
    return OS_EOK;
}

static oserr_t
__PollStatus(void* context)
{
    return *(oserr_t*)context;
}

// Start a fresh pool, arena and table. The limits differ per test, so a mix-up
// between the device address limit and the physical limit is visible.
static void
__Begin(uint32_t rootLevel, uint64_t addressLimit, uint64_t physicalLimit)
{
    size_t bytes = MemoryStackCalculateSize(__POOL / __PAGE);

    memset(&g_machine, 0, sizeof(g_machine));
    memset(&g_mapping, 0, sizeof(g_mapping));
    memset(&g_memory, 0, sizeof(g_memory));
    memset(&g_table, 0, sizeof(g_table));
    g_heapCount = 0;
    g_limitHeap = false;
    g_barriers = 0;
    g_failures = 0;
    g_lastFailure = OS_EOK;
    g_poolPages = __POOL / __PAGE;
    g_poolStorage = malloc(bytes);
    assert(g_poolStorage != NULL);
    SystemMemoryConstruct(&g_machine.PhysicalMemory, __POOL_BASE, __POOL, __PAGE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory,
        __POOL_BASE, __POOL, false, g_poolStorage, bytes) == OS_EOK);
    assert(ArmSMMUMemoryInitialize(&g_memory, ARM_SMMU_MEMORY_MAX_SIZE, physicalLimit) == OS_EOK);

    memset(&g_unit, 0, sizeof(g_unit));
    g_unit.Context = &g_unit;
    g_unit.Description = &g_description;
    g_unit.Memory = &g_memory;
    g_unit.RootLevel = rootLevel;
    g_unit.AddressLimit = addressLimit;
    g_unit.PhysicalLimit = physicalLimit;
    g_unit.Barrier = __Barrier;
    g_unit.Fail = __Fail;
    assert(ArmSMMUStage2TableCreate(&g_table, &g_unit) == OS_EOK);
    assert(g_memory.BlockCount == 1 && g_table.MappingCount == 0 && g_table.Pages.count == 0);
    assert(g_failures == 0);
    g_heapBaseline = g_heapCount;
}

// Everything the test took from the arena, the heap and the pool must be back.
static void
__End(void)
{
    assert(ArmSMMUStage2TableDestroy(&g_table) == OS_EOK);
    assert(g_memory.BlockCount == 0 && g_failures == 0);
    assert(ArmSMMUMemoryDestroy(&g_memory) == OS_EOK);
    assert(MemoryStackFreeBlockCount(&g_machine.PhysicalMemory.DeviceMemory.Stack) == g_poolPages);
    assert(g_heapCount == 0 && g_mapping.Address == NULL);
    free(g_poolStorage);
}

static DMAIOMMUMappingConfig_t
__Config(uint64_t address, uint64_t length, const SHMSG_t* extents, int count)
{
    DMAIOMMUMappingConfig_t config = { .DeviceAddress = address, .Length = length,
        .Direction = DMABidirectional, .Extents = extents, .ExtentCount = count };

    return config;
}

// Read the leaf for a device address by walking the tables in the arena.
static uint64_t
__ReadLeaf(uint64_t deviceAddress)
{
    volatile uint64_t* table = g_table.Root.VirtualAddress;
    uint64_t           entry;
    uint32_t           level;

    for (level = g_unit.RootLevel; level < 3; level++) {
        entry = table[(deviceAddress >> (39 - level * 9)) & 511];
        assert((entry & 3) == 3);
        table = (volatile uint64_t*)(g_memory.Mapping + ((entry & ARM_SMMU_S2_ADDRESS_MASK) -
                                                         g_memory.Allocation.PhysicalBase));
    }
    return table[(deviceAddress >> 12) & 511];
}

static uint64_t
__Attributes(enum DMADirection direction)
{
    uint64_t attributes = ARM_SMMU_S2_PAGE | ARM_SMMU_S2_ACCESS_FLAG | ARM_SMMU_S2_EXECUTE_NEVER |
                          ARM_SMMU_S2_NORMAL_WRITE_BACK | ARM_SMMU_S2_OUTER_SHAREABLE;

    if (direction != DMAFromDevice) attributes |= ARM_SMMU_S2_READ;
    if (direction != DMAToDevice) attributes |= ARM_SMMU_S2_WRITE;
    return attributes;
}

static void
__Reserve(ArmSMMUStage2Mapping_t* mapping, const DMAIOMMUMappingConfig_t* config)
{
    memset(mapping, 0, sizeof(*mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, config, mapping) == OS_EOK);
}

static void
__Release(ArmSMMUStage2Mapping_t* mapping)
{
    assert(ArmSMMUStage2MappingRelease(&g_table, mapping) == OS_EOK);
    assert(!ArmSMMUStage2MappingHoldsPages(mapping));
}

static void
__RejectMapping(const DMAIOMMUMappingConfig_t* config)
{
    ArmSMMUStage2Mapping_t mapping;

    memset(&mapping, 0, sizeof(mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, config, &mapping) == OS_EINVALPARAMS);
    assert(!ArmSMMUStage2MappingHoldsPages(&mapping));
    assert(g_table.MappingCount == 0 && g_memory.BlockCount == 1 && g_failures == 0);
}

static void
__TestValidation(void)
{
    SHMSG_t good = { .Address = 0x100000, .Length = __PAGE };
    SHMSG_t extent;
    SHMSG_t pair[2];
    DMAIOMMUMappingConfig_t config;
    ArmSMMUStage2Mapping_t  mapping;

    // The device limit is 48 bits and the physical limit is 36 bits.
    __Begin(0, __LIMIT_48, __LIMIT_36);
    __RejectMapping(NULL);
    config = __Config(0x2000, __PAGE, NULL, 1);
    __RejectMapping(&config);
    config = __Config(0x2000, __PAGE, &good, 0);
    __RejectMapping(&config);
    config = __Config(0x2000, __PAGE, &good, -1);
    __RejectMapping(&config);
    config = __Config(0x2000, 0, &good, 1);
    __RejectMapping(&config);
    config = __Config(0x2001, __PAGE, &good, 1);
    __RejectMapping(&config);
    config = __Config(0x2000, __PAGE + 1, &good, 1);
    __RejectMapping(&config);

    // Device range limits, including the last byte and wrap around.
    config = __Config(__LIMIT_48 + 1, __PAGE, &good, 1);
    __RejectMapping(&config);
    extent.Address = 0x100000;
    extent.Length = 2 * __PAGE;
    config = __Config(__LIMIT_48 + 1 - __PAGE, 2 * __PAGE, &extent, 1);
    __RejectMapping(&config);
    config = __Config(UINT64_MAX - __PAGE + 1, __PAGE, &good, 1);
    __RejectMapping(&config);

    config = __Config(0x2000, __PAGE, &good, 1);
    config.Direction = (enum DMADirection)99;
    __RejectMapping(&config);

    // Extents are checked against the physical limit, not the device limit.
    extent = good;
    extent.Address = 0x100001;
    config = __Config(0x2000, __PAGE, &extent, 1);
    __RejectMapping(&config);
    extent = good;
    extent.Length = 0;
    __RejectMapping(&config);
    extent.Address = 0x100000;
    extent.Length = __PAGE + 1;
    __RejectMapping(&config);
    extent.Address = __LIMIT_36 + 1;
    extent.Length = __PAGE;
    __RejectMapping(&config);
    extent.Address = __LIMIT_36 + 1 - __PAGE;
    extent.Length = 2 * __PAGE;
    config = __Config(0x2000, 2 * __PAGE, &extent, 1);
    __RejectMapping(&config);
    extent.Address = UINT64_MAX - __PAGE + 1;
    extent.Length = __PAGE;
    config = __Config(0x2000, __PAGE, &extent, 1);
    __RejectMapping(&config);

    // The extents must cover the device range exactly.
    pair[0] = good;
    pair[1] = good;
    config = __Config(0x2000, __PAGE, pair, 2);
    __RejectMapping(&config);
    config = __Config(0x2000, 2 * __PAGE, pair, 1);
    __RejectMapping(&config);
    config = __Config(0x2000, 3 * __PAGE, pair, 2);
    __RejectMapping(&config);

    // The last device page and the last physical page are both accepted.
    extent.Address = __LIMIT_36 + 1 - __PAGE;
    extent.Length = __PAGE;
    config = __Config(__LIMIT_48 + 1 - __PAGE, __PAGE, &extent, 1);
    __Reserve(&mapping, &config);
    assert(mapping.SparePages.count == 3 && g_memory.BlockCount == 4 && g_table.MappingCount == 1);
    __Release(&mapping);
    assert(g_table.MappingCount == 0 && g_memory.BlockCount == 1);
    __End();
}

static void
__TestWalk(enum DMADirection direction)
{
    SHMSG_t extents[2] = { { .Address = 0x80000000, .Length = __PAGE },
                           { .Address = 0x90000000, .Length = 2 * __PAGE } };
    DMAIOMMUMappingConfig_t config = __Config(0x1FF000, 3 * __PAGE, extents, 2);
    ArmSMMUStage2Mapping_t  mapping;
    unsigned int            unlimited = UINT_MAX;
    uint64_t                attributes = __Attributes(direction);

    // Level 1 root. The range crosses a 2 MiB boundary, which needs two leaf
    // tables below one table of the second level.
    config.Direction = direction;
    __Begin(1, __LIMIT_39, __LIMIT_39);
    __Reserve(&mapping, &config);
    assert(mapping.SparePages.count == 3 && mapping.InstalledBytes == 0 && g_table.MappingCount == 1);
    assert(g_memory.BlockCount == 4);
    g_limitHeap = true;
    g_heapBudget = 0;
    assert(ArmSMMUStage2WriteLeaves(&g_table, &mapping, __PollBudget, &unlimited) == OS_EOK);
    assert(mapping.InstalledBytes == 3 * __PAGE && mapping.SparePages.count == 0);
    assert(g_table.Pages.count == 3 && g_barriers == 3);
    assert(__ReadLeaf(0x1FF000) == (0x80000000ULL | attributes));
    assert(__ReadLeaf(0x200000) == (0x90000000ULL | attributes));
    assert(__ReadLeaf(0x201000) == (0x90001000ULL | attributes));
    assert(ArmSMMUStage2ClearLeaves(&g_table, &mapping, __PollBudget, &unlimited) == OS_EOK);
    assert(mapping.InstalledBytes == 0 && g_table.Pages.count == 3);
    assert(__ReadLeaf(0x1FF000) == 0 && __ReadLeaf(0x200000) == 0 && __ReadLeaf(0x201000) == 0);
    __Release(&mapping);
    g_limitHeap = false;
    __End();
}

static void
__TestSharedPaths(void)
{
    SHMSG_t extent = { .Address = 0x80000000, .Length = __PAGE };
    SHMSG_t pair[2] = { { .Address = 0x90000000, .Length = __PAGE },
                        { .Address = 0x91000000, .Length = __PAGE } };
    DMAIOMMUMappingConfig_t first = __Config(0x1000, __PAGE, &extent, 1);
    DMAIOMMUMappingConfig_t second = __Config(0x2000, __PAGE, &extent, 1);
    DMAIOMMUMappingConfig_t overlap = __Config(0x0, 2 * __PAGE, pair, 2);
    ArmSMMUStage2Mapping_t  a;
    ArmSMMUStage2Mapping_t  b;
    ArmSMMUStage2Mapping_t  c;
    unsigned int            unlimited = UINT_MAX;

    __Begin(1, __LIMIT_39, __LIMIT_39);
    __Reserve(&a, &first);
    __Reserve(&b, &second);
    __Reserve(&c, &overlap);
    assert(g_table.MappingCount == 3 && g_memory.BlockCount == 7);
    assert(ArmSMMUStage2WriteLeaves(&g_table, &a, __PollBudget, &unlimited) == OS_EOK);
    assert(g_table.Pages.count == 2 && g_barriers == 2);

    // The second mapping finds both tables in place and keeps its spare pages
    // until it is released.
    assert(ArmSMMUStage2WriteLeaves(&g_table, &b, __PollBudget, &unlimited) == OS_EOK);
    assert(g_table.Pages.count == 2 && g_barriers == 2 && b.SparePages.count == 2);
    assert(ArmSMMUStage2ClearLeaves(&g_table, &b, __PollBudget, &unlimited) == OS_EOK);
    __Release(&b);
    assert(g_memory.BlockCount == 5 && g_table.MappingCount == 2);

    // The overlapping mapping writes its first page and stops at the second.
    // Clearing it removes only its own page.
    assert(ArmSMMUStage2WriteLeaves(&g_table, &c, __PollBudget, &unlimited) == OS_EEXISTS);
    assert(c.InstalledBytes == __PAGE && g_failures == 0);
    assert(__ReadLeaf(0x0) == (0x90000000ULL | __Attributes(DMABidirectional)));
    assert(ArmSMMUStage2ClearLeaves(&g_table, &c, __PollBudget, &unlimited) == OS_EOK);
    assert(c.InstalledBytes == 0 && __ReadLeaf(0x0) == 0);
    assert(__ReadLeaf(0x1000) == (0x80000000ULL | __Attributes(DMABidirectional)));
    __Release(&c);
    assert(ArmSMMUStage2ClearLeaves(&g_table, &a, __PollBudget, &unlimited) == OS_EOK);
    __Release(&a);
    __End();
}

static void
__TestPoll(void)
{
    SHMSG_t extent = { .Address = 0x80000000, .Length = 8 * __PAGE };
    DMAIOMMUMappingConfig_t config = __Config(0x10000, 8 * __PAGE, &extent, 1);
    ArmSMMUStage2Mapping_t  mapping;
    unsigned int            budget = 5;
    unsigned int            unlimited = UINT_MAX;
    oserr_t                 stop = OS_EDEVFAULT;

    __Begin(1, __LIMIT_39, __LIMIT_39);
    __Reserve(&mapping, &config);

    // A poll status is returned unchanged and stops the walk before a page.
    assert(ArmSMMUStage2WriteLeaves(&g_table, &mapping, __PollStatus, &stop) == OS_EDEVFAULT);
    assert(mapping.InstalledBytes == 0 && g_table.Pages.count == 0);
    assert(ArmSMMUStage2WriteLeaves(&g_table, &mapping, __PollBudget, &budget) == OS_ETIMEOUT);
    assert(mapping.InstalledBytes == 5 * __PAGE);

    // Clearing continues where the previous call stopped.
    budget = 2;
    assert(ArmSMMUStage2ClearLeaves(&g_table, &mapping, __PollBudget, &budget) == OS_ETIMEOUT);
    assert(mapping.InstalledBytes == 3 * __PAGE);
    assert(__ReadLeaf(0x10000 + 3 * __PAGE) == 0 && __ReadLeaf(0x10000 + 2 * __PAGE) != 0);
    assert(ArmSMMUStage2ClearLeaves(&g_table, &mapping, __PollStatus, &stop) == OS_EDEVFAULT);
    assert(mapping.InstalledBytes == 3 * __PAGE);
    assert(ArmSMMUStage2ClearLeaves(&g_table, &mapping, __PollBudget, &unlimited) == OS_EOK);
    assert(mapping.InstalledBytes == 0 && __ReadLeaf(0x10000) == 0);
    __Release(&mapping);
    __End();
}

// Damage one root entry. Clearing must fail the unit once and keep the leaf.
static void
__CorruptEntry(ArmSMMUStage2Mapping_t* mapping, uint64_t entry, unsigned int expectedFailures)
{
    volatile uint64_t* root = g_table.Root.VirtualAddress;
    unsigned int       unlimited = UINT_MAX;
    uint64_t           saved = root[0];

    root[0] = entry;
    assert(ArmSMMUStage2ClearLeaves(&g_table, mapping, __PollBudget, &unlimited) == OS_EUNKNOWN);
    assert(g_failures == expectedFailures && g_lastFailure == OS_EUNKNOWN);
    assert(mapping->InstalledBytes == __PAGE);
    root[0] = saved;
}

static void
__TestCorruptTables(void)
{
    SHMSG_t extent = { .Address = 0x80000000, .Length = __PAGE };
    DMAIOMMUMappingConfig_t config = __Config(0x1000, __PAGE, &extent, 1);
    ArmSMMUStage2Mapping_t  mapping;
    unsigned int            unlimited = UINT_MAX;
    uint64_t                table;

    __Begin(1, __LIMIT_39, __LIMIT_39);
    __Reserve(&mapping, &config);
    assert(ArmSMMUStage2WriteLeaves(&g_table, &mapping, __PollBudget, &unlimited) == OS_EOK);
    table = ((volatile uint64_t*)g_table.Root.VirtualAddress)[0];

    __CorruptEntry(&mapping, 0, 1);
    __CorruptEntry(&mapping, (table & ~3ULL) | 1ULL, 2);
    __CorruptEntry(&mapping, (g_memory.Allocation.PhysicalBase - __PAGE) | ARM_SMMU_S2_TABLE, 3);
    __CorruptEntry(&mapping, (g_memory.Allocation.PhysicalBase + g_memory.Allocation.Length) |
                             ARM_SMMU_S2_TABLE, 4);
    g_failures = 0;
    assert(ArmSMMUStage2ClearLeaves(&g_table, &mapping, __PollBudget, &unlimited) == OS_EOK);
    __Release(&mapping);
    __End();
}

static void
__TestReservationLimits(void)
{
    SHMSG_t huge = { .Address = 0x100000000ULL, .Length = 0x60000000ULL };
    SHMSG_t extent = { .Address = 0x80000000, .Length = 2 * __PAGE };
    DMAIOMMUMappingConfig_t tooLarge = __Config(0, 0x60000000ULL, &huge, 1);
    DMAIOMMUMappingConfig_t needsFour = __Config(0x1FF000, 2 * __PAGE, &extent, 1);
    ArmSMMUStage2Mapping_t  mapping;
    ArmSMMUMemoryBlock_t    block;
    size_t                  heapCalls;

    // More table pages than the whole arena holds are rejected up front,
    // before any page record is allocated.
    __Begin(0, __LIMIT_48, __LIMIT_48);
    memset(&mapping, 0, sizeof(mapping));
    heapCalls = g_heapCalls;
    assert(ArmSMMUStage2MappingReserve(&g_table, &tooLarge, &mapping) == OS_EOOM);
    assert(g_heapCalls == heapCalls);
    assert(!ArmSMMUStage2MappingHoldsPages(&mapping) && g_memory.BlockCount == 1);
    assert(g_table.MappingCount == 0 && g_failures == 0);

    // The mapping counter cannot wrap.
    g_table.MappingCount = UINT32_MAX;
    memset(&mapping, 0, sizeof(mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, &needsFour, &mapping) == OS_EOVERFLOW);
    assert(!ArmSMMUStage2MappingHoldsPages(&mapping) && g_memory.BlockCount == 1);
    g_table.MappingCount = 0;

    // The arena runs out after three of four pages. Nothing stays reserved
    // and a plain shortage is not a unit failure.
    assert(ArmSMMUMemoryAllocate(&g_memory, 508 * __PAGE, 0, &block) == OS_EOK);
    memset(&mapping, 0, sizeof(mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, &needsFour, &mapping) == OS_EOOM);
    assert(!ArmSMMUStage2MappingHoldsPages(&mapping) && g_memory.BlockCount == 2);
    assert(g_table.MappingCount == 0 && g_failures == 0 && g_memory.State == ArmSMMUMemoryReady);
    assert(ArmSMMUMemoryFree(&g_memory, &block) == OS_EOK);

    // The same shortage from the heap.
    g_limitHeap = true;
    g_heapBudget = 2;
    memset(&mapping, 0, sizeof(mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, &needsFour, &mapping) == OS_EOOM);
    assert(!ArmSMMUStage2MappingHoldsPages(&mapping) && g_memory.BlockCount == 1);
    assert(g_table.MappingCount == 0 && g_failures == 0 && g_heapCount == g_heapBaseline);
    g_limitHeap = false;
    __End();
}

static void
__TestFailedArena(void)
{
    SHMSG_t extent = { .Address = 0x80000000, .Length = __PAGE };
    DMAIOMMUMappingConfig_t config = __Config(0x1000, __PAGE, &extent, 1);
    ArmSMMUStage2Mapping_t  mapping;
    ArmSMMUStage2Table_t    other;

    __Begin(1, __LIMIT_39, __LIMIT_39);

    // A failed arena fails the table creation and the reservation once each.
    g_memory.State = ArmSMMUMemoryFailed;
    memset(&other, 0, sizeof(other));
    assert(ArmSMMUStage2TableCreate(&other, &g_unit) == OS_ECANCELLED);
    assert(g_failures == 1 && g_lastFailure == OS_ECANCELLED);
    memset(&mapping, 0, sizeof(mapping));
    assert(ArmSMMUStage2MappingReserve(&g_table, &config, &mapping) == OS_ECANCELLED);
    assert(g_failures == 2 && !ArmSMMUStage2MappingHoldsPages(&mapping) && g_table.MappingCount == 0);
    g_memory.State = ArmSMMUMemoryReady;
    g_failures = 0;

    // Pages that cannot be returned stay in the mapping for a later retry,
    // and the unit is failed so nobody reuses the arena.
    __Reserve(&mapping, &config);
    g_memory.State = ArmSMMUMemoryFailed;
    assert(ArmSMMUStage2MappingRelease(&g_table, &mapping) == OS_ECANCELLED);
    assert(g_failures == 1 && g_lastFailure == OS_ECANCELLED);
    assert(ArmSMMUStage2MappingHoldsPages(&mapping) && g_table.MappingCount == 1);
    assert(ArmSMMUStage2TableDestroy(&g_table) == OS_ECANCELLED);
    assert(g_failures == 2);
    g_memory.State = ArmSMMUMemoryReady;
    g_failures = 0;
    __Release(&mapping);

    // Releasing a mapping that was never reserved is a bookkeeping error.
    memset(&mapping, 0, sizeof(mapping));
    list_construct(&mapping.SparePages);
    assert(ArmSMMUStage2MappingRelease(&g_table, &mapping) == OS_EUNKNOWN);
    assert(g_failures == 1 && g_lastFailure == OS_EUNKNOWN);
    g_failures = 0;
    __End();
}

int main(void)
{
    __TestValidation();
    __TestWalk(DMABidirectional);
    __TestWalk(DMAToDevice);
    __TestWalk(DMAFromDevice);
    __TestSharedPaths();
    __TestPoll();
    __TestCorruptTables();
    __TestReservationLimits();
    __TestFailedArena();
    puts("Arm SMMU stage-2 tables: validation, leaves, shared paths, polling, corruption and failures passed");
    return 0;
}
