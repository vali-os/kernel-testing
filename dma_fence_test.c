#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
typedef int errno_t;
#include <internal/_syscalls.h>

static atomic_uint g_live;
static unsigned int g_allocations;
static bool g_oom;
static void*
__FenceAllocate(size_t size)
{
    void* memory;

    g_allocations++;
    if (g_oom) {
        return NULL;
    }
    memory = malloc(size);
    assert(memory != NULL);
    atomic_fetch_add(&g_live, 1);
    return memory;
}
static void
__FenceFree(void* memory)
{
    assert(atomic_fetch_sub(&g_live, 1) != 0);
    free(memory);
}
#define malloc __FenceAllocate
#define free __FenceFree
#include "../librt/libos/fence_storage.c"
#include "../librt/libos/fence.c"
#include "../librt/libddk/dma/fence.c"
#undef free
#undef malloc

struct DMAMapping { unsigned int Uses; int Payload; };
static _Thread_local struct thread_storage g_tls;
static _Thread_local OSAsyncContext_t g_async;
static bool g_forkWait;
static oserr_t g_asyncResult;
static unsigned int g_asyncWaits;
struct thread_storage* __tls_current(void) { return &g_tls; }
void usched_wait_async(void)
{
    assert(g_tls.async_context == &g_async);
    g_asyncWaits++;
    g_async.ErrorCode = g_asyncResult;
}
static oserr_t g_beginError;
static enum SHMWaitMode g_beginMode;
static const OSTimestamp_t* g_beginDeadline;
static oserr_t g_endError;
static atomic_uint g_waitCalls;
static atomic_uint g_wakeCalls;
static atomic_uint g_sleepers;
static pthread_mutex_t g_waitMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_waitCondition = PTHREAD_COND_INITIALIZER;
typedef void (*__WaitHook)(void);
static _Atomic(__WaitHook) g_beforeWait;
static _Atomic(__WaitHook) g_beforeWake;
static oserr_t g_waitError;
static int g_spurious;
static const OSTimestamp_t* g_expectedDeadline;

void SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
    assert(0);
}
uuid_t usched_notification_handle(void) { assert(g_tls.async_context != NULL); return 1; }
oserr_t
DMABeginUse(struct DMAMapping* mapping, struct DMAUse* use,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    g_beginMode = mode;
    g_beginDeadline = deadline;
    if (g_beginError != OS_EOK) {
        return g_beginError;
    }
    assert(use->Mapping == NULL);
    mapping->Uses++;
    use->Mapping = mapping;
    return OS_EOK;
}
oserr_t
DMAEndUse(struct DMAUse* use)
{
    if (g_endError != OS_EOK) {
        return g_endError;
    }
    assert(use->Mapping != NULL && use->Mapping->Uses != 0);
    use->Mapping->Uses--;
    use->Mapping = NULL;
    return OS_EOK;
}
static oserr_t
__ModelFutex(OSFutexParameters_t* parameters, OSAsyncContext_t* async)
{
    void (*hook)(void);

    if (FUTEX_FLAG_ACTION(parameters->Flags) == FUTEX_FLAG_WAIT) {
        atomic_fetch_add(&g_waitCalls, 1);
        assert(parameters->Flags == (FUTEX_FLAG_WAIT | FUTEX_FLAG_PRIVATE));
        assert(async == g_tls.async_context && parameters->Deadline == g_expectedDeadline);
        hook = atomic_exchange(&g_beforeWait, NULL);
        if (hook != NULL) {
            hook();
        }
        if (g_spurious) {
            g_spurious--;
            return OS_EOK;
        }
        if (g_waitError != OS_EOK) {
            return g_waitError;
        }
        assert(pthread_mutex_lock(&g_waitMutex) == 0);
        if (atomic_load(parameters->Futex0) != parameters->Expected0) {
            assert(pthread_mutex_unlock(&g_waitMutex) == 0);
            return OS_EINTERRUPTED;
        }
        atomic_fetch_add(&g_sleepers, 1);
        assert(pthread_cond_wait(&g_waitCondition, &g_waitMutex) == 0);
        atomic_fetch_sub(&g_sleepers, 1);
        assert(pthread_mutex_unlock(&g_waitMutex) == 0);
        return OS_EOK;
    }
    atomic_fetch_add(&g_wakeCalls, 1);
    assert(parameters->Flags == (FUTEX_FLAG_WAKE | FUTEX_FLAG_PRIVATE));
    assert(parameters->Expected0 == INT_MAX && async == NULL);
    hook = atomic_exchange(&g_beforeWake, NULL);
    if (hook != NULL) {
        hook();
    }
    assert(pthread_mutex_lock(&g_waitMutex) == 0);
    assert(pthread_cond_broadcast(&g_waitCondition) == 0);
    assert(pthread_mutex_unlock(&g_waitMutex) == 0);
    return OS_EOK;
}

