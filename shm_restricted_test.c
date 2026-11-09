// Real grants, restricted SHM and VM operations with a deterministic page table.
#define main __GrantBaseline
#define MemorySpaceCopyUser __GrantCopyUser
#include "handle_grants_test.c"
#undef MemorySpaceCopyUser
#undef main

oserr_t MemorySpaceCopyUser(void*, void*, size_t, bool);
#define MemorySpaceCopyUser __ProductionCopyUser
#undef __MODULE
#include "../kernel/memory/space/map.c"
#include "../kernel/memory/space/mapping.c"
#undef MemorySpaceCopyUser
#include "../kernel/memory/space/allocations.c"
#include "../kernel/memory/space/unmap.c"
#define GetCurrentMemorySpace __ProductionGetCurrentMemorySpace
#include "../kernel/memory/space/utils.c"
#undef GetCurrentMemorySpace
#define MemorySpaceGetSecurityContext __ProductionSecurityContext
#include "../kernel/memory/space/context.c"
#undef MemorySpaceGetSecurityContext
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#include "../kernel/memory/shm/shm.c"
#undef __MODULE
#define THREAD_GET(id) ((Thread_t*)LookupHandle(id, HandleTypeThread))
#include "../kernel/api/memory.c"

#define PAGE_SIZE 4096
#define VM_BASE 0x10000000

/** Simulated page-table entry; clean-page tests use host memory for physical pages. */
struct __TestPage {
    vaddr_t      Address;
    paddr_t      Physical;
    unsigned int Flags;
};
static struct __TestPage g_pages[128];
static MemorySpace_t    g_space;
static SystemMachine_t  g_machine;
static vaddr_t          g_nextAddress = VM_BASE;
static unsigned int    g_backingDeletes;
static unsigned int    g_pageChanges;
static bool            g_failMap;
static bool            g_failUnmap;
static vaddr_t         g_failUnmapAddress;
static bool            g_failCopyIn;
static size_t          g_copyOutLimit = SIZE_MAX;
static Mutex_t*        g_heldLocks[16];
static size_t          g_lockCount;
static bool            g_testCleanPages;
static unsigned int    g_cleanPageCount;
static unsigned int    g_cleanPageFrees;
static _Alignas(PAGE_SIZE) unsigned char g_cleanPages[4][PAGE_SIZE];

