/**
 * Host replacement for the timespec helpers of the Vali C library.
 *
 * The production sources of netd call timespec_add, which the Vali C library
 * declares in time.h. The C library of the host has no such function, so a
 * host test that compiles those sources includes this header first.
 */

#ifndef __TEST_HOST_TIMESPEC_H__
#define __TEST_HOST_TIMESPEC_H__

#include <time.h>

#define HOST_NANOSECONDS_PER_SECOND 1000000000L

// Adds two times. Both must be normalized, which means that their tv_nsec is
// below one second. The result is normalized too, and may be one of the inputs.
static inline void
timespec_add(
    const struct timespec* a,
    const struct timespec* b,
    struct timespec*       result)
{
    result->tv_sec = a->tv_sec + b->tv_sec;
    result->tv_nsec = a->tv_nsec + b->tv_nsec;
    if (result->tv_nsec >= HOST_NANOSECONDS_PER_SECOND) {
        result->tv_sec++;
        result->tv_nsec -= HOST_NANOSECONDS_PER_SECOND;
    }
}

#endif //!__TEST_HOST_TIMESPEC_H__
