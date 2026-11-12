/* Exercise the production amd64 stack allocator with protected host mappings. */
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The host inttypes header does not define Vali's native-integer format.
#ifndef PRIiIN
#define PRIiIN "i"
#endif

#ifndef AMD64_CONTEXT_SOURCE
#define AMD64_CONTEXT_SOURCE "../kernel/arch/x86/x64/context.c"
#endif
#include AMD64_CONTEXT_SOURCE

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "AMD64 stack check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static MemorySpace_t g_space;
static void*         g_mapping;
static size_t        g_reserved;
static size_t        g_expectedCommit;
static size_t        g_committed;
static unsigned int  g_expectedFlags;
static unsigned int  g_expectedPlacement;
static unsigned int  g_mapCalls;
static unsigned int  g_unmapCalls;
static unsigned int  g_heapAllocations;
static oserr_t       g_mapStatus;
static oserr_t       g_commitStatus;
static int           g_failHeap;
static unsigned char g_original[16384] __attribute__((aligned(4096)));

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* allocation;

    if (g_failHeap) {
        return NULL;
    }
    allocation = malloc(size);
    CHECK(allocation != NULL);
    g_heapAllocations++;
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    CHECK(allocation != NULL && g_heapAllocations == 1);
    g_heapAllocations--;
    free(allocation);
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*       format,
    ...)
{
}