static struct __TestPage*
__Page(
    _In_ vaddr_t address)
{
    size_t i;

    address &= ~(PAGE_SIZE - 1);
    for (i = 0; i < 128; i++) {
        if (g_pages[i].Address == address) {
            return &g_pages[i];
        }
    }
    for (i = 0; i < 128; i++) {
        if (g_pages[i].Address == 0) {
            g_pages[i].Address = address;
            return &g_pages[i];
        }
    }
    abort();
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

void
MutexConstruct(
    _In_ Mutex_t* mutex,
    _In_ unsigned int flags)
{
    memset(mutex, 0, sizeof(*mutex));
}

void
MutexDestruct(
    _In_ Mutex_t* mutex)
{
}

void
MutexLock(
    _In_ Mutex_t* mutex)
{
    size_t i;

    for (i = 0; i < g_lockCount; i++) {
        assert(g_heldLocks[i] != mutex);
    }
    assert(g_lockCount < 16);
    g_heldLocks[g_lockCount++] = mutex;
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    assert(g_lockCount != 0 && g_heldLocks[g_lockCount - 1] == mutex);
    g_lockCount--;
}

uintptr_t
DynamicMemoryPoolAllocate(
    _In_ DynamicMemoryPool_t* pool,
    _In_ size_t size)
{
    vaddr_t result = g_nextAddress;

    g_nextAddress += DIVUP(size, PAGE_SIZE) * PAGE_SIZE + PAGE_SIZE;
    return result;
}

void
DynamicMemoryPoolFree(
    _In_ DynamicMemoryPool_t* pool,
    _In_ uintptr_t address)
{
}

int
DynamicMemoryPoolContains(
    _In_ DynamicMemoryPool_t* pool,
    _In_ uintptr_t address)
{
    return address >= VM_BASE;
}

void
DynamicMemoryPoolDestroy(
    _In_ DynamicMemoryPool_t* pool)
{
}

uintptr_t
StaticMemoryPoolAllocate(
    _In_ StaticMemoryPool_t* pool,
    _In_ size_t size)
{
    if (g_testCleanPages) {
        assert(size == PAGE_SIZE && g_cleanPageCount != 0);
        return (vaddr_t)g_cleanPages[g_cleanPageCount - 1];
    }
    return 0;
}

void
StaticMemoryPoolFree(
    _In_ StaticMemoryPool_t* pool,
    _In_ uintptr_t address)
{
}

int
StaticMemoryPoolContains(
    _In_ StaticMemoryPool_t* pool,
    _In_ uintptr_t address)
{
    return 0;
}

oserr_t
AllocatePhysicalMemory(
    _In_ size_t mask,
    _In_ int count,
    _Out_ uintptr_t* pages)
{
    int i;

    for (i = 0; i < count; i++) {
        if (g_testCleanPages) {
            assert(g_cleanPageCount < 3);
            pages[i] = (paddr_t)g_cleanPages[g_cleanPageCount++];
            memset((void*)pages[i], 0xa5, PAGE_SIZE);
        } else {
            pages[i] = 0xA00000 + i * PAGE_SIZE;
        }
    }
    return OS_EOK;
}

void
FreePhysicalMemory(
    _In_ int count,
    _In_ const uintptr_t* pages)
{
    if (g_testCleanPages) {
        g_cleanPageFrees += count;
        return;
    }
    // Restricted mappings must never free their backing through the page table.
    assert(count == 0);
}

void
MSSync(
    _In_ MemorySpace_t* space,
    _In_ uintptr_t address,
    _In_ size_t size)
{
}

oserr_t
ArchMmuSetVirtualPages(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ const paddr_t* pages,
    _In_ int count,
    _In_ unsigned int flags,
    _Out_ int* updated)
{
    int i;
    struct __TestPage* page;

    *updated = 0;
    for (i = 0; i < count; i++) {
        page = __Page(address + i * PAGE_SIZE);
        page->Physical = pages[i];
        page->Flags = flags;
        (*updated)++;
        g_pageChanges++;
        if (g_failMap) {
            return OS_EOOM;
        }
    }
    return OS_EOK;
}

oserr_t
ArchMmuReserveVirtualPages(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ int count,
    _In_ unsigned int flags,
    _Out_ int* updated)
{
    *updated = 0;
    return OS_ENOTSUPPORTED;
}

oserr_t
ArchMmuSetContiguousVirtualPages(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ paddr_t physical,
    _In_ int count,
    _In_ unsigned int flags,
    _Out_ int* updated)
{
    *updated = 0;
    return OS_ENOTSUPPORTED;
}

oserr_t
ArchMmuClearVirtualPages(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ int count,
    _Out_ paddr_t* physical,
    _Out_ int* freed,
    _Out_ int* cleared)
{
    int i;
    struct __TestPage* page;

    *cleared = 0;
    *freed = 0;
    if (address == g_failUnmapAddress) {
        return OS_EDEVFAULT;
    }
    if (g_failUnmap) {
        return OS_EBUSY;
    }
    for (i = 0; i < count; i++) {
        page = __Page(address + i * PAGE_SIZE);
        if (page->Flags == 0) {
            return OS_ENOENT;
        }
        if (page->Physical != 0) {
            assert(page->Flags & MAPPING_PERSISTENT);
        }
        page->Physical = 0;
        page->Flags = 0;
        (*cleared)++;
    }
    return OS_EOK;
}

oserr_t
ArchMmuVirtualToPhysical(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ int count,
    _Out_ paddr_t* physical,
    _Out_ int* retrieved)
{
    int i;

    for (i = 0; i < count; i++) {
        physical[i] = __Page(address + i * PAGE_SIZE)->Physical;
    }
    *retrieved = count;
    return OS_EOK;
}

oserr_t
ArchMmuGetPageAttributes(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ int count,
    _Out_ unsigned int* attributes,
    _Out_ int* retrieved)
{
    int i;

    for (i = 0; i < count; i++) {
        attributes[i] = __Page(address + i * PAGE_SIZE)->Flags;
    }
    *retrieved = count;
    return OS_EOK;
}

oserr_t
ArchMmuUpdatePageAttributes(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ int count,
    _InOut_ unsigned int* attributes,
    _Out_ int* updated)
{
    unsigned int old = __Page(address)->Flags;
    int i;

    for (i = 0; i < count; i++) {
        __Page(address + i * PAGE_SIZE)->Flags = *attributes;
        g_pageChanges++;
    }
    *attributes = old;
    *updated = count;
    return OS_EOK;
}

oserr_t
ArchMmuCommitVirtualPage(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ const paddr_t* physical,
    _In_ int count,
    _Out_ int* committed)
{
    int i;

    for (i = 0; i < count; i++) {
        __Page(address + i * PAGE_SIZE)->Physical = physical[i];
        __Page(address + i * PAGE_SIZE)->Flags |= MAPPING_COMMIT;
    }
    *committed = count;
    return OS_EOK;
}

oserr_t
MemorySpaceCopyUser(
    _In_ void* user,
    _In_ void* kernel,
    _In_ size_t length,
    _In_ bool toUser)
{
    if (user == NULL || (!toUser && g_failCopyIn)) {
        return OS_EINVALPARAMS;
    }
    if (toUser && g_copyOutLimit < length) {
        memcpy(user, kernel, g_copyOutLimit);
        return OS_EINVALPARAMS;
    }
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}

oserr_t
SystemMemoryReleaseAllocation(
    _In_ struct SystemMemoryAllocation* allocation)
{
    abort();
}

static void
__DeleteBacking(
    _In_ void* resource)
{
    g_backingDeletes++;
    kfree(resource);
}

static void
__TestSparseMapping(
    _In_ struct SHMBuffer* backing,
    _In_ MSContextSecurity_t* sender,
    _In_ MSContextSecurity_t* receiver)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
        .Data.SHM.Offset = 2 * PAGE_SIZE,
        .Data.SHM.Length = 2 * PAGE_SIZE
    };
    GrantToken_t token;
    SHMHandle_t view;
    vaddr_t address;
    unsigned int previous;

    backing->Pages[2] = 0;
    g_currentContext = sender;
    assert(GrantTokenCreate(backing->ID, &options, &token) == OS_EOK);
    g_currentContext = receiver;
    assert(SHMAttachToken(&token, &view) == OS_EOK);
    assert(SHMMap(&view, 0, 2 * PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
    address = (vaddr_t)view.Buffer;
    assert(!(__Page(address)->Flags & MAPPING_COMMIT));
    assert(__Page(address + PAGE_SIZE)->Flags & MAPPING_COMMIT);
    assert(MemorySpaceChangeProtection(&g_space, address, 2 * PAGE_SIZE,
                                      MAPPING_USERSPACE, &previous) == OS_EOK);
    assert(!(__Page(address)->Flags & MAPPING_COMMIT));
    assert(__Page(address + PAGE_SIZE)->Flags & MAPPING_COMMIT);
    assert(!(__Page(address + PAGE_SIZE)->Flags & MAPPING_READONLY));
    assert(DestroyHandle(view.ID) == OS_EOK);
    g_currentContext = sender;
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    g_currentContext = receiver;
    __DrainJanitor();
    assert(SHMCommitFault(backing->ID, address, address + 100) == OS_EOK);
    assert(__Page(address)->Physical == backing->Pages[2] && backing->Pages[2] != 0);
    assert(__Page(address)->Flags & MAPPING_COMMIT);

    // A full cleanup must work even after a subrange was unmapped.
    assert(MemorySpaceUnmap(&g_space, address, PAGE_SIZE) == OS_EINCOMPLETE);
    assert(MemorySpaceChangeProtection(&g_space, address, PAGE_SIZE,
                                      MAPPING_READONLY, &previous) == OS_EINVALPARAMS);
    assert(MemorySpaceUnmap(&g_space, address, 2 * PAGE_SIZE) == OS_EOK);
    __DrainJanitor();

    // Write-only tokens are valid attachments but cannot expose CPU pages.
    options.Scope = GrantTokenScopeWrite;
    g_currentContext = sender;
    assert(GrantTokenCreate(backing->ID, &options, &token) == OS_EOK);
    g_currentContext = receiver;
    assert(SHMAttachToken(&token, &view) == OS_EOK);
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EPERMISSIONS);
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_WRITE) == OS_EPERMISSIONS);
    assert(SHMDetach(&view) == OS_EOK);
    g_currentContext = sender;
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    g_currentContext = receiver;
    __DrainJanitor();
}

