/** Exercise ID lookup, completion waits, exit requests and temporary thread references. */
#include <setjmp.h>
#include "../../kernel/threads/threads.c"
#undef __MODULE
#include "../../kernel/sync/signal.c"
#undef __MODULE
#include "../../kernel/api/threads.c"
#include "../../kernel/components/cpu_private.h"

static SystemCpuCore_t g_core;
static Thread_t g_threads[3];
static MemorySpace_t g_spaces[2];
static unsigned int g_references[3];
static unsigned int g_waits;
static unsigned int g_wakes;
static unsigned int g_expedites;
static unsigned int g_recordFrees;
static irqstate_t g_irqEnabled = 1;
static void (*g_disableHook)(void);
static TxuFunction_t g_callback;
static void* g_callbackArgument;
static oserr_t g_deliveryStatus;

// The yield stub leaves ThreadExit by jumping back to the test, as the real
// yield never returns to a thread that is exiting.
static jmp_buf g_yieldExit;
static int g_yieldArmed;
static unsigned int g_yields;
static int g_yieldState;
static irqstate_t g_yieldIrq;

// Tests that expect an assertion to fail arm the panic stub and jump back.
static jmp_buf g_panicExit;
static int g_panicArmed;
static unsigned int g_panics;

SystemMachine_t*
GetMachine(void)
{
    static SystemMachine_t machine;
    return &machine;
}

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
    size_t index;

    for (index = 0; index < size; index++) {
        bytes[index] = value;
    }
    return destination;
}

