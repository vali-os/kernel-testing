/** Exercise signal frame delivery: queue handling, stack selection and failures. */
#include <setjmp.h>
#include "../../kernel/sync/signal.c"
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#include "../../librt/libds/streambuffer.c"

#define USER_CODE 0x8000001000ULL

// One recorded call to ArchThreadContextPushInterceptor.
struct PushCall {
    Context_t*   Context;
    uintptr_t    Stack;
    uintptr_t    Handler;
    uintptr_t    Flags;
    uintptr_t    Argument;
};

static SystemMachine_t  g_machine;
static SystemCpuCore_t* g_core = (SystemCpuCore_t*)0x1000;
static Thread_t         g_thread;
static Context_t        g_userContext;
static Context_t        g_signalStack;
static Context_t*       g_interruptContext;
static Thread_t*        g_current;
static uintptr_t        g_handler = 0x8000002000ULL;
static unsigned char    g_kernelCode[64];

// Push results to return, one per call, and the calls that were made.
static oserr_t          g_pushResults[8];
static unsigned int     g_pushResultCount;
static struct PushCall  g_pushes[8];
static unsigned int     g_pushCount;

static unsigned int     g_errors;
static unsigned int     g_expedites;
static unsigned int     g_releases;

// ThreadExit never returns, so the stub jumps back to the test.
static jmp_buf          g_exitJump;
static int              g_exitArmed;
static int              g_exitCode;
static unsigned int     g_exits;
static int              g_interruptsEnabled;

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
memset(
    _Out_ void*  destination,
    _In_  int    value,
    _In_  size_t size)
{
    unsigned char* bytes = destination;
    size_t         index;

    for (index = 0; index < size; index++) {
        bytes[index] = value;
    }
    return destination;
}

void*
memcpy(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    unsigned char*       output = destination;
    const unsigned char* input = source;
    size_t               index;

    for (index = 0; index < length; index++) {
        output[index] = input[index];
    }
    return destination;
}

void
_assert_panic(
    _In_ const char* message)
{
    __Check(0, 100);
}

void* dsalloc(size_t size) { __Check(0, 101); return NULL; }
void dsfree(void* pointer) { __Check(0, 102); }
void dswait(OSFutexParameters_t* parameters, OSAsyncContext_t* context) { __Check(0, 103); }
void dswake(OSFutexParameters_t* parameters) { }

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 0;
}

Thread_t*
ThreadCurrentForCore(
    _In_ uuid_t coreId)
{
    return g_current;
}

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return g_core;
}

Context_t*
CpuCoreInterruptContext(
    _In_ SystemCpuCore_t* core)
{
    return g_interruptContext;
}

void
SchedulerExpediteObject(
    _In_ SchedulerObject_t* object)
{
    g_expedites++;
}

uuid_t
SchedulerObjectGetAffinity(
    _In_ SchedulerObject_t* object)
{
    return 0;
}

irqstate_t
InterruptDisable(void)
{
    return 0;
}

irqstate_t
InterruptRestoreState(
    _In_ irqstate_t state)
{
    return state;
}

irqstate_t
InterruptEnable(void)
{
    g_interruptsEnabled = 1;
    return 0;
}

oserr_t
AcquireGlobalHandle(
    _In_  uuid_t       handle,
    _In_  HandleType_t type,
    _Out_ void**       resourceOut)
{
    __Check(handle == g_thread.Handle && type == HandleTypeThread, 104);
    *resourceOut = &g_thread;
    return OS_EOK;
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t handle)
{
    __Check(handle == g_thread.Handle, 105);
    g_releases++;
    return OS_EOK;
}

oserr_t
TxuMessageSend(
    _In_ uuid_t                  coreId,
    _In_ SystemCpuFunctionType_t type,
    _In_ TxuFunction_t           function,
    _In_ void*                   argument,
    _In_ int                     asynchronous)
{
    __Check(0, 106);
    return OS_ENOTSUPPORTED;
}

void
DebugPanic(
    _In_ int         scope,
    _In_ Context_t*  context,
    _In_ const char* message, ...)
{
    __Check(0, 107);
}

uintptr_t
MemorySpaceSignalHandler(
    _In_ MemorySpace_t* memorySpace)
{
    return g_handler;
}

const char*
ThreadName(
    _In_ Thread_t* thread)
{
    return "signal-test";
}

_Noreturn void
ThreadExit(
    _In_ int exitCode)
{
    __Check(g_exitArmed, 108);
    g_exitCode = exitCode;
    g_exits++;
    longjmp(g_exitJump, 1);
}

