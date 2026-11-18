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
 * ARM SMMU Control Memory Tests
 * Checks arena lifetime and descriptor fields with the real kernel allocators.
 */

#include "../../kernel/arch/aarch64/smmu/memory.h"
#include "../../kernel/arch/aarch64/smmu/stage2.h"
#include <assert.h>
#include <log.h>
#include <machine.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define __PAGE 4096U
#define __POOL (4U * 1024U * 1024U)

struct __Mapping {
    paddr_t Physical;
    void*   Address;
    size_t  Length;
};

static SystemMachine_t  g_machine;
static struct __Mapping g_mappings[4];
static void*            g_poolStorage;
static size_t           g_poolPages;
static size_t           g_heapCount;
static size_t           g_pageSize = __PAGE;
static bool             g_failHeap;
static bool             g_failMap;
static bool             g_partialMap;
static bool             g_failUnmap;
static bool             g_failRelease;
static unsigned int     g_maps;
static unsigned int     g_unmaps;
static unsigned int     g_releases;

SystemMachine_t* GetMachine(void) { return &g_machine; }
MemorySpace_t* GetDomainMemorySpace(void) { return &g_machine.SystemSpace; }
size_t GetMemorySpacePageSize(void) { return g_pageSize; }

void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) { assert(atomic_exchange(&lock->Current, 1) == 0); }
void SpinlockReleaseIrq(Spinlock_t* lock) { assert(atomic_exchange(&lock->Current, 0) == 1); }

void* kmalloc(size_t size)
{
    void* result;

    if (g_failHeap) return NULL;
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
    unsigned int expected = MAPPING_COMMIT | MAPPING_PERSISTENT;

    assert(space == &g_machine.SystemSpace);
    if (!g_machine.PhysicalMemory.DeviceMemory.Cached) expected |= MAPPING_NOCACHE;
    assert(options->Flags == expected);
    assert(options->PlacementFlags == (MAPPING_PHYSICAL_CONTIGUOUS | MAPPING_VIRTUAL_GLOBAL));
    assert(options->Length != 0 && options->Length % __PAGE == 0);
    assert(options->Pages == NULL && options->SHMTag == 0 && options->SHMView == NULL);
    g_maps++;
    if (g_failMap && !g_partialMap) return OS_EOOM;
    for (size_t i = 0; i < 4; i++) {
        if (g_mappings[i].Address == NULL) {
            g_mappings[i].Address = aligned_alloc(__PAGE, options->Length);
            assert(g_mappings[i].Address != NULL);
            g_mappings[i].Physical = options->PhysicalStart;
            g_mappings[i].Length = options->Length;
            memset(g_mappings[i].Address, 0xa5, options->Length);
            *output = (vaddr_t)g_mappings[i].Address;
            return g_failMap ? OS_EUNKNOWN : OS_EOK;
        }
    }
    assert(!"unexpected test mapping exhaustion");
    return OS_EOOM;
}

oserr_t
MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    assert(space == &g_machine.SystemSpace);
    g_unmaps++;
    for (size_t i = 0; i < 4; i++) {
        if ((vaddr_t)g_mappings[i].Address == address) {
            assert(address != 0 && g_mappings[i].Length == length);
            if (g_failUnmap) return OS_ETIMEOUT;
            free(g_mappings[i].Address);
            memset(&g_mappings[i], 0, sizeof(g_mappings[i]));
            return OS_EOK;
        }
    }
    assert(!"unmap repeated or supplied an unknown address");
    return OS_ENOENT;
}

oserr_t __real_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation);
oserr_t
__wrap_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation)
{
    if (allocation->Owner != NULL) {
        // CPU unmap must finish before physical memory is returned or reused.
        for (size_t i = 0; i < 4; i++) {
            assert(g_mappings[i].Address == NULL || g_mappings[i].Physical != allocation->PhysicalBase);
        }
        g_releases++;
        if (g_failRelease) return OS_EBUSY;
    }
    return __real_SystemMemoryReleaseAllocation(allocation);
}

