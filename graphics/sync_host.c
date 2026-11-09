/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include <time.h>
#include <internal/_tls.h>
#include <os/futex.h>
#include <assert.h>

static _Thread_local struct thread_storage g_tls;

struct thread_storage*
__tls_current(void)
{
    return &g_tls;
}

uuid_t
usched_notification_handle(void)
{
    // The service executor polls private pools. Entering a blocking libos wait
    // would prevent the same executor from progressing its pending jobs.
    assert(0 && "graphics acceptance must not enter an asynchronous wait");
    return UUID_INVALID;
}

oserr_t
OSFutex(
    _In_    OSFutexParameters_t* parameters,
    _In_    OSAsyncContext_t*    async)
{
    (void)parameters;
    (void)async;
    // No client can bind this private pool or add a waiter. Keep unexpected OS
    // calls observable instead of replacing fence behavior with a fake success.
    assert(0 && "graphics acceptance must not wait on the service executor");
    return OS_ENOTSUPPORTED;
}
