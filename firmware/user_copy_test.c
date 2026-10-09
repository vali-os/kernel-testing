#include "../../kernel/memory/ms_utils.c"
#include "../../kernel/api/priv.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static SystemMachine_t g_machine = { .MemoryGranularity = 4096 };
static struct MSContext g_context;
static MemorySpace_t g_space = { .Context = &g_context };
static uint32_t g_freed;
static struct MSAllocation g_allocation = {
    .Address = 0x1000,
    .Length = 8192,
    .Flags = MAPPING_USERSPACE,
    .References = 1,
    .Pages = { .total = 2, .data = &g_freed }
};
static uint8_t g_bytes[8192];
static unsigned int g_attributes[2];
static int g_failAlloc;
static int g_failMap;
static int g_mappings;
static int g_userMappings;
static int g_locked;
static int g_untracked;
static uint8_t g_table[] = { 1, 2, 3, 4 };

SystemMachine_t* GetMachine(void) { return &g_machine; }
SystemDomain_t* GetCurrentDomain(void) { return NULL; }
uuid_t ArchGetProcessorCoreId(void) { return 0; }
Thread_t* ThreadCurrentForCore(uuid_t core) { (void)core; return (Thread_t*)1; }
MemorySpace_t* ThreadMemorySpace(Thread_t* thread) { (void)thread; return &g_space; }
void* kmalloc(size_t length) { return g_failAlloc ? NULL : malloc(length); }
void kfree(void* memory) { free(memory); }

void MutexLock(Mutex_t* mutex)
{
    CHECK(mutex == &g_context.UserCopyLock && !g_locked);
    g_locked = 1;
}

void MutexUnlock(Mutex_t* mutex)
{
    CHECK(mutex == &g_context.UserCopyLock && g_locked);
    g_locked = 0;
}

struct MSAllocation*
MSAllocationAcquire(
        struct MSContext* context,
        vaddr_t address)
{
    CHECK(context == &g_context);
    CHECK(g_locked);
    if (g_untracked || address < 0x1000 || address >= 0x3000) {
        return NULL;
    }
    g_allocation.References++;
    return &g_allocation;
}

oserr_t
MSAllocationRelease(
        struct MSContext* context,
        struct MSAllocation* allocation)
{
    CHECK(context == &g_context && allocation == &g_allocation);
    CHECK(allocation->References == 2);
    allocation->References--;
    return OS_EOK;
}

oserr_t
ArchMmuGetPageAttributes(
        MemorySpace_t* space,
        vaddr_t address,
        int count,
        unsigned int* attributes,
        int* retrieved)
{
    CHECK(space == &g_space && count == 1 && g_locked);
    if (address < 0x1000 || address >= 0x3000) {
        *retrieved = 0;
        return OS_ENOENT;
    }
    CHECK(g_allocation.References == (g_untracked ? 1 : 2));
    *attributes = g_attributes[(address - 0x1000) / 4096];
    *retrieved = 1;
    return OS_EOK;
}

oserr_t
ArchMmuVirtualToPhysical(
        MemorySpace_t* space,
        vaddr_t address,
        int count,
        paddr_t* pages,
        int* retrieved)
{
    int index;

    CHECK(space == &g_space && g_locked);
    CHECK(g_allocation.References == (g_untracked ? 1 : 2));
    for (index = 0; index < count; index++) {
        pages[index] = address + (size_t)index * 4096;
    }
    *retrieved = count;
    return OS_EOK;
}

oserr_t
MemorySpaceMap(
        MemorySpace_t* space,
        struct MemorySpaceMapOptions* options,
        vaddr_t* mappingOut)
{
    if (options->Flags & MAPPING_USERSPACE) {
        CHECK(space == &g_space && g_allocation.References == 1);
        *mappingOut = 0x2000;
        g_userMappings++;
        g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
        return OS_EOK;
    }
    CHECK(space == &g_machine.SystemSpace && g_locked);
    CHECK(g_allocation.References == (g_untracked ? 1 : 2));
    CHECK(options->Flags == (MAPPING_COMMIT | MAPPING_PERSISTENT));
    CHECK(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_GLOBAL));
    if (g_failMap) {
        return OS_EOOM;
    }
    *mappingOut = (uintptr_t)(g_bytes + options->Pages[0] - 0x1000);
    g_mappings++;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
        MemorySpace_t* space,
        vaddr_t address,
        size_t length)
{
    (void)address;
    if (space == &g_space) {
        CHECK(g_allocation.References == 1 && length == sizeof(g_table));
        g_userMappings--;
        g_attributes[1] = 0;
        return OS_EOK;
    }
    CHECK(space == &g_machine.SystemSpace && length != 0 && g_locked);
    CHECK(g_allocation.References == (g_untracked ? 1 : 2));
    g_mappings--;
    return OS_EOK;
}

oserr_t
ArchMmuUpdatePageAttributes(
        MemorySpace_t* space,
        vaddr_t address,
        int count,
        unsigned int* attributes,
        int* updated)
{
    CHECK(space == &g_space && address == 0x2000 && count == 1);
    g_attributes[1] = *attributes;
    *updated = count;
    return OS_EOK;
}

