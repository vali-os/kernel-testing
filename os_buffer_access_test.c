#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#undef FUTEX_OP
#include <os/futex.h>
#include <internal/_tls.h>

// Include the real implementations to inject overflow and contention at their
// boundaries. The runner also compiles standalone objects and audits imports.
#include "../librt/libos/fence_pool.c"
#include "../librt/libos/buffer_access.c"

static _Thread_local struct thread_storage g_tls;
static atomic_uint g_futexCalls;
static bool g_allowFutex;
static oserr_t g_wakeError;
static void (*g_beforeWake)(void);
extern atomic_uint g_fenceLogCalls;

struct thread_storage*
__tls_current(void)
{
    return &g_tls;
}

uuid_t
usched_notification_handle(void)
{
    assert(0);
    return UUID_INVALID;
}

oserr_t
OSFutex(
    _In_    OSFutexParameters_t* parameters,
    _In_    OSAsyncContext_t*    async)
{
    struct timespec timeout = { .tv_sec = 3 };
    int             operation;
    long            result;
    void          (*hook)(void);

    assert(g_allowFutex && async == NULL);
    assert(!(parameters->Flags & FUTEX_FLAG_PRIVATE));
    atomic_fetch_add(&g_futexCalls, 1);
    if (FUTEX_FLAG_ACTION(parameters->Flags) == FUTEX_FLAG_WAIT) {
        if (parameters->Deadline != NULL) {
            return OS_ETIMEOUT;
        }
        operation = FUTEX_WAIT;
    } else {
        operation = FUTEX_WAKE;
        hook = g_beforeWake;
        g_beforeWake = NULL;
        if (hook != NULL) {
            hook();
        }
        if (g_wakeError != OS_EOK) {
            return g_wakeError;
        }
    }
    result = syscall(SYS_futex, parameters->Futex0, operation,
        parameters->Expected0, &timeout, NULL, 0);
    if (result >= 0) {
        return OS_EOK;
    }
    if (errno == ETIMEDOUT) {
        return OS_ETIMEOUT;
    }
    assert(errno == EAGAIN || errno == EINTR);
    return OS_EINTERRUPTED;
}

/** Setup storage is allocated once; all tested job operations reuse it. */
union __Storage {
    max_align_t Alignment;
    char        Bytes[16384];
};

struct __Fixture {
    union __Storage            PoolStorage;
    union __Storage            BufferStorage[3];
    OSFencePool_t         Pool;
    OSBufferReservation_t Buffers[3];
};

struct __Job {
    OSBufferAccessJob_t     Job;
    OSBufferAccessRequest_t Requests[8];
    OSBufferDependency_t    Dependencies[32];
};

static void
__InitializeFixture(
    _Out_   struct __Fixture* fixture,
    _In_    unsigned int      readers)
{
    OSBufferIdentity_t identity = { 1, 0, 1 };
    unsigned int            i;

    assert(OSFencePoolSize(64) <= sizeof(fixture->PoolStorage));
    assert(OSFencePoolInitialize(&fixture->PoolStorage, sizeof(fixture->PoolStorage), 0, 64) == OS_EOK);
    assert(OSFencePoolBind(&fixture->PoolStorage, sizeof(fixture->PoolStorage), &fixture->Pool) == OS_EOK);
    for (i = 0; i < 3; i++) {
        identity.Region = i;
        assert(OSBufferReservationInitialize(&fixture->BufferStorage[i],
            sizeof(fixture->BufferStorage[i]), &identity, readers) == OS_EOK);
        assert(OSBufferReservationBind(&fixture->BufferStorage[i],
            sizeof(fixture->BufferStorage[i]), &fixture->Buffers[i]) == OS_EOK);
    }
}

static void
__InitializeJob(
    _Out_   struct __Job* job)
{
    memset(job, 0, sizeof(*job));
    job->Job.Scratch = (OSBufferAccessScratch_t) {
        job->Requests, 8, job->Dependencies, 32
    };
}