void
_assert_panic(
    _In_ const char* message)
{
    // Only tests that provoke an assertion on purpose may reach this.
    __Check(g_panicArmed, 100);
    g_panics++;
    longjmp(g_panicExit, 1);
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
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

SystemCpuCore_t*
GetProcessorCore(
    _In_ uuid_t coreId)
{
    return &g_core;
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 0;
}

irqstate_t
InterruptDisable(void)
{
    irqstate_t previous = g_irqEnabled;
    void (*hook)(void) = g_disableHook;

    g_irqEnabled = 0;

    // The hook runs once, and lets a test change thread state at the point
    // where a terminate request is about to be applied.
    g_disableHook = NULL;
    if (hook != NULL) {
        hook();
    }
    return previous;
}

irqstate_t
InterruptRestoreState(
    _In_ irqstate_t state)
{
    g_irqEnabled = state;
    return state;
}

void*
LookupGlobalHandle(
    _In_ uuid_t       id,
    _In_ HandleType_t type)
{
    __Check(type == HandleTypeThread, 101);
    if (id < 1 || id > 3 || g_references[id - 1] == 0) {
        return NULL;
    }
    return &g_threads[id - 1];
}

oserr_t
AcquireGlobalHandle(
    _In_  uuid_t       id,
    _In_  HandleType_t type,
    _Out_ void**       resource)
{
    void* thread = LookupGlobalHandle(id, type);

    if (thread == NULL) {
        return OS_ENOENT;
    }
    g_references[id - 1]++;
    if (resource != NULL) {
        *resource = thread;
    }
    return OS_EOK;
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t id)
{
    __Check(id >= 1 && id <= 3, 102);
    __Check(g_references[id - 1] != 0, 103);
    g_references[id - 1]--;
    return g_references[id - 1] ? OS_EINCOMPLETE : OS_EOK;
}

oserr_t
DestroyHandle(
    _In_ uuid_t id)
{
    // No thread operation may close a grant in whichever process is running.
    __Check(0, 104);
    return OS_ENOENT;
}

oserr_t
AreMemorySpacesRelated(
    _In_ MemorySpace_t* first,
    _In_ MemorySpace_t* second)
{
    __Check(first != NULL && second != NULL, 105);
    return first->GroupGlobalHandleID == second->GroupGlobalHandleID ? OS_EOK : OS_EUNKNOWN;
}

void
MutexLock(
    _In_ Mutex_t* mutex)
{
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    // A timer must not finish self-exit while its mutex is still locked.
    if (mutex == &g_threads[0].SyncObject &&
        atomic_load(&g_threads[0].State) == ThreadState_TERMINATING) {
        __Check(!g_irqEnabled, 115);
    }
}

void
SchedulerExpediteObject(
    _In_ SchedulerObject_t* object)
{
    g_expedites++;
}

void
SchedulerDestroyObject(
    _In_ SchedulerObject_t* object)
{
    __Check(0, 116);
}

void
ArchThreadContextDestroy(
    _In_ Context_t* context,
    _In_ int        type,
    _In_ size_t     size)
{
    __Check(context == NULL, 117);
}

oserr_t
ArchThreadDestroy(
    _In_ Thread_t* thread)
{
    return OS_EOK;
}

void
ArchThreadYield(void)
{
    // ThreadExit only flags the thread, and the scheduler finishes it once the
    // yield has switched away. Record what the scheduler would see at that point.
    __Check(g_yieldArmed, 119);
    g_yields++;
    g_yieldState = atomic_load(&g_core.CurrentThread->State);
    g_yieldIrq = g_irqEnabled;
    longjmp(g_yieldExit, 1);
}

void
kfree(
    _In_ void* memory)
{
    if (memory != NULL) {
        __Check(memory == &g_threads[1], 118);
        g_recordFrees++;
    }
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* value,
    _In_ int           count,
    _In_ int           flags)
{
    __Check(atomic_load(value) == ThreadState_FINISHED, 106);
    __Check(count == INT_MAX, 107);
    g_wakes++;
    return OS_EOK;
}

oserr_t
FutexWait(
    _In_ OSAsyncContext_t* asyncContext,
    _In_ _Atomic(int)*     value,
    _In_ int              expected,
    _In_ int              flags,
    _In_ _Atomic(int)*     other,
    _In_ int              count,
    _In_ int              operation,
    _In_ OSTimestamp_t*   deadline)
{
    __Check(value == &g_threads[1].State, 108);
    __Check(atomic_load(&g_threads[0].JoinTarget) == 2, 109);
    __Check(g_references[1] == 2, 110);
    __Check(expected != ThreadState_FINISHED, 111);
    g_waits++;

    // Complete just before the actual wait would publish its queue entry.
    // The changed futex value must let join finish without losing the wake.
    g_threads[1].RetCode = 42;
    atomic_store(&g_threads[1].State, ThreadState_TERMINATING);
    ThreadingFinish(2);
    __Check(g_references[1] == 1, 112);
    return OS_EINTERRUPTED;
}

Context_t*
CpuCoreInterruptContext(
    _In_ SystemCpuCore_t* core)
{
    return NULL;
}

uuid_t
SchedulerObjectGetAffinity(
    _In_ SchedulerObject_t* object)
{
    return 1;
}

size_t
streambuffer_stream_out(
    _In_ streambuffer_t*            stream,
    _In_ void*                     buffer,
    _In_ size_t                    length,
    _In_ streambuffer_rw_options_t* options)
{
    return length;
}

uintptr_t
MemorySpaceSignalHandler(
    _In_ MemorySpace_t* space)
{
    return 0;
}

size_t
streambuffer_stream_in(
    _In_ streambuffer_t*            stream,
    _In_ void*                     buffer,
    _In_ size_t                    length,
    _In_ streambuffer_rw_options_t* options)
{
    return 0;
}

void
ArchThreadContextPushInterceptor(
    _In_ Context_t* context,
    _In_ uintptr_t alternativeStack,
    _In_ uintptr_t interceptor,
    _In_ uintptr_t argument0,
    _In_ uintptr_t argument1,
    _In_ uintptr_t argument2)
{
    __Check(0, 113);
}

oserr_t
TxuMessageSend(
    _In_ uuid_t                  coreId,
    _In_ SystemCpuFunctionType_t type,
    _In_ TxuFunction_t           function,
    _In_ void*                   argument,
    _In_ int                     asynchronous)
{
    __Check(asynchronous == 1 && g_callback == NULL, 114);
    g_callback = function;
    g_callbackArgument = argument;
    return g_deliveryStatus;
}

static void
__Reset(void)
{
    unsigned int index;

    memset(g_threads, 0, sizeof(g_threads));
    g_core.CurrentThread = &g_threads[0];
    g_spaces[0].GroupGlobalHandleID = 10;
    g_spaces[1].GroupGlobalHandleID = 20;
    for (index = 0; index < 3; index++) {
        g_threads[index].Handle = index + 1;
        g_threads[index].ParentHandle = 3;
        g_threads[index].JoinTarget = UUID_INVALID;
        g_threads[index].MemorySpace = &g_spaces[0];
        g_threads[index].MemorySpaceHandle = UUID_INVALID;
        g_references[index] = 1;
    }
    g_waits = 0;
    g_wakes = 0;
    g_expedites = 0;
    g_recordFrees = 0;
    g_yields = 0;
    g_panics = 0;
    g_callback = NULL;
    g_disableHook = NULL;
    g_deliveryStatus = OS_EOK;
}

// Runs ThreadExit until it reaches the yield. The yield stub records the
// thread state at that point and jumps back here.
static void
__RunThreadExit(
    _In_ int exitCode)
{
    g_yields = 0;
    g_yieldArmed = 1;
    if (setjmp(g_yieldExit) == 0) {
        ThreadExit(exitCode);
    }
    g_yieldArmed = 0;
}

// Runs the operation and reports whether an assertion failed. The failed
// assertion must come before any effect of the operation.
static int
__Panics(
    _In_ void (*operation)(int),
    _In_ int    argument)
{
    g_panics = 0;
    g_panicArmed = 1;
    if (setjmp(g_panicExit) == 0) {
        operation(argument);
    }
    g_panicArmed = 0;
    return g_panics != 0;
}

static void
__FinishOperation(
    _In_ int threadId)
{
    ThreadingFinish((uuid_t)threadId);
}

static void
__ExitOperation(
    _In_ int exitCode)
{
    ThreadExit(exitCode);
}

static void
__TestJoin(void)
{
    int exitCode;

    __Reset();
    __Check(ScThreadJoin(2, &exitCode) == OS_EOK && exitCode == 42, 1);
    __Check(g_waits == 1 && g_wakes == 1 && g_references[1] == 0, 2);
    __Check(atomic_load(&g_threads[0].JoinTarget) == UUID_INVALID, 3);
    __Check(ScThreadJoin(2, &exitCode) == OS_ENOENT, 4);

    __Reset();
    g_threads[1].State = ThreadState_TERMINATING;
    __Check(ScThreadJoin(2, &exitCode) == OS_EOK && g_waits == 1, 5);

    __Reset();
    __Check(ScThreadJoin(1, &exitCode) == OS_EINVALPARAMS, 6);
    g_threads[1].MemorySpace = &g_spaces[1];
    __Check(ScThreadJoin(2, &exitCode) != OS_EOK && g_waits == 0, 7);
    __Check(g_references[1] == 1, 8);
    g_threads[1].MemorySpace = &g_spaces[0];
    g_threads[1].ParentHandle = UUID_INVALID;
    __Check(ScThreadJoin(2, &exitCode) == OS_EINVALPARAMS, 9);

    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    g_threads[1].MemorySpace = NULL;
    __Check(ScThreadJoin(2, &exitCode) == OS_ENOENT, 10);
    __Check(ThreadMemorySpace(&g_threads[1]) == NULL, 11);
    __Check(ThreadDetach(2) == OS_ENOENT && g_references[1] == 1, 12);
}

static void
__TestForcedJoinExit(void)
{
    __Reset();
    // This is the state published by ThreadJoin before blocking. Finishing
    // runs in a different process and must release the abandoned target pin.
    g_threads[0].JoinTarget = 2;
    g_references[1]++;
    g_core.CurrentThread = &g_threads[2];
    g_threads[2].MemorySpace = &g_spaces[1];
    g_threads[0].State = ThreadState_TERMINATING;
    ThreadingFinish(1);
    __Check(g_references[0] == 0 && g_references[1] == 1, 20);
    __Check(g_threads[0].JoinTarget == UUID_INVALID, 21);
    g_threads[1].State = ThreadState_TERMINATING;
    ThreadingFinish(2);
    __Check(g_references[1] == 0, 22);
}

static void
__TestCreationFailure(void)
{
    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    __AbortThreadCreation(&g_threads[1]);
    __Check(g_references[1] == 0 && g_recordFrees == 0, 50);
    __Check(LookupGlobalHandle(2, HandleTypeThread) == NULL, 51);
    // Registered resources are freed later by the handle janitor.
    __DestroyThread(&g_threads[1]);
    __Check(g_recordFrees == 1 && g_references[2] == 1, 52);

    __Reset();
    g_threads[1].Handle = UUID_INVALID;
    g_threads[1].ParentHandle = UUID_INVALID;
    g_references[1] = 0;
    __AbortThreadCreation(&g_threads[1]);
    __Check(g_recordFrees == 1, 53);
}

static void
__TestParentListCleanup(void)
{
    __Reset();
    g_threads[2].Children = &g_threads[1];
    g_threads[1].Sibling = &g_threads[0];
    __Check(ThreadDetach(2) == OS_EOK, 60);
    __Check(g_threads[2].Children == &g_threads[0], 61);
    __Check(g_threads[1].ParentHandle == UUID_INVALID, 62);
    __Check(g_references[1] == 1 && g_references[2] == 1, 63);
    ReleaseGlobalHandle(2);
    __DestroyThread(&g_threads[1]);
    __Check(g_recordFrees == 1 && g_threads[2].Children == &g_threads[0], 64);

    __Reset();
    g_threads[2].Children = &g_threads[0];
    g_threads[0].Sibling = &g_threads[1];
    ReleaseGlobalHandle(2);
    __DestroyThread(&g_threads[1]);
    __Check(g_threads[2].Children == &g_threads[0], 65);
    __Check(g_threads[0].Sibling == NULL && g_recordFrees == 1, 66);
    __Check(g_references[2] == 1, 67);
}

static void
__TestSelfExit(void)
{
    __Reset();
    __RunThreadExit(17);

    // The thread must be flagged before it leaves the CPU. No extra reference
    // is taken, as the scheduler can finish the thread before ThreadExit would
    // get the chance to release it.
    __Check(g_yields == 1 && g_yieldState == ThreadState_TERMINATING, 40);
    __Check(g_references[0] == 1 && g_threads[0].RetCode == 17, 41);
    __Check(g_threads[0].State == ThreadState_TERMINATING && g_irqEnabled && g_yieldIrq, 42);
    ThreadingFinish(1);
    __Check(g_references[0] == 0, 43);

    __Reset();
    g_threads[0].Children = &g_threads[1];
    __RunThreadExit(23);
    __Check(g_yields == 1, 44);
    __Check(g_references[0] == 1 && g_references[1] == 1, 45);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 46);
    __Check(g_threads[1].RetCode == 23 && g_expedites == 1, 47);

    // Idle threads never exit, and must not get as far as the yield.
    __Reset();
    g_threads[0].Flags = THREADING_IDLE;
    __Check(__Panics(__ExitOperation, 9), 49);
    __Check(g_yields == 0 && g_threads[0].State == ThreadState_RUNNING, 89);
    __Check(g_threads[0].RetCode == 0, 90);
}

