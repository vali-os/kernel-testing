/* Exercise mutex waiters that lose the lock after being woken. */
#ifndef MUTEX_SOURCE
#define MUTEX_SOURCE "../kernel/sync/mutex.c"
#endif
#include MUTEX_SOURCE
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Mutex waiter check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static SystemMachine_t g_machine;
static Mutex_t         g_mutex;
static element_t       g_waiter;
static uuid_t          g_currentThread;
static int             g_blockCount;
static int             g_wakeCount;
static int             g_competingLocks;
static int             g_lockHeld;
static irqstate_t      g_interruptsEnabled;

uuid_t
ThreadCurrentHandle(void)
{
    return g_currentThread;
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

irqstate_t
InterruptDisable(void)
{
    irqstate_t previous = g_interruptsEnabled;

    g_interruptsEnabled = 0;
    return previous;
}

irqstate_t
InterruptRestoreState(
    _In_ irqstate_t state)
{
    irqstate_t previous = g_interruptsEnabled;

    g_interruptsEnabled = state;
    return previous;
}

void
SpinlockConstruct(
    _In_ Spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

void
SpinlockAcquire(
    _In_ Spinlock_t* lock)
{
    CHECK(lock == &g_mutex.Lock);
    CHECK(!g_lockHeld);
    g_lockHeld = 1;
}

void
SpinlockRelease(
    _In_ Spinlock_t* lock)
{
    CHECK(lock == &g_mutex.Lock);
    CHECK(g_lockHeld);
    g_lockHeld = 0;
}

void
SpinlockAcquireIrq(
    _In_ Spinlock_t* lock)
{
    CHECK(lock == &g_mutex.BlockQueue.lock);
    CHECK(atomic_load(&lock->Current) == 0);
    lock->IrqState = InterruptDisable();
    atomic_store(&lock->Current, 1);
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* lock)
{
    CHECK(lock == &g_mutex.BlockQueue.lock);
    CHECK(atomic_load(&lock->Current) == 1);
    atomic_store(&lock->Current, 0);
    InterruptRestoreState(lock->IrqState);
}

oserr_t
SchedulerBlock(
    _In_ list_t*        queue,
    _In_ OSTimestamp_t* deadline)
{
    CHECK(queue == &g_mutex.BlockQueue);
    CHECK(deadline == NULL);
    CHECK(g_currentThread == 7);
    CHECK(g_lockHeld && !g_interruptsEnabled);
    CHECK(list_count(queue) == 0);
    CHECK(g_blockCount == g_wakeCount);
    list_append(queue, &g_waiter);
    g_blockCount++;
    return OS_EOK;
}

oserr_t
SchedulerQueueObject(
    _In_ SchedulerObject_t* object)
{
    CHECK((void*)object == &g_waiter);
    CHECK(g_lockHeld);
    CHECK(list_count(&g_mutex.BlockQueue) == 0);
    g_wakeCount++;
    return OS_EOK;
}

oserr_t
SchedulerGetTimeoutReason(void)
{
    CHECK(g_wakeCount == g_blockCount);
    return OS_EOK;
}

void
ArchThreadYield(void)
{
    CHECK(g_currentThread == 7);
    CHECK(!g_lockHeld && g_interruptsEnabled);

    // Run the current holder until it wakes the blocked thread.
    g_currentThread = atomic_load(&g_mutex.Owner);
    CHECK(g_currentThread != UUID_INVALID && g_currentThread != 7);
    MutexUnlock(&g_mutex);
    CHECK(g_wakeCount == g_blockCount);
    CHECK(list_count(&g_mutex.BlockQueue) == 0);

    // A competing thread may take the lock before the waiter resumes.
    if (g_competingLocks > 0) {
        g_currentThread = 11;
        MutexLock(&g_mutex);
        CHECK(atomic_load(&g_mutex.Owner) == 11);
        g_competingLocks--;
    }
    g_currentThread = 7;
}

static void
__TestWaiter(
    _In_ int coreCount,
    _In_ int competingLocks)
{
    atomic_store(&g_machine.NumberOfActiveCores, coreCount);
    MutexConstruct(&g_mutex, MUTEX_FLAG_PLAIN);
    memset(&g_waiter, 0, sizeof(g_waiter));
    g_waiter.value = &g_waiter;
    g_blockCount = 0;
    g_wakeCount = 0;
    g_competingLocks = competingLocks;
    g_interruptsEnabled = 1;

    g_currentThread = 9;
    MutexLock(&g_mutex);
    CHECK(atomic_load(&g_mutex.Owner) == 9);
    g_currentThread = 7;
    MutexLock(&g_mutex);

    CHECK(g_blockCount == competingLocks + 1);
    CHECK(g_wakeCount == g_blockCount);
    CHECK(atomic_load(&g_mutex.Owner) == 7);
    CHECK(g_mutex.ReferenceCount == 1);
    CHECK(list_count(&g_mutex.BlockQueue) == 0);
    CHECK(!g_lockHeld && g_interruptsEnabled);
    MutexUnlock(&g_mutex);
    CHECK(atomic_load(&g_mutex.Owner) == UUID_INVALID);
    CHECK(g_mutex.ReferenceCount == 0);
    printf("Mutex waiter: %d cores, %d competing locks passed\n", coreCount, competingLocks);
}

int
main(void)
{
    __TestWaiter(1, 0);
    __TestWaiter(4, 0);
    __TestWaiter(1, 1);
    __TestWaiter(4, 1);
    __TestWaiter(4, 8);
    return 0;
}