oserr_t
FutexWait(OSAsyncContext_t* async, atomic_int* futex, int expected, int flags,
    atomic_int* second, int count, int operation, OSTimestamp_t* deadline)
{
    OSFutexParameters_t parameters = { .Futex0 = futex, .Expected0 = expected,
        .Flags = flags, .Futex1 = second, .Count = count, .Op = operation, .Deadline = deadline };
    oserr_t status = __ModelFutex(&parameters, async);

    if (g_forkWait) {
        assert(async == &g_async && async->ErrorCode == OS_ESCSTARTED);
        g_asyncResult = status;
        return OS_EFORKED;
    }
    return status;
}
oserr_t
FutexWake(atomic_int* futex, int count, int flags)
{
    OSFutexParameters_t parameters = { .Futex0 = futex, .Expected0 = count, .Flags = flags };

    return __ModelFutex(&parameters, NULL);
}
// These are referenced by the real sync syscall object, but not used by fences.
oserr_t FutexWakeOperation(atomic_int* a, int b, atomic_int* c, int d, int e, int f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; assert(0); return OS_ENOTSUPPORTED; }
oserr_t UserEventCreate(unsigned int initial, unsigned int flags, uuid_t* out, atomic_int** address)
{ (void)initial; (void)flags; (void)out; (void)address; assert(0); return OS_ENOTSUPPORTED; }
oserr_t ScFutexWait(OSAsyncContext_t*, OSFutexParameters_t*);
oserr_t ScFutexWake(OSFutexParameters_t*);
SCTYPE
_syscall(SCTYPE function, SCTYPE a, SCTYPE b, SCTYPE c, SCTYPE d, SCTYPE e)
{
    assert(c == 0 && d == 0 && e == 0);
    if (function == 45) {
        return ScFutexWait((OSAsyncContext_t*)(uintptr_t)a, (OSFutexParameters_t*)(uintptr_t)b);
    }
    assert(function == 46 && b == 0);
    return ScFutexWake((OSFutexParameters_t*)(uintptr_t)a);
}