static void
__TestTerminateOther(void)
{
    // The caller is thread 1, and the target is thread 2.
    __Reset();
    __Check(ThreadTerminate(2, 31) == OS_EOK, 120);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 121);
    __Check(g_threads[1].RetCode == 31 && g_expedites == 1, 122);

    // The temporary reference is released, and the caller is left alone.
    __Check(g_references[0] == 1 && g_references[1] == 1, 123);
    __Check(g_threads[0].State == ThreadState_RUNNING && g_irqEnabled, 124);

    // The calling thread must use ThreadExit, which also leaves the CPU.
    __Reset();
    __Check(ThreadTerminate(1, 5) == OS_EINVALPARAMS, 125);
    __Check(g_threads[0].State == ThreadState_RUNNING && g_threads[0].RetCode == 0, 126);
    __Check(g_references[0] == 1 && g_expedites == 0, 127);

    __Reset();
    g_references[1] = 0;
    __Check(ThreadTerminate(2, 5) == OS_ENOENT, 128);
    __Check(g_threads[1].State == ThreadState_RUNNING && g_expedites == 0, 129);

    // A thread that is still being created cannot be found by id.
    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    __Check(ThreadTerminate(2, 5) == OS_ENOENT, 130);
    __Check(g_threads[1].State == ThreadState_INITIALIZING, 131);
    __Check(g_references[1] == 1 && g_expedites == 0, 132);

    __Reset();
    g_threads[1].Flags = THREADING_IDLE;
    __Check(ThreadTerminate(2, 5) == OS_EPERMISSIONS, 133);
    __Check(g_threads[1].State == ThreadState_RUNNING && g_references[1] == 1, 134);

    // Children go first, and every thread that is known to the scheduler is woken.
    __Reset();
    g_threads[1].Children = &g_threads[2];
    __Check(ThreadTerminate(2, 31) == OS_EOK, 135);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 136);
    __Check(g_threads[2].State == ThreadState_TERMINATING, 137);
    __Check(g_threads[2].RetCode == 31 && g_expedites == 2, 138);

    // A caller below the target is flagged too, but is not woken. It keeps
    // running until the scheduler switches away from it.
    __Reset();
    g_threads[1].Children = &g_threads[0];
    __Check(ThreadTerminate(2, 31) == OS_EOK, 139);
    __Check(g_threads[0].State == ThreadState_TERMINATING, 140);
    __Check(g_threads[0].RetCode == 31 && g_expedites == 1, 141);

    // A thread that is already stopping keeps its exit code.
    __Reset();
    g_threads[1].State = ThreadState_TERMINATING;
    g_threads[1].RetCode = 7;
    __Check(ThreadTerminate(2, 31) == OS_EOK, 142);
    __Check(g_threads[1].RetCode == 7 && g_expedites == 0, 143);
}

