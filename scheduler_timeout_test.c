// Exercise the production scheduler with deterministic delivery of remote wakeups.
#ifdef _MSC_VER
void _ReadBarrier(void);
void _ReadWriteBarrier(void);
#endif
#ifndef SCHEDULER_SOURCE
#define SCHEDULER_SOURCE "../kernel/components/scheduler.c"
#endif
#include SCHEDULER_SOURCE
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    printf("line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

static Scheduler_t g_scheduler;
static char g_cores[2];
static unsigned int g_currentCore;
static OSTimestamp_t g_now;
static TxuFunction_t g_pendingWake;
static void* g_pendingObject;
static unsigned int g_warnings;

// These platform services are outside the scheduling paths exercised here.
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
WriteVolatileMemory(
    _In_ volatile void* pointer,
    _In_ void*          data,
    _In_ size_t         length)
{
    abort();
}

SystemDomain_t*
GetCurrentDomain(void)
{
    abort();
}

SystemMachine_t*
GetMachine(void)
{
    abort();
}

SystemCpuCore_t*
CpuCoreNext(
    _In_ SystemCpuCore_t* core)
{
    abort();
}

SystemCpuState_t
CpuCoreState(
    _In_ SystemCpuCore_t* core)
{
    abort();
}

SystemCpuCore_t*
GetProcessorCore(
    _In_ uuid_t coreId)
{
    abort();
}

Thread_t*
CpuCoreCurrentThread(
    _In_ SystemCpuCore_t* core)
{
    abort();
}

SchedulerObject_t*
ThreadSchedulerHandle(
    _In_ Thread_t* thread)
{
    abort();
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return g_currentCore;
}

void
SystemTimerStall(
    _In_ OSTimestamp_t* deadline)
{
    abort();
}

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return (SystemCpuCore_t*)&g_cores[g_currentCore];
}

uuid_t
CpuCoreId(
    _In_ SystemCpuCore_t* core)
{
    return (uuid_t)((char*)core - g_cores);
}

Scheduler_t*
CpuCoreScheduler(
    _In_ SystemCpuCore_t* core)
{
    return &g_scheduler;
}

int
ThreadIsCurrentIdle(
    _In_ uuid_t coreId)
{
    return 0;
}

const char*
ThreadName(
    _In_ Thread_t* thread)
{
    return "timeout-test";
}

void
ArchThreadYield(void)
{
    abort();
}

void
SystemTimerGetWallClockTime(
    _Out_ OSTimestamp_t* time)
{
    *time = g_now;
}

