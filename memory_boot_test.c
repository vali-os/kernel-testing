/** Exercise boot metadata sizing and contiguous allocation with the real stack. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define PRIuIN "zu"
#define DEBUG(...) LogAppendMessage(OSSYSLOGLEVEL_DEBUG, __VA_ARGS__)
#include "../kernel/memory/memory.c"

#define PAGE 0x1000

static SystemMachine_t g_machine;
static uintptr_t g_virtualResult;
static int g_virtualFrees;
static size_t g_globalMetadataSize;

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)level;
    (void)format;
}

void
SpinlockConstruct(
    _In_ Spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

uintptr_t
StaticMemoryPoolAllocate(
    _In_ StaticMemoryPool_t* pool,
    _In_ size_t length)
{
    (void)pool;
    (void)length;
    return g_virtualResult;
}

void
StaticMemoryPoolFree(
    _In_ StaticMemoryPool_t* pool,
    _In_ uintptr_t address)
{
    (void)pool;
    assert(address == g_virtualResult);
    g_virtualFrees++;
}

size_t
StaticMemoryPoolCalculateSize(
    _In_ size_t length,
    _In_ size_t chunkSize)
{
    (void)length;
    (void)chunkSize;
    return g_globalMetadataSize;
}

static void
__TestMetadataAndSeeding(void)
{
    struct VBootMemoryEntry entries[] = {
        { VBootMemoryType_Available, 0, 0, 0x1000000, 0 },
        { VBootMemoryType_Reserved, 0x1000000, 0, 0x1000000, 0 },
        { VBootMemoryType_Available, 0x2000000, 0, 3 * PAGE, 0 },
        { VBootMemoryType_Available, 0x3000000, 0, PAGE, 0 }
    };
    struct VBoot boot = { 0 };
    struct MemoryBootContext context = { 0 };
    PlatformMemoryConfiguration_t configuration = { 0 };
    SystemMemoryAllocator_t* allocator = &g_machine.PhysicalMemory.Allocator;
    uintptr_t pages[3];
    size_t total;
    size_t expected;
    void* storage;
    int count;

    memset(&g_machine, 0, sizeof(g_machine));
    g_machine.MemoryGranularity = PAGE;
    boot.Memory.NumberOfEntries = 4;
    boot.Memory.Entries = (uint64_t)(uintptr_t)entries;
    configuration.PageSize = PAGE;
    configuration.MemoryMaskCount = 3;
    configuration.MemoryMasks[0] = 0x7fffff;
    configuration.MemoryMasks[1] = 0x3ffffff;
    configuration.MemoryMasks[2] = UINTPTR_MAX;
    assert(__SizeAllocatorStorage(&context, &boot, &configuration) == OS_EOK);
    expected = MemoryStackCalculateSize(0x800000 / PAGE);
    assert(context.AllocatorStorageSize[0] == expected);
    expected += MemoryStackCalculateSize(3) + MemoryStackCalculateSize(1);
    expected = DIVUP(expected, PAGE) * PAGE;
    assert(context.AllocatorStorageSize[1] == expected);
    assert(context.AllocatorStorageSize[2] == PAGE);
    total = context.AllocatorStorageSize[0] + context.AllocatorStorageSize[1] + PAGE;
    storage = calloc(1, total);
    assert(storage != NULL);
    context.BootMemoryEnabled = 1;
    context.BootMemoryAddress = (uintptr_t)storage;
    assert(__InitializePhysicalMemory(&context, allocator, &configuration) == OS_EOK);
    assert(context.BootMemoryAddress == (uintptr_t)storage + total);
    for (int i = 0; i < 3; i++) {
        assert(context.IdentityMappings[i].Length == context.AllocatorStorageSize[i]);
        assert(allocator->Region[i].Stack.data_size == context.AllocatorStorageSize[i]);
    }

    // Use a physical boot arena and reserved device span within the same map.
    context.BootMemoryStart = 0;
    context.BootMemoryAddress = 3 * PAGE;
    context.DevicePoolBase = 0x600000;
    context.DevicePoolLength = 0x200000;
    assert(__FillPhysicalMemory(&context, &boot, allocator, PAGE) == OS_EOK);
    assert(MemoryStackFreeBlockCount(&allocator->Region[0].Stack) == 1533);
    assert(MemoryStackFreeBlockCount(&allocator->Region[1].Stack) == 2052);
    assert(MemoryStackFreeBlockCount(&allocator->Region[2].Stack) == 0);
    assert(g_machine.NumberOfFreeMemoryBlocks == 3585);
    count = 3;
    assert(MemoryStackPop(&allocator->Region[0].Stack, &count, pages) == OS_EOK);
    for (int i = 0; i < count; i++) {
        assert(pages[i] >= 3 * PAGE && pages[i] < context.DevicePoolBase);
    }

    // Inclusive upper-mask arithmetic must also handle base zero.
    allocator->Region[2].Stack.index = 0;
    allocator->MaskCount = 1;
    allocator->Masks[0] = UINTPTR_MAX;
    allocator->Region[0].Stack.index = 0;
    assert(__FillRange(allocator, 0, PAGE, PAGE) == OS_EOK);
    count = 1;
    assert(MemoryStackPop(&allocator->Region[0].Stack, &count, pages) == OS_EOK);
    assert(pages[0] == 0);
    free(storage);

    entries[0].PhysicalBase = UINT64_MAX - PAGE;
    entries[0].Length = 2 * PAGE;
    assert(__SizeAllocatorStorage(&context, &boot, &configuration) == OS_EINVALPARAMS);
}

static void
__TestIdentityReservation(void)
{
    struct VBootMemoryEntry entries[] = {
        { VBootMemoryType_Available, 0x100000, 0, 8 * PAGE, 0 },
        { VBootMemoryType_Available, 0x200000, 0, 16 * PAGE, 0 }
    };
    struct VBoot boot = { 0 };
    struct MemoryBootContext context = { 0 };
    PlatformMemoryConfiguration_t configuration = { 0 };

    boot.Memory.NumberOfEntries = 2;
    boot.Memory.Entries = (uint64_t)(uintptr_t)entries;
    configuration.PageSize = PAGE;
    configuration.MemoryMaskCount = 1;
    context.AllocatorStorageSize[0] = 6 * PAGE;
    g_globalMetadataSize = 1;
    // Six metadata pages, two mapping pages, and one rounded GA page.
    assert(__InitializeIdentityMemory(&context, &boot, &configuration) == OS_EOK);
    assert(context.BootMemoryStart == 0x200000);
    context.DevicePoolBase = 0x200000;
    context.DevicePoolLength = PAGE;
    assert(__InitializeIdentityMemory(&context, &boot, &configuration) == OS_EOOM);
    context.AllocatorStorageSize[0] = SIZE_MAX;
    assert(__InitializeIdentityMemory(&context, &boot, &configuration) == OS_EOOM);
    context.AllocatorStorageSize[0] = PAGE;
    g_globalMetadataSize = SIZE_MAX;
    assert(__InitializeIdentityMemory(&context, &boot, &configuration) == OS_EOOM);
}

static void
__TestContiguousBootAllocation(void)
{
    SystemMemoryAllocator_t* allocator = &g_machine.PhysicalMemory.Allocator;
    MemoryStack_t* stack = &allocator->Region[0].Stack;
    uintptr_t storage[64];
    PlatformMemoryMapping_t mappings[4] = { 0 };
    paddr_t physical = 0xfeed;
    vaddr_t virtual = 0xfeed;

    memset(&g_machine, 0, sizeof(g_machine));
    g_machine.MemoryGranularity = PAGE;
    allocator->MaskCount = 1;
    assert(MemoryStackConstruct(stack, PAGE, (uintptr_t)storage, sizeof(storage)) == OS_EOK);
    assert(MemoryStackPush(stack, 0x10000, 2) == OS_EOK);
    assert(MemoryStackPush(stack, 0x20000, 2) == OS_EOK);
    g_machine.NumberOfFreeMemoryBlocks = 4;
    assert(MachineAllocateBootMemory(3 * PAGE, NULL, &physical) == OS_EOOM);
    assert(physical == 0xfeed && MemoryStackFreeBlockCount(stack) == 4);
    assert(g_machine.NumberOfFreeMemoryBlocks == 4);
    assert(MachineAllocateBootMemory(0, NULL, &physical) == OS_EINVALPARAMS);
    assert(MachineAllocateBootMemory(SIZE_MAX, NULL, &physical) == OS_EINVALPARAMS);
    assert(MachineAllocateBootMemory(PAGE, NULL, NULL) == OS_EINVALPARAMS);

    // A fitting run must be returned intact on either virtual allocation or
    // mapping registration failure, including accounting and output values.
    assert(MemoryStackPush(stack, 0x30000, 4) == OS_EOK);
    g_machine.NumberOfFreeMemoryBlocks = 8;
    g_virtualResult = 0;
    assert(MachineAllocateBootMemory(3 * PAGE, &virtual, &physical) == OS_EOOM);
    assert(MemoryStackFreeBlockCount(stack) == 8);
    g_virtualResult = 0x800000;
    g_kernelMappings = mappings;
    g_kernelMappingCapacity = 2;
    g_kernelMappingIndex = 1;
    g_virtualFrees = 0;
    assert(MachineAllocateBootMemory(3 * PAGE, &virtual, &physical) == OS_EOOM);
    assert(g_virtualFrees == 1 && g_kernelMappingIndex == 1);
    assert(physical == 0xfeed && virtual == 0xfeed);
    assert(MemoryStackFreeBlockCount(stack) == 8);
    assert(g_machine.NumberOfFreeMemoryBlocks == 8);

    g_kernelMappingCapacity = 4;
    g_kernelMappingIndex = 0;
    assert(MachineAllocateBootMemory(2 * PAGE + 1, &virtual, &physical) == OS_EOK);
    assert(physical == 0x30000 && virtual == g_virtualResult);
    assert(mappings[0].PhysicalBase == physical && mappings[0].VirtualBase == virtual);
    assert(mappings[0].Length == 2 * PAGE + 1);
    assert(g_machine.NumberOfFreeMemoryBlocks == 5 && MemoryStackFreeBlockCount(stack) == 5);
    assert(MachineAllocateBootMemory(2 * PAGE, NULL, &physical) == OS_EOK);
    assert(physical == 0x10000);
    assert(g_machine.NumberOfFreeMemoryBlocks == 3 && g_kernelMappingIndex == 1);
}

int
main(void)
{
    __TestMetadataAndSeeding();
    __TestIdentityReservation();
    __TestContiguousBootAllocation();
    puts("Memory boot: metadata, exclusions, contiguous allocation and rollback passed");
    return 0;
}