static void
__TestFenceLifecycle(void)
{
    struct DMAMapping mapping = { 0 };
    struct DMAFencedUse use = { 0 };
    struct DMAFencedUse other = { 0 };
    struct OSFence* fence = NULL;
    struct OSFence* alias;
    oserr_t result = OS_EUNKNOWN;
    unsigned int allocations;
    unsigned int waits;
    unsigned int wakes;
    unsigned int i;

    assert(OSFenceCreate(NULL) == OS_EINVALPARAMS);
    g_oom = true;
    assert(OSFenceCreate(&fence) == OS_EOOM && fence == NULL);
    g_oom = false;
    assert(OSFenceCreate(&fence) == OS_EOK);
    assert(OSFenceCreate(&fence) == OS_EBUSY);
    assert(OSFencePoll(fence, &result) == OS_EINVALPARAMS && result == OS_EUNKNOWN);
    assert(OSFenceWait(fence, NULL, &result) == OS_EINVALPARAMS);
    assert(OSFencePoll(fence, NULL) == OS_EINVALPARAMS);
    assert(OSFenceReset(NULL) == OS_EINVALPARAMS);
    assert(OSFenceAcquire(NULL) == OS_EINVALPARAMS);
    atomic_store(&fence->References, UINT_MAX);
    assert(OSFenceAcquire(fence) == OS_EOVERFLOW);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOVERFLOW);
    atomic_store(&fence->References, 1);
    g_beginError = OS_EBUSY;
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitNonBlocking, NULL) == OS_EBUSY);
    assert(g_beginMode == SHMWaitNonBlocking && g_beginDeadline == NULL);
    assert(use.Fence == NULL && use.Use.Mapping == NULL && mapping.Uses == 0);
    assert(atomic_load(&fence->References) == 1 && atomic_load(&fence->Storage->State) == __OSFenceIdle);
    g_beginError = OS_ETIMEOUT;
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_ETIMEOUT);
    assert(g_beginMode == SHMWaitUntilAvailable && g_beginDeadline->Seconds == INT64_MAX);
    assert(use.Fence == NULL && use.Use.Mapping == NULL);
    g_beginError = OS_EOK;
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_EOK);
    assert(OSFencePoll(fence, &result) == OS_EINPROGRESS && result == OS_EUNKNOWN);
    assert(DMABeginUseFenced(&mapping, &other, fence, SHMWaitUntilAvailable, NULL) == OS_EBUSY);
    assert(OSFenceReset(fence) == OS_EBUSY);
    g_endError = OS_EUNKNOWN;
    assert(DMAEndUseFenced(&use, OS_EDEVFAULT) == OS_EUNKNOWN);
    assert(use.Fence == fence && use.Use.Mapping == &mapping && mapping.Uses == 1);
    assert(OSFencePoll(fence, &result) == OS_EINPROGRESS);
    g_endError = OS_EOK;
    assert(DMAEndUseFenced(&use, OS_EDEVFAULT) == OS_EOK);
    assert(use.Fence == NULL && use.Use.Mapping == NULL && mapping.Uses == 0);
    assert(DMAEndUseFenced(&use, OS_EOK) == OS_EINVALPARAMS);
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK && result == OS_EDEVFAULT);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EBUSY);
    assert(OSFenceAcquire(fence) == OS_EOK);
    alias = fence;
    assert(OSFenceReset(fence) == OS_EBUSY);
    OSFenceRelease(&alias);
    allocations = g_allocations;
    waits = atomic_load(&g_waitCalls);
    wakes = atomic_load(&g_wakeCalls);
    g_oom = true;
    for (i = 0; i < 1000; i++) {
        assert(OSFenceReset(fence) == OS_EOK);
        assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
        assert(OSFencePoll(fence, &result) == OS_EINPROGRESS);
        assert(DMAEndUseFenced(&use, i % 2 ? OS_ECANCELLED : OS_EOK) == OS_EOK);
        assert(OSFenceWait(fence, NULL, &result) == OS_EOK);
        assert(result == (i % 2 ? OS_ECANCELLED : OS_EOK));
    }
    g_oom = false;
    assert(g_allocations == allocations && atomic_load(&g_waitCalls) == waits && atomic_load(&g_wakeCalls) == wakes);
    OSFenceRelease(&fence);
    OSFenceRelease(&fence);
    OSFenceRelease(NULL);
    assert(atomic_load(&g_live) == 0);
}