static void
__TestFinishState(void)
{
    // Only a flagged thread may be finished, and a failed check must not
    // change anything. Finishing a thread that still runs would free its stack.
    __Reset();
    g_threads[1].State = ThreadState_RUNNING;
    __Check(__Panics(__FinishOperation, 2), 150);
    __Check(g_threads[1].State == ThreadState_RUNNING, 151);
    __Check(g_references[1] == 1 && g_wakes == 0, 152);

    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    __Check(__Panics(__FinishOperation, 2), 153);
    __Check(g_threads[1].State == ThreadState_INITIALIZING, 154);
    __Check(g_references[1] == 1 && g_wakes == 0, 155);

    // The scheduler's reference is handed over once, so finishing twice is a bug.
    __Reset();
    g_threads[1].State = ThreadState_FINISHED;
    __Check(__Panics(__FinishOperation, 2), 156);
    __Check(g_references[1] == 1 && g_wakes == 0, 157);

    __Reset();
    g_threads[1].State = ThreadState_TERMINATING;
    __Check(!__Panics(__FinishOperation, 2), 158);
    __Check(g_threads[1].State == ThreadState_FINISHED, 159);
    __Check(g_references[1] == 0 && g_wakes == 1, 160);
}

static void
__QueueChildHook(void)
{
    __MarkThreadReady(&g_threads[1]);
}

