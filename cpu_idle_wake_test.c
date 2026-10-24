// Run production AP startup and then wake a blocked task on the idle AP itself.
#ifdef _MSC_VER
void _ReadBarrier(void);
void _ReadWriteBarrier(void);
void _WriteBarrier(void);
#endif
#define __need_minmax
#include <ddk/ddkdefs.h>

static void
__TestSetReserved(
    _In_ size_t    index,
    _In_ TLS_VALUE value);

// The host must not overwrite its own TLS while executing kernel startup.
#define __set_reserved __TestSetReserved
#ifndef CPU_SOURCE
#define CPU_SOURCE "../kernel/components/cpu.c"
#endif
#include CPU_SOURCE
#undef __set_reserved
#undef __TRACE
#include "../kernel/components/scheduler.c"
#include "../librt/libds/list.c"
#include "../librt/libds/queue.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static SystemCpuCore_t g_core;
static SystemMachine_t g_machine;
static MemorySpace_t g_memorySpace;
static unsigned int g_yields;
static int g_interruptsEnabled;

// Fail if startup or the wake path unexpectedly needs these unrelated services.
void*
kmalloc(
    _In_ size_t size)
{
    abort();
}

void
kfree(
    _In_ void* memory)
{
    abort();
}

void
ArchPlatformInitialize(
    _In_ SystemCpu_t*     cpu,
    _In_ SystemCpuCore_t* core)
{
    abort();
}

oserr_t
DestroyHandle(
    _In_ uuid_t handleId)
{
    abort();
}

void
InterruptSetActiveStatus(
    _In_ int active)
{
    abort();
}

void
InterruptsSetPriority(
    _In_ uint32_t priority)
{
    abort();
}

unsigned int
ThreadFlags(
    _In_ Thread_t* thread)
{
    return thread->Flags;
}

SchedulerObject_t*
ThreadSchedulerHandle(
    _In_ Thread_t* thread)
{
    abort();
}

void
SystemTimerStall(
    _In_ OSTimestamp_t* deadline)
{
    abort();
}

void
SpinlockConstruct(
    _In_ Spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

void
ReadVolatileMemory(
    _In_ const volatile void* pointer,
    _In_ volatile void*       out,
    _In_ size_t               length)
{
    memcpy((void*)out, (const void*)pointer, length);
}

static void
__TestSetReserved(
    _In_ size_t    index,
    _In_ TLS_VALUE value)
{
    CHECK(index == 0);
    CHECK(value == (uintptr_t)&g_core);
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 1;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

SystemDomain_t*
GetCurrentDomain(void)
{
    return NULL;
}

oserr_t
CreateMemorySpace(
    _In_  unsigned int flags,
    _Out_ uuid_t*      handleOut)
{
    CHECK(flags == MEMORY_SPACE_INHERIT);
    *handleOut = 99;
    return OS_EOK;
}

void*
LookupHandleOfType(
    _In_ uuid_t       id,
    _In_ HandleType_t type)
{
    CHECK(id == 99 && type == HandleTypeMemorySpace);
    return &g_memorySpace;
}

void
MemorySpaceSwitch(
    _In_ MemorySpace_t* memorySpace)
{
    CHECK(memorySpace == &g_memorySpace);
}

void
ThreadingEnable(
    _In_ SystemCpuCore_t* core)
{
    CHECK(core == &g_core);
    core->IdleThread.Flags = THREADING_IDLE;
    core->CurrentThread = &core->IdleThread;
}

int
ThreadIsCurrentIdle(
    _In_ uuid_t coreId)
{
    CHECK(coreId == g_core.Id);
    return g_core.CurrentThread == &g_core.IdleThread;
}

const char*
ThreadName(
    _In_ Thread_t* thread)
{
    return "idle-wake-test";
}

irqstate_t
InterruptEnable(void)
{
    g_interruptsEnabled = 1;
    return 0;
}

void
ArchThreadYield(void)
{
    CHECK(g_interruptsEnabled);
    g_yields++;
}

void
WriteVolatileMemory(
    _In_ volatile void* pointer,
    _In_ void*          data,
    _In_ size_t         length)
{
    memcpy((void*)pointer, data, length);
}

void
SystemTimerGetWallClockTime(
    _Out_ OSTimestamp_t* time)
{
    time->Seconds = 1;
    time->Nanoseconds = 0;
}

void
SpinlockAcquireIrq(
    _In_ Spinlock_t* lock)
{
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* lock)
{
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
}

void
DebugPanic(
    _In_ int         scope,
    _In_ Context_t*  context,
    _In_ const char* message, ...)
{
    abort();
}

void
ArchProcessorHalt(void)
{
    abort();
}

void
StartApplicationCore(
    _In_ SystemCpuCore_t* core)
{
    abort();
}

oserr_t
TxuMessageSend(
    _In_ uuid_t                  coreId,
    _In_ SystemCpuFunctionType_t type,
    _In_ TxuFunction_t           function,
    _In_ void*                   argument,
    _In_ int                     asynchronous)
{
    // A same-core wake must never need a remote message.
    abort();
}

void
ArchProcessorIdle(void)
{
    SchedulerObject_t sleeper;
    unsigned int      yieldsBeforeWake;
    clock_t           deadline;

    CHECK(g_core.State == CpuStateRunning);
    CHECK(atomic_load(&g_machine.NumberOfActiveCores) == 1);
    CHECK(g_interruptsEnabled);

    // No runnable task and no timer deadline: only the subsequent wake can
    // request a scheduling interrupt to get this AP out of idle.
    CHECK(SchedulerAdvance(NULL, 0, 0, &deadline) == NULL);
    CHECK(deadline == 0);
    yieldsBeforeWake = g_yields;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.State = STATE_BLOCKED;
    sleeper.CoreId = g_core.Id;
    sleeper.TimeSlice = SCHEDULER_TIMESLICE_INITIAL;
    sleeper.TimeSliceLeft = sleeper.TimeSlice;
    sleeper.Object = &sleeper;
    CHECK(SchedulerQueueObject(&sleeper) == OS_EOK);
    CHECK(g_yields == yieldsBeforeWake + 1);
    CHECK(SchedulerAdvance(NULL, 0, 0, &deadline) == &sleeper);
    CHECK(sleeper.State == STATE_RUNNING);
    CHECK(deadline == sleeper.TimeSlice);
    CHECK(g_core.Scheduler.Enabled);
    puts("AP startup: same-core idle wake requests scheduling and runs the task");
    exit(0);
}

int
main(void)
{
    __ConstructCpuCore(&g_core, 1, CpuStateShutdown, 0);
    g_coreTable[g_core.Id] = &g_core;
    CpuCoreStart();
    return 1;
}
