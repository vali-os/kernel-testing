/** Exercise memory ownership using the real memory-stack allocator for both region types. */
#include <component/memory.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
SpinlockConstruct(
    Spinlock_t* lock)
{
    atomic_init(&lock->Current, 0);
    atomic_init(&lock->Next, 0);
}

void
SpinlockAcquireIrq(
    Spinlock_t* lock)
{
    unsigned int ticket = atomic_fetch_add(&lock->Next, 1);
    while (atomic_load(&lock->Current) != ticket) {
    }
}

void
SpinlockReleaseIrq(
    Spinlock_t* lock)
{
    atomic_fetch_add(&lock->Current, 1);
}

size_t
GetMemorySpacePageSize(void)
{
    return 0x1000;
}
static void
__TestOwnership(void)
{
    SystemMemory_t                first;
    SystemMemory_t                second;
    SystemMemory_t                snapshot;
    uintptr_t                      firstStorage[128];
    uintptr_t                      secondStorage[128];
    struct SystemMemoryAllocation allocation = { 0 };
    struct SystemMemoryAllocation other = { 0 };
    struct SystemMemoryAllocation saved;
    uintptr_t                     ordinary = 0;

    SystemMemoryConstruct(&first, 0x1000, 0x10000, 0x1000, 0);
    SystemMemoryConstruct(&second, 0, 0, 0x1000, 0);
    assert(SystemMemoryAllocateDevice(&first, 1, 0, 0, &allocation) == OS_ENOTSUPPORTED);
    snapshot = first;
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x1001, 0x8000, false, firstStorage, sizeof(firstStorage)) == OS_EINVALPARAMS);
    assert(memcmp(&first, &snapshot, sizeof(first)) == 0);
    // Registration must reject malformed or undersized metadata without
    // partially publishing a device region.
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x101000, 0, false, firstStorage,
                                             sizeof(firstStorage)) == OS_EINVALPARAMS);
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x101000, 0x8001, false, firstStorage,
                                             sizeof(firstStorage)) == OS_EINVALPARAMS);
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x101000, 0x8000, false, firstStorage,
                                             MemoryStackCalculateSize(8) - 1) == OS_EINVALPARAMS);
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x101000, 0x8000, false,
                                             (char*)firstStorage + 1,
                                             sizeof(firstStorage) - 1) == OS_EINVALPARAMS);
    assert(SystemMemoryInitializeDeviceRegion(&first, UINTPTR_MAX - 0xfff, 0x2000, false,
                                             firstStorage, sizeof(firstStorage)) == OS_EINVALPARAMS);
    assert(memcmp(&first, &snapshot, sizeof(first)) == 0);
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x101000, 0x8000, false, firstStorage, sizeof(firstStorage)) == OS_EOK);
    // Two owners can manage identical addresses in this fixture. Release must
    // consult the allocation's owner rather than any current/global component.
    assert(SystemMemoryInitializeDeviceRegion(&second, 0x101000, 0x8000, true, secondStorage, sizeof(secondStorage)) == OS_EOK);
    assert(SystemMemoryAllocateDevice(&first, 0x1800, 0x4000, 0x4000, &allocation) == OS_EOK);
    assert(allocation.Owner == &first && allocation.PhysicalBase == 0x104000);
    assert(allocation.Length == 0x2000 && !allocation.Cached);
    assert(SystemMemoryAllocateDevice(&second, 0x1800, 0x4000, 0x4000, &other) == OS_EOK);
    assert(other.Cached && other.PhysicalBase == allocation.PhysicalBase);
    saved = allocation;
    assert(SystemMemoryAllocateDevice(&second, 1, 0, 0, &allocation) == OS_EBUSY);
    assert(memcmp(&allocation, &saved, sizeof(saved)) == 0);
    assert(SystemMemoryInitializeDeviceRegion(&first, 0x200000, 0x8000, true, firstStorage, sizeof(firstStorage)) == OS_EBUSY);
    assert(!first.DeviceMemory.Cached);
    assert(SystemMemoryReleaseAllocation(&allocation) == OS_EOK);
    assert(allocation.Owner == NULL && allocation.PhysicalBase == 0 && allocation.Length == 0 &&
           !allocation.Cached);
    assert(MemoryStackFreeBlockCount(&first.DeviceMemory.Stack) == 8);
    assert(MemoryStackFreeBlockCount(&second.DeviceMemory.Stack) == 6);
    assert(SystemMemoryReleaseAllocation(&allocation) == OS_EOK);
    assert(SystemMemoryReleaseAllocation(&other) == OS_EOK);

    // Invalid and exhausted requests leave output and allocator untouched.
    saved = allocation;
    assert(SystemMemoryAllocateDevice(&first, 0, 0, 0, &allocation) == OS_EINVALPARAMS);
    assert(SystemMemoryAllocateDevice(&first, SIZE_MAX, 0, 0, &allocation) == OS_EINVALPARAMS);
    assert(SystemMemoryAllocateDevice(&first, 1, 3, 0, &allocation) == OS_EINVALPARAMS);
    assert(SystemMemoryAllocateDevice(&first, 0x2000, 0, 0x1000, &allocation) == OS_EINVALPARAMS);
    assert(memcmp(&allocation, &saved, sizeof(saved)) == 0);
    assert(SystemMemoryAllocateDevice(&first, 0x8000, 0, 0, &allocation) == OS_EOK);
    assert(SystemMemoryAllocateDevice(&first, 1, 0, 0, &other) == OS_EOOM);
    assert(other.Owner == NULL);
    assert(SystemMemoryAllocate(&first, 0, 1, &ordinary) == OS_EOOM);
    assert(SystemMemoryReleaseAllocation(&allocation) == OS_EOK);

    // A failed release retains the record; a corrected record can be retried.
    assert(SystemMemoryAllocateDevice(&first, 1, 0, 0, &allocation) == OS_EOK);
    saved = allocation;
    allocation.Length = 0x9000;
    assert(SystemMemoryReleaseAllocation(&allocation) == OS_EINVALPARAMS);
    assert(allocation.Owner == &first);
    allocation = saved;
    assert(SystemMemoryReleaseAllocation(&allocation) == OS_EOK);
    assert(SystemMemoryAllocateDevice(NULL, 1, 0, 0, &allocation) == OS_EINVALPARAMS);
    assert(SystemMemoryAllocateDevice(&first, 1, 0, 0, NULL) == OS_EINVALPARAMS);
    assert(SystemMemoryReleaseAllocation(NULL) == OS_EINVALPARAMS);

    // Physical zero is a valid allocation record, independent of SHM's sentinel.
    SystemMemoryConstruct(&second, 0, 0, 0x1000, 0);
    assert(SystemMemoryInitializeDeviceRegion(&second, 0, 0x1000, true, secondStorage, sizeof(secondStorage)) == OS_EOK);
    assert(SystemMemoryAllocateDevice(&second, 1, 0, 0, &other) == OS_EOK);
    assert(other.Owner == &second && other.PhysicalBase == 0);
    assert(SystemMemoryReleaseAllocation(&other) == OS_EOK);
}