void
SpinlockConstruct(
    _In_ Spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
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
DebugPanic(
    _In_ int         scope,
    _In_ Context_t*  context,
    _In_ const char* message, ...)
{
    puts(message);
    abort();
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    CHECK(level == OSSYSLOGLEVEL_WARNING);
    puts(format);
    g_warnings++;
}

oserr_t
TxuMessageSend(
    _In_ uuid_t                  coreId,
    _In_ SystemCpuFunctionType_t type,
    _In_ TxuFunction_t           function,
    _In_ void*                   argument,
    _In_ int                     asynchronous)
{
    CHECK(g_currentCore == 1 && coreId == 0);
    CHECK(type == CpuFunctionCustom && asynchronous);
    CHECK(g_pendingWake == NULL);
    g_pendingWake = function;
    g_pendingObject = argument;
    return OS_EOK;
}

static void
__Reset(void)
{
    memset(&g_scheduler, 0, sizeof(g_scheduler));
    g_currentCore = 0;
    g_now.Seconds = 1;
    g_now.Nanoseconds = 0;
    g_pendingWake = NULL;
    g_pendingObject = NULL;
    g_warnings = 0;
}

static void
__InitializeSleeper(
    _Out_ SchedulerObject_t* object,
    _In_  list_t*            waitQueue)
{
    memset(object, 0, sizeof(*object));
    ELEMENT_INIT(&object->Header, 0, object);
    object->State = STATE_BLOCKED;
    object->WakeUpTime = g_now;
    object->TimeSlice = SCHEDULER_TIMESLICE_INITIAL;
    object->TimeSliceLeft = object->TimeSlice;
    object->Object = object;
    object->WaitQueueHandle = waitQueue;
    if (waitQueue != NULL) {
        list_append(waitQueue, &object->Header);
    }
    __AppendToQueue(&g_scheduler.SleepQueue, object, object);
}

static void
__DeliverWake(void)
{
    TxuFunction_t function = g_pendingWake;
    void*         object = g_pendingObject;

    CHECK(function != NULL);
    g_currentCore = 0;
    g_pendingWake = NULL;
    g_pendingObject = NULL;
    function(object);
}

static void
__TestRemoteWakeWins(
    _In_ int expedited)
{
    SchedulerObject_t sleeper;
    SchedulerObject_t nextSleeper;
    list_t            waitQueue;
    clock_t           deadline;

    __Reset();
    list_construct(&waitQueue);
    __InitializeSleeper(&sleeper, &waitQueue);
    __InitializeSleeper(&nextSleeper, NULL);
    nextSleeper.WakeUpTime.Seconds++;

    // The remote CPU claims the wakeup, but its interrupt cannot run while
    // the owning CPU is handling the timer. Keep that callback pending.
    g_currentCore = 1;
    if (expedited) {
        SchedulerExpediteObject(&sleeper);
    } else {
        CHECK(list_remove(&waitQueue, &sleeper.Header) == 0);
        CHECK(SchedulerQueueObject(&sleeper) == OS_EOK);
    }
    CHECK(sleeper.State == STATE_QUEUEING && g_pendingWake != NULL);
    CHECK(list_count(&waitQueue) == 0);

    g_currentCore = 0;
    CHECK(SchedulerAdvance(NULL, 1, 0, &deadline) == NULL);
    CHECK(g_warnings == 0);
    CHECK(deadline == NSEC_PER_SEC);
    CHECK(g_scheduler.SleepQueue.Head == &nextSleeper);
    CHECK(g_scheduler.SleepQueue.Tail == &nextSleeper && sleeper.Link == NULL);
    CHECK(sleeper.TimeoutReason == (expedited ? OS_EINTERRUPTED : OS_EOK));
    CHECK(sleeper.WakeUpTime.Seconds == g_now.Seconds);
    CHECK(sleeper.WakeUpTime.Nanoseconds == g_now.Nanoseconds);
    CHECK(sleeper.State == STATE_QUEUEING);

    // Another timer pass must neither retry the old timeout nor make the
    // thread runnable before the winning wakeup finishes its bookkeeping.
    CHECK(SchedulerAdvance(NULL, 1, 0, &deadline) == NULL);
    __DeliverWake();
    CHECK(SchedulerAdvance(NULL, 1, 0, &deadline) == &sleeper);
    CHECK(sleeper.State == STATE_RUNNING);
    CHECK(g_scheduler.Queues[0].Head == NULL && g_scheduler.Queues[0].Tail == NULL);
    CHECK(sleeper.TimeoutReason == (expedited ? OS_EINTERRUPTED : OS_EOK));
    CHECK(g_warnings == 0);

    // Reusing the object for a new timed wait must still expire normally.
    sleeper.State = STATE_BLOCKING;
    sleeper.WakeUpTime = g_now;
    sleeper.TimeoutReason = OS_EOK;
    sleeper.WaitQueueHandle = &waitQueue;
    list_append(&waitQueue, &sleeper.Header);
    CHECK(SchedulerAdvance(&sleeper, 0, 0, &deadline) == &sleeper);
    CHECK(sleeper.TimeoutReason == OS_ETIMEOUT && list_count(&waitQueue) == 0);
    CHECK(!__HasDeadlineSet(&sleeper) && g_warnings == 0);
}

static void
__TestTimeoutWins(void)
{
    SchedulerObject_t first;
    SchedulerObject_t second;
    list_t            waitQueue;
    clock_t           deadline;

    __Reset();
    list_construct(&waitQueue);
    __InitializeSleeper(&first, &waitQueue);
    __InitializeSleeper(&second, NULL);
    CHECK(SchedulerAdvance(NULL, 1, 0, &deadline) == &first);
    CHECK(first.TimeoutReason == OS_ETIMEOUT && second.TimeoutReason == OS_ETIMEOUT);
    CHECK(!__HasDeadlineSet(&first) && !__HasDeadlineSet(&second));
    CHECK(list_count(&waitQueue) == 0);
    CHECK(g_scheduler.SleepQueue.Head == NULL && g_scheduler.SleepQueue.Tail == NULL);

    g_currentCore = 1;
    SchedulerExpediteObject(&first);
    SchedulerExpediteObject(&second);
    CHECK(g_pendingWake == NULL);
    CHECK(first.TimeoutReason == OS_ETIMEOUT && second.TimeoutReason == OS_ETIMEOUT);
    g_currentCore = 0;
    CHECK(SchedulerAdvance(NULL, 1, 0, &deadline) == &second);
    CHECK(g_scheduler.Queues[0].Head == NULL && g_warnings == 0);
}

static void
__TestPendingWakeLinks(
    _In_ int pendingIndex)
{
    SchedulerObject_t sleepers[3];
    int               index;

    __Reset();
    for (index = 0; index < 3; index++) {
        __InitializeSleeper(&sleepers[index], NULL);
    }
    g_currentCore = 1;
    CHECK(SchedulerQueueObject(&sleepers[pendingIndex]) == OS_EOK);
    g_currentCore = 0;
    CHECK(__UpdateSleepQueue(&g_scheduler, &g_now) == __MASK);
    CHECK(g_scheduler.SleepQueue.Head == NULL && g_scheduler.SleepQueue.Tail == NULL);
    CHECK(sleepers[pendingIndex].State == STATE_QUEUEING);
    CHECK(sleepers[pendingIndex].TimeoutReason == OS_EOK);
    CHECK(g_warnings == 0);

    // Removing a pending wake from the head, middle or tail must not skip
    // another expired sleeper or damage the runnable list's links.
    for (index = 0; index < 3; index++) {
        if (index != pendingIndex) {
            CHECK(g_scheduler.Queues[0].Head == &sleepers[index]);
            CHECK(sleepers[index].TimeoutReason == OS_ETIMEOUT);
            CHECK(__RemoveFromQueue(&g_scheduler.Queues[0], &sleepers[index]) == OS_EOK);
        }
    }
    __DeliverWake();
    CHECK(g_scheduler.Queues[0].Head == &sleepers[pendingIndex]);
    CHECK(g_scheduler.Queues[0].Tail == &sleepers[pendingIndex]);
    CHECK(sleepers[pendingIndex].Link == NULL && g_warnings == 0);
}

static void
__TestInvalidState(void)
{
    SchedulerObject_t sleeper;

    __Reset();
    __InitializeSleeper(&sleeper, NULL);
    sleeper.State = STATE_BLOCKING;
    __PerformObjectTimeout(&g_scheduler, &sleeper);
    CHECK(g_warnings == 1);
    CHECK(sleeper.State == STATE_BLOCKING && sleeper.TimeoutReason == OS_EOK);
    CHECK(g_scheduler.Queues[0].Head == NULL);
}

int
main(void)
{
    __TestRemoteWakeWins(0);
    __TestRemoteWakeWins(1);
    __TestTimeoutWins();
    __TestPendingWakeLinks(0);
    __TestPendingWakeLinks(1);
    __TestPendingWakeLinks(2);
    __TestInvalidState();
    puts("Scheduler timeout: delayed remote wake, interruption, timeout winner and repeated waits passed");
    return 0;
}
