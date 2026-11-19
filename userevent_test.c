/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * UserEventSignal against the real handle table and I/O sets.
 * - A reader that sleeps on the counter runs in its own thread, so a missing
 *   wake shows up as a timed out wait. The futex below is a small model: only
 *   the counter under test can sleep, and every I/O set wait returns at once,
 *   as in the other handle suites.
 */

// This suite supplies its own futex and SHM export functions.
#define HANDLE_TEST_REAL_SHM
#include "handle_test_support.inc"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>

// The parts of the memory and heap interfaces that UserEventCreate uses. The
// tests build their events by hand and never call it.
typedef uintptr_t vaddr_t;
typedef struct MemoryCache MemoryCache_t;
struct MemorySpaceMapOptions {
    uintptr_t*   Pages;
    size_t       Length;
    unsigned int Flags;
    unsigned int PlacementFlags;
};
#define HEAP_SLAB_NO_ATOMIC_CACHE 0
#define MAPPING_COMMIT            0x1
#define MAPPING_DOMAIN            0x2
#define MAPPING_USERSPACE         0x4
#define MAPPING_PERSISTENT        0x8
#define MAPPING_PHYSICAL_FIXED    0x10
#define MAPPING_VIRTUAL_PROCESS   0x20
MemoryCache_t* MemoryCacheCreate(const char*, size_t, size_t, int, unsigned int, void*, void*);
void*          MemoryCacheAllocate(MemoryCache_t*);
void           MemoryCacheFree(MemoryCache_t*, void*);
size_t         GetMemorySpacePageSize(void);
MemorySpace_t* GetCurrentMemorySpace(void);
oserr_t        GetMemorySpaceMapping(MemorySpace_t*, vaddr_t, int, uintptr_t*);
oserr_t        MemorySpaceMap(MemorySpace_t*, struct MemorySpaceMapOptions*, vaddr_t*);

#ifndef USEREVENT_SOURCE
#define USEREVENT_SOURCE "../kernel/sync/userevent.c"
#endif
#include USEREVENT_SOURCE

// How long the sleeping reader waits for a wake before it gives up.
#define __READER_TIMEOUT_SECONDS 2

static pthread_mutex_t g_futexLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_futexCondition = PTHREAD_COND_INITIALIZER;
static _Atomic(int)*   g_sleepCounter;    // The only futex that may sleep.
static _Atomic(int)*   g_sleeping;        // Set while the reader sleeps on it.
static unsigned int    g_readerWakes;     // Wakes that reached the reader.
static oserr_t         g_wakeResult = OS_EOK;
static unsigned int    g_warnings;

void
TestLogWarning(
    _In_ const char* format,
    ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
    g_warnings++;
}

oserr_t
FutexWait(
    _In_ OSAsyncContext_t* async,
    _In_ _Atomic(int)*     value,
    _In_ int               expected,
    _In_ int               flags,
    _In_ _Atomic(int)*     other,
    _In_ int               count,
    _In_ int               operation,
    _In_ OSTimestamp_t*    deadline)
{
    struct timespec until;
    oserr_t         status = OS_EOK;

    (void)async;
    (void)flags;
    (void)other;
    (void)count;
    (void)operation;
    (void)deadline;
    if (value != g_sleepCounter) {
        return OS_ETIMEOUT;
    }

    // Check the value and start sleeping under one lock, like the kernel does
    // with the block queue lock. A wake can then never fall between the two.
    pthread_mutex_lock(&g_futexLock);
    if (atomic_load(value) != expected) {
        pthread_mutex_unlock(&g_futexLock);
        return OS_EINTERRUPTED;
    }

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += __READER_TIMEOUT_SECONDS;
    g_sleeping = value;
    pthread_cond_broadcast(&g_futexCondition);
    while (g_sleeping == value) {
        if (pthread_cond_timedwait(&g_futexCondition, &g_futexLock, &until) == ETIMEDOUT) {
            g_sleeping = NULL;
            status = OS_ETIMEOUT;
        }
    }
    pthread_mutex_unlock(&g_futexLock);
    return status;
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* value,
    _In_ int           count,
    _In_ int           flags)
{
    (void)count;
    (void)flags;
    if (value == g_sleepCounter && g_wakeResult != OS_EOK) {
        return g_wakeResult;
    }

    // The kernel returns OS_ENOENT when nobody sleeps on the futex.
    pthread_mutex_lock(&g_futexLock);
    if (g_sleeping != value) {
        pthread_mutex_unlock(&g_futexLock);
        return OS_ENOENT;
    }
    g_sleeping = NULL;
    g_readerWakes++;
    pthread_cond_broadcast(&g_futexCondition);
    pthread_mutex_unlock(&g_futexLock);
    return OS_EOK;
}