static void
__TestOrdinaryPages(void)
{
    SystemMemory_t memory;
    uintptr_t      storage[3][128];
    uintptr_t      pages[6] = { 0 };
    uintptr_t      freed[3];
    uintptr_t       deviceStorage[128];
    int            index;

    SystemMemoryConstruct(&memory, 0, 0x100000, 0x1000, 0);
    memory.Allocator.MaskCount = 3;
    memory.Allocator.Masks[0] = 0xffff;
    memory.Allocator.Masks[1] = 0xfffff;
    memory.Allocator.Masks[2] = SIZE_MAX;
    for (int i = 0; i < 3; i++) {
        SpinlockConstruct(&memory.Allocator.Region[i].Lock);
        MemoryStackConstruct(&memory.Allocator.Region[i].Stack, 0x1000, (uintptr_t)storage[i],
                             sizeof(storage[i]));
    }
    MemoryStackPush(&memory.Allocator.Region[0].Stack, 0x1000, 2);
    MemoryStackPush(&memory.Allocator.Region[1].Stack, 0x10000, 2);
    MemoryStackPush(&memory.Allocator.Region[2].Stack, 0x100000, 1);
    assert(SystemMemoryInitializeDeviceRegion(&memory, 0x80000, 0x4000, false, deviceStorage, sizeof(deviceStorage)) == OS_EOK);

    // Crossing all three masks must append, not overwrite earlier pages.
    assert(SystemMemoryAllocate(&memory, 0, 5, pages) == OS_EOK);
    assert(pages[0] == 0x100000 && pages[1] == 0x10000 && pages[2] == 0x11000);
    assert(pages[3] == 0x1000 && pages[4] == 0x2000);
    assert(SystemMemoryFree(&memory, 5, pages) == OS_EOK);

    // A request one page too large must put every taken page back.
    memset(pages, 0, sizeof(pages));
    assert(SystemMemoryAllocate(&memory, 0, 6, pages) == OS_EOOM);
    for (int i = 0; i < 6; i++) {
        assert(pages[i] == 0);
    }
    assert(SystemMemoryAllocate(&memory, 0, 5, pages) == OS_EOK);
    assert(SystemMemoryFree(&memory, 5, pages) == OS_EOK);
    assert(SystemMemoryAllocate(&memory, 0xffff, 3, pages) == OS_EOOM);
    assert(SystemMemoryAllocate(&memory, 0xffff, 2, pages) == OS_EOK);
    assert(pages[0] <= 0xffff && pages[1] <= 0xffff && pages[0] != pages[1]);
    assert(SystemMemoryAllocate(&memory, 0xfff, 1, pages + 2) == OS_EOOM);

    // Mixed ownership or malformed frees are rejected before changing a stack.
    index = memory.Allocator.Region[0].Stack.index;
    freed[0] = pages[0];
    freed[1] = 0;
    freed[2] = 0x80000;
    assert(SystemMemoryFree(&memory, 3, freed) == OS_EINVALPARAMS);
    assert(memory.Allocator.Region[0].Stack.index == index);
    freed[2] = pages[1] + 1;
    assert(SystemMemoryFree(&memory, 3, freed) == OS_EINVALPARAMS);
    assert(memory.Allocator.Region[0].Stack.index == index);
    freed[2] = pages[1];
    assert(SystemMemoryFree(&memory, 3, freed) == OS_EOK);
    assert(SystemMemoryAllocate(&memory, 0xffff, 2, pages) == OS_EOK);
    assert(SystemMemoryFree(&memory, 2, pages) == OS_EOK);
    assert(SystemMemoryAllocate(&memory, 0, 0, pages) == OS_EINVALPARAMS);
    assert(SystemMemoryFree(&memory, -1, pages) == OS_EINVALPARAMS);
    assert(SystemMemoryContainsAddress(&memory, 0xfffff) == OS_EOK);
    assert(SystemMemoryContainsAddress(&memory, 0x100000) != OS_EOK);
}

