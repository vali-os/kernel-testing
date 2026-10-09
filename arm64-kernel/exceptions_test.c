/** Exercise EL0 demand faults and signal-frame validation without privileged instructions. */
#include "../../kernel/arch/aarch64/common/exceptions.c"

static MemorySpace_t g_space;
static unsigned int g_attributes;
static unsigned int g_faultCalls;
static oserr_t g_copyStatus;
static Context_t g_signalFrame;

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

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

oserr_t
GetMemorySpaceAttributes(
    _In_  MemorySpace_t* space,
    _In_  vaddr_t        address,
    _In_  size_t         length,
    _Out_ unsigned int*  attributes)
{
    __Check(space == &g_space, 1);
    __Check((address & 4095) + length == 4096, 2);
    *attributes = g_attributes;
    return OS_EOK;
}

enum OSPageFaultCode
DebugPageFault(
    _In_ Context_t* context,
    _In_ uintptr_t  address)
{
    __Check(address == context->FaultAddress, 3);
    g_faultCalls++;
    return OSPAGEFAULT_RESULT_MAPPED;
}

oserr_t
MemorySpaceCopyUser(
    _In_ void*  userBuffer,
    _In_ void*  kernelBuffer,
    _In_ size_t length,
    _In_ bool   toUser)
{
    __Check(userBuffer == (void*)0x8000001000ULL, 4);
    __Check(length == sizeof(Context_t) && !toUser, 5);
    *(Context_t*)kernelBuffer = g_signalFrame;
    return g_copyStatus;
}

void
_start(void)
{
    Context_t context = {0};
    register long result __asm__("x0") = 0;
    register long syscallNumber __asm__("x8") = 93;

    context.FaultAddress = 0x8000001007ULL;
    context.ErrorCode = (0x24ULL << 26) | 7;
    g_attributes = 0;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 10);
    g_attributes = MAPPING_READONLY;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 11);
    g_attributes = MAPPING_USERSPACE;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_MAPPED, 12);
    __Check(g_faultCalls == 1, 13);
    context.ErrorCode |= 1ULL << 6;
    g_attributes |= MAPPING_READONLY;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 14);
    context.ErrorCode = (0x20ULL << 26) | 7;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 15);
    g_attributes |= MAPPING_EXECUTABLE;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_MAPPED, 16);
    context.ErrorCode = (0x24ULL << 26) | 15;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 17);
    context.ErrorCode = (0x24ULL << 26) | 7;
    context.FaultAddress = 0xffff800000001000ULL;
    __Check(__ResolvePageFault(&context, 1) == OSPAGEFAULT_RESULT_FAULT, 18);
    __Check(g_faultCalls == 2, 19);
    __Check(__ResolvePageFault(&context, 0) == OSPAGEFAULT_RESULT_MAPPED, 20);

    context.X[0] = 0x8000001000ULL;
    context.Sp = 0xffff800000002000ULL;
    g_signalFrame.Pc = 0x8000002000ULL;
    g_signalFrame.UserSp = 0xff8000001000ULL;
    g_signalFrame.Sp = 0x12340000;
    g_signalFrame.Pstate = UINT64_MAX;
    g_copyStatus = OS_EINVALPARAMS;
    __Check(__ReturnFromSignal(&context) == NULL, 21);
    g_copyStatus = OS_EOK;
    g_signalFrame.Pc++;
    __Check(__ReturnFromSignal(&context) == NULL, 22);
    g_signalFrame.Pc--;
    g_signalFrame.UserSp++;
    __Check(__ReturnFromSignal(&context) == NULL, 23);
    g_signalFrame.UserSp--;
    __Check(__ReturnFromSignal(&context) == &context, 24);
    __Check(context.Sp == 0xffff800000002000ULL, 25);
    __Check(context.Pstate == 0xf0000000ULL, 26);

    __Check(__IsAbort(ARM64_EC_INSTRUCTION_ABORT_USER), 27);
    __Check(__IsAbort(ARM64_EC_INSTRUCTION_ABORT_KERNEL), 28);
    __Check(__IsAbort(ARM64_EC_DATA_ABORT_USER), 29);
    __Check(__IsAbort(ARM64_EC_DATA_ABORT_KERNEL), 30);
    __Check(!__IsAbort(0x22) && !__IsAbort(0x23), 31);
    __Check(!__IsAbort(ARM64_EC_MOPS), 32);
    __Check(__SignalForException(ARM64_EC_FP_EXCEPTION) == SIGFPE, 33);
    __Check(__SignalForException(ARM64_EC_BREAKPOINT) == SIGTRAP, 34);
    __Check(__SignalForException(ARM64_EC_SOFTWARE_STEP) == SIGTRAP, 35);
    __Check(__SignalForException(ARM64_EC_BRK) == SIGTRAP, 36);
    __Check(__SignalForException(ARM64_EC_UNKNOWN) == SIGILL, 37);
    __Check(__SignalForException(ARM64_EC_FP_ACCESS) == SIGILL, 38);
    __Check(__SignalForException(ARM64_EC_SVC) == SIGSEGV, 39);
    __Check(__SignalForException(ARM64_EC_MOPS) == SIGSEGV, 40);
    __asm__ volatile("svc 0" :: "r"(result), "r"(syscallNumber) : "memory");
    __builtin_unreachable();
}
