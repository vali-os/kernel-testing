#include "../../kernel/memory/space/utils.c"
#ifndef USER_COPY_SOURCE
#define USER_COPY_SOURCE "../../kernel/memory/space/usercopy.c"
#endif
#include USER_COPY_SOURCE
#include "../../kernel/api/priv.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static SystemMachine_t g_machine = { .MemoryGranularity = 4096 };
static struct MSContext g_context;
static MemorySpace_t g_space = { .GroupGlobalHandleID = 1 };
static uint32_t g_freed;
static struct MSAllocation g_allocation = {
    .Length = 8192,
    .Flags = MAPPING_USERSPACE,
    .References = 1,
    .Pages = { .total = 2, .data = &g_freed }
};
static _Alignas(4096) uint8_t g_bytes[8192];
#define USER_BASE ((uintptr_t)g_bytes)
#define USER_END (USER_BASE + sizeof(g_bytes))
#define USER(offset) ((void*)(g_bytes + (offset)))
static unsigned int g_attributes[2];
static int g_failAlloc;
static unsigned int g_copyCalls;
static unsigned int g_attributeCalls;
static unsigned int g_syncCalls;
static int g_addressOnly;
static int g_userMappings;
static int g_locked;
static int g_untracked;
static oserr_t g_attributeStatus;
static uint8_t g_table[] = { 1, 2, 3, 4 };

SystemMachine_t* GetMachine(void) { return &g_machine; }
SystemDomain_t* GetCurrentDomain(void) { return NULL; }
uuid_t ArchGetProcessorCoreId(void) { return 0; }
Thread_t* ThreadCurrentForCore(uuid_t core) { (void)core; return (Thread_t*)1; }
MemorySpace_t* ThreadMemorySpace(Thread_t* thread) { (void)thread; return &g_space; }
void* kmalloc(size_t length) { return g_failAlloc ? NULL : malloc(length); }
void kfree(void* memory) { free(memory); }

void*
LookupGlobalHandle(
    _In_ uuid_t       globalHandleID,
    _In_ HandleType_t type)
{
    CHECK(globalHandleID == g_space.GroupGlobalHandleID && type == HandleTypeMSContext);
    return &g_context;
}

oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    CHECK(capabilities == CAPABILITY_FIRMWARE_ACCESS);
    return OS_EOK;
}

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
    if (g_untracked || address < USER_BASE || address >= USER_END) {
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
    g_attributeCalls++;
    if (g_attributeStatus != OS_EOK) {
        return g_attributeStatus;
    }
    if (g_addressOnly) {
        *attributes = MAPPING_USERSPACE | MAPPING_COMMIT;
        *retrieved = 1;
        return OS_EOK;
    }
    if (address < USER_BASE || address >= USER_END) {
        *retrieved = 0;
        return OS_ENOENT;
    }
    CHECK(g_allocation.References == (g_untracked ? 1 : 2));
    *attributes = g_attributes[(address - USER_BASE) / 4096];
    *retrieved = 1;
    return OS_EOK;
}

void
ArchCopyFromUser(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    CHECK(g_locked && g_allocation.References == (g_untracked ? 1 : 2));
    g_copyCalls++;
    if (!g_addressOnly) {
        memcpy(destination, source, length);
    }
}

void
ArchCopyToUser(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    ArchCopyFromUser(destination, source, length);
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mappingOut)
{
    CHECK(!g_locked && space == &g_space && g_allocation.References == 1);
    CHECK(options->Flags & MAPPING_USERSPACE);
    *mappingOut = USER_BASE + 4096;
    g_userMappings++;
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    CHECK(!g_locked && space == &g_space && address == USER_BASE + 4096);
    CHECK(g_allocation.References == 1 && length == sizeof(g_table));
    g_userMappings--;
    g_attributes[1] = 0;
    return OS_EOK;
}

oserr_t
MSAllocationFindSHM(
    _In_  struct MSContext*     context,
    _In_  vaddr_t               address,
    _In_  size_t                length,
    _In_  bool                  allowFreedPages,
    _Out_ struct MSAllocation** allocationOut)
{
    CHECK(context == &g_context && g_locked && !allowFreedPages);
    CHECK(address == USER_BASE + 4096 && length == sizeof(g_table));
    *allocationOut = NULL;
    return OS_EOK;
}