static oserr_t
__Publish(
    _In_    struct __Fixture*                   fixture,
    _InOut_ struct __Job*                       job,
    _In_    const OSBufferAccessRequest_t* requests,
    _In_    size_t                              count)
{
    OSFenceReference_t completion = { 0 };
    oserr_t                 status;

    assert(OSFencePoolTryTake(&fixture->Pool, &completion) == OS_EOK);
    status = OSBufferAccessTryPublish(&fixture->Pool, 1, requests, count, &completion, &job->Job);
    assert(OSFencePoolRelease(&fixture->Pool, &completion) == OS_EOK);
    return status;
}

static void
__AssertDrained(
    _In_    struct __Fixture* fixture)
{
    OSFenceReference_t references[64] = { 0 };
    OSFenceReference_t extra = { 0 };
    unsigned int            i;

    for (i = 0; i < 3; i++) {
        assert(OSBufferReservationReclaim(&fixture->Pool, 1, &fixture->Buffers[i]) == OS_EOK);
    }
    for (i = 0; i < 64; i++) {
        assert(OSFencePoolTryTake(&fixture->Pool, &references[i]) == OS_EOK);
    }
    assert(OSFencePoolTryTake(&fixture->Pool, &extra) == OS_EBUSY);
    for (i = 0; i < 64; i++) {
        assert(OSFencePoolRelease(&fixture->Pool, &references[i]) == OS_EOK);
    }
}

static void
__TestPool(void)
{
    union __Storage         storage;
    OSFencePool_t      pool;
    OSFenceReference_t references[37] = { 0 };
    OSFenceReference_t alias;
    OSFenceReference_t extra = { 0 };
    OSFenceReference_t stale;
    OSTimestamp_t           deadline = { 0 };
    oserr_t                 result = OS_EUNKNOWN;
    unsigned int            i;

    assert(OSFencePoolSize(0) == 0);
    assert(OSFencePoolInitialize(storage.Bytes + 1, sizeof(storage) - 1, 0, 37) == OS_EINVALPARAMS);
    assert(OSFencePoolInitialize(&storage, 1, 0, 37) == OS_EINVALPARAMS);
    assert(OSFencePoolInitialize(&storage, sizeof(storage), 0, 37) == OS_EOK);
    assert(OSFencePoolBind(&storage, OSFencePoolSize(37) - 1, &pool) == OS_EINVALPARAMS);
    assert(OSFencePoolBind(&storage, sizeof(storage), &pool) == OS_EOK);
    for (i = 0; i < 37; i++) {
        assert(OSFencePoolTryTake(&pool, &references[i]) == OS_EOK);
        assert(references[i].Slot < 37);
    }
    assert(OSFencePoolTryTake(&pool, &extra) == OS_EBUSY);
    assert(OSFencePoolTryTake(&pool, &references[0]) == OS_EBUSY);
    assert(OSFencePoolPoll(&pool, &references[0], &result) == OS_EINVALPARAMS);
    assert(result == OS_EUNKNOWN);
    assert(OSFencePoolSignal(&pool, &references[0], OS_EOK) == OS_EINVALPARAMS);
    atomic_store(&pool.Slots[references[0].Slot].References, UINT_MAX);
    assert(OSFencePoolAcquire(&pool, &references[0]) == OS_EOVERFLOW);
    atomic_store(&pool.Slots[references[0].Slot].References, 1);
    assert(OSFencePoolSubmit(&pool, &references[0]) == OS_EOK);
    assert(OSFencePoolSubmit(&pool, &references[0]) == OS_EBUSY);
    assert(OSFencePoolRelease(&pool, &references[0]) == OS_EBUSY);
    g_allowFutex = true;
    assert(OSFencePoolWait(&pool, &references[0], &deadline, &result) == OS_ETIMEOUT);
    assert(atomic_load(&pool.Slots[references[0].Slot].Completion.Waiters) == 0);
    g_allowFutex = false;
    assert(OSFencePoolAcquire(&pool, &references[0]) == OS_EOK);
    alias = references[0];
    assert(OSFencePoolSignal(&pool, &references[0], OS_EDEVFAULT) == OS_EOK);
    assert(OSFencePoolSignal(&pool, &references[0], OS_EOK) == OS_EINVALPARAMS);
    assert(OSFencePoolRelease(&pool, &references[0]) == OS_EOK);
    assert(OSFencePoolTryTake(&pool, &extra) == OS_EBUSY);
    assert(OSFencePoolWait(&pool, &alias, NULL, &result) == OS_EOK && result == OS_EDEVFAULT);
    stale = alias;
    assert(OSFencePoolRelease(&pool, &alias) == OS_EOK);
    assert(OSFencePoolTryTake(&pool, &extra) == OS_EOK);
    assert(extra.Slot == stale.Slot && extra.Generation != stale.Generation);
    assert(OSFencePoolAcquire(&pool, &stale) == OS_ENOENT);
    assert(OSFencePoolRelease(&pool, &stale) == OS_ENOENT);
    assert(OSFencePoolPoll(&pool, &stale, &result) == OS_ENOENT);
    // Force the last generation while the unsubmitted slot is exclusively held.
    atomic_store(&pool.Slots[extra.Slot].Generation, UINT_MAX);
    extra.Generation = UINT_MAX;
    assert(OSFencePoolRelease(&pool, &extra) == OS_EOK);
    assert(OSFencePoolTryTake(&pool, &extra) == OS_EBUSY);
    for (i = 1; i < 37; i++) {
        assert(OSFencePoolRelease(&pool, &references[i]) == OS_EOK);
    }
}

