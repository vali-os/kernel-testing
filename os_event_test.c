// Exercise OSEventLock and OSEventUnlock against a scripted futex. A failed
// wait must return its error and leave the counter as it is. A wait the
// kernel refuses because the counter changed is not a failure. OSEventUnlock
// must wake one waiter per unit it added, and never raise the counter above
// its maximum.
#ifndef OS_EVENT_SOURCE
#define OS_EVENT_SOURCE "../librt/libos/event.c"
#endif
#include OS_EVENT_SOURCE
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define MAX_WAIT_STEPS 4

// One scripted futex wait. Before the wait returns, the counter is set to
// Counter when SetCounter is true, as if another thread changed it.
struct __WaitStep {
    bool    SetCounter;
    int     Counter;
    oserr_t Result;
};

static _Atomic(int)          g_counter;
static struct Event          g_event = { .SyncAddress = &g_counter, .MaxValue = 1 };
static OSHandle_t            g_handle = { .ID = 5, .Payload = &g_event };
static struct thread_storage g_threadStorage;
static struct __WaitStep     g_waitSteps[MAX_WAIT_STEPS];
static unsigned int          g_waitStepCount;
static unsigned int          g_waits;
static unsigned int          g_wakes;
static int                   g_lastWakeCount;
static oserr_t               g_wakeResult;
static unsigned int          g_errors;

oserr_t
OSFutex(
    _In_ OSFutexParameters_t* parameters,
    _In_ OSAsyncContext_t*    asyncContext)
{
    struct __WaitStep* step;

    (void)asyncContext;
    CHECK(parameters->Futex0 == &g_counter);
    if (FUTEX_FLAG_ACTION(parameters->Flags) != FUTEX_FLAG_WAIT) {
        g_wakes++;
        g_lastWakeCount = parameters->Expected0;
        return g_wakeResult;
    }

    // A wait is only made for the value the caller just read.
    CHECK(g_waits < g_waitStepCount);
    CHECK(parameters->Expected0 == atomic_load(&g_counter));
    step = &g_waitSteps[g_waits++];
    if (step->SetCounter) {
        atomic_store(&g_counter, step->Counter);
    }
    return step->Result;
}

struct thread_storage*
__tls_current(void)
{
    return &g_threadStorage;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
}

// Only OSEvent and OSTimeoutEvent make system calls, and they are not tested.
SCTYPE
syscall1(SCTYPE function, SCTYPE arg0)
{
    (void)function;
    (void)arg0;
    abort();
}

SCTYPE
syscall4(SCTYPE function, SCTYPE arg0, SCTYPE arg1, SCTYPE arg2, SCTYPE arg3)
{
    (void)function;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    abort();
}

oserr_t
OSHandleWrap(
    _In_ uuid_t            id,
    _In_ enum OSHandleType type,
    _In_ void*             payload,
    _In_ bool              ownership,
    _In_ struct OSHandle*  handle)
{
    (void)id;
    (void)type;
    (void)payload;
    (void)ownership;
    (void)handle;
    abort();
}

static void
__Reset(
    _In_ int          counter,
    _In_ unsigned int maxValue)
{
    atomic_store(&g_counter, counter);
    g_event.MaxValue = maxValue;
    g_waitStepCount = 0;
    g_waits = 0;
    g_wakes = 0;
    g_lastWakeCount = 0;
    g_wakeResult = OS_EOK;
    g_errors = 0;
}

static void
__AddWaitStep(
    _In_ bool    setCounter,
    _In_ int     counter,
    _In_ oserr_t result)
{
    CHECK(g_waitStepCount < MAX_WAIT_STEPS);
    g_waitSteps[g_waitStepCount++] = (struct __WaitStep) {
        .SetCounter = setCounter,
        .Counter = counter,
        .Result = result
    };
}

static void
__TestLock(void)
{
    // A raised counter is taken without a wait.
    __Reset(1, 1);
    CHECK(OSEventLock(&g_handle, 0) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 0);

    // A non-blocking lock on an empty counter does not wait or change it.
    __Reset(0, 1);
    CHECK(OSEventLock(&g_handle, OSEVENT_LOCK_NONBLOCKING) == OS_EBUSY);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 0);

    // A wait that ends after a signal takes the unit.
    __Reset(0, 1);
    __AddWaitStep(true, 1, OS_EOK);
    CHECK(OSEventLock(&g_handle, 0) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 1);

    // A failed wait returns its error and leaves the counter at 0. It used to
    // take a unit anyway, which made the counter -1.
    __Reset(0, 1);
    __AddWaitStep(false, 0, OS_ETIMEOUT);
    CHECK(OSEventLock(&g_handle, 0) == OS_ETIMEOUT);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 1);

    // An interrupted wait with an unchanged counter is a failure too.
    __Reset(0, 1);
    __AddWaitStep(false, 0, OS_EINTERRUPTED);
    CHECK(OSEventLock(&g_handle, 0) == OS_EINTERRUPTED);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 1);

    // The kernel refuses the wait with OS_EINTERRUPTED when the counter was
    // raised before the thread got to sleep. That is not a failure: the unit
    // is taken and the lock succeeds. It used to take the unit and still
    // return the error, so the signal was lost.
    __Reset(0, 1);
    __AddWaitStep(true, 1, OS_EINTERRUPTED);
    CHECK(OSEventLock(&g_handle, 0) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 1);

    // A wake that finds the unit already taken by someone else waits again.
    __Reset(0, 1);
    __AddWaitStep(false, 0, OS_EOK);
    __AddWaitStep(true, 1, OS_EOK);
    CHECK(OSEventLock(&g_handle, 0) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 0 && g_waits == 2);
    puts("OSEventLock: taken, busy, woken, failed and refused waits passed");
}

static void
__TestUnlock(void)
{
    // One unit is added and one waiter is woken.
    __Reset(0, 1);
    CHECK(OSEventUnlock(&g_handle, 1) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 1 && g_wakes == 1 && g_lastWakeCount == 1);

    // At the maximum nothing is added and nobody is woken.
    CHECK(OSEventUnlock(&g_handle, 1) == OS_EINCOMPLETE);
    CHECK(atomic_load(&g_counter) == 1 && g_wakes == 1);

    // Several units stop at the maximum, and only the added units are woken.
    __Reset(1, 3);
    CHECK(OSEventUnlock(&g_handle, 5) == OS_EOK);
    CHECK(atomic_load(&g_counter) == 3 && g_wakes == 1 && g_lastWakeCount == 2);

    // A wake with nobody asleep is normal. Any other failed wake is logged,
    // but the units stay added, so the unlock still succeeds.
    __Reset(0, 1);
    g_wakeResult = OS_ENOENT;
    CHECK(OSEventUnlock(&g_handle, 1) == OS_EOK && g_errors == 0);
    __Reset(0, 1);
    g_wakeResult = OS_EUNKNOWN;
    CHECK(OSEventUnlock(&g_handle, 1) == OS_EOK && g_errors == 1);
    CHECK(atomic_load(&g_counter) == 1);
    puts("OSEventUnlock: maximum, wake count and failed wakes passed");
}

int
main(void)
{
    __TestLock();
    __TestUnlock();
    return 0;
}
