// Checks the driver time helpers: DDKGetTimeUs, DDKGetTimeMs and DDKStallUs
// (librt/libddk/utils/time.c), and usched_deadline_after_us and
// usched_job_sleep_us (librt/libos/uthreads/deadline.c). Both production
// sources are compiled into this file. Doubles replace OSGetTime, OSStall,
// the spinlock, the system log, timespec_get (wrapped at link time) and
// usched_job_sleep.
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef DDK_TIME_SOURCE
#define DDK_TIME_SOURCE "../librt/libddk/utils/time.c"
#endif
#ifndef USCHED_DEADLINE_SOURCE
#define USCHED_DEADLINE_SOURCE "../librt/libos/uthreads/deadline.c"
#endif
#include DDK_TIME_SOURCE
#include USCHED_DEADLINE_SOURCE

// What OSGetTime answers for each source. A source with a nonzero
// g_clockStatus fails with that status.
static OSTimestamp_t g_clock[__TIME_SOURCE_COUNT];
static oserr_t       g_clockStatus[__TIME_SOURCE_COUNT];
static unsigned int  g_clockReads;

// The duration of the last OSStall call, and what the next call returns.
static uint64_t     g_stallNanoseconds;
static unsigned int g_stalls;
static oserr_t      g_stallStatus = OS_EOK;

// The spinlock must be released as often as it is taken.
static int g_spinlockDepth;

// Log lines written by the helpers.
static unsigned int g_warnings;
static unsigned int g_errors;

// The UTC clock that timespec_get reads, and whether it fails.
static struct timespec g_utc;
static bool            g_utcFails;

// The time the last usched_job_sleep call was asked to sleep until, and what
// it returns.
static struct timespec g_sleptUntil;
static unsigned int    g_sleeps;
static int             g_sleepResult;

oserr_t
OSGetTime(
    _In_ enum OSTimeSource source,
    _In_ OSTimestamp_t*    time)
{
    assert((unsigned int)source < __TIME_SOURCE_COUNT);
    g_clockReads++;
    if (g_clockStatus[source] != OS_EOK) {
        return g_clockStatus[source];
    }
    *time = g_clock[source];
    return OS_EOK;
}

// The real OSStall refuses a zero duration, and so does this double.
oserr_t
OSStall(
    _In_ UInteger64_t* duration)
{
    if (duration->QuadPart == 0) {
        return OS_EINVALPARAMS;
    }
    g_stalls++;
    g_stallNanoseconds = duration->QuadPart;
    return g_stallStatus;
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
    assert(g_spinlockDepth == 0);
    g_spinlockDepth++;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
    assert(g_spinlockDepth == 1);
    g_spinlockDepth--;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    } else if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
}

int
__wrap_timespec_get(
    struct timespec* time,
    int              base)
{
    assert(base == TIME_UTC);
    if (g_utcFails) {
        errno = EIO;
        return 0;
    }
    *time = g_utc;
    return base;
}

int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    g_sleeps++;
    g_sleptUntil = *until;
    return g_sleepResult;
}

static void
__SetClock(
    _In_ enum OSTimeSource source,
    _In_ int64_t           seconds,
    _In_ int64_t           nanoseconds)
{
    g_clock[source] = (OSTimestamp_t){ .Seconds = seconds, .Nanoseconds = nanoseconds };
    g_clockStatus[source] = OS_EOK;
}

static void
__ReadTime(void)
{
    // Whole microseconds, rounded down.
    __SetClock(OSTimeSource_MONOTONIC, 5, 250000999);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 5250000);

    // The seconds are multiplied in 64 bits, so a long uptime does not wrap.
    __SetClock(OSTimeSource_MONOTONIC, 5000000, 0);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 5000000ULL * 1000000ULL);

    // Milliseconds are the same clock, rounded down.
    __SetClock(OSTimeSource_MONOTONIC, 6000000, 999999);
    assert(DDKGetTimeMs(OSTimeSource_MONOTONIC) == 6000000000ULL);
    __SetClock(OSTimeSource_MONOTONIC, 6000000, 1000000);
    assert(DDKGetTimeMs(OSTimeSource_MONOTONIC) == 6000000001ULL);

    // The source is passed on as it is.
    __SetClock(OSTimeSource_UTC, 700, 1000);
    assert(DDKGetTimeUs(OSTimeSource_UTC) == 700000001);
    assert(g_spinlockDepth == 0);
}