static void
__TestDependencies(void)
{
    struct __Fixture           fixture;
    struct __Job               writer;
    struct __Job               first;
    struct __Job               second;
    struct __Job               next;
    struct __Job               later;
    OSBufferAccessRequest_t request;
    oserr_t                    result = OS_EUNKNOWN;
    unsigned int               calls = atomic_load(&g_futexCalls);
    unsigned int               logs = atomic_load(&g_fenceLogCalls);

    __InitializeFixture(&fixture, 2);
    __InitializeJob(&writer);
    __InitializeJob(&first);
    __InitializeJob(&second);
    __InitializeJob(&next);
    __InitializeJob(&later);
    request = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessWrite };
    assert(__Publish(&fixture, &writer, &request, 1) == OS_EOK);
    request.Mode = OSBufferAccessRead;
    assert(__Publish(&fixture, &first, &request, 1) == OS_EOK);
    assert(__Publish(&fixture, &second, &request, 1) == OS_EOK);
    assert(__Publish(&fixture, &later, &request, 1) == OS_EBUSY);
    assert(OSBufferAccessPoll(&first.Job, &result) == OS_EINPROGRESS && result == OS_EUNKNOWN);
    request.Mode = OSBufferAccessWrite;
    assert(__Publish(&fixture, &next, &request, 1) == OS_EOK);
    request.Mode = OSBufferAccessRead;
    assert(__Publish(&fixture, &later, &request, 1) == OS_EOK);
    // A cancelled replacement writer still protects its unfinished predecessors.
    assert(OSBufferAccessFinish(&next.Job, OS_ECANCELLED) == OS_EINPROGRESS);
    assert(OSBufferAccessFinish(&writer.Job, OS_EOK) == OS_EOK);
    assert(OSBufferAccessPoll(&first.Job, &result) == OS_EOK && result == OS_EOK);
    assert(OSBufferAccessPoll(&second.Job, &result) == OS_EOK && result == OS_EOK);
    assert(OSBufferAccessFinish(&first.Job, OS_EOK) == OS_EOK);
    assert(OSBufferAccessFinish(&next.Job, OS_ECANCELLED) == OS_EINPROGRESS);
    assert(OSBufferAccessPoll(&later.Job, &result) == OS_EINPROGRESS);
    assert(OSBufferAccessFinish(&second.Job, OS_EDEVFAULT) == OS_EOK);
    // Failed reads end access but do not invalidate data for a later writer.
    assert(OSBufferAccessPoll(&next.Job, &result) == OS_EOK && result == OS_EOK);
    assert(OSBufferAccessFinish(&next.Job, OS_ECANCELLED) == OS_EOK);
    assert(OSBufferAccessPoll(&later.Job, &result) == OS_EOK && result == OS_ECANCELLED);
    assert(OSBufferAccessFinish(&later.Job, OS_EOK) == OS_EOK);
    __AssertDrained(&fixture);
    assert(atomic_load(&g_futexCalls) == calls);
