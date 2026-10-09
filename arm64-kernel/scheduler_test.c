/** Exercise sleep deadlines using the production scheduler on AArch64. */
#include "../../kernel/components/scheduler.c"

static Scheduler_t g_scheduler;
static char g_core;
static OSTimestamp_t g_now;
static unsigned int g_waitQueueRemovals;

void*
memset(
    _Out_ void* destination,
    _In_  int   value,
    _In_  size_t length)
{
    unsigned char* bytes = destination;
    size_t index;

    for (index = 0; index < length; index++) {
        bytes[index] = (unsigned char)value;
    }
    return destination;
}

static void
__Exit(
    _In_ int code)
{
    register long value __asm__("x0") = code;
    register long number __asm__("x8") = 93;

    __asm__ volatile("svc 0" :: "r"(value), "r"(number) : "memory");
    __builtin_unreachable();
}

static void
__Check(
    _In_ int condition,
    _In_ int code)
{
    if (!condition) {
        __Exit(code);
    }
}

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return (SystemCpuCore_t*)&g_core;
}

Scheduler_t*
CpuCoreScheduler(
    _In_ SystemCpuCore_t* core)
{
    __Check((void*)core == &g_core, 1);
    return &g_scheduler;
}

void
SystemTimerGetWallClockTime(
    _In_ OSTimestamp_t* time)
{
    *time = g_now;
}

void
DebugPanic(
    _In_ int         scope,
    _In_ Context_t*  context,
    _In_ const char* message, ...)
{
    __Exit(2);
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    __Exit(3);
}

void
_assert_panic(
    _In_ const char* message)
{
    __Exit(4);
}

int
list_remove(
    _In_ list_t*    list,
    _In_ element_t* element)
{
    g_waitQueueRemovals++;
    return 0;
}

static void
__TestDeadlineSelection(void)
{
    Scheduler_t scheduler = {0};
    SchedulerObject_t first = {0};
    SchedulerObject_t second = {0};
    OSTimestamp_t now = {0};

    __Check(!__HasDeadlineSet(&first), 10);
    first.WakeUpTime.Nanoseconds = 5000000;
    __Check(__HasDeadlineSet(&first), 11);
    second.WakeUpTime.Seconds = 1;
    __Check(__HasDeadlineSet(&second), 12);

    // The list is not ordered. Its final entry must not hide an earlier wakeup.
    first.Link = &second;
    scheduler.SleepQueue.Head = &first;
    scheduler.SleepQueue.Tail = &second;
    __Check(__UpdateSleepQueue(&scheduler, &now) == 5000000, 13);
    first.WakeUpTime.Seconds = 2;
    __Check(__UpdateSleepQueue(&scheduler, &now) == 1000000000, 14);
}

static void
__TestConsecutiveTimeouts(void)
{
    Scheduler_t scheduler = {0};
    SchedulerObject_t first = {0};
    SchedulerObject_t second = {0};
    SchedulerObject_t third = {0};
    OSTimestamp_t now = {.Seconds = 1};
    list_t waitQueue = {0};

    first.State = STATE_BLOCKED;
    first.WakeUpTime.Nanoseconds = 5000000;
    first.WaitQueueHandle = &waitQueue;
    first.Link = &second;
    second.State = STATE_BLOCKED;
    second.WakeUpTime.Seconds = 1;
    second.Link = &third;
    third.State = STATE_BLOCKED;
    third.WakeUpTime.Seconds = 2;
    scheduler.SleepQueue.Head = &first;
    scheduler.SleepQueue.Tail = &third;

    // Requeueing the first expired object changes its Link. The second one
    // must still expire in this pass, including an exact-second deadline.
    __Check(__UpdateSleepQueue(&scheduler, &now) == 1000000000, 20);
    __Check(first.State == STATE_QUEUED && second.State == STATE_QUEUED, 21);
    __Check(first.TimeoutReason == OS_ETIMEOUT && second.TimeoutReason == OS_ETIMEOUT, 22);
    __Check(!__HasDeadlineSet(&first) && !__HasDeadlineSet(&second), 23);
    __Check(g_waitQueueRemovals == 1, 24);
    __Check(scheduler.SleepQueue.Head == &third && scheduler.SleepQueue.Tail == &third, 25);
    __Check(scheduler.Queues[0].Head == &first && scheduler.Queues[0].Tail == &second, 26);
    __Check(first.Link == &second && second.Link == NULL, 27);
}

static void
__TestLastRunnableSleeps(void)
{
    SchedulerObject_t sleeper = {0};
    clock_t nextDeadline = 0;
    void* next;

    sleeper.State = STATE_BLOCKING;
    sleeper.WakeUpTime.Nanoseconds = 5000000;
    sleeper.TimeSlice = SCHEDULER_TIMESLICE_INITIAL;
    sleeper.Object = &sleeper;
    next = SchedulerAdvance(&sleeper, 0, 0, &nextDeadline);
    __Check(next == NULL && nextDeadline == 5000000, 30);
    __Check(sleeper.State == STATE_BLOCKED, 31);

    // An idle core still needs a timer deadline to resume this thread.
    g_now.Nanoseconds = 5000000;
    next = SchedulerAdvance(NULL, 1, 5000000, &nextDeadline);
    __Check(next == &sleeper && sleeper.State == STATE_RUNNING, 32);
    __Check(nextDeadline == SCHEDULER_TIMESLICE_INITIAL, 33);
    __Check(g_scheduler.SleepQueue.Head == NULL && g_scheduler.SleepQueue.Tail == NULL, 34);

    // A wait without a deadline must permit the timer to stop on an idle core.
    sleeper.State = STATE_BLOCKING;
    next = SchedulerAdvance(&sleeper, 0, 0, &nextDeadline);
    __Check(next == NULL && nextDeadline == 0, 35);
}

void
_start(void)
{
    __TestDeadlineSelection();
    __TestConsecutiveTimeouts();
    __TestLastRunnableSleeps();
    __Exit(0);
}