oserr_t
SHMViewValidateMapping(
    _In_ const struct SHMView* view,
    _In_ size_t          offset,
    _In_ size_t          length,
    _In_ unsigned int    flags)
{
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

oserr_t
ArchMmuUpdatePageAttributes(
        MemorySpace_t* space,
        vaddr_t address,
        int count,
        unsigned int* attributes,
        int* updated)
{
    CHECK(space == &g_space && address == USER_BASE + 4096 && count == 1 && g_locked);
    g_attributes[1] = *attributes;
    *updated = count;
    return OS_EOK;
}

void MSSync(MemorySpace_t* space, uintptr_t address, size_t length)
{
    CHECK(g_locked && space == &g_space && address == USER_BASE + 4096);
    CHECK(length == sizeof(g_table));
    g_syncCalls++;
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

static void
__TestCopies(void)
{
    uint8_t      copy[32] = { 7 };
    unsigned int calls;
    size_t       length;

    CHECK(MemorySpaceCopyToUser(USER(4095), copy, sizeof(copy)) == OS_EOK);
    CHECK(g_bytes[4095] == 7 && g_allocation.References == 1);
    memset(copy, 0, sizeof(copy));
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EOK && copy[0] == 7);
    CHECK(MemorySpaceCopyToUser((void*)UINTPTR_MAX, copy, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyToUser(copy, copy, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(copy, NULL, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyToUser(NULL, copy, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyToUser(USER(0), NULL, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(NULL, USER(0), sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(copy, USER(0), 0) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyToUser(USER(0), copy, 0) == OS_EINVALPARAMS);
    g_attributeStatus = OS_EUNKNOWN;
    CHECK(MemorySpaceCopyToUser(USER(0), copy, sizeof(copy)) == OS_EUNKNOWN);
    g_attributeStatus = OS_ENOENT;
    CHECK(MemorySpaceCopyFromUser(copy, USER(0), sizeof(copy)) == OS_EINVALPARAMS);
    g_attributeStatus = OS_EOK;
    CHECK(MemorySpaceCopyToUser(USER(8191), copy, sizeof(copy)) == OS_EINVALPARAMS);

    // The second page is rejected before the valid first byte is changed.
    calls = g_copyCalls;
    g_bytes[4095] = 0x55;
    g_attributes[1] |= MAPPING_READONLY;
    CHECK(MemorySpaceCopyToUser(USER(4095), copy, sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(g_bytes[4095] == 0x55 && g_copyCalls == calls);
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EOK);
    g_attributes[1] = 0;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_USERSPACE;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_COMMIT;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT | MAPPING_TRAPPAGE;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT | MAPPING_DEVICE;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyToUser(USER(4095), copy, sizeof(copy)) == OS_EINVALPARAMS);
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT | MAPPING_NOCACHE;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EOK);
    CHECK(MemorySpaceCopyToUser(USER(4095), copy, sizeof(copy)) == OS_EOK);
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
    g_freed = 2;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_freed = 0;
    g_allocation.Flags = 0;
    CHECK(MemorySpaceCopyFromUser(copy, USER(0), sizeof(copy)) == OS_EINVALPARAMS);
    g_allocation.Flags = MAPPING_USERSPACE;
    g_allocation.Length = 4096;
    CHECK(MemorySpaceCopyFromUser(copy, USER(4095), sizeof(copy)) == OS_EINVALPARAMS);
    g_allocation.Length = sizeof(g_bytes);

    // Small and unaligned copies must work even when the heap cannot allocate.
    g_failAlloc = 1;
    for (length = 1; length <= sizeof(copy); length++) {
        CHECK(MemorySpaceCopyToUser(USER(4095), copy, length) == OS_EOK);
        CHECK(MemorySpaceCopyFromUser(copy, USER(4095), length) == OS_EOK);
    }
    g_failAlloc = 0;
    g_untracked = 1;
    CHECK(MemorySpaceCopyToUser(USER(2048), copy, sizeof(copy)) == OS_EOK);
    CHECK(MemorySpaceCopyFromUser(copy, USER(2048), sizeof(copy)) == OS_EOK);
    g_untracked = 0;
    CHECK(!g_locked && g_allocation.References == 1 && g_syncCalls == 0);
}

static void
__TestAddressRanges(void)
{
    SystemMemoryMap_t saved = g_machine.MemoryMap;
    uint8_t          byte = 7;
    unsigned int     calls;

    // These checks exercise range arithmetic without dereferencing synthetic addresses.
    g_addressOnly = 1;
    g_untracked = 1;
    g_machine.MemoryMap.UserCode = (SystemMemoryRange_t) { 0x1000, 4096 };
    g_machine.MemoryMap.UserHeap = (SystemMemoryRange_t) { 0x2000, 4096 };
    g_machine.MemoryMap.ThreadLocal = (SystemMemoryRange_t) { UINTPTR_MAX - 4095, 4095 };
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x1fff, 2) == OS_EOK);
    CHECK(MemorySpaceCopyToUser((void*)UINTPTR_MAX, &byte, 1) == OS_EOK);
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)UINTPTR_MAX, 1) == OS_EOK);
    calls = g_attributeCalls;
    CHECK(MemorySpaceCopyToUser((void*)UINTPTR_MAX, &byte, 2) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x3000, 1) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x2fff, 2) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x0000800000001000ULL, 1) == OS_EINVALPARAMS);
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x1000, SIZE_MAX) == OS_EINVALPARAMS);
    CHECK(g_attributeCalls == calls);
    g_machine.MemoryMap.UserHeap.Start = 0x3000;
    CHECK(MemorySpaceCopyFromUser(&byte, (void*)0x1fff, 4098) == OS_EINVALPARAMS);
    g_addressOnly = 0;
    g_untracked = 0;
    g_machine.MemoryMap = saved;
}