#ifndef __TRACE
    assert(atomic_load(&g_fenceLogCalls) == logs);
#else
    (void)logs;
#endif
}

static void
__TestResultsAndAliases(void)
{
    struct __Fixture            fixture;
    struct __Job                writer;
    struct __Job                reader;
    struct __Job                overwrite;
    OSBufferReservation_t  alias;
    OSBufferAccessRequest_t requests[3];
    oserr_t                     result;

    __InitializeFixture(&fixture, 2);
    __InitializeJob(&writer);
    __InitializeJob(&reader);
    __InitializeJob(&overwrite);
    assert(OSBufferReservationBind(&fixture.BufferStorage[0],
        sizeof(fixture.BufferStorage[0]), &alias) == OS_EOK);
    requests[0] = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessWrite };
    assert(__Publish(&fixture, &writer, requests, 1) == OS_EOK);
    assert(OSBufferAccessFinish(&writer.Job, OS_EDEVFAULT) == OS_EOK);
    __AssertDrained(&fixture);
    requests[0].Mode = OSBufferAccessRead;
    assert(__Publish(&fixture, &reader, requests, 1) == OS_EOK);
    assert(OSBufferAccessPoll(&reader.Job, &result) == OS_EOK && result == OS_EDEVFAULT);
    assert(OSBufferAccessFinish(&reader.Job, OS_EOK) == OS_EOK);
    requests[0].Mode = OSBufferAccessOverwrite;
    assert(__Publish(&fixture, &overwrite, requests, 1) == OS_EOK);
    assert(OSBufferAccessPoll(&overwrite.Job, &result) == OS_EOK && result == OS_EOK);
    assert(OSBufferAccessFinish(&overwrite.Job, OS_EOK) == OS_EOK);
    requests[0] = (OSBufferAccessRequest_t) { &fixture.Buffers[1], OSBufferAccessRead };
    requests[1] = (OSBufferAccessRequest_t) { &alias, OSBufferAccessOverwrite };
    requests[2] = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessRead };
    assert(__Publish(&fixture, &writer, requests, 3) == OS_EOK);
    assert(writer.Job.BufferCount == 2);
    assert(writer.Job.Scratch.Buffers[0].Mode == OSBufferAccessWrite);
    assert(writer.Job.Scratch.Buffers[0].Buffer->Identity.Region == 0);
    requests[0] = (OSBufferAccessRequest_t) { &fixture.Buffers[2], OSBufferAccessWrite };
    assert(__Publish(&fixture, &reader, requests, 1) == OS_EOK);
    assert(OSBufferAccessPoll(&reader.Job, &result) == OS_EOK);
    assert(OSBufferAccessFinish(&reader.Job, OS_EOK) == OS_EOK);
    assert(OSBufferAccessFinish(&writer.Job, OS_EOK) == OS_EOK);
    __AssertDrained(&fixture);
}