oserr_t
SHMReferenceForExport(
    _In_ uuid_t                     id,
    _In_ const GrantTokenOptions_t* options,
    _In_ HandleType_t*              type)
{
    (void)id;
    (void)options;
    (void)type;
    return OS_ENOTSUPPORTED;
}

oserr_t
SHMAttachToken(
    _In_  const GrantToken_t*  token,
    _In_  MSContextSecurity_t* receiver,
    _Out_ SHMHandle_t*         handle)
{
    (void)token;
    (void)receiver;
    (void)handle;
    return OS_ENOTSUPPORTED;
}

/**
 * @brief An event handle watched by an I/O set, as a driver sets it up.
 * Status is what a fast interrupt handler saves for the driver before it
 * signals.
 */
struct __Fixture {
    atomic_int   Counter;
    UserEvent_t  Event;
    uuid_t       Handle;
    uuid_t       Queue;
    atomic_uint  Status;
};

static void
__FixtureCreate(
    _Out_ struct __Fixture* fixture,
    _In_  unsigned int      maximum)
{
    struct ioset_event watch = { .events = IOSETSYN };

    atomic_init(&fixture->Counter, 0);
    atomic_init(&fixture->Status, 0);
    fixture->Event = (UserEvent_t) {
        .InitialValue = maximum,
        .flags = maximum == 1 ? EVT_RESET_EVENT : EVT_SEM_EVENT,
        .KernelMapping = &fixture->Counter
    };
    fixture->Handle = CreateHandle(HandleTypeUserEvent, NULL, NULL, &fixture->Event);
    assert(fixture->Handle != UUID_INVALID);
    fixture->Queue = CreateHandleSet(0);
    assert(fixture->Queue != UUID_INVALID);
    watch.data.handle = fixture->Handle;
    assert(ControlHandleSet(fixture->Queue, IOSET_ADD, fixture->Handle, &watch) == OS_EOK);
}

static void
__FixtureDestroy(
    _In_ struct __Fixture* fixture)
{
    assert(ControlHandleSet(fixture->Queue, IOSET_DEL, fixture->Handle, NULL) == OS_EOK);
    assert(DestroyHandle(fixture->Handle) == OS_EOK);
    assert(DestroyHandle(fixture->Queue) == OS_EOK);
    __Drain();
}

static int
__PendingEvents(
    _In_ struct __Fixture* fixture)
{
    return atomic_load(&((struct __HandleSet*)LookupHandle(fixture->Queue, HandleTypeSet))->EventsPending);
}

static void
__TakeQueuedEvent(
    _In_ struct __Fixture* fixture)
{
    struct ioset_event event;
    int                count;

    assert(WaitForHandleSet(fixture->Queue, NULL, &event, 1, 0, NULL, &count) == OS_EOK);
    assert(count == 1 && event.events == IOSETSYN && event.data.handle == fixture->Handle);
}

// Take one unit the way a non-blocking read() does. Returns false when the
// counter is empty.
static bool
__TakeUnit(
    _In_ struct __Fixture* fixture)
{
    int value = atomic_load(&fixture->Counter);

    while (value > 0) {
        if (atomic_compare_exchange_strong(&fixture->Counter, &value, value - 1)) {
            return true;
        }
    }
    return false;
}

static oserr_t g_readerStatus;