#include "shm_clean_test.inc"
#include "shm_remap_cleanup_test.inc"

static void
__TestRestricted(
    _In_ bool exitWithMapping)
{
    struct MSContext* context;
    struct SHMBuffer* backing;
    MSContextSecurity_t receiver = { .GlobalHandleID = 999 };
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .Scope = GrantTokenScopeRead,
        .Data.SHM.Offset = PAGE_SIZE,
        .Data.SHM.Length = 2 * PAGE_SIZE
    };
    GrantToken_t token;
    GrantToken_t derived;
    SHMHandle_t view;
    SHMHandle_t second;
    SHMHandle_t alias;
    SHMHandle_t original;
    SHMHandle_t sentinel;
    struct SHMReference resolved;
    vaddr_t clone = 0;
    unsigned int attributes;
    unsigned int changes;
    unsigned int count;
    paddr_t wrongPage = 0xdead000;
    size_t i;
    uuid_t backingID;
    uuid_t viewID;
    int sgCount;
    SHMSG_t segments[3];
    int failure;

    g_backingDeletes = 0;
    memset(g_pages, 0, sizeof(g_pages));
    assert(InitializeHandles() == OS_EOK);
    context = kmalloc(sizeof(*context));
    memset(context, 0, sizeof(*context));
    list_construct(&context->Allocations);
    context->Security.HandleTable = CreateHandleGrantsTable();
    context->Security.GlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, __MSContextDelete, context);
    g_space.GroupGlobalHandleID = context->Security.GlobalHandleID;
    g_space.Flags = MEMORY_SPACE_APPLICATION;
    g_currentContext = &context->Security;
    receiver.HandleTable = CreateHandleGrantsTable();

    backing = kmalloc(sizeof(*backing) + 4 * sizeof(paddr_t));
    memset(backing, 0, sizeof(*backing) + 4 * sizeof(paddr_t));
    backing->Length = 4 * PAGE_SIZE;
    backing->PageCount = 4;
    backing->Owner = &g_space;
    for (i = 0; i < 4; i++) {
        backing->Pages[i] = 0x900000 + i * PAGE_SIZE;
    }
    backingID = CreateHandle(HandleTypeSHM, NULL, __DeleteBacking, backing);
    backing->ID = backingID;
    assert(GrantTokenCreate(backingID, &options, &token) == OS_EOK);

    g_currentContext = &receiver;
    assert(SHMAttach(backingID, &view) == OS_ENOENT);
    assert(SHMAttachToken(&token, &view) == OS_EOK);
    assert(view.ID != backingID && view.Buffer == NULL && view.Capacity == 2 * PAGE_SIZE);
    assert(SHMAttachToken(&token, &second) == OS_EOK && second.ID != view.ID);
    assert(SHMResolve(view.ID, &resolved) == OS_EOK);
    assert(resolved.Buffer == backing && resolved.Offset == PAGE_SIZE);
    assert(resolved.Scope == GrantTokenScopeRead);
    ReleaseGlobalHandle(resolved.GlobalHandleID);
    assert(SHMAttach(view.ID, &alias) == OS_EOK);
    assert(alias.ID == view.ID && alias.Capacity == view.Capacity);
    assert(SHMDetach(&alias) == OS_EOK);
    assert(SHMBuildSG(view.ID, &sgCount, NULL) == OS_EOK && sgCount == 1);
    assert(SHMBuildSG(view.ID, &sgCount, segments) == OS_EOK);
    assert(segments[0].Address == 0x901000 && segments[0].Length == 2 * PAGE_SIZE);
    // Clip partial boundary pages and split discontiguous physical pages.
    assert(SHMResolve(view.ID, &resolved) == OS_EOK);
    resolved.View->Offset += 17;
    resolved.View->Length -= 30;
    backing->Pages[2] = 0xa00000;
    assert(SHMBuildSG(view.ID, &sgCount, NULL) == OS_EOK && sgCount == 2);
    assert(SHMBuildSG(view.ID, &sgCount, segments) == OS_EOK);
    assert(segments[0].Address == 0x901011 && segments[0].Length == PAGE_SIZE - 17);
    assert(segments[1].Address == 0xa00000 && segments[1].Length == PAGE_SIZE - 13);
    segments[1].Address = 0xdead;
    sgCount = 1;
    assert(SHMBuildSG(view.ID, &sgCount, segments) == OS_EOK && segments[1].Address == 0xdead);
    backing->Pages[1] = 0;
    assert(SHMBuildSG(view.ID, &sgCount, segments) == OS_EOK && segments[0].Address == 0);
    backing->Pages[1] = 0x901000;
    backing->Pages[2] = 0x902000;
    resolved.View->Offset -= 17;
    resolved.View->Length += 30;
    ReleaseGlobalHandle(resolved.GlobalHandleID);

    memset(&sentinel, 0xa5, sizeof(sentinel));
    count = __LookupGlobalSafe(backingID)->References;
    for (failure = 0; failure < 2; failure++) {
        alias = sentinel;
        g_failAllocationAfter = failure;
        assert(SHMAttachToken(&token, &alias) == OS_EOOM);
        g_failAllocationAfter = -1;
        __DrainJanitor();
        assert(memcmp(&alias, &sentinel, sizeof(alias)) == 0);
        assert(__LookupGlobalSafe(backingID)->References == count);
    }
    count = g_handles.element_count;
    for (i = 0; i < sizeof(alias); i++) {
        g_copyOutLimit = i;
        assert(ScSHMAttachToken(&token, &alias) == OS_EINVALPARAMS);
        __DrainJanitor();
        assert(g_handles.element_count == count);
    }
    g_copyOutLimit = SIZE_MAX;
    g_failCopyIn = true;
    assert(ScSHMAttachToken(&token, &alias) == OS_EINVALPARAMS);
    g_failCopyIn = false;

    __TestCleanMapping(backing);
    __TestSparseMapping(backing, &context->Security, &receiver);
    __TestMapCleanup(view.ID, second.ID);
    __TestRemapCleanup(view.ID, second.ID);
    for (i = 0; i < sizeof(alias); i++) {
        alias = view;
        g_copyOutLimit = i;
        assert(ScSHMMap(&alias, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EINVALPARAMS);
        __DrainJanitor();
        assert(context->Allocations.head == NULL);
    }
    g_copyOutLimit = SIZE_MAX;

    original = view;
    changes = g_pageChanges;
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ | SHM_ACCESS_WRITE) == OS_EPERMISSIONS);
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_EXECUTE) == OS_EPERMISSIONS);
    assert(SHMMap(&view, 2 * PAGE_SIZE, 1, SHM_ACCESS_READ) == OS_EINVALPARAMS);
    assert(memcmp(&view, &original, sizeof(view)) == 0 && changes == g_pageChanges);
    // Allocation-record failures must release the reference added for mapping.
    count = __LookupGlobalSafe(view.ID)->References;
    for (failure = 0; failure < 2; failure++) {
        g_failAllocationAfter = failure;
        assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOOM);
        g_failAllocationAfter = -1;
        assert(__LookupGlobalSafe(view.ID)->References == count);
        assert(memcmp(&view, &original, sizeof(view)) == 0);
        assert(context->Allocations.head == NULL);
    }
    g_failMap = true;
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOOM);
    g_failMap = false;
    assert(context->Allocations.head == NULL);
    assert(SHMMap(&view, 100, 500, SHM_ACCESS_READ) == OS_EOK);
    assert((vaddr_t)view.Buffer % PAGE_SIZE == 100);
    assert(__Page((vaddr_t)view.Buffer)->Physical == backing->Pages[1]);
    changes = g_pageChanges;
    assert(MemorySpaceChangeProtection(&g_space, (vaddr_t)view.Buffer, 500, MAPPING_USERSPACE, &attributes) == OS_EPERMISSIONS);
    assert(MemorySpaceCloneMapping(&g_space, &g_space, (vaddr_t)view.Buffer, &clone, 500,
                                  MAPPING_USERSPACE, MAPPING_VIRTUAL_PROCESS) == OS_EPERMISSIONS);
    assert(changes == g_pageChanges);
    assert(MemorySpaceChangeProtection(&g_space, (vaddr_t)view.Buffer, 500,
                                      MAPPING_READONLY | MAPPING_USERSPACE, &attributes) == OS_EOK);
    assert(__Page((vaddr_t)view.Buffer)->Flags & MAPPING_PERSISTENT);
    assert(__Page((vaddr_t)view.Buffer)->Flags & MAPPING_COMMIT);
    assert(MemorySpaceCloneMapping(&g_space, &g_space, (vaddr_t)view.Buffer, &clone, 500,
                                  MAPPING_READONLY | MAPPING_USERSPACE, MAPPING_VIRTUAL_PROCESS) == OS_EOK);
    assert(__Page(clone)->Physical == backing->Pages[1]);
    assert(MemorySpaceChangeProtection(&g_space, clone, 500, MAPPING_USERSPACE, &attributes) == OS_EPERMISSIONS);
    assert(MemorySpaceCommit(&g_space, clone & ~(PAGE_SIZE - 1), &wrongPage, PAGE_SIZE, 0,
                            MAPPING_PHYSICAL_FIXED) == OS_EPERMISSIONS);
    assert(SHMCommit(view.ID, view.Buffer, view.Buffer, 100) == OS_EOK);

    options.Data.SHM.Offset = PAGE_SIZE;
    options.Data.SHM.Length = PAGE_SIZE;
    options.Scope = GrantTokenScopeRead | GrantTokenScopeWrite;
    assert(GrantTokenCreate(second.ID, &options, &derived) == OS_EPERMISSIONS);
    options.Scope = GrantTokenScopeRead;
    assert(GrantTokenCreate(second.ID, &options, &derived) == OS_EOK);
    assert(SHMAttachToken(&derived, &alias) == OS_EOK);
    assert(SHMResolve(alias.ID, &resolved) == OS_EOK);
    assert(resolved.Offset == 2 * PAGE_SIZE && resolved.Length == PAGE_SIZE);
    ReleaseGlobalHandle(resolved.GlobalHandleID);
    assert(GrantTokenRevoke(derived.ControlHandleID) == OS_EOK);
    assert(SHMDetach(&alias) == OS_EOK);

    g_currentContext = &context->Security;
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(DestroyHandle(backingID) == OS_EOK);
    g_currentContext = &receiver;
    assert(SHMAttachToken(&token, &alias) != OS_EOK);
    g_failUnmap = true;
    original = view;
    assert(SHMDetach(&view) == OS_EBUSY);
    assert(memcmp(&view, &original, sizeof(view)) == 0);
    g_failUnmap = false;
    viewID = view.ID;
    assert(DestroyHandle(viewID) == OS_EOK);
    assert(SHMDetach(&second) == OS_EOK);
    __DrainJanitor();
    assert(g_backingDeletes == 0);
    assert(MemorySpaceChangeProtection(&g_space, clone, 500, MAPPING_USERSPACE, &attributes) == OS_EPERMISSIONS);
    assert(MemorySpaceUnmap(&g_space, (vaddr_t)view.Buffer - 100, PAGE_SIZE) == OS_EOK);
    __DrainJanitor();
    assert(g_backingDeletes == 0);
    if (exitWithMapping) {
        // Architecture teardown removes the page table before context cleanup.
        memset(g_pages, 0, sizeof(g_pages));
        assert(g_backingDeletes == 0 && context->Allocations.head != NULL);
    } else {
        assert(MemorySpaceUnmap(&g_space, clone - 100, PAGE_SIZE) == OS_EOK);
        __DrainJanitor();
        assert(g_backingDeletes == 1 && context->Allocations.head == NULL);
    }
    DestroyHandleGrantsTable(receiver.HandleTable);
    g_currentContext = NULL;
    ReleaseGlobalHandle(g_space.GroupGlobalHandleID);
    __DrainJanitor();
    assert(g_backingDeletes == 1);
    assert(g_liveAllocations == 0 && g_lockCount == 0);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
}

int
main(void)
{
    setbuf(stdout, NULL);
    g_machine.MemoryGranularity = PAGE_SIZE;
    __GrantBaseline();
    __TestRestricted(false);
    __TestRestricted(true);
    puts("Restricted SHM: clean pages, grants, ranges, aliases, protection, clones, copy faults and mapping lifetime passed");
    return 0;
}