static void
__TestPoolBindingsAndInputBounds(void)
{
    struct __Fixture            fixture;
    union __Storage             storage;
    OSFencePool_t          pools[2];
    struct __Job                writer;
    struct __Job                reader;
    OSFenceReference_t     completion = { 0 };
    OSBufferAccessRequest_t requests[7];
    OSBufferReservation_t  stale;
    unsigned int               order[7] = { 2, 1, 2, 0, 1, 0, 2 };
    unsigned int               i;
    oserr_t                    result;

    __InitializeFixture(&fixture, 2);
    __InitializeJob(&writer);
    __InitializeJob(&reader);
    pools[0] = fixture.Pool;
    assert(OSFencePoolInitialize(&storage, sizeof(storage), 1, 4) == OS_EOK);
    assert(OSFencePoolBind(&storage, sizeof(storage), &pools[1]) == OS_EOK);
    assert(OSBufferReservationSize(0) == 0);
    assert(OSBufferReservationBind(fixture.BufferStorage[0].Bytes + 1,
        sizeof(fixture.BufferStorage[0]) - 1, &stale) == OS_EINVALPARAMS);
    assert(OSBufferReservationBind(&fixture.BufferStorage[0],
        OSBufferReservationSize(2) - 1, &stale) == OS_EINVALPARAMS);
    for (i = 0; i < 7; i++) {
        requests[i] = (OSBufferAccessRequest_t) { &fixture.Buffers[order[i]], OSBufferAccessWrite };
    }
    assert(OSFencePoolTryTake(&pools[1], &completion) == OS_EOK);
    writer.Job.Scratch.BufferCapacity = 2;
    assert(OSBufferAccessTryPublish(pools, 2, requests, 7, &completion, &writer.Job) == OS_EBUFFER);
    writer.Job.Scratch.BufferCapacity = 8;
    assert(OSBufferAccessTryPublish(pools, 1, requests, 7, &completion, &writer.Job) == OS_EINVALPARAMS);
    stale = fixture.Buffers[0];
    stale.Identity.Generation++;
    requests[0].Buffer = &stale;
    assert(OSBufferAccessTryPublish(pools, 2, requests, 7, &completion, &writer.Job) == OS_ENOENT);
    requests[0].Buffer = &fixture.Buffers[2];
    assert(OSBufferAccessTryPublish(pools, 2, requests, 7, &completion, &writer.Job) == OS_EOK);
    assert(writer.Job.BufferCount == 3);
    for (i = 0; i < 3; i++) {
        assert(writer.Requests[i].Buffer->Identity.Region == i);
    }
    requests[0] = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessRead };
    assert(OSFencePoolTryTake(&pools[0], &completion) == OS_EOK);
    assert(OSBufferAccessTryPublish(pools, 2, requests, 1, &completion, &reader.Job) == OS_EOK);
    assert(reader.Dependencies[0].Fence.Pool == 1);
    assert(OSBufferAccessPoll(&reader.Job, &result) == OS_EINPROGRESS);
    assert(OSBufferAccessFinish(&writer.Job, OS_EOK) == OS_EOK);
    assert(OSBufferAccessFinish(&reader.Job, OS_EOK) == OS_EOK);
    for (i = 0; i < 3; i++) {
        assert(OSBufferReservationReclaim(pools, 2, &fixture.Buffers[i]) == OS_EOK);
    }
    assert(atomic_load(&pools[1].Available[0]) == 15);
    __AssertDrained(&fixture);
}

static void
__TestRollback(void)
{
    struct __Fixture            fixture;
    struct __Job                first;
    struct __Job                second;
    struct __Job                attempt;
    OSBufferAccessRequest_t requests[2];
    OSFenceReference_t     completion = { 0 };
    union __Storage             before[2];
    oserr_t                     result = OS_EUNKNOWN;
    unsigned int                references;

    __InitializeFixture(&fixture, 1);
    __InitializeJob(&first);
    __InitializeJob(&second);
    __InitializeJob(&attempt);
    requests[0] = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessWrite };
    requests[1] = (OSBufferAccessRequest_t) { &fixture.Buffers[1], OSBufferAccessRead };
    assert(__Publish(&fixture, &first, requests, 1) == OS_EOK);
    assert(__Publish(&fixture, &second, requests + 1, 1) == OS_EOK);
    memcpy(before, fixture.BufferStorage, sizeof(before));
    assert(OSFencePoolTryTake(&fixture.Pool, &completion) == OS_EOK);
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 2, &completion, &attempt.Job) == OS_EBUSY);
    assert(memcmp(before, fixture.BufferStorage, sizeof(before)) == 0);
    assert(atomic_load(&fixture.Pool.Slots[completion.Slot].References) == 1);
    assert(attempt.Job.Completion.Generation == 0);
    attempt.Job.Scratch.DependencyCapacity = 0;
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 1, &completion, &attempt.Job) == OS_EBUFFER);
    assert(memcmp(before, fixture.BufferStorage, sizeof(before)) == 0);
    attempt.Job.Scratch.DependencyCapacity = 32;
    atomic_store(&fixture.Pool.Slots[completion.Slot].References, UINT_MAX - 1);
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 1, &completion, &attempt.Job) == OS_EOVERFLOW);
    assert(atomic_load(&fixture.Pool.Slots[completion.Slot].References) == UINT_MAX - 1);
    atomic_store(&fixture.Pool.Slots[completion.Slot].References, 1);
    references = atomic_load(&fixture.Pool.Slots[first.Job.Completion.Slot].References);
    atomic_store(&fixture.Pool.Slots[first.Job.Completion.Slot].References, UINT_MAX);
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 1, &completion, &attempt.Job) == OS_EOVERFLOW);
    atomic_store(&fixture.Pool.Slots[first.Job.Completion.Slot].References, references);
    assert(memcmp(before, fixture.BufferStorage, sizeof(before)) == 0);
    atomic_store(&fixture.Buffers[1].Storage->Lock, 1);
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 2, &completion, &attempt.Job) == OS_EBUSY);
    assert(atomic_load(&fixture.Buffers[0].Storage->Lock) == 0);
    assert(OSBufferReservationReclaim(&fixture.Pool, 1, &fixture.Buffers[1]) == OS_EBUSY);
    atomic_store(&fixture.Buffers[1].Storage->Lock, 0);
    requests[1].Mode = OSBufferAccessWrite;
    assert(OSBufferAccessTryPublish(&fixture.Pool, 1, requests, 2, &completion, &attempt.Job) == OS_EOK);
    assert(completion.Generation == 0);
    assert(OSBufferAccessFinish(&first.Job, OS_EDEVFAULT) == OS_EOK);
    assert(OSBufferAccessPoll(&attempt.Job, &result) == OS_EINPROGRESS && result == OS_EUNKNOWN);
    assert(OSBufferAccessFinish(&attempt.Job, OS_ECANCELLED) == OS_EINPROGRESS);
    assert(OSBufferAccessFinish(&second.Job, OS_EOK) == OS_EOK);
    assert(OSBufferAccessPoll(&attempt.Job, &result) == OS_EOK && result == OS_EDEVFAULT);
    assert(OSBufferAccessFinish(&attempt.Job, OS_EOK) == OS_EOK);
    __AssertDrained(&fixture);
}