static void
__Begin(paddr_t base, size_t length, bool cached)
{
    size_t bytes = MemoryStackCalculateSize(length / __PAGE);

    memset(&g_machine, 0, sizeof(g_machine));
    memset(g_mappings, 0, sizeof(g_mappings));
    g_heapCount = 0;
    g_pageSize = __PAGE;
    g_failHeap = g_failMap = g_partialMap = g_failUnmap = g_failRelease = false;
    g_maps = g_unmaps = g_releases = 0;
    g_poolPages = length / __PAGE;
    g_poolStorage = malloc(bytes);
    assert(g_poolStorage != NULL);
    SystemMemoryConstruct(&g_machine.PhysicalMemory, base, length, __PAGE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory,
        base, length, cached, g_poolStorage, bytes) == OS_EOK);
}

static void
__Finish(void)
{
    assert(MemoryStackFreeBlockCount(&g_machine.PhysicalMemory.DeviceMemory.Stack) == g_poolPages);
    assert(g_heapCount == 0);
    for (size_t i = 0; i < 4; i++) assert(g_mappings[i].Address == NULL);
    free(g_poolStorage);
}

static void
__CheckZero(const void* data, size_t length)
{
    const unsigned char* bytes = data;

    for (size_t i = 0; i < length; i++) assert(bytes[i] == 0);
}

static void
__RejectBlock(ArmSMMUMemory_t* memory, ArmSMMUMemoryBlock_t block)
{
    ArmSMMUMemoryBlock_t saved = block;
    size_t freePages = MemoryStackFreeBlockCount(&memory->FreePages);
    size_t count = memory->BlockCount;

    assert(ArmSMMUMemoryFree(memory, &block) == OS_EINVALPARAMS);
    assert(memcmp(&saved, &block, sizeof(block)) == 0);
    assert(MemoryStackFreeBlockCount(&memory->FreePages) == freePages && memory->BlockCount == count);
    assert(memory->State == ArmSMMUMemoryReady);
}

static void
__TestBlocks(bool cached)
{
    ArmSMMUMemory_t memory = {0};
    ArmSMMUMemory_t other = {0};
    ArmSMMUMemoryBlock_t first;
    ArmSMMUMemoryBlock_t second;
    ArmSMMUMemoryBlock_t foreign;
    ArmSMMUMemoryBlock_t invalid;
    paddr_t reused;

    __Begin(0x80001000, __POOL, cached);
    assert(ArmSMMUMemoryInitialize(&memory, ARM_SMMU_MEMORY_MAX_SIZE, UINT64_MAX) == OS_EOK);
    assert(memory.State == ArmSMMUMemoryReady && memory.PageCount == 512);
    assert(memory.FreePages.data_size == MemoryStackCalculateSize(512));
    __CheckZero((void*)memory.Mapping, memory.Allocation.Length);
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_EBUSY);
    assert(ArmSMMUMemoryInitialize(&other, __PAGE, UINT64_MAX) == OS_EOK);
    assert(ArmSMMUMemoryAllocate(&other, 1, 0, &foreign) == OS_EOK);
    __RejectBlock(&memory, foreign);
    assert(ArmSMMUMemoryFree(&other, &foreign) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&other) == OS_EOK);

    // All future Allocate/Free calls must work without heap allocation.
    g_failHeap = true;
    assert(ArmSMMUMemoryAllocate(&memory, 1, 0, &first) == OS_EOK);
    assert(first.Length == __PAGE && first.PhysicalAddress == memory.Allocation.PhysicalBase);
    assert(first.VirtualAddress == (void*)memory.Mapping);
    assert(ArmSMMUMemoryAllocate(&memory, __PAGE + 1, 65536, &second) == OS_EOK);
    assert(second.Length == 2 * __PAGE && second.PhysicalAddress % 65536 == 0);
    assert(second.VirtualAddress == (void*)(memory.Mapping + second.PhysicalAddress - memory.Allocation.PhysicalBase));
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EBUSY && memory.State == ArmSMMUMemoryReady);
    assert(g_unmaps == 1);
    invalid = second;
    invalid.Length = __PAGE;
    __RejectBlock(&memory, invalid);
    invalid = second;
    invalid.PhysicalAddress += __PAGE;
    invalid.VirtualAddress = (char*)invalid.VirtualAddress + __PAGE;
    invalid.Length = __PAGE;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.VirtualAddress = (char*)invalid.VirtualAddress + 8;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.PhysicalAddress++;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.Length = 0;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.Length = __PAGE + 1;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.PhysicalAddress = memory.Allocation.PhysicalBase - __PAGE;
    __RejectBlock(&memory, invalid);
    invalid = first;
    invalid.Length = memory.Allocation.Length + __PAGE;
    __RejectBlock(&memory, invalid);

    memset(first.VirtualAddress, 0x5a, first.Length);
    invalid = first;
    reused = first.PhysicalAddress;
    assert(ArmSMMUMemoryFree(&memory, &first) == OS_EOK);
    __CheckZero(&first, sizeof(first));
    __RejectBlock(&memory, invalid);
    __RejectBlock(&memory, first);
    assert(ArmSMMUMemoryAllocate(&memory, __PAGE, 1, &first) == OS_EOK);
    assert(first.PhysicalAddress == reused);
    __CheckZero(first.VirtualAddress, first.Length);
    assert(ArmSMMUMemoryFree(&memory, &first) == OS_EOK);
    assert(ArmSMMUMemoryFree(&memory, &second) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __CheckZero(&memory, sizeof(memory));
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __Finish();
}

