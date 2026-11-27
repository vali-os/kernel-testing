/** Exercise kernel stack commitment and relocation for asynchronous syscalls. */
#include "../../kernel/arch/aarch64/common/thread.c"

static unsigned char g_original[8192] __attribute__((aligned(4096)));
static unsigned char g_forked[8192] __attribute__((aligned(4096)));
static unsigned char g_cached[8192] __attribute__((aligned(4096)));
static uintptr_t g_pages[2];
static MemorySpace_t g_space;
static unsigned int g_commits;
static unsigned int g_frees;
static int g_cacheHasStack;
static unsigned int g_cacheGives;
static uintptr_t g_cacheGivenTop;

// User stack for interceptor frames, and what the checked user copy reports.
static unsigned char g_userStack[2048] __attribute__((aligned(16)));
static oserr_t g_copyStatus;
static uintptr_t g_copyAddress;
static unsigned int g_copies;

static void
__Check(
    _In_ int condition,
    _In_ int code)
{
    register long value __asm__("x0") = code;
    register long number __asm__("x8") = 93;

    if (!condition) {
        __asm__ volatile("svc 0" :: "r"(value), "r"(number) : "memory");
        __builtin_unreachable();
    }
}

void*
memcpy(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    unsigned char* output = destination;
    const unsigned char* input = source;
    size_t index;

    for (index = 0; index < length; index++) {
        output[index] = input[index];
    }
    return destination;
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

void*
kmalloc(
    _In_ size_t length)
{
    __Check(length <= sizeof(g_pages), 1);
    return g_pages;
}

void
kfree(
    _In_ void* memory)
{
    __Check(memory == g_pages, 2);
    g_frees++;
}

oserr_t
MemorySpaceTryCopyToUser(
    _Out_ void*       userBuffer,
    _In_  const void* kernelBuffer,
    _In_  size_t      length)
{
    g_copies++;
    g_copyAddress = (uintptr_t)userBuffer;
    if (g_copyStatus != OS_EOK) {
        return g_copyStatus;
    }
    __Check(g_copyAddress >= (uintptr_t)g_userStack, 30);
    __Check(g_copyAddress + length <= (uintptr_t)g_userStack + sizeof(g_userStack), 31);
    memcpy(userBuffer, kernelBuffer, length);
    return OS_EOK;
}

static int
__SameContext(
    _In_ const Context_t* left,
    _In_ const Context_t* right)
{
    const unsigned char* a = (const unsigned char*)left;
    const unsigned char* b = (const unsigned char*)right;
    size_t index;

    for (index = 0; index < sizeof(Context_t); index++) {
        if (a[index] != b[index]) {
            return 0;
        }
    }
    return 1;
}

static void
__TestInterceptor(void)
{
    uintptr_t userTop = (uintptr_t)g_userStack + sizeof(g_userStack);
    uintptr_t frame;
    Context_t original = {0};
    Context_t context;

    original.Pc = 0x8000001000ULL;
    original.UserSp = userTop - 8;
    original.X[0] = 11;
    original.X[1] = 22;
    original.Pstate = 0;

    // The frame goes below the user stack pointer, aligned to 16 bytes, and
    // holds the context as it was before the interceptor was added.
    context = original;
    __Check(ArchThreadContextPushInterceptor(&context, 0, 0x8000002000ULL, 1, 2, 3) == OS_EOK, 40);
    frame = (userTop - 8 - sizeof(Context_t)) & ~15ULL;
    __Check(g_copyAddress == frame && __SameContext((Context_t*)frame, &original), 41);
    __Check(context.Pc == 0x8000002000ULL && context.UserSp == frame && context.X[0] == frame, 42);
    __Check(context.X[1] == 1 && context.X[2] == 2 && context.X[3] == 3, 43);

    // A separate stack is used instead of the user stack pointer when given.
    context = original;
    context.UserSp = 0x1234;
    __Check(ArchThreadContextPushInterceptor(&context, userTop, 0x8000002000ULL, 0, 0, 0) == OS_EOK, 44);
    __Check(g_copyAddress == ((userTop - sizeof(Context_t)) & ~15ULL), 45);

    // A failed or busy copy leaves the context exactly as it was.
    g_copyStatus = OS_EINVALPARAMS;
    context = original;
    __Check(ArchThreadContextPushInterceptor(&context, 0, 0x8000002000ULL, 1, 2, 3) == OS_EINVALPARAMS, 46);
    __Check(__SameContext(&context, &original), 47);
    g_copyStatus = OS_EBUSY;
    __Check(ArchThreadContextPushInterceptor(&context, 0, 0x8000002000ULL, 1, 2, 3) == OS_EBUSY, 48);
    __Check(__SameContext(&context, &original), 49);
    g_copyStatus = OS_EOK;

    // A stack pointer too small to hold the frame is rejected before it can
    // wrap around to the top of the address space.
    g_copies = 0;
    context = original;
    context.UserSp = 16;
    __Check(ArchThreadContextPushInterceptor(&context, 0, 0x8000002000ULL, 0, 0, 0) == OS_EINVALPARAMS, 50);
    __Check(g_copies == 0 && context.UserSp == 16 && context.Pc == original.Pc, 51);
}

// The stack cache is empty until the test puts g_cached in it.
oserr_t
StackCacheAllocate(
    _In_  size_t     stackSize,
    _Out_ uintptr_t* stackTopOut)
{
    if (!g_cacheHasStack) {
        return OS_ENOENT;
    }
    __Check(stackSize == sizeof(g_cached), 8);
    g_cacheHasStack = 0;
    *stackTopOut = (uintptr_t)g_cached + sizeof(g_cached);
    return OS_EOK;
}

oserr_t
StackCacheRelease(
    _In_ uintptr_t stackTop,
    _In_ size_t    stackSize)
{
    __Check(stackSize == sizeof(g_cached), 9);
    g_cacheGives++;
    g_cacheGivenTop = stackTop;
    return OS_EOK;
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*              space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                    address)
{
    __Check(space == &g_space && options->Length == sizeof(g_forked), 3);
    __Check(options->Flags == (MAPPING_DOMAIN | MAPPING_STACK), 4);
    *address = (uintptr_t)g_forked + sizeof(g_forked);
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
    // The copied frames fit in one page, but future exception entry needs
    // every page of the reserved kernel stack to be present already.
    __Check(address == (uintptr_t)g_forked && length == sizeof(g_forked), 5);
    __Check(pages == g_pages, 6);
    g_commits++;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    __Check(0, 7);
    return OS_EOK;
}

void
_start(void)
{
    uintptr_t originalTop = (uintptr_t)g_original + sizeof(g_original);
    uintptr_t forkedTop = (uintptr_t)g_forked + sizeof(g_forked);
    uintptr_t cachedTop = (uintptr_t)g_cached + sizeof(g_cached);
    Context_t source = {0};
    Context_t* base;
    Context_t* context;
    register long result __asm__("x0") = 0;
    register long number __asm__("x8") = 93;

    source.Sp = originalTop - 1024;
    source.Pc = 0x12345678;
    source.Pstate = 0xa00003c5;
    source.X[0] = originalTop - 128;
    source.X[1] = 0x1234;
    source.X[29] = originalTop - 512;
    *(uintptr_t*)(originalTop - 512) = originalTop - 256;
    *(uintptr_t*)(originalTop - 504) = 0x87654321;

    __Check(ArchThreadContextFork(
        (Context_t*)(originalTop - sizeof(Context_t)),
        &source,
        THREADING_CONTEXT_LEVEL0,
        sizeof(g_original),
        &base,
        &context) == OS_EOK, 10);
    __Check(g_commits == 1 && g_frees == 1, 11);
    __Check((uintptr_t)base + sizeof(*base) == forkedTop, 12);
    __Check(context->Sp == forkedTop - 1024, 13);
    __Check(context->Pc == source.Pc && context->Pstate == 0xa0000005, 14);
    __Check(context->X[0] == forkedTop - 128 && context->X[1] == 0x1234, 15);
    __Check(context->X[29] == forkedTop - 512, 16);
    __Check(*(uintptr_t*)(forkedTop - 512) == forkedTop - 256, 17);
    __Check(*(uintptr_t*)(forkedTop - 504) == 0x87654321, 18);

    // A cached stack is already mapped and committed. The copy goes there,
    // and destroying it gives the stack back instead of unmapping it.
    g_cacheHasStack = 1;
    __Check(ArchThreadContextFork(
        (Context_t*)(originalTop - sizeof(Context_t)),
        &source,
        THREADING_CONTEXT_LEVEL0,
        sizeof(g_original),
        &base,
        &context) == OS_EOK, 19);
    __Check(g_commits == 1 && (uintptr_t)base + sizeof(*base) == cachedTop, 20);
    __Check(context->Sp == cachedTop - 1024 && context->X[29] == cachedTop - 512, 21);
    __Check(*(uintptr_t*)(cachedTop - 512) == cachedTop - 256, 22);
    ArchThreadContextDestroy(base, THREADING_CONTEXT_LEVEL0, sizeof(g_cached));
    __Check(g_cacheGives == 1 && g_cacheGivenTop == cachedTop, 23);
    __TestInterceptor();
    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