static OSFencePool_t* g_notifyPool;
static OSFenceReference_t g_notifyObserver;
static void
__ReleaseAtNotification(void)
{
    OSFenceReference_t other = { 0 };

    assert(OSFencePoolRelease(g_notifyPool, &g_notifyObserver) == OS_EOK);
    assert(OSFencePoolTryTake(g_notifyPool, &other) == OS_EBUSY);
}

static void
__TestNotification(void)
{
    struct __Fixture            fixture;
    struct __Job                job;
    OSBufferAccessRequest_t request;
    OSFenceReference_t     producer = { 0 };
    OSFenceReference_t     observer;
    oserr_t                     result;

    __InitializeFixture(&fixture, 1);
    assert(OSFencePoolInitialize(&fixture.PoolStorage, sizeof(fixture.PoolStorage), 0, 1) == OS_EOK);
    assert(OSFencePoolBind(&fixture.PoolStorage, sizeof(fixture.PoolStorage), &fixture.Pool) == OS_EOK);
    assert(OSFencePoolTryTake(&fixture.Pool, &producer) == OS_EOK);
    assert(OSFencePoolSubmit(&fixture.Pool, &producer) == OS_EOK);
    assert(OSFencePoolAcquire(&fixture.Pool, &producer) == OS_EOK);
    g_notifyObserver = producer;
    g_notifyPool = &fixture.Pool;
    atomic_store(&fixture.Pool.Slots[producer.Slot].Completion.Waiters, 1);
    g_beforeWake = __ReleaseAtNotification;
    g_allowFutex = true;
    g_wakeError = OS_EUNKNOWN;
    assert(OSFencePoolSignal(&fixture.Pool, &producer, OS_EOK) == OS_EUNKNOWN);
    assert(OSFencePoolPoll(&fixture.Pool, &producer, &result) == OS_EOK && result == OS_EOK);
    assert(OSFencePoolSignal(&fixture.Pool, &producer, OS_EDEVFAULT) == OS_EINVALPARAMS);
    g_wakeError = OS_EOK;
    assert(OSFencePoolSignal(&fixture.Pool, &producer, OS_EOK) == OS_EOK);
    assert(OSFencePoolRelease(&fixture.Pool, &producer) == OS_EOK);
    __InitializeJob(&job);
    request = (OSBufferAccessRequest_t) { &fixture.Buffers[0], OSBufferAccessWrite };
    assert(__Publish(&fixture, &job, &request, 1) == OS_EOK);
    assert(OSFencePoolAcquire(&fixture.Pool, &job.Job.Completion) == OS_EOK);
    observer = job.Job.Completion;
    atomic_store(&fixture.Pool.Slots[observer.Slot].Completion.Waiters, 1);
    g_wakeError = OS_EUNKNOWN;
    assert(OSBufferAccessFinish(&job.Job, OS_ECANCELLED) == OS_EUNKNOWN);
    assert(job.Job.Completion.Generation != 0);
    assert(OSFencePoolPoll(&fixture.Pool, &observer, &result) == OS_EOK && result == OS_ECANCELLED);
    g_wakeError = OS_EOK;
    assert(OSBufferAccessFinish(&job.Job, OS_EOK) == OS_EINVALPARAMS);
    assert(OSBufferAccessFinish(&job.Job, OS_ECANCELLED) == OS_EOK);
    assert(OSFencePoolRelease(&fixture.Pool, &observer) == OS_EOK);
    assert(OSBufferReservationReclaim(&fixture.Pool, 1, &fixture.Buffers[0]) == OS_EOK);
    g_allowFutex = false;
}