oserr_t
ArchThreadContextPushInterceptor(
    _In_ Context_t* context,
    _In_ uintptr_t  temporaryStack,
    _In_ uintptr_t  address,
    _In_ uintptr_t  argument0,
    _In_ uintptr_t  argument1,
    _In_ uintptr_t  argument2)
{
    oserr_t status = OS_EOK;

    __Check(g_pushCount < 8, 109);
    if (g_pushCount < g_pushResultCount) {
        status = g_pushResults[g_pushCount];
    }
    g_pushes[g_pushCount] = (struct PushCall) {
        .Context = context,
        .Stack = temporaryStack,
        .Handler = address,
        .Flags = argument0,
        .Argument = argument1
    };
    g_pushCount++;
    return status;
}

static void
__Reset(void)
{
    ThreadSignal_t ignored;
    size_t         bytesRead;

    // Empty the queue with plain reads, so the stream indexes stay valid.
    do {
        bytesRead = streambuffer_stream_in(g_thread.Signaling.Signals, &ignored, sizeof(ignored),
            &(streambuffer_rw_options_t) { .flags = STREAMBUFFER_NO_BLOCK });
    } while (bytesRead != 0);
    atomic_store(&g_thread.Signaling.Pending, 0);
    g_pushResultCount = 0;
    g_pushCount = 0;
    g_errors = 0;
}

static void
__Queue(
    _In_ int          signal,
    _In_ unsigned int flags,
    _In_ uintptr_t    argument)
{
    ThreadSignal_t threadSignal = {
        .Signal = signal,
        .Argument = (void*)argument,
        .Flags = flags
    };

    __Check(streambuffer_stream_out(g_thread.Signaling.Signals, &threadSignal, sizeof(threadSignal),
        &(streambuffer_rw_options_t) { .flags = STREAMBUFFER_NO_BLOCK }) == sizeof(threadSignal), 110);
    atomic_fetch_add(&g_thread.Signaling.Pending, 1);
}

static size_t
__QueuedBytes(void)
{
    size_t available;

    streambuffer_get_bytes_available_in(g_thread.Signaling.Signals, &available);
    return available;
}

static void
__TestQueuedSignals(void)
{
    // Without a handler nothing is taken from the queue.
    __Reset();
    __Queue(SIGINT, 0, 7);
    g_handler = 0;
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 0 && __QueuedBytes() == sizeof(ThreadSignal_t), 1);
    g_handler = 0x8000002000ULL;

    // A frame that is written removes the signal from the queue. Without the
    // separate stack flag it goes on the current user stack.
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 1 && g_pushes[0].Context == &g_userContext, 2);
    __Check(g_pushes[0].Stack == 0 && g_pushes[0].Handler == g_handler, 3);
    __Check(g_pushes[0].Flags == ((uintptr_t)SIGINT << 24) && g_pushes[0].Argument == 7, 4);
    __Check(__QueuedBytes() == 0 && atomic_load(&g_thread.Signaling.Pending) == 0, 5);

    // The separate stack flag selects the thread's signal stack.
    __Reset();
    __Queue(SIGUSR1, SIGNAL_FLAG_SEPERATE_STACK, 9);
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 1 && g_pushes[0].Stack == (uintptr_t)&g_signalStack, 6);
    __Check(g_pushes[0].Flags == (((uintptr_t)SIGUSR1 << 24) | SIGNAL_FLAG_SEPERATE_STACK), 7);
    __Check(atomic_load(&g_thread.Signaling.Pending) == 0, 8);

    // A busy mapping lock keeps this and later signals queued, in order.
    __Reset();
    __Queue(SIGINT, 0, 1);
    __Queue(SIGUSR1, 0, 2);
    g_pushResults[0] = OS_EBUSY;
    g_pushResultCount = 1;
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 1 && g_errors == 0, 9);
    __Check(__QueuedBytes() == 2 * sizeof(ThreadSignal_t), 10);
    __Check(atomic_load(&g_thread.Signaling.Pending) == 2, 11);
    g_pushResultCount = 0;
    g_pushCount = 0;
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 2 && g_pushes[0].Argument == 1 && g_pushes[1].Argument == 2, 12);
    __Check(atomic_load(&g_thread.Signaling.Pending) == 0, 13);

    // A stack that cannot hold the frame drops that signal with an error, and
    // the next signal is still delivered.
    __Reset();
    __Queue(SIGINT, 0, 1);
    __Queue(SIGUSR1, 0, 2);
    g_pushResults[0] = OS_EINVALPARAMS;
    g_pushResultCount = 1;
    SignalProcessQueued(&g_thread, &g_userContext);
    __Check(g_pushCount == 2 && g_pushes[1].Argument == 2 && g_errors == 1, 14);
    __Check(__QueuedBytes() == 0 && atomic_load(&g_thread.Signaling.Pending) == 0, 15);
}