// A blocking read() on the counter, written like OSEventLock.
static void*
__BlockingReader(
    _In_ void* argument)
{
    _Atomic(int)* counter = argument;
    oserr_t       status;
    int           value;

    for (;;) {
        value = atomic_load(counter);
        if (value > 0) {
            if (atomic_compare_exchange_strong(counter, &value, value - 1)) {
                g_readerStatus = OS_EOK;
                return NULL;
            }
            continue;
        }

        status = FutexWait(NULL, counter, value, 0, NULL, 0, 0, NULL);
        if (status == OS_EINTERRUPTED) {
            continue;
        }
        if (status != OS_EOK) {
            g_readerStatus = status;
            return NULL;
        }
    }
}

// A raise wakes a reader that sleeps in read(). A wake with nobody asleep is
// normal and is not logged. A failed wake is logged, and the signal still
// reports the raise and still queues the I/O set event.
static void
__TestBlockedReaderWakes(void)
{
    struct __Fixture fixture;
    pthread_t        reader;

    __FixtureCreate(&fixture, 1);
    g_sleepCounter = &fixture.Counter;
    g_readerStatus = OS_EUNKNOWN;
    g_readerWakes = 0;
    assert(pthread_create(&reader, NULL, __BlockingReader, &fixture.Counter) == 0);
    pthread_mutex_lock(&g_futexLock);
    while (g_sleeping != &fixture.Counter) {
        pthread_cond_wait(&g_futexCondition, &g_futexLock);
    }
    pthread_mutex_unlock(&g_futexLock);

    g_warnings = 0;
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(pthread_join(reader, NULL) == 0);
    assert(g_readerStatus == OS_EOK && g_readerWakes == 1);
    assert(atomic_load(&fixture.Counter) == 0);
    assert(g_warnings == 0);

    // The reader left before it saw the I/O set event. The event stays queued.
    assert(__PendingEvents(&fixture) == 1);
    __TakeQueuedEvent(&fixture);

    // Nobody sleeps now. The wake finds no reader, which is not an error.
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(g_readerWakes == 1 && g_warnings == 0);
    assert(__TakeUnit(&fixture));
    __TakeQueuedEvent(&fixture);

    g_wakeResult = OS_EUNKNOWN;
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    g_wakeResult = OS_EOK;
    assert(g_warnings == 1 && atomic_load(&fixture.Counter) == 1);
    __TakeQueuedEvent(&fixture);
    assert(__TakeUnit(&fixture));
    g_sleepCounter = NULL;
    __FixtureDestroy(&fixture);
}

// A reset event counts to 1. A signal that finds it raised merges into it and
// queues nothing, also after the earlier event was taken. A raise after the
// unit was taken queues a new event.
static void
__TestResetEventMarks(void)
{
    struct __Fixture fixture;

    __FixtureCreate(&fixture, 1);
    g_warnings = 0;
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(atomic_load(&fixture.Counter) == 1 && __PendingEvents(&fixture) == 1);
    assert(UserEventSignal(fixture.Handle) == OS_EOK);
    assert(atomic_load(&fixture.Counter) == 1 && __PendingEvents(&fixture) == 1);

    // Take the event, then signal before the unit is taken. This is where a
    // merged signal used to queue a second event with nothing to take.
    __TakeQueuedEvent(&fixture);
    assert(UserEventSignal(fixture.Handle) == OS_EOK);
    assert(atomic_load(&fixture.Counter) == 1);
    __ExpectNoWakeup(fixture.Queue);
    assert(__TakeUnit(&fixture));
    assert(!__TakeUnit(&fixture));

    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(__PendingEvents(&fixture) == 1);
    __TakeQueuedEvent(&fixture);
    assert(__TakeUnit(&fixture));
    __ExpectNoWakeup(fixture.Queue);
    assert(g_warnings == 0);
    __FixtureDestroy(&fixture);

    // Without a watching set the mark finds nobody. That is normal too.
    atomic_store(&fixture.Counter, 0);
    fixture.Handle = CreateHandle(HandleTypeUserEvent, NULL, NULL, &fixture.Event);
    assert(fixture.Handle != UUID_INVALID);
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(g_warnings == 0 && atomic_load(&fixture.Counter) == 1);
    assert(DestroyHandle(fixture.Handle) == OS_EOK);
    __Drain();
    assert(UserEventSignal(fixture.Handle) == OS_EINVALPARAMS);
}