static void
__TestRequestTermination(void)
{
    int previous;

    __Reset();
    previous = __RequestTermination(&g_threads[1]);
    __Check(previous == ThreadState_RUNNING, 80);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 81);

    g_threads[1].State = ThreadState_INITIALIZING;
    previous = __RequestTermination(&g_threads[1]);
    __Check(previous == ThreadState_INITIALIZING, 82);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 83);

    // A thread that is already stopping keeps its state.
    previous = __RequestTermination(&g_threads[1]);
    __Check(previous == ThreadState_TERMINATING, 84);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 85);
    g_threads[1].State = ThreadState_FINISHED;
    previous = __RequestTermination(&g_threads[1]);
    __Check(previous == ThreadState_FINISHED, 86);
    __Check(g_threads[1].State == ThreadState_FINISHED, 87);
}

static void
__TestTerminateInitializingChild(void)
{
    __Reset();

    // A child is already in its parent's list while it is being built. The
    // cascade must flag it, but it is not queued yet so nothing can be woken.
    g_threads[0].Children = &g_threads[1];
    g_threads[1].State = ThreadState_INITIALIZING;
    __RunThreadExit(23);
    __Check(g_yields == 1, 70);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 71);
    __Check(g_threads[1].RetCode == 23 && g_expedites == 0, 72);

    // The creator finishing afterwards must not undo the request.
    __MarkThreadReady(&g_threads[1]);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 73);

    // Without a request the creator publishes the thread as usual.
    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    __MarkThreadReady(&g_threads[1]);
    __Check(g_threads[1].State == ThreadState_RUNNING, 74);

    // The creator may queue the child after the cascade first looked at it,
    // but before the request is applied. The scheduler knows the child by
    // then, so it must be woken to be finished.
    __Reset();
    g_threads[0].Children = &g_threads[1];
    g_threads[1].State = ThreadState_INITIALIZING;
    g_disableHook = __QueueChildHook;
    __RunThreadExit(31);
    __Check(g_yields == 1, 75);
    __Check(g_threads[1].State == ThreadState_TERMINATING, 76);
    __Check(g_threads[1].RetCode == 31 && g_expedites == 1, 77);

    // A child that is already stopping keeps its exit code.
    __Reset();
    g_threads[0].Children = &g_threads[1];
    g_threads[1].State = ThreadState_FINISHED;
    g_threads[1].RetCode = 7;
    __RunThreadExit(23);
    __Check(g_yields == 1, 78);
    __Check(g_threads[1].State == ThreadState_FINISHED, 79);
    __Check(g_threads[1].RetCode == 7 && g_expedites == 0, 88);
}