static struct DMAFencedUse* g_raceUse;
static struct OSFence** g_releaseOnWake;
static void
__CompleteBeforeSleep(void)
{
    assert(DMAEndUseFenced(g_raceUse, OS_ECANCELLED) == OS_EOK);
}
static void
__ReleaseDuringWake(void)
{
    OSFenceRelease(g_releaseOnWake);
    assert(atomic_load(&g_live) == 1);
}
static void
__TestWaitRaces(void)
{
    struct DMAMapping mapping = { 0 };
    struct DMAFencedUse use = { 0 };
    struct OSFence* fence = NULL;
    OSTimestamp_t deadline = { .Seconds = 1234, .Nanoseconds = 5678 };
    oserr_t result = OS_EUNKNOWN;
    unsigned int calls;

    assert(OSFenceCreate(&fence) == OS_EOK);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
    atomic_store(&fence->Storage->Waiters, UINT_MAX);
    assert(OSFenceWait(fence, NULL, &result) == OS_EOVERFLOW);
    atomic_store(&fence->Storage->Waiters, 0);
    g_waitError = OS_ETIMEOUT;
    g_spurious = 1;
    g_expectedDeadline = &deadline;
    calls = atomic_load(&g_waitCalls);
    assert(OSFenceWait(fence, &deadline, &result) == OS_ETIMEOUT && result == OS_EUNKNOWN);
    assert(atomic_load(&g_waitCalls) == calls + 2 && mapping.Uses == 1);
    g_waitError = OS_EINTERRUPTED;
    assert(OSFenceWait(fence, &deadline, &result) == OS_EINTERRUPTED);
    assert(atomic_load(&fence->Storage->Waiters) == 0);
    g_waitError = OS_EOK;
    g_expectedDeadline = NULL;
    g_beforeWait = __CompleteBeforeSleep;
    g_raceUse = &use;
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK && result == OS_ECANCELLED);
    assert(OSFenceReset(fence) == OS_EOK);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
    g_beforeWait = __CompleteBeforeSleep;
    g_waitError = OS_ETIMEOUT;
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK && result == OS_ECANCELLED);
    g_waitError = OS_EOK;
    assert(OSFenceReset(fence) == OS_EOK);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
    g_tls.async_context = &g_async;
    g_forkWait = true;
    g_beforeWait = __CompleteBeforeSleep;
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK && result == OS_ECANCELLED);
    assert(g_asyncWaits == 1);
    g_forkWait = false;
    g_tls.async_context = NULL;
    assert(OSFenceReset(fence) == OS_EOK);
    assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
    // Simulate a consumer releasing its reference after publication but before
    // notification finishes. The job reference must still protect the object.
    atomic_store(&fence->Storage->Waiters, 1);
    g_releaseOnWake = &fence;
    g_beforeWake = __ReleaseDuringWake;
    assert(DMAEndUseFenced(&use, OS_EOK) == OS_EOK);
    assert(fence == NULL && atomic_load(&g_live) == 0);
}

struct __FenceWaiter {
    struct OSFence* Fence;
    struct DMAMapping* Mapping;
};
static void*
__WaitForFence(void* argument)
{
    struct __FenceWaiter* waiter = argument;
    oserr_t result;

    assert(OSFenceWait(waiter->Fence, NULL, &result) == OS_EOK && result == OS_EOK);
    assert(waiter->Mapping->Payload == 42);
    OSFenceRelease(&waiter->Fence);
    return NULL;
}
static void*
__PollForFence(void* argument)
{
    struct __FenceWaiter* waiter = argument;
    oserr_t result;
    oserr_t status;

    do {
        status = OSFencePoll(waiter->Fence, &result);
        if (status == OS_EINPROGRESS) {
            sched_yield();
        }
    } while (status == OS_EINPROGRESS);
    // This reader has no pthread condition/mutex synchronization with the
    // writer. Result and payload visibility must come from fence publication.
    assert(status == OS_EOK && result == OS_EOK && waiter->Mapping->Payload == 42);
    OSFenceRelease(&waiter->Fence);
    return NULL;
}

static void
__TestConcurrentWaiters(void)
{
    struct DMAMapping mapping = { 0 };
    struct DMAFencedUse use = { 0 };
    struct OSFence* fence = NULL;
    struct __FenceWaiter waiters[5];
    pthread_t threads[5];
    unsigned int i;
    unsigned int iteration;

    for (iteration = 0; iteration < 100; iteration++) {
        mapping.Payload = 0;
        assert(OSFenceCreate(&fence) == OS_EOK);
        assert(DMABeginUseFenced(&mapping, &use, fence, SHMWaitUntilAvailable, NULL) == OS_EOK);
        for (i = 0; i < 5; i++) {
            assert(OSFenceAcquire(fence) == OS_EOK);
            waiters[i] = (struct __FenceWaiter) { fence, &mapping };
            assert(pthread_create(&threads[i], NULL, i == 4 ? __PollForFence : __WaitForFence, &waiters[i]) == 0);
        }
        while (atomic_load(&g_sleepers) != 4) {
            sched_yield();
        }
        OSFenceRelease(&fence);
        mapping.Payload = 42;
        assert(DMAEndUseFenced(&use, OS_EOK) == OS_EOK);
        for (i = 0; i < 5; i++) {
            assert(pthread_join(threads[i], NULL) == 0);
        }
        assert(atomic_load(&g_live) == 0);
    }
}

int main(void)
{
    __TestFenceLifecycle();
    __TestWaitRaces();
    __TestConcurrentWaiters();
    puts("DMA fences: lifecycle, failed submission/end, result publication, timeout, wake races and concurrent waiters passed");
    return 0;
}