static void
__TestFragmentation(void)
{
    ArmSMMUMemory_t memory = {0};
    ArmSMMUMemoryBlock_t blocks[512];
    ArmSMMUMemoryBlock_t whole;
    ArmSMMUMemoryBlock_t sentinel = {1, (void*)2, 3};
    ArmSMMUMemoryBlock_t output = sentinel;

    __Begin(0, __POOL, false);
    assert(ArmSMMUMemoryInitialize(&memory, ARM_SMMU_MEMORY_MAX_SIZE, UINT64_MAX) == OS_EOK);
    g_failHeap = true;
    for (size_t i = 0; i < 512; i++) {
        assert(ArmSMMUMemoryAllocate(&memory, __PAGE, 0, &blocks[i]) == OS_EOK);
        assert(blocks[i].PhysicalAddress == i * __PAGE);
        memset(blocks[i].VirtualAddress, 0x5a, __PAGE);
    }
    assert(ArmSMMUMemoryAllocate(&memory, __PAGE, 0, &output) == OS_EOOM);
    assert(memcmp(&sentinel, &output, sizeof(output)) == 0);
    // Combining two neighboring allocations must not bypass the exact-block check.
    whole = blocks[0];
    whole.Length = 2 * __PAGE;
    __RejectBlock(&memory, whole);
    for (size_t i = 0; i < 512; i += 2) assert(ArmSMMUMemoryFree(&memory, &blocks[i]) == OS_EOK);
    assert(memory.FreePages.index == 256);
    assert(ArmSMMUMemoryAllocate(&memory, 2 * __PAGE, 0, &output) == OS_EOOM);
    assert(memcmp(&sentinel, &output, sizeof(output)) == 0);
    for (size_t i = 1; i < 512; i += 2) assert(ArmSMMUMemoryFree(&memory, &blocks[i]) == OS_EOK);
    assert(ArmSMMUMemoryAllocate(&memory, ARM_SMMU_MEMORY_MAX_SIZE, 0, &whole) == OS_EOK);
    __CheckZero(whole.VirtualAddress, whole.Length);
    assert(ArmSMMUMemoryFree(&memory, &whole) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __Finish();
}

static void
__TestLimits(void)
{
    ArmSMMUMemory_t memory = {0};
    ArmSMMUMemory_t second = {0};
    ArmSMMUMemory_t third = {0};
    ArmSMMUMemoryBlock_t output = {1, (void*)2, 3};
    ArmSMMUMemoryBlock_t saved = output;

    __Begin(0x1000, __POOL, false);
    assert(ArmSMMUMemoryInitialize(NULL, __PAGE, UINT64_MAX) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryInitialize(&memory, 0, UINT64_MAX) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryInitialize(&memory, ARM_SMMU_MEMORY_MAX_SIZE + 1, UINT64_MAX) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryInitialize(&memory, SIZE_MAX, UINT64_MAX) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(&memory, 1, 0, &output) == OS_EBUSY);
    assert(ArmSMMUMemoryFree(&memory, &output) == OS_EBUSY);
    g_pageSize = 65536;
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_ENOTSUPPORTED);
    g_pageSize = __PAGE;
    g_machine.PhysicalMemory.BlockSize = 65536;
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_ENOTSUPPORTED);
    g_machine.PhysicalMemory.BlockSize = __PAGE;
    __CheckZero(&memory, sizeof(memory));
    assert(ArmSMMUMemoryInitialize(&memory, 8 * __PAGE - 1, UINT64_MAX) == OS_EOK);
    assert(memory.Allocation.Length == 8 * __PAGE);
    assert(ArmSMMUMemoryAllocate(&memory, 0, 0, &output) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(&memory, SIZE_MAX, 0, &output) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(&memory, 1, 3, &output) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(NULL, 1, 0, &output) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(&memory, 1, 0, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryFree(NULL, &output) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryFree(&memory, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryDestroy(NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUMemoryAllocate(&memory, 1, (size_t)1 << 63, &output) == OS_EOOM);
    assert(memcmp(&saved, &output, sizeof(output)) == 0);
    assert(ArmSMMUMemoryAllocate(&memory, 1, 16384, &output) == OS_EOK);
    assert(output.PhysicalAddress == 0x4000);
    assert(MemoryStackFreeBlockCount(&memory.FreePages) == 7);
    assert(ArmSMMUMemoryFree(&memory, &output) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);

    // The full pool fits two arenas. Exhaustion must leave no published arena.
    assert(ArmSMMUMemoryInitialize(&memory, ARM_SMMU_MEMORY_MAX_SIZE, UINT64_MAX) == OS_EOK);
    assert(ArmSMMUMemoryInitialize(&second, ARM_SMMU_MEMORY_MAX_SIZE, UINT64_MAX) == OS_EOK);
    assert(ArmSMMUMemoryInitialize(&third, __PAGE, UINT64_MAX) == OS_EOOM);
    assert(third.State == ArmSMMUMemoryPreparing && third.Mapping == 0);
    assert(ArmSMMUMemoryAllocate(&third, __PAGE, 0, &output) == OS_EBUSY);
    assert(ArmSMMUMemoryDestroy(&third) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&second) == OS_EOK);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __Finish();
}

static void
__TestPhysicalLimit(paddr_t base, uint64_t limit, oserr_t expected)
{
    ArmSMMUMemory_t memory = {0};

    __Begin(base, 2 * __PAGE, false);
    assert(ArmSMMUMemoryInitialize(&memory, 2 * __PAGE, limit) == expected);
    assert(g_maps == (expected == OS_EOK ? 1U : 0U));
    assert(memory.Allocation.Owner != NULL);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __Finish();
}

static void
__TestPrepareFailures(void)
{
    ArmSMMUMemory_t memory = {0};

    __Begin(0, __POOL, false);
    g_failHeap = true;
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_EOOM);
    assert(memory.State == ArmSMMUMemoryPreparing && memory.Allocation.Owner == NULL);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    g_failHeap = false;
    g_failMap = true;
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_EOOM);
    assert(memory.State == ArmSMMUMemoryPreparing && memory.Mapping == 0);
    g_failRelease = true;
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EBUSY);
    assert(memory.State == ArmSMMUMemoryClosing && memory.Allocation.Owner != NULL);
    g_failRelease = false;
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    // Even a mapping API error with a recorded partial view must be cleaned
    // before physical release. This checks the record's conservative lifetime.
    g_partialMap = true;
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_EUNKNOWN);
    assert(memory.Mapping != 0 && memory.State == ArmSMMUMemoryPreparing);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    __Finish();
}