static void
__TestSignalReferences(void)
{
    __Reset();
    g_threads[1].MemorySpace = &g_spaces[1];
    __Check(ScThreadSignal(2, 1) != OS_EOK && g_callback == NULL, 30);
    __Check(g_references[1] == 1, 31);
    g_threads[1].MemorySpace = &g_spaces[0];
    __Check(ScThreadSignal(2, 1) == OS_EOK && g_references[1] == 2, 32);
    g_threads[1].State = ThreadState_TERMINATING;
    ThreadingFinish(2);
    __Check(g_references[1] == 1, 33);
    g_callback(g_callbackArgument);
    __Check(g_references[1] == 0 && g_expedites == 0, 34);
    __Check(ScThreadSignal(2, 1) == OS_ENOENT, 35);

    __Reset();
    g_deliveryStatus = OS_EUNKNOWN;
    __Check(ScThreadSignal(2, 1) == OS_EUNKNOWN, 36);
    __Check(g_references[1] == 2 && g_callback != NULL, 37);
    // Failed interrupt delivery still leaves the callback queued. A never
    // started target has no active context and must not be dereferenced.
    g_callback(g_callbackArgument);
    __Check(g_references[1] == 1 && g_expedites == 0, 38);

    __Reset();
    g_threads[1].State = ThreadState_INITIALIZING;
    g_threads[1].MemorySpace = NULL;
    __Check(ScThreadSignal(2, 1) == OS_ENOENT && g_callback == NULL, 39);
}

void
_start(void)
{
    register long result __asm__("x0") = 0;
    register long number __asm__("x8") = 93;

    __TestJoin();
    __TestForcedJoinExit();
    __TestCreationFailure();
    __TestParentListCleanup();
    __TestSelfExit();
    __TestTerminateOther();
    __TestFinishState();
    __TestRequestTermination();
    __TestTerminateInitializingChild();
    __TestSignalReferences();
    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