struct __Concurrent {
    struct __Fixture* Fixture;
    unsigned int      ID;
    atomic_uint*      Readers;
    atomic_uint*      Writers;
    int*              Value;
};

static void*
__RunJobs(
    _In_    void* argument)
{
    struct __Concurrent*        context = argument;
    struct __Job                job;
    OSBufferAccessRequest_t requests[2];
    OSFenceReference_t     completion;
    unsigned int                i;
    oserr_t                     status;
    oserr_t                     result;

    __InitializeJob(&job);
    requests[0] = (OSBufferAccessRequest_t) { &context->Fixture->Buffers[context->ID % 2], OSBufferAccessRead };
    requests[1] = (OSBufferAccessRequest_t) { &context->Fixture->Buffers[1 - context->ID % 2], OSBufferAccessRead };
    for (i = 0; i < 300; i++) {
        requests[0].Mode = i % 2 ? OSBufferAccessWrite : OSBufferAccessRead;
        requests[1].Mode = requests[0].Mode;
        completion = (OSFenceReference_t) { 0 };
        assert(OSFencePoolTryTake(&context->Fixture->Pool, &completion) == OS_EOK);
        do {
            status = OSBufferAccessTryPublish(&context->Fixture->Pool, 1, requests, 2, &completion, &job.Job);
            if (status == OS_EBUSY) {
                sched_yield();
            }
        } while (status == OS_EBUSY);
        assert(status == OS_EOK);
        do {
            status = OSBufferAccessPoll(&job.Job, &result);
            if (status == OS_EINPROGRESS) {
                sched_yield();
            }
        } while (status == OS_EINPROGRESS);
        assert(status == OS_EOK && result == OS_EOK);
        if (requests[0].Mode == OSBufferAccessWrite) {
            assert(atomic_fetch_add(context->Writers, 1) == 0);
            assert(atomic_load(context->Readers) == 0);
            (*context->Value)++;
            sched_yield();
            assert(atomic_fetch_sub(context->Writers, 1) == 1);
        } else {
            atomic_fetch_add(context->Readers, 1);
            assert(atomic_load(context->Writers) == 0);
            assert(*context->Value >= 0);
            sched_yield();
            atomic_fetch_sub(context->Readers, 1);
        }
        assert(OSBufferAccessFinish(&job.Job, OS_EOK) == OS_EOK);
    }
    return NULL;
}