static void
__TestDestroyRetries(void)
{
    ArmSMMUMemory_t memory = {0};
    ArmSMMUMemoryBlock_t output = {1, (void*)2, 3};
    ArmSMMUMemoryBlock_t saved = output;
    vaddr_t mapping;

    __Begin(0, __POOL, false);
    assert(ArmSMMUMemoryInitialize(&memory, __PAGE, UINT64_MAX) == OS_EOK);
    mapping = memory.Mapping;
    g_failUnmap = true;
    assert(ArmSMMUMemoryDestroy(&memory) == OS_ETIMEOUT);
    assert(memory.State == ArmSMMUMemoryClosing && memory.Mapping == mapping);
    assert(memory.Allocation.Owner != NULL && g_releases == 0 && g_heapCount == 1);
    assert(ArmSMMUMemoryAllocate(&memory, 1, 0, &output) == OS_EBUSY);
    assert(memcmp(&saved, &output, sizeof(output)) == 0);
    g_failUnmap = false;
    g_failRelease = true;
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EBUSY);
    assert(memory.Mapping == 0 && memory.Allocation.Owner != NULL && g_unmaps == 2);
    assert(g_releases == 1 && g_heapCount == 1);
    g_failRelease = false;
    assert(ArmSMMUMemoryDestroy(&memory) == OS_EOK);
    assert(g_unmaps == 2 && g_releases == 2);
    __CheckZero(&memory, sizeof(memory));
    __Finish();
}