static void
__TestDeliveryCases(void)
{
    Context_t interrupted = { .Pc = USER_CODE };

    // CASE 1.2: the thread runs user code on this core, so the frame is
    // written to the interrupted context straight away.
    __Reset();
    g_current = &g_thread;
    g_thread.ContextActive = &g_userContext;
    g_interruptContext = &interrupted;
    __Check(SignalSend(g_thread.Handle, SIGINT, NULL) == OS_EOK, 20);
    __Check(g_pushCount == 1 && g_pushes[0].Context == &interrupted, 21);
    __Check(atomic_load(&g_thread.Signaling.Pending) == 0 && g_releases == 1, 22);

    // CASE 2.2: the thread waits to run user code while another thread is
    // current. Its memory space may not be active, so nothing is written yet.
    __Reset();
    g_current = (Thread_t*)0x2000;
    __Check(SignalSend(g_thread.Handle, SIGINT, NULL) == OS_EOK, 23);
    __Check(g_pushCount == 0 && g_expedites == 0, 24);
    __Check(atomic_load(&g_thread.Signaling.Pending) == 1, 25);

    // CASE 2.1: a waiting thread inside the kernel is woken to handle it.
    __Reset();
    g_userContext.Pc = (uintptr_t)g_kernelCode + 8;
    __Check(SignalSend(g_thread.Handle, SIGINT, NULL) == OS_EOK, 26);
    __Check(g_pushCount == 0 && g_expedites == 1, 27);
    g_userContext.Pc = USER_CODE;
    g_current = &g_thread;
}

static void
__TestTraps(void)
{
    Context_t trap = { .Pc = USER_CODE };

    // A written frame uses the signal stack and returns normally.
    __Reset();
    SignalExecuteLocalThreadTrap(&trap, SIGSEGV, SIGNAL_FLAG_PAGEFAULT, (void*)0x1234, NULL);
    __Check(g_pushCount == 1 && g_pushes[0].Stack == (uintptr_t)&g_signalStack, 30);
    __Check(g_pushes[0].Flags == (((uintptr_t)SIGSEGV << 24) | SIGNAL_FLAG_SEPERATE_STACK |
                                  SIGNAL_FLAG_HARDWARE_TRAP | SIGNAL_FLAG_PAGEFAULT), 31);

    // A busy mapping lock returns without ending the thread. The instruction
    // runs again and traps again.
    __Reset();
    g_pushResults[0] = OS_EBUSY;
    g_pushResultCount = 1;
    SignalExecuteLocalThreadTrap(&trap, SIGSEGV, 0, NULL, NULL);
    __Check(g_pushCount == 1 && g_exits == 0 && g_errors == 0, 32);

    // A signal stack that is not writable ends the thread with interrupts
    // enabled, because returning would only trap again.
    __Reset();
    g_pushResults[0] = OS_EINVALPARAMS;
    g_pushResultCount = 1;
    g_exitArmed = 1;
    if (setjmp(g_exitJump) == 0) {
        SignalExecuteLocalThreadTrap(&trap, SIGILL, 0, NULL, NULL);
        __Check(0, 33);
    }
    g_exitArmed = 0;
    __Check(g_exits == 1 && g_exitCode == EXIT_FAILURE, 34);
    __Check(g_interruptsEnabled && g_errors == 1, 35);
}

void
_start(void)
{
    static uint8_t storage[sizeof(streambuffer_t) + sizeof(ThreadSignal_t) * 4];
    register long  result __asm__("x0") = 0;
    register long  number __asm__("x8") = 93;

    g_machine.BootInformation.Kernel.Base = (uintptr_t)g_kernelCode;
    g_machine.BootInformation.Kernel.Length = sizeof(g_kernelCode);
    g_userContext.Pc = USER_CODE;
    g_thread.Handle = 5;
    g_thread.Contexts[THREADING_CONTEXT_SIGNAL] = &g_signalStack;
    g_thread.Signaling.Signals = (streambuffer_t*)storage;
    streambuffer_construct(g_thread.Signaling.Signals, sizeof(ThreadSignal_t) * 4,
        STREAMBUFFER_MULTIPLE_WRITERS | STREAMBUFFER_GLOBAL);
    atomic_store(&g_thread.State, ThreadState_RUNNING);
    g_current = &g_thread;

    __TestQueuedSignals();
    __TestDeliveryCases();
    __TestTraps();
    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