static SystemMemory_t g_concurrent;
static atomic_uint    g_owned[64];

static void*
__AllocateConcurrently(
    void* argument)
{
    struct SystemMemoryAllocation allocation = { 0 };
    size_t                        count = (size_t)argument;
    size_t                        first;

    for (int iteration = 0; iteration < 2000; iteration++) {
        assert(SystemMemoryAllocateDevice(&g_concurrent, count * 0x1000, 0x4000, 0x10000, &allocation) ==
               OS_EOK);
        first = (allocation.PhysicalBase - 0x100000) / 0x1000;
        assert(first + count <= 64);
        for (size_t i = first; i < first + count; i++) {
            assert(atomic_fetch_add(&g_owned[i], 1) == 0);
        }
        for (size_t i = first; i < first + count; i++) {
            assert(atomic_fetch_sub(&g_owned[i], 1) == 1);
        }
        assert(SystemMemoryReleaseAllocation(&allocation) == OS_EOK);
    }
    return NULL;
}

int
main(void)
{
    uintptr_t  deviceStorage[128];
    pthread_t threads[4];

    __TestOwnership();
    __TestOrdinaryPages();
    SystemMemoryConstruct(&g_concurrent, 0, 0, 0x1000, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_concurrent, 0x100000, 0x40000, false, deviceStorage, sizeof(deviceStorage)) == OS_EOK);
    for (size_t i = 0; i < 4; i++) {
        assert(pthread_create(&threads[i], NULL, __AllocateConcurrently, (void*)(i + 1)) == 0);
    }
    for (int i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    assert(MemoryStackFreeBlockCount(&g_concurrent.DeviceMemory.Stack) == 64);
    puts("System memory: ownership, release, constraints, mask fallback, rollback and 8000 "
         "concurrent allocations passed");
    return 0;
}