static void
__TestInvariant(unsigned int point)
{
    ArmSMMUMemory_t memory = {0};
    ArmSMMUMemoryBlock_t block;
    ArmSMMUMemoryBlock_t output = {1, (void*)2, 3};
    ArmSMMUMemoryBlock_t saved = output;

    __Begin(0, __POOL, false);
    assert(ArmSMMUMemoryInitialize(&memory, 2 * __PAGE, UINT64_MAX) == OS_EOK);
    if (point < 3) {
        assert(ArmSMMUMemoryAllocate(&memory, 2 * __PAGE, 0, &block) == OS_EOK);
        if (point == 0) {
            // Put an allocated range back into the stack without changing the
            // allocation table. Free must detect the conflicting free range.
            assert(MemoryStackPush(&memory.FreePages, block.PhysicalAddress, 2) == OS_EOK);
        } else if (point == 1) {
            memory.BlockPages[1] = 0;
        } else {
            memory.BlockCount = 0;
        }
        assert(ArmSMMUMemoryFree(&memory, &block) == OS_EUNKNOWN);
        assert(block.Length == 2 * __PAGE);
    } else if (point == 3) {
        assert(ArmSMMUMemoryAllocate(&memory, 2 * __PAGE, 0, &block) == OS_EOK);
        assert(MemoryStackPush(&memory.FreePages, block.PhysicalAddress, 2) == OS_EOK);
        assert(ArmSMMUMemoryAllocate(&memory, __PAGE, 0, &output) == OS_EUNKNOWN);
        assert(memcmp(&saved, &output, sizeof(output)) == 0);
    } else if (point == 4) {
        memory.FreePages.index = 0;
        assert(ArmSMMUMemoryDestroy(&memory) == OS_EUNKNOWN);
    } else {
        memory.BlockPages[0] = 1;
        assert(ArmSMMUMemoryDestroy(&memory) == OS_EUNKNOWN);
    }
    assert(memory.State == ArmSMMUMemoryFailed);
    assert(ArmSMMUMemoryAllocate(&memory, __PAGE, 0, &output) == OS_ECANCELLED);
    assert(ArmSMMUMemoryFree(&memory, &output) == OS_ECANCELLED);
    assert(ArmSMMUMemoryDestroy(&memory) == OS_ECANCELLED);
    assert(memory.Mapping != 0 && memory.Allocation.Owner != NULL);
    assert(g_heapCount == 1 && g_releases == 0 && g_unmaps == 0);
    // The child exits with pinned memory. No force-free escape is supplied.
}