#ifdef __OSCONFIG_ACPI_SUPPORT
static struct AcpiPciQuery g_query;
static unsigned int       g_queryCalls;

oserr_t
AcpiPciQuery(
    _In_  const struct AcpiPciQuery* query,
    _Out_ union AcpiPciResult*      result)
{
    CHECK(!g_locked && query != USER(0));
    CHECK(memcmp(query, &g_query, sizeof(g_query)) == 0);
    memset(USER(0), 0xff, sizeof(g_query));
    CHECK(memcmp(query, &g_query, sizeof(g_query)) == 0);
    ((uint8_t*)result)[0] = 0x42;
    g_queryCalls++;
    return OS_EOK;
}
#endif

static void
__TestAcpiQuery(void)
{
    struct AcpiPciQuery query = { 0 };
    union AcpiPciResult result;
    size_t             i;

    memcpy(USER(0), &query, sizeof(query));
    memset(USER(2048), 0xcc, sizeof(result));
    CHECK(ScAcpiQueryPci(&query, USER(2048)) == OS_EINVALPARAMS);
#ifdef __OSCONFIG_ACPI_SUPPORT
    g_query = query;
    CHECK(ScAcpiQueryPci(USER(0), USER(2048)) == OS_EOK);
    CHECK(g_queryCalls == 1);
    memcpy(&result, USER(2048), sizeof(result));
    CHECK(((uint8_t*)&result)[0] == 0x42);
    for (i = 1; i < sizeof(result); i++) {
        CHECK(((uint8_t*)&result)[i] == 0);
    }
    memcpy(USER(0), &query, sizeof(query));
    CHECK(ScAcpiQueryPci(USER(0), &result) == OS_EINVALPARAMS);
#else
    (void)i;
    CHECK(ScAcpiQueryPci(USER(0), USER(2048)) == OS_ENOTSUPPORTED);
#endif
}

int
main(void)
{
    OSFirmwareTableKey_t key = { .Source = OSFIRMWARE_DEVICETREE };
    size_t              length;
    const void*         mapping;

    g_allocation.Address = USER_BASE;
    g_machine.MemoryMap.UserHeap = (SystemMemoryRange_t) { USER_BASE, sizeof(g_bytes) };
    g_attributes[0] = MAPPING_USERSPACE | MAPPING_COMMIT;
    g_attributes[1] = MAPPING_USERSPACE | MAPPING_COMMIT;
    __TestCopies();
    __TestAddressRanges();
    __TestAcpiQuery();
    memcpy(g_bytes + 0x100, &key, sizeof(key));
    CHECK(ScFirmwareQuery(USER(0x200)) == OS_EOK);
    CHECK(ScFirmwareQuery((void*)&key) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableLocate((void*)&key, USER(0x200)) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableLocate(USER(0x100), (void*)&key) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead(USER(0x100), USER(0x300), 2, USER(0x200)) == OS_EBUFFER);
    memcpy(&length, g_bytes + 0x200, sizeof(length));
    CHECK(length == sizeof(g_table));
    CHECK(ScFirmwareTableRead(USER(0x100), &key, 4, USER(0x200)) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead(USER(0x100), USER(0x300), 4, (void*)&key) == OS_EINVALPARAMS);
    CHECK(ScFirmwareTableRead(USER(0x100), USER(0x300), 4, USER(0x200)) == OS_EOK);
    CHECK(memcmp(g_bytes + 0x300, g_table, sizeof(g_table)) == 0);
    CHECK(g_allocation.References == 1);
    CHECK(ScFirmwareTableMap(USER(0x100), (void*)&key, USER(0x250)) == OS_EINVALPARAMS);
    CHECK(g_userMappings == 0);
    CHECK(ScFirmwareTableMap(USER(0x100), USER(0x200), (void*)&key) == OS_EINVALPARAMS);
    CHECK(g_userMappings == 0);
    CHECK(ScFirmwareTableMap(USER(0x100), USER(0x200), USER(0x250)) == OS_EOK);
    memcpy(&mapping, g_bytes + 0x200, sizeof(mapping));
    memcpy(&length, g_bytes + 0x250, sizeof(length));
    CHECK(mapping == USER(0x1000) && length == sizeof(g_table));
    CHECK(memcmp(g_bytes + 4096, g_table, sizeof(g_table)) == 0);
    CHECK(g_attributes[1] & MAPPING_READONLY);
    CHECK(MemorySpaceUnmap(&g_space, USER_BASE + 4096, length) == OS_EOK && g_userMappings == 0);
    CHECK(g_allocation.References == 1);
    puts("Checked user copies and firmware syscall boundaries: passed");
    return 0;
}