void
DebugPanic(
    _In_ int         scope,
    _In_ Context_t*  context,
    _In_ const char* message,
    ...)
{
    CHECK(0);
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*              space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                    address)
{
    CHECK(space == &g_space);
    CHECK(g_mapping == NULL);
    CHECK(options->Flags == g_expectedFlags);
    CHECK(options->PlacementFlags == g_expectedPlacement);
    g_mapCalls++;
    if (g_mapStatus != OS_EOK) {
        return g_mapStatus;
    }

    g_reserved = options->Length;
    g_mapping = mmap(NULL, g_reserved + PAGE_SIZE * 2, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(g_mapping != MAP_FAILED);
    *address = (uintptr_t)g_mapping + PAGE_SIZE + g_reserved;
    return OS_EOK;
}

oserr_t
MemorySpaceCommit(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ uintptr_t*     pages,
    _In_ size_t         length,
    _In_ size_t         mask,
    _In_ unsigned int   flags)
{
    uintptr_t base = (uintptr_t)g_mapping + PAGE_SIZE;
    size_t    i;

    CHECK(space == &g_space && g_mapping != NULL);
    CHECK(length == g_expectedCommit);
    CHECK(address == base + g_reserved - length);
    CHECK(address >= base && length <= g_reserved);
    CHECK(address % PAGE_SIZE == 0 && length % PAGE_SIZE == 0);
    CHECK(mask == 0 && flags == 0);

    // Write every output slot. Sanitizers detect a scalar or short page array.
    for (i = 0; i < length / PAGE_SIZE; i++) {
        pages[i] = 0x100000 + i * PAGE_SIZE;
    }
    if (g_commitStatus != OS_EOK) {
        return g_commitStatus;
    }
    CHECK(mprotect((void*)address, length, PROT_READ | PROT_WRITE) == 0);
    g_committed = length;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    CHECK(space == &g_space && g_mapping != NULL);
    CHECK(address == (uintptr_t)g_mapping + PAGE_SIZE);
    CHECK(length == g_reserved);
    CHECK(munmap(g_mapping, g_reserved + PAGE_SIZE * 2) == 0);
    g_mapping = NULL;
    g_unmapCalls++;
    return OS_EOK;
}

static void
__Reset(
    _In_ int    contextType,
    _In_ size_t committed)
{
    CHECK(g_mapping == NULL && g_heapAllocations == 0);
    g_reserved = 0;
    g_committed = 0;
    g_mapCalls = 0;
    g_unmapCalls = 0;
    g_mapStatus = OS_EOK;
    g_commitStatus = OS_EOK;
    g_failHeap = 0;
    g_expectedCommit = committed;
    g_expectedFlags = MAPPING_DOMAIN | MAPPING_STACK;
    g_expectedPlacement = MAPPING_VIRTUAL_GLOBAL;
    if (contextType != THREADING_CONTEXT_LEVEL0) {
        g_expectedFlags |= MAPPING_USERSPACE;
        g_expectedPlacement = MAPPING_VIRTUAL_THREAD;
    }
}

static void
__TestCreate(
    _In_ int    contextType,
    _In_ size_t size)
{
    Context_t* context;
    uintptr_t  top;
    size_t     committed;

    committed = contextType == THREADING_CONTEXT_LEVEL0 ? size : PAGE_SIZE;
    __Reset(contextType, committed);
    context = ArchThreadContextCreate(contextType, size);
    CHECK(context != NULL);
    top = (uintptr_t)g_mapping + PAGE_SIZE + size;
    CHECK((uintptr_t)context + sizeof(*context) == top);
    CHECK(g_committed == committed && g_heapAllocations == 0);

    // Touch both ends of the backed range, including the lowest kernel page.
    *(volatile unsigned char*)(top - committed) = 0x35;
    *(volatile unsigned char*)(top - 1) = 0x71;
    if (contextType == THREADING_CONTEXT_LEVEL0) {
        ArchThreadContextDestroy(context, contextType, size);
    } else {
        CHECK(MemorySpaceUnmap(&g_space, top - size, size) == OS_EOK);
    }
    CHECK(g_mapping == NULL && g_unmapCalls == 1);
}

static void
__TestFork(
    _In_ size_t used)
{
    Context_t  original = {0};
    Context_t* base;
    Context_t* context;
    uintptr_t  oldTop = (uintptr_t)g_original + sizeof(g_original);
    uintptr_t  newTop;
    size_t     i;

    __Reset(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    for (i = 0; i < sizeof(g_original); i++) {
        g_original[i] = (unsigned char)(i * 7);
    }
    original.Rsp = oldTop - used;
    original.Rbp = oldTop - 64;
    original.Rip = 0x123456;
    original.Rax = 1;
    CHECK(ArchThreadContextFork((Context_t*)(oldTop - sizeof(Context_t)),
        &original, THREADING_CONTEXT_LEVEL0, sizeof(g_original), &base, &context) == OS_EOK);
    newTop = (uintptr_t)g_mapping + PAGE_SIZE + g_reserved;
    CHECK((uintptr_t)base + sizeof(*base) == newTop);
    CHECK((uintptr_t)context == newTop - used - sizeof(*context));
    CHECK(context->Rsp == newTop - used && context->Rbp == newTop - 64);
    CHECK(context->Rip == original.Rip && context->Rax == original.Rax);
    CHECK(context->Cs == GDT_KCODE_SEGMENT && context->UserSs == GDT_KDATA_SEGMENT);
    CHECK(context->UserRsp == newTop - used);
    CHECK(memcmp((void*)(newTop - used), (void*)(oldTop - used), used) == 0);
    CHECK(g_committed == sizeof(g_original) && g_heapAllocations == 0);
    *(volatile unsigned char*)(newTop - sizeof(g_original)) = 0x56;
    ArchThreadContextDestroy(base, THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    CHECK(g_mapping == NULL && g_unmapCalls == 1);
}

static void
__TestFailures(void)
{
    Context_t* context;
    uintptr_t  address = 0x1234;

    __Reset(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    g_mapStatus = OS_EOOM;
    context = ArchThreadContextCreate(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    CHECK(context == NULL && g_mapping == NULL && g_unmapCalls == 0);

    __Reset(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    g_failHeap = 1;
    context = ArchThreadContextCreate(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    CHECK(context == NULL && g_mapping == NULL && g_unmapCalls == 1);

    __Reset(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    g_commitStatus = OS_EOOM;
    CHECK(__AllocateStackInMemory(MAPPING_VIRTUAL_GLOBAL, MAPPING_DOMAIN | MAPPING_STACK,
        sizeof(g_original), sizeof(g_original), &address) == OS_EOOM);
    CHECK(address == 0x1234 && g_mapping == NULL && g_unmapCalls == 1);
    CHECK(g_heapAllocations == 0);
}

static void
__TestBounds(void)
{
    Context_t  original = {0};
    Context_t* base = NULL;
    Context_t* context = NULL;
    uintptr_t  oldTop = (uintptr_t)g_original + sizeof(g_original);
    uintptr_t  address = 0x1234;

    __Reset(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    CHECK(ArchThreadContextCreate(THREADING_CONTEXT_LEVEL0, PAGE_SIZE - 1) == NULL);
    CHECK(__AllocateStackInMemory(MAPPING_VIRTUAL_GLOBAL, MAPPING_STACK,
        PAGE_SIZE, PAGE_SIZE + 1, &address) == OS_EINVALPARAMS);
    CHECK(__AllocateStackInMemory(MAPPING_VIRTUAL_GLOBAL, MAPPING_STACK,
        PAGE_SIZE, 0, &address) == OS_EINVALPARAMS);
    CHECK(address == 0x1234 && g_mapCalls == 0);

    original.Rsp = oldTop + 8;
    CHECK(ArchThreadContextFork((Context_t*)(oldTop - sizeof(Context_t)),
        &original, THREADING_CONTEXT_LEVEL0, sizeof(g_original), &base, &context) == OS_EINVALPARAMS);
    original.Rsp = (uintptr_t)g_original;
    CHECK(ArchThreadContextFork((Context_t*)(oldTop - sizeof(Context_t)),
        &original, THREADING_CONTEXT_LEVEL0, sizeof(g_original), &base, &context) == OS_EINVALPARAMS);
    CHECK(base == NULL && context == NULL && g_mapCalls == 0);

    // Partial-page initial backing must start on a page boundary below the top.
    __Reset(THREADING_CONTEXT_LEVEL1, PAGE_SIZE * 2);
    CHECK(__AllocateStackInMemory(g_expectedPlacement, g_expectedFlags,
        sizeof(g_original), PAGE_SIZE + 1, &address) == OS_EOK);
    CHECK(MemorySpaceUnmap(&g_space, address - sizeof(g_original), sizeof(g_original)) == OS_EOK);
    CHECK(g_heapAllocations == 0);
}

int
main(void)
{
    __TestCreate(THREADING_CONTEXT_LEVEL0, sizeof(g_original));
    __TestFork(512);
    __TestFork(PAGE_SIZE * 2 + 64);
    __TestFork(sizeof(g_original) - sizeof(Context_t));
    __TestCreate(THREADING_CONTEXT_LEVEL1, sizeof(g_original));
    __TestCreate(THREADING_CONTEXT_SIGNAL, sizeof(g_original));
    __TestFailures();
    __TestBounds();
    CHECK(THREADING_KERNEL_STACK_SIZE == 16384);
    puts("AMD64 stacks: full kernel/fork backing, lazy user stacks, bounds and cleanup passed");
    return 0;
}
