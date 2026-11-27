/** Ensure the outgoing address space survives until the CPU switches away. */
#include "../../kernel/threads/threads.c"
#include "../../kernel/components/cpu_private.h"

static SystemCpuCore_t g_core;
static Thread_t g_outgoing;
static Thread_t g_cancelled;
static Thread_t g_next;
static Context_t g_context;
static unsigned int g_scheduled;
static unsigned int g_destroyed;
static int g_switched;
static SystemMachine_t g_machine;
static unsigned int g_signalCalls;
static int g_signalPhase;
static unsigned char g_kernelCode[64];

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

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return &g_core;
}

Thread_t*
CpuCoreCurrentThread(
    _In_ SystemCpuCore_t* core)
{
    return core->CurrentThread;
}

Thread_t*
CpuCoreIdleThread(
    _In_ SystemCpuCore_t* core)
{
    return &core->IdleThread;
}

Context_t*
CpuCoreInterruptContext(
    _In_ SystemCpuCore_t* core)
{
    return &g_context;
}

void
CpuCoreSetInterruptContext(
    _In_ SystemCpuCore_t* core,
    _In_ Context_t*       context)
{
    core->InterruptRegisters = context;
}

void
CpuCoreSetCurrentThread(
    _In_ SystemCpuCore_t* core,
    _In_ Thread_t*        thread)
{
    core->CurrentThread = thread;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

void
SignalProcessQueued(
    _In_ Thread_t*  thread,
    _In_ Context_t* context)
{
    // Signals are only added for the selected thread, once its memory space
    // is active and its context returns to userspace.
    __Check(g_signalPhase, 22);
    __Check(thread == &g_next && context == &g_context, 23);
    __Check(g_core.CurrentThread == &g_next, 24);
    g_signalCalls++;
}

void
ArchThreadLeave(
    _In_ Thread_t* thread)
{
    __Check(g_signalPhase && thread == &g_next, 1);
}

void
ArchThreadEnter(
    _In_ SystemCpuCore_t* core,
    _In_ Thread_t*        thread)
{
    __Check(thread == &g_next, 2);
    __Check(core->CurrentThread == thread, 3);
    __Check(g_destroyed == 1, 4);
    g_switched = 1;
}

void
SchedulerExpediteObject(
    _In_ SchedulerObject_t* object)
{
    __Check(g_signalPhase, 5);
}

void*
SchedulerAdvance(
    _In_  SchedulerObject_t* object,
    _In_  int                preemptive,
    _In_  clock_t            elapsed,
    _Out_ clock_t*           deadline)
{
    __Check(object == NULL || g_signalPhase, 6);
    g_scheduled++;
    if (g_scheduled == 1) {
        return &g_cancelled;
    }
    return &g_next;
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t handle)
{
    if (handle == g_outgoing.Handle) {
        __Check(g_switched, 7);
        __Check(g_core.InterruptRegisters == &g_context, 8);
    } else {
        __Check(handle == g_cancelled.Handle, 9);
        __Check(!g_switched, 10);
    }
    g_destroyed++;
    return OS_EOK;
}

void*
LookupGlobalHandle(
    _In_ uuid_t       handle,
    _In_ HandleType_t type)
{
    __Check(type == HandleTypeThread, 18);
    return handle == g_outgoing.Handle ? &g_outgoing : &g_cancelled;
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* value,
    _In_ int           count,
    _In_ int           flags)
{
    __Check(atomic_load(value) == ThreadState_FINISHED, 19);
    __Check(count == INT_MAX, 20);
    return OS_EOK;
}

void
_assert_panic(
    _In_ const char* message)
{
    __Check(0, 21);
}

void
CpuCoreDeferFinish(
    _In_ SystemCpuCore_t* core,
    _In_ uuid_t           threadHandle)
{
    __Check(core == &g_core && g_switched, 13);
    __Check(threadHandle == g_outgoing.Handle, 14);
    __Check(core->PendingFinish == UUID_INVALID, 15);
    core->PendingFinish = threadHandle;
}

void
_start(void)
{
    clock_t deadline;
    register long result __asm__("x0") = 0;
    register long number __asm__("x8") = 93;

    g_outgoing.Handle = 1;
    g_outgoing.State = ThreadState_TERMINATING;
    g_outgoing.JoinTarget = UUID_INVALID;
    g_cancelled.Handle = 2;
    g_cancelled.State = ThreadState_TERMINATING;
    g_cancelled.JoinTarget = UUID_INVALID;
    g_next.ContextActive = &g_context;
    g_core.CurrentThread = &g_outgoing;

    __Check(ThreadingAdvance(0, 0, &deadline) == OS_EOK, 11);
    __Check(g_destroyed == 1 && g_scheduled == 2, 12);
    __Check(g_core.PendingFinish == g_outgoing.Handle, 16);

    // The next outer interrupt runs on the selected stack and finishes the
    // outgoing thread. Returning from ThreadingAdvance alone cannot free it.
    ThreadingFinish(g_core.PendingFinish);
    g_core.PendingFinish = UUID_INVALID;
    __Check(g_destroyed == 2, 17);

    // A thread that keeps running in userspace gets its queued signals added
    // as it is selected again.
    g_signalPhase = 1;
    g_machine.BootInformation.Kernel.Base = (uintptr_t)g_kernelCode;
    g_machine.BootInformation.Kernel.Length = sizeof(g_kernelCode);
    g_context.Pc = 0x8000001000ULL;
    atomic_store(&g_next.Signaling.Pending, 1);
    __Check(ThreadingAdvance(1, 0, &deadline) == OS_EOK, 25);
    __Check(g_signalCalls == 1, 26);

    // A thread that resumes inside the kernel adds its signals when its system
    // call returns, not here.
    g_context.Pc = (uintptr_t)g_kernelCode + 16;
    __Check(ThreadingAdvance(1, 0, &deadline) == OS_EOK, 27);
    __Check(g_signalCalls == 1, 28);

    // Nothing is done without pending signals.
    g_context.Pc = 0x8000001000ULL;
    atomic_store(&g_next.Signaling.Pending, 0);
    __Check(ThreadingAdvance(1, 0, &deadline) == OS_EOK, 29);
    __Check(g_signalCalls == 1, 30);
    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