static void
__TestDescriptors(void)
{
    uint64_t leaf = ARM_SMMU_S2_PAGE | ARM_SMMU_S2_WRITE | ARM_SMMU_S2_ACCESS_FLAG |
        ARM_SMMU_S2_INNER_SHAREABLE | ARM_SMMU_S2_NORMAL_WRITE_BACK | ARM_SMMU_S2_EXECUTE_NEVER;

    assert(ARM_SMMU_S2_PAGE_SHIFT == 12 && ARM_SMMU_S2_PAGE_SIZE == 4096);
    assert(ARM_SMMU_S2_TABLE_ENTRIES == 512 && ARM_SMMU_S2_LEVEL_BITS == 9 && ARM_SMMU_S2_LEVEL_COUNT == 4);
    assert(ARM_SMMU_S2_LEVEL0_SHIFT == 39 && ARM_SMMU_S2_LEVEL1_SHIFT == 30);
    assert(ARM_SMMU_S2_LEVEL2_SHIFT == 21 && ARM_SMMU_S2_LEVEL3_SHIFT == 12);
    assert(ARM_SMMU_S2_ADDRESS_LIMIT == 0x0000ffffffffffffULL);
    assert(ARM_SMMU_S2_ADDRESS_MASK == 0x0000fffffffff000ULL);
    assert(ARM_SMMU_S2_INVALID == 0 && ARM_SMMU_S2_TYPE_MASK == 3);
    assert(ARM_SMMU_S2_TABLE == 3 && ARM_SMMU_S2_PAGE == 3);
    assert(ARM_SMMU_S2_READ == 0x40 && ARM_SMMU_S2_WRITE == 0x80 && ARM_SMMU_S2_PERMISSION_MASK == 0xc0);
    assert(ARM_SMMU_S2_ACCESS_FLAG == 0x400 && ARM_SMMU_S2_EXECUTE_NEVER == 0x0040000000000000ULL);
    assert(ARM_SMMU_S2_SHAREABILITY_MASK == 0x300 && ARM_SMMU_S2_OUTER_SHAREABLE == 0x200);
    assert(ARM_SMMU_S2_INNER_SHAREABLE == 0x300 && ARM_SMMU_S2_MEMORY_MASK == 0x3c);
    assert(ARM_SMMU_S2_NORMAL_UNCACHED == 0x14 && ARM_SMMU_S2_NORMAL_WRITE_BACK == 0x3c);
    // Decode without production masks. WRITE alone grants no read permission.
    // XN == 2 denies execution even when the two-bit XN extension is present.
    assert(((leaf >> 6) & 3) == 2 && ((leaf >> 53) & 3) == 2);
    assert((leaf & 0x0000fffffffff000ULL) == 0);
    assert(leaf == 0x00400000000007bfULL);
}

int
main(void)
{
    __TestDescriptors();
    __TestBlocks(false);
    __TestBlocks(true);
    __TestFragmentation();
    __TestLimits();
    __TestPhysicalLimit(0, 8191, OS_EOK);
    __TestPhysicalLimit(0, 8190, OS_ENOTSUPPORTED);
    __TestPhysicalLimit(0x2000, 0x1fff, OS_ENOTSUPPORTED);
    __TestPhysicalLimit(0xffffffffffffe000ULL, UINT64_MAX, OS_EOK);
    __TestPhysicalLimit(0xffffffffffffe000ULL, UINT64_MAX - 1, OS_ENOTSUPPORTED);
    __TestPrepareFailures();
    __TestDestroyRetries();
    for (unsigned int point = 0; point < 6; point++) {
        int status;
        pid_t child = fork();

        if (child < 0) {
            perror("fork");
            return EXIT_FAILURE;
        }
        if (child == 0) {
            __TestInvariant(point);
            _exit(0);
        }
        if (waitpid(child, &status, 0) != child) {
            perror("waitpid");
            return EXIT_FAILURE;
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "arena child %u failed with wait status %d\n", point, status);
            // Propagate only an actual abort. The Python runner also requires
            // assertion text from the child and rejects sanitizer diagnostics.
            // Other signals and exit codes must not become parent assertions.
            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT) {
                if (raise(SIGABRT) != 0) {
                    perror("raise");
                }
            }
            return EXIT_FAILURE;
        }
    }
    puts("Arm SMMU memory: blocks, fragmentation, limits, cleanup retries, pinned failures and descriptors passed");
    return 0;
}
