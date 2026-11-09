// Futex scheduler seam for real SHM range coordination tests.
#include <futex.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

_Thread_local atomic_int g_rangeWaitError;
_Thread_local bool g_rangeAdvanceOnError;
_Thread_local OSTimestamp_t* g_rangeWaitDeadline;
atomic_uint g_rangeWakeCalls;

static pthread_mutex_t g_waitLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_waitChanged = PTHREAD_COND_INITIALIZER;

oserr_t
FutexWait(OSAsyncContext_t* async, atomic_int* event, int expected, int flags,
    atomic_int* second, int count, int operation, OSTimestamp_t* deadline)
{
    struct timespec until;
    int result = atomic_exchange(&g_rangeWaitError, 0);
    (void)second;
    (void)count;
    (void)operation;
    assert(async == NULL && flags == 0);
    g_rangeWaitDeadline = deadline;
    if (result != 0) {
        if (g_rangeAdvanceOnError) {
            atomic_fetch_add(event, 1);
            g_rangeAdvanceOnError = false;
        }
        return (oserr_t)result;
    }
    pthread_mutex_lock(&g_waitLock);
    if (deadline != NULL) {
        until.tv_sec = deadline->Seconds;
        until.tv_nsec = deadline->Nanoseconds;
    }
    while (atomic_load(event) == expected && result == 0) {
        result = deadline == NULL ? pthread_cond_wait(&g_waitChanged, &g_waitLock) :
            pthread_cond_timedwait(&g_waitChanged, &g_waitLock, &until);
    }
    pthread_mutex_unlock(&g_waitLock);
    return result == ETIMEDOUT ? OS_ETIMEOUT : OS_EOK;
}

oserr_t
FutexWake(atomic_int* event, int count, int flags)
{
    (void)event;
    assert(count == 1 && flags == 0);
    atomic_fetch_add(&g_rangeWakeCalls, 1);
    pthread_mutex_lock(&g_waitLock);
    pthread_cond_broadcast(&g_waitChanged);
    pthread_mutex_unlock(&g_waitLock);
    return OS_EOK;
}