static void
__ReadFailures(void)
{
    unsigned int warnings;

    // Before any good read, a failed read gives zero.
    g_clockStatus[OSTimeSource_THREAD] = OS_ENOTSUPPORTED;
    assert(DDKGetTimeUs(OSTimeSource_THREAD) == 0);

    // A failed read repeats the last value read from the same source, and the
    // warning is written once only.
    __SetClock(OSTimeSource_MONOTONIC, 7000000, 0);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000000000000ULL);
    g_clockStatus[OSTimeSource_MONOTONIC] = OS_ENOTSUPPORTED;
    warnings = g_warnings;
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000000000000ULL);
    assert(DDKGetTimeMs(OSTimeSource_MONOTONIC) == 7000000000ULL);
    assert(g_warnings <= warnings + 1);

    // Each source keeps its own last value.
    assert(DDKGetTimeUs(OSTimeSource_UTC) == 700000001);
    g_clockStatus[OSTimeSource_UTC] = OS_ENOTSUPPORTED;
    assert(DDKGetTimeUs(OSTimeSource_UTC) == 700000001);

    // A negative time is handled as a failed read.
    __SetClock(OSTimeSource_MONOTONIC, -1, 0);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000000000000ULL);
    __SetClock(OSTimeSource_MONOTONIC, 7000001, -1);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000000000000ULL);

    // A good read that is older than the stored value does not replace it,
    // so the fallback never moves back.
    __SetClock(OSTimeSource_MONOTONIC, 6999999, 0);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 6999999000000ULL);
    g_clockStatus[OSTimeSource_MONOTONIC] = OS_ENOTSUPPORTED;
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000000000000ULL);

    // A newer good read moves it forward.
    __SetClock(OSTimeSource_MONOTONIC, 7000002, 0);
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000002000000ULL);
    g_clockStatus[OSTimeSource_MONOTONIC] = OS_ENOTSUPPORTED;
    assert(DDKGetTimeUs(OSTimeSource_MONOTONIC) == 7000002000000ULL);
    assert(g_spinlockDepth == 0);
}

static void
__UnknownSource(void)
{
    unsigned int reads = g_clockReads;
    unsigned int errors = g_errors;

    // An unknown source is refused before the clock is read, and logged.
    assert(DDKGetTimeUs((enum OSTimeSource)__TIME_SOURCE_COUNT) == 0);
    assert(DDKGetTimeUs((enum OSTimeSource)-1) == 0);
    assert(g_clockReads == reads);
    assert(g_errors == errors + 2);
}

static void
__Stall(void)
{
    // The kernel stall takes nanoseconds.
    assert(DDKStallUs(10) == OS_EOK && g_stallNanoseconds == 10000);

    // The largest delay does not wrap around in 32 bits on the way there.
    assert(DDKStallUs(UINT32_MAX) == OS_EOK);
    assert(g_stallNanoseconds == (uint64_t)UINT32_MAX * 1000);

    // A zero delay returns at once, without the kernel call that refuses it.
    g_stalls = 0;
    assert(DDKStallUs(0) == OS_EOK);
    assert(g_stalls == 0);

    // A failed stall is passed on to the caller.
    g_stallStatus = OS_EDEVFAULT;
    assert(DDKStallUs(1) == OS_EDEVFAULT);
    g_stallStatus = OS_EOK;
}

static void
__DeadlineAfter(void)
{
    struct timespec until;

    // Microseconds are added to the nanoseconds of the UTC time.
    g_utcFails = false;
    g_utc = (struct timespec){ .tv_sec = 100, .tv_nsec = 1000 };
    assert(usched_deadline_after_us(5, &until) == 0);
    assert(until.tv_sec == 100 && until.tv_nsec == 6000);

    // Whole seconds of the sum are carried, so tv_nsec stays below a second.
    g_utc = (struct timespec){ .tv_sec = 100, .tv_nsec = 999999000 };
    assert(usched_deadline_after_us(2, &until) == 0);
    assert(until.tv_sec == 101 && until.tv_nsec == 1000);

    // Whole seconds of the delay go to tv_sec.
    g_utc = (struct timespec){ .tv_sec = 100, .tv_nsec = 500000000 };
    assert(usched_deadline_after_us(3700000, &until) == 0);
    assert(until.tv_sec == 104 && until.tv_nsec == 200000000);

    // A zero delay is the current time.
    assert(usched_deadline_after_us(0, &until) == 0);
    assert(until.tv_sec == 100 && until.tv_nsec == 500000000);

    // A missing output is refused.
    errno = 0;
    assert(usched_deadline_after_us(1, NULL) == -1 && errno == EINVAL);

    // When the clock cannot be read, the error is passed on and the output
    // is left as it was.
    g_utcFails = true;
    errno = 0;
    until = (struct timespec){ .tv_sec = 1, .tv_nsec = 2 };
    assert(usched_deadline_after_us(1, &until) == -1 && errno == EIO);
    assert(until.tv_sec == 1 && until.tv_nsec == 2);
    g_utcFails = false;
}

static void
__JobSleep(void)
{
    // The sleep is until the current UTC time plus the delay.
    g_utc = (struct timespec){ .tv_sec = 200, .tv_nsec = 999000000 };
    g_sleeps = 0;
    assert(usched_job_sleep_us(1500) == 0);
    assert(g_sleeps == 1);
    assert(g_sleptUntil.tv_sec == 201 && g_sleptUntil.tv_nsec == 500000);

    // The result of the sleep is passed on.
    g_sleepResult = -1;
    assert(usched_job_sleep_us(1) == -1);
    g_sleepResult = 0;

    // Without the clock the job does not sleep at all.
    g_utcFails = true;
    g_sleeps = 0;
    errno = 0;
    assert(usched_job_sleep_us(1000) == -1 && errno == EIO);
    assert(g_sleeps == 0);
    g_utcFails = false;
}

int
main(void)
{
    __ReadTime();
    __ReadFailures();
    __UnknownSource();
    __Stall();
    __DeadlineAfter();
    __JobSleep();
    printf("ddk time helper tests passed\n");
    return 0;
}