void MSSync(MemorySpace_t* space, uintptr_t address, size_t length)
{
    (void)space; (void)address; (void)length;
}

oserr_t
FirmwareQuery(
        OSFirmwareInfo_t* info)
{
    memset(info, 0, sizeof(*info));
    info->Primary = OSFIRMWARE_DEVICETREE;
    return OS_EOK;
}

oserr_t
FirmwareLocate(
        const OSFirmwareTableKey_t* key,
        const void** data,
        OSFirmwareTable_t* table)
{
    CHECK(key->Source == OSFIRMWARE_DEVICETREE);
    memset(table, 0, sizeof(*table));
    table->Length = sizeof(g_table);
    *data = g_table;
    return OS_EOK;
}

int
main(void)
{
    uint8_t copy[32] = { 7 };
    OSFirmwareTableKey_t key = { .Source = OSFIRMWARE_DEVICETREE };
    size_t length;
    const void* mapping;

    g_attributes[0] = MAPPING_USERSPACE | MAPPING_COMMIT;
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), true) == OS_EOK);
    CHECK(g_bytes[4095] == 7 && g_mappings == 0 && g_allocation.References == 1);
    memset(copy, 0, sizeof(copy));
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), false) == OS_EOK && copy[0] == 7);
    CHECK(MemorySpaceCopyUser((void*)UINTPTR_MAX, copy, sizeof(copy), true) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyUser(g_bytes, copy, sizeof(copy), true) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyUser((void*)0x2fff, copy, sizeof(copy), true) == OS_EINVALPARAMS);
    g_attributes[1] |= MAPPING_READONLY;
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), true) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), false) == OS_EOK);
    g_attributes[1] = 0;
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), false) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
    g_freed = 2;
    CHECK(MemorySpaceCopyUser((void*)0x1fff, copy, sizeof(copy), false) == OS_EINVALPARAMS);
    g_freed = 0;
    g_allocation.Flags = 0;
    CHECK(MemorySpaceCopyUser((void*)0x1000, copy, sizeof(copy), false) == OS_EINVALPARAMS);
    g_allocation.Flags = MAPPING_USERSPACE;
    g_failAlloc = 1;
    CHECK(MemorySpaceCopyUser((void*)0x1000, copy, sizeof(copy), true) == OS_EOOM);
    g_failAlloc = 0;
    g_failMap = 1;
    CHECK(MemorySpaceCopyUser((void*)0x1000, copy, sizeof(copy), true) == OS_EOOM);
    g_failMap = 0;
    CHECK(g_mappings == 0 && g_allocation.References == 1);
    g_untracked = 1;
    CHECK(MemorySpaceCopyUser((void*)0x1800, copy, sizeof(copy), true) == OS_EOK);
    CHECK(MemorySpaceCopyUser((void*)0x1800, copy, sizeof(copy), false) == OS_EOK);
    CHECK(!g_locked && g_allocation.References == 1);
    g_untracked = 0;
    memcpy(g_bytes + 0x100, &key, sizeof(key));
    CHECK(ScFirmwareQuery((void*)0x1200) == OS_EOK);
    CHECK(ScFirmwareQuery((void*)g_bytes) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableLocate((void*)g_bytes, (void*)0x1200) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableLocate((void*)0x1100, (void*)g_bytes) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead((void*)0x1100, (void*)0x1300, 2, (void*)0x1200) == OS_EBUFFER);
    memcpy(&length, g_bytes + 0x200, sizeof(length));
    CHECK(length == sizeof(g_table));
    CHECK(ScFirmwareTableRead((void*)0x1100, g_bytes, 4, (void*)0x1200) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead((void*)0x1100, (void*)0x1300, 4, (void*)g_bytes) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead((void*)0x1100, (void*)0x1300, 4, (void*)0x1200) == OS_EOK);
    CHECK(memcmp(g_bytes + 0x300, g_table, sizeof(g_table)) == 0);
    CHECK(g_mappings == 0 && g_allocation.References == 1);
    CHECK(ScFirmwareTableMap((void*)0x1100, (void*)g_bytes, (void*)0x1250) == OS_EINVALPARAMS);
    CHECK(g_userMappings == 0);
    CHECK(ScFirmwareTableMap((void*)0x1100, (void*)0x1200, (void*)g_bytes) == OS_EINVALPARAMS);
    CHECK(g_userMappings == 0);
    CHECK(ScFirmwareTableMap((void*)0x1100, (void*)0x1200, (void*)0x1250) == OS_EOK);
    memcpy(&mapping, g_bytes + 0x200, sizeof(mapping));
    memcpy(&length, g_bytes + 0x250, sizeof(length));
    CHECK(mapping == (void*)0x2000 && length == sizeof(g_table));
    CHECK(memcmp(g_bytes + 4096, g_table, sizeof(g_table)) == 0);
    CHECK(g_attributes[1] & MAPPING_READONLY);
    CHECK(MemorySpaceUnmap(&g_space, 0x2000, length) == OS_EOK && g_userMappings == 0);
    CHECK(g_mappings == 0 && g_allocation.References == 1);
    puts("Firmware checked user copies: passed");
    return 0;
}