// A counting event with a maximum of 3. Every signal that adds a unit marks
// the set, so a unit added after the reader took its event always gets a new
// event, also when the counter was above 0. A signal at the maximum adds
// nothing and queues nothing.
static void
__TestCountingEventMarks(void)
{
    struct __Fixture fixture;

    __FixtureCreate(&fixture, 3);
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    __TakeQueuedEvent(&fixture);

    // The reader looked at the counter and saw 1 unit. A signal then raises
    // it to 2 before the reader takes that unit.
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(__TakeUnit(&fixture));
    assert(atomic_load(&fixture.Counter) == 1 && __PendingEvents(&fixture) == 1);
    __TakeQueuedEvent(&fixture);
    assert(__TakeUnit(&fixture));

    // Raises while an event is queued merge in the set.
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(UserEventSignal(fixture.Handle) == OS_EINCOMPLETE);
    assert(atomic_load(&fixture.Counter) == 3 && __PendingEvents(&fixture) == 1);
    __TakeQueuedEvent(&fixture);
    assert(UserEventSignal(fixture.Handle) == OS_EOK);
    assert(atomic_load(&fixture.Counter) == 3);
    __ExpectNoWakeup(fixture.Queue);
    assert(__TakeUnit(&fixture) && __TakeUnit(&fixture) && __TakeUnit(&fixture));
    assert(!__TakeUnit(&fixture));
    __FixtureDestroy(&fixture);
}

// The steps of a driver's event handler, in the order the drivers use. The
// second interrupt is inserted before the step given by position.
enum __DriverStep {
    __StepTakeEvent,
    __StepTakeCounter,
    __StepTakeStatus,
    __StepDone
};

static void
__FastHandler(
    _In_ struct __Fixture* fixture,
    _In_ unsigned int      bit)
{
    oserr_t status;

    atomic_fetch_or(&fixture->Status, bit);
    status = UserEventSignal(fixture->Handle);
    assert(status == OS_EOK || status == OS_EINCOMPLETE);
}

// A fast handler saves its status and then signals. The driver takes the set
// event, then the counter if it is raised, then the status. Whatever step the
// second interrupt lands before, both status bits are handled, and the
// counter and the set end empty. No pass finds the counter empty, because a
// merged signal no longer queues an event.
static void
__TestInterruptInterleavings(void)
{
    const unsigned int expectedPasses[] = { 1, 1, 2, 2 };

    for (unsigned int position = __StepTakeEvent; position <= __StepDone; position++) {
        struct __Fixture fixture;
        unsigned int     handled = 0;
        unsigned int     passes = 0;
        unsigned int     emptyCounter = 0;
        bool             secondSent = false;

        __FixtureCreate(&fixture, 1);
        __FastHandler(&fixture, 0x1);
        while (__PendingEvents(&fixture) != 0) {
            for (unsigned int step = __StepTakeEvent; step <= __StepDone; step++) {
                if (!secondSent && step == position) {
                    __FastHandler(&fixture, 0x2);
                    secondSent = true;
                }
                if (step == __StepTakeEvent) {
                    __TakeQueuedEvent(&fixture);
                } else if (step == __StepTakeCounter) {
                    if (!__TakeUnit(&fixture)) {
                        emptyCounter++;
                    }
                } else if (step == __StepTakeStatus) {
                    handled |= atomic_exchange(&fixture.Status, 0);
                }
            }
            passes++;
        }

        assert(handled == 0x3 && atomic_load(&fixture.Status) == 0);
        assert(atomic_load(&fixture.Counter) == 0);
        assert(passes == expectedPasses[position] && emptyCounter == 0);
        __ExpectNoWakeup(fixture.Queue);
        __FixtureDestroy(&fixture);
    }
}

int
main(void)
{
    MSContextSecurity_t driver;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    (void)AllocateHandleId();
    __NewGroup(&driver);
    g_current = &driver;

    __TestBlockedReaderWakes();
    __TestResetEventMarks();
    __TestCountingEventMarks();
    __TestInterruptInterleavings();

    g_current = NULL;
    DestroyHandleGrantsTable(driver.HandleTable);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0);
    assert(g_allocations == 0);
    puts("User events: reader wake, I/O set marks, counting events and interrupt interleavings passed");
    return 0;
}