static void
__TestConcurrentPublication(void)
{
    struct __Fixture    fixture;
    struct __Concurrent contexts[4];
    pthread_t           threads[4];
    atomic_uint         readers = 0;
    atomic_uint         writers = 0;
    int                 value = 0;
    unsigned int        i;

    __InitializeFixture(&fixture, 4);
    for (i = 0; i < 4; i++) {
        contexts[i] = (struct __Concurrent) { &fixture, i, &readers, &writers, &value };
        assert(pthread_create(&threads[i], NULL, __RunJobs, &contexts[i]) == 0);
    }
    for (i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    assert(value == 600);
    __AssertDrained(&fixture);
}

static void
__TestSharedMappings(void)
{
    const size_t                size = 32768;
    FILE*                       file;
    char*                       parent;
    char*                       peer;
    OSFencePool_t          pool;
    OSBufferReservation_t  buffer;
    OSBufferIdentity_t     identity = { 42, 0, 1 };
    OSBufferAccessRequest_t request;
    OSFenceReference_t     completion = { 0 };
    struct __Job                first;
    struct __Job                next;
    oserr_t                     result;
    pid_t                       child;
    int                         ready[2];
    int                         status;
    char                        message = 1;

    file = tmpfile();
    assert(file != NULL && ftruncate(fileno(file), size) == 0);
    parent = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
    peer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(file), 0);
    assert(parent != MAP_FAILED && peer != MAP_FAILED && parent != peer);
    fclose(file);
    assert(OSFencePoolInitialize(parent, 16384, 0, 64) == OS_EOK);
    assert(OSBufferReservationInitialize(parent + 16384, 4096, &identity, 4) == OS_EOK);
    assert(OSFencePoolBind(parent, 16384, &pool) == OS_EOK);
    assert(OSBufferReservationBind(parent + 16384, 4096, &buffer) == OS_EOK);
    __InitializeJob(&first);
    __InitializeJob(&next);
    request = (OSBufferAccessRequest_t) { &buffer, OSBufferAccessWrite };
    assert(OSFencePoolTryTake(&pool, &completion) == OS_EOK);
    assert(OSBufferAccessTryPublish(&pool, 1, &request, 1, &completion, &first.Job) == OS_EOK);
    assert(pipe(ready) == 0);
    g_allowFutex = true;
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(munmap(parent, size) == 0);
        close(ready[0]);
        assert(OSFencePoolBind(peer, 16384, &pool) == OS_EOK);
        assert(OSBufferReservationBind(peer + 16384, 4096, &buffer) == OS_EOK);
        request.Mode = OSBufferAccessRead;
        assert(OSFencePoolTryTake(&pool, &completion) == OS_EOK);
        assert(OSBufferAccessTryPublish(&pool, 1, &request, 1, &completion, &next.Job) == OS_EOK);
        assert(next.Job.DependencyCount == 1);
        assert(write(ready[1], &message, 1) == 1);
        assert(OSFencePoolWait(&pool, &next.Dependencies[0].Fence, NULL, &result) == OS_EOK);
        assert(result == OS_EOK && *(int*)(peer + 24576) == 42);
        assert(OSBufferAccessFinish(&next.Job, OS_EOK) == OS_EOK);
        assert(munmap(peer, size) == 0);
        _exit(0);
    }
    assert(munmap(peer, size) == 0);
    close(ready[1]);
    assert(read(ready[0], &message, 1) == 1);
    // Capture the remote reader before ending the first writer.
    assert(OSFencePoolTryTake(&pool, &completion) == OS_EOK);
    assert(OSBufferAccessTryPublish(&pool, 1, &request, 1, &completion, &next.Job) == OS_EOK);
    assert(next.Job.DependencyCount == 2);
    *(int*)(parent + 24576) = 42;
    assert(OSBufferAccessFinish(&first.Job, OS_EOK) == OS_EOK);
    assert(OSFencePoolWait(&pool, &next.Dependencies[1].Fence, NULL, &result) == OS_EOK);
    assert(OSBufferAccessFinish(&next.Job, OS_EOK) == OS_EOK);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(OSBufferReservationReclaim(&pool, 1, &buffer) == OS_EOK);
    close(ready[0]);
    assert(munmap(parent, size) == 0);
    g_allowFutex = false;
}

int
main(void)
{
    __TestPool();
    __TestDependencies();
    __TestResultsAndAliases();
    __TestPoolBindingsAndInputBounds();
    __TestRollback();
    __TestNotification();
    __TestConcurrentPublication();
    __TestSharedMappings();
    puts("Pooled completions and buffer access: bounds, rollback, generations, ordering, cancellation, concurrent publication and shared mappings passed");
    return 0;
}
