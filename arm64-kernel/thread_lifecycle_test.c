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

void
ArchThreadLeave(
    _In_ Thread_t* thread)
{
    __Check(0, 1);
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
    __Check(0, 5);
}

void*
SchedulerAdvance(
    _In_  SchedulerObject_t* object,
    _In_  int                preemptive,
    _In_  clock_t            elapsed,
    _Out_ clock_t*           deadline)
{
    __Check(object == NULL, 6);
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
    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
