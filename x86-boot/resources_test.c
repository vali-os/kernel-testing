#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/arch/x86/components/memory.c"

static SystemMachine_t machine;
static PageTable_t table __attribute__((aligned(4096)));
static size_t allocated;
static int failAllocation;

SystemMachine_t*
GetMachine(void)
{
    return &machine;
}
oserr_t
CpuHasFeatures(
    _In_ unsigned int ecx,
    _In_ unsigned int edx)
{
    (void)ecx;
    (void)edx;
    return OS_ENOTSUPPORTED;
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
uintptr_t
StaticMemoryPoolAllocate(
    _In_ StaticMemoryPool_t* pool,
    _In_ size_t size)
{
    (void)pool;
    allocated += size;
    return failAllocation ? 0 : 0x10000000;
}
PageTable_t*
MmBootGetPageTable(
    _In_ PAGE_MASTER_LEVEL* directory,
    _In_ uintptr_t address)
{
    (void)directory;
    (void)address;
    return &table;
}

int
main(void)
{
    // A small resource inside a 512 MiB descriptor must consume only its pages,
    // not the full descriptor (the real global window is only 256 MiB).
    struct VBootMemoryEntry entries[] = {
        { .Type = VBootMemoryType_Firmware, .PhysicalBase = 0x40000000, .Length = 0x20000000 },
        { .Type = VBootMemoryType_Firmware, .PhysicalBase = 0x60000000, .Length = 0x1000 },
        { .Type = VBootMemoryType_Reserved, .PhysicalBase = 0x90000000, .Length = 0x100000 }
    };
    struct VBoot boot = { .Memory = {3, sizeof(entries[0]), (uintptr_t)entries} };
    MemorySpace_t space = {0};
    vaddr_t address;
    uintptr_t stack;

    assert(__MapBootResource(&space, &boot, 0x40000123, 0x1000, &address) == OS_EOK);
    assert(address == 0x10000123 && allocated == 0x2000);
    assert((atomic_load(&table.Pages[0]) & PAGE_MASK) == 0x40000000);
    assert((atomic_load(&table.Pages[1]) & PAGE_MASK) == 0x40001000);
    allocated = 0;
    assert(__MapBootResource(&space, &boot, 0x5ffff800, 0x1000, &address) == OS_EOK);
    assert(allocated == 0x2000);
    assert((atomic_load(&table.Pages[1]) & PAGE_MASK) == 0x60000000);
    allocated = 0;
    entries[1].Attributes = VBOOT_MEMORY_NO_MAP;
    assert(__MapBootResource(&space, &boot, 0x5ffff800, 0x1000, &address) != OS_EOK);
    assert(!allocated);
    entries[1].Attributes = 0;
    assert(__MapBootResource(&space, &boot, 0x90000000, 0x1000, &address) != OS_EOK);
    assert(__MapBootResource(&space, &boot, 0x80000000, 0x1000, &address) != OS_EOK);
    assert(__MapBootResource(&space, &boot, UINT64_MAX - 4, 10, &address) != OS_EOK);
    assert(!allocated);
    assert(__MapBootResource(&space, &boot, 0, 0, &address) == OS_EOK && !address);
    failAllocation = 1;
    assert(__MapBootResource(&space, &boot, 0x40000000, 0x1000, &address) == OS_EOOM);
    failAllocation = 0;
    allocated = 0;
    boot.Stack = (struct VBootStack){0x40000000, 0x4000};
    boot.Ramdisk = (struct VBootRamdisk){0x40008005, 0x1000};
    boot.ConfigurationTable = 0x40010004;
    boot.ConfigurationTableCount = 4;
    boot.ConfigurationEntrySize = 24;
    assert(__MapBootResources(&space, &boot, &stack) == OS_EOK);
    assert(stack == 0x10000000 && boot.Stack.Base == stack);
    assert(boot.Ramdisk.Data == 0x10000005 && boot.ConfigurationTable == 0x10000004);
    assert(allocated == 0x7000);
    puts("x86 live boot resource mapping PASS");
}
