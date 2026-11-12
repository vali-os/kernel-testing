// Exercise production job, wait and dispatch code with deterministic platform adapters.
#include <stdlib.h>
static void __TestFree(void* memory);
#define free __TestFree
#ifndef SCHEDULER_SOURCE
#define SCHEDULER_SOURCE "../librt/libos/uthreads/scheduler.c"
#endif
#include SCHEDULER_SOURCE
#include "../librt/libos/uthreads/job.c"
#include "../librt/libos/uthreads/mutex.c"
#include "../librt/libos/uthreads/cond.c"
#undef free

extern _Noreturn void test_exit(int status);
#define CHECK(condition) do { if (!(condition)) test_exit(__LINE__ % 200 + 1); } while (0)

/**
 * @brief Tracks stack bounds and finalization for each allocated test job.
 */
struct __TestJob {
    struct usched_job* Job;
    uintptr_t         Stack;
    unsigned int      StackSize;
    bool              Finalized;
    bool              Freed;
};

static struct usched_execution_context g_executions[3];
static struct execution_unit_tls g_units[3];
static struct thread_storage g_nativeTls[3];
static uint64_t g_slots[3][12];
static struct execution_manager g_manager;
static struct job_entry g_entries[64];
static struct __TestJob g_jobs[64];
static int g_jobCount;
static int g_phase;
static struct timespec g_now;
static void (*g_afterSchedulerUnlock)(void);
static bool g_remoteTimerScan;
static int g_freedJobs;
static int g_schedulerVisits;
static int g_handleCount;
static int g_handlesDestroyed;
static int g_lockDepth;
static struct usched_job* g_dedicatedJob;
static struct usched_mtx g_mutex;
static struct usched_cnd g_condition;

static uintptr_t
__StackPointer(void)
{
    uintptr_t stack;

    __asm__ volatile("mov %0, sp" : "=r"(stack));
    return stack;
}

static bool
__OnJobStack(
    _In_ struct __TestJob* job)
{
    uintptr_t stack;

    stack = __StackPointer();
    return stack >= job->Stack && stack < job->Stack + job->StackSize;
}

static void
__CheckNativeStack(void)
{
    int i;

    CHECK(__tls_current() == __usched_get_execution()->tls);
    CHECK(__usched_get_execution()->current == NULL);
    for (i = 0; i < g_jobCount; i++) {
        CHECK(!__OnJobStack(&g_jobs[i]));
    }
}

static void
__SelectUnit(
    _In_ int index)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_slots[index]) : "memory");
    __tls_switch(&g_nativeTls[index]);
}

static void
__TestFree(
    _In_ void* memory)
{
    int i;

    for (i = 0; i < g_jobCount; i++) {
        if (memory == g_jobs[i].Job || (uintptr_t)memory == g_jobs[i].Stack) {
            __CheckNativeStack();
            CHECK(g_jobs[i].Finalized);
            if (memory == g_jobs[i].Job) {
                CHECK(!g_jobs[i].Freed);
                g_jobs[i].Freed = true;
                g_freedJobs++;
            }
        }
    }
    free(memory);
}

static uuid_t
__Queue(
    _In_ usched_task_fn entry)
{
    struct usched_job_parameters params;
    uuid_t id;

    usched_job_parameters_init(&params);
    params.stack_size = 65536;
    id = usched_job_queue3(entry, NULL, &params);
    CHECK(id != UUID_INVALID);
    return id;
}

static void
__Child(
    _In_ void* argument,
    _In_ void* token)
{
    (void)argument;
    (void)token;
    CHECK(g_phase == 1);
    g_phase = 2;
    usched_job_exit(37);
    test_exit(201);
}

static void
__WakeCondition(
    _In_ void* argument,
    _In_ void* token)
{
    (void)argument;
    (void)token;
    CHECK(g_phase == 3);
    usched_mtx_lock(&g_mutex);
    g_phase = 4;
    usched_cnd_notify_one(&g_condition);
    usched_mtx_unlock(&g_mutex);
}

static void
__WaitForMutex(
    _In_ void* argument,
    _In_ void* token)
{
    (void)argument;
    (void)token;
    CHECK(g_phase == 5);
    g_phase = 6;
    usched_mtx_lock(&g_mutex);
    CHECK(g_phase == 7);
    g_phase = 8;
    usched_mtx_unlock(&g_mutex);
}

static void
__Parent(
    _In_ void* argument,
    _In_ void* token)
{
    struct usched_job* job;
    struct usched_wait earlyWait;
    struct timespec deadline = { 0, 0 };
    uuid_t child;
    int visits;
    int exitCode;

    (void)argument;
    job = token;
    CHECK(job == g_executions[0].current && job->state == JobState_RUNNING);
    visits = g_schedulerVisits;
    job->tls.seed = 1234;
    usched_job_yield();
    CHECK(g_schedulerVisits > visits);
    CHECK(__tls_current() == &job->tls && job->tls.seed == 1234);

    g_phase = 1;
    child = __Queue(__Child);
    usched_job_yield();
    CHECK(g_phase == 2 && g_freedJobs == 1);
    CHECK(usched_job_join(child, &exitCode) == 0 && exitCode == 37);

    // A remote unit notifies after registration but before this job yields.
    spinlock_acquire(&g_condition.lock);
    __usched_wait_prepare(&earlyWait, NULL);
    __usched_wait_enqueue(&g_condition.queue, &earlyWait);
    spinlock_release(&g_condition.lock);
    __SelectUnit(1);
    usched_cnd_notify_one(&g_condition);
    CHECK(g_scheduler.ready == NULL);
    CHECK(job->state == JobState_RUNNING && g_executions[0].current == job);
    __SelectUnit(0);
    __tls_switch(&job->tls);
    usched_job_yield();
    CHECK(job->state == JobState_RUNNING && g_scheduler.ready == NULL);
    CHECK(earlyWait.Result == UschedWaitStatus_SIGNALLED && g_condition.queue == NULL);

    g_phase = 3;
    child = __Queue(__WakeCondition);
    usched_mtx_lock(&g_mutex);
    usched_cnd_wait(&g_condition, &g_mutex);
    CHECK(g_phase == 4 && g_freedJobs == 2);
    CHECK(g_mutex.owner == job);
    usched_mtx_unlock(&g_mutex);
    CHECK(usched_job_join(child, &exitCode) == 0);

    g_phase = 5;
    usched_mtx_lock(&g_mutex);
    child = __Queue(__WaitForMutex);
    usched_job_yield();
    CHECK(g_phase == 6);
    g_phase = 7;
    usched_mtx_unlock(&g_mutex);
    CHECK(usched_job_join(child, &exitCode) == 0);
    CHECK(g_phase == 8 && g_freedJobs == 3);

    CHECK(usched_job_sleep(&deadline) == 0);
    CHECK(g_scheduler.timers == NULL);
    job->async_context.ErrorCode = OS_EOK;
    usched_wait_async();
    CHECK(g_scheduler.syscalls_pending == NULL);

    // Returning to entry() leaves this saved context on the shared ready queue.
    g_phase = 9;
    __usched_request_stop(&g_executions[0]);
    job->async_context.ErrorCode = OS_ESCSTARTED;
    usched_wait_async();
    CHECK(g_scheduler.syscalls_pending == NULL);
    usched_job_yield();
    CHECK(__usched_get_execution() == &g_executions[1]);
    CHECK(__tls_current() == &job->tls && job->tls.seed == 1234);
    CHECK(job->scheduler == &g_scheduler && job->state == JobState_RUNNING);
    g_phase = 10;
}

static void
__Dedicated(
    _In_ void* argument,
    _In_ void* token)
{
    struct usched_job* job;

    (void)argument;
    job = token;
    CHECK(__usched_get_execution() == &g_executions[2]);
    CHECK(job->scheduler != &g_scheduler);
    CHECK(job->scheduler == g_executions[2].scheduler);
    usched_job_yield();
    CHECK(g_executions[2].current == job);
}

static void
__HandoffMain(void)
{
    struct usched_job_parameters params;
    struct timespec deadline;
    int i;
    int destroyed;

    for (i = 0; i < 3; i++) {
        g_units[i].context = &g_executions[i];
        g_slots[i][2] = (uintptr_t)&g_units[i];
    }
    __SelectUnit(0);
    __usched_startup();
    __usched_init(&g_executions[0], &(struct usched_init_params) { NULL });
    __SelectUnit(1);
    __usched_init(&g_executions[1], &(struct usched_init_params) { NULL });
    CHECK(g_executions[0].scheduler == g_executions[1].scheduler);

    __SelectUnit(0);
    usched_mtx_init(&g_mutex, USCHED_MUTEX_PLAIN);
    usched_cnd_init(&g_condition);
    __Queue(__Parent);
    CHECK(usched_yield(&deadline) == -1 && errno == ESHUTDOWN);
    CHECK(g_scheduler.syscalls_pending != NULL);
    __CheckNativeStack();
    CHECK(g_phase == 9 && g_scheduler.ready == NULL);
    destroyed = g_handlesDestroyed;
    __usched_destroy(&g_executions[0]);
    CHECK(g_handlesDestroyed == destroyed + 1);
    __atomic_store_n(&g_scheduler.syscalls_pending->async_context->ErrorCode, OS_EOK, __ATOMIC_RELEASE);

    __SelectUnit(1);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    __CheckNativeStack();
    CHECK(g_phase == 10 && g_freedJobs == 4);

    usched_job_parameters_init(&params);
    params.stack_size = 65536;
    params.detached = true;
    CHECK(usched_job_queue3(__Dedicated, NULL, &params) != UUID_INVALID);
    CHECK(g_scheduler.ready == NULL && g_dedicatedJob != NULL);
    __SelectUnit(2);
    __usched_init(&g_executions[2], &(struct usched_init_params) { g_dedicatedJob });
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_freedJobs == 5 && g_lockDepth == 0);
    destroyed = g_handlesDestroyed;
    __usched_destroy(&g_executions[2]);
    CHECK(g_handlesDestroyed == destroyed + 2);

    // Pooled context destruction must leave the shared scheduler usable.
    __SelectUnit(1);
    destroyed = g_handlesDestroyed;
    __usched_destroy(&g_executions[0]);
    CHECK(g_handlesDestroyed == destroyed);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    __usched_destroy(&g_executions[1]);

    // A request made before the kernel thread starts survives initialization.
    memset(&g_executions[0], 0, sizeof(g_executions[0]));
    __usched_request_stop(&g_executions[0]);
    __SelectUnit(0);
    __usched_init(&g_executions[0], &(struct usched_init_params) { NULL });
    CHECK(usched_yield(&deadline) == -1 && errno == ESHUTDOWN);
    __usched_destroy(&g_executions[0]);
    test_exit(0);
}

#ifdef SHARED_WAITS
#include "scheduler_shared_waits_test.inc"
#endif

void
entry(void)
{
#ifdef SHARED_WAITS
    __SharedWaitsMain();
#else
    __HandoffMain();
#endif
}

// The adapters below model platform bookkeeping, not scheduling decisions.
struct execution_manager*
__xunit_manager(void)
{
    return &g_manager;
}

struct execution_unit_tls*
__usched_xunit_tls_current(void)
{
    return (void*)__get_reserved(2);
}

int
__xunit_start_detached(
    _In_ struct usched_job*            job,
    _In_ struct usched_job_parameters* params)
{
    CHECK(params->detached);
    g_dedicatedJob = job;
    return 0;
}

void*
hashtable_get(
    _In_ hashtable_t* table,
    _In_ const void*  key)
{
    int i;

    (void)table;
    for (i = 0; i < g_jobCount; i++) {
        if (g_entries[i].id == ((const struct job_entry*)key)->id) {
            return &g_entries[i];
        }
    }
    return NULL;
}

void*
hashtable_set(
    _In_ hashtable_t* table,
    _In_ const void*  value)
{
    struct usched_job* job;

    (void)table;
    CHECK(g_jobCount < 64);
    g_entries[g_jobCount] = *(const struct job_entry*)value;
    job = g_entries[g_jobCount].context->job;
    g_jobs[g_jobCount].Job = job;
    g_jobs[g_jobCount].Stack = (uintptr_t)job->stack;
    g_jobs[g_jobCount].StackSize = job->stack_size;
    g_jobCount++;
    return NULL;
}

void
__cxa_threadinitialize(void)
{
    CHECK(__tls_current() == &__usched_get_execution()->current->tls);
}

void
__cxa_threadfinalize(void)
{
    int i;

    for (i = 0; i < g_jobCount; i++) {
        if (g_jobs[i].Job == __usched_get_execution()->current) {
            CHECK(__OnJobStack(&g_jobs[i]) && !g_jobs[i].Finalized);
            g_jobs[i].Finalized = true;
            return;
        }
    }
    test_exit(202);
}

void*
calloc(
    _In_ size_t count,
    _In_ size_t size)
{
    void* memory;

    memory = malloc(count * size);
    if (memory != NULL) {
        memset(memory, 0, count * size);
    }
    return memory;
}

const char* const*
__crt_environment(void)
{
    static const char* const empty[] = { NULL };

    return empty;
}

oserr_t
MutexInitialize(
    _In_ Mutex_t* mutex,
    _In_ int      flags)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->Flags = flags;
    return OS_EOK;
}

oserr_t
MutexLock(
    _In_ Mutex_t* mutex)
{
    CHECK(atomic_exchange(&mutex->Value, 1) == 0);
    g_lockDepth++;
    return OS_EOK;
}

oserr_t
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    void (*callback)(void);

    CHECK(atomic_exchange(&mutex->Value, 0) == 1);
    g_lockDepth--;
    if (mutex == &g_scheduler.lock && g_afterSchedulerUnlock != NULL) {
        callback = g_afterSchedulerUnlock;
        g_afterSchedulerUnlock = NULL;
        callback();
    }
    return OS_EOK;
}

int
timespec_get(
    _Out_ struct timespec* time,
    _In_  int              base)
{
    if (!g_remoteTimerScan) {
        __CheckNativeStack();
    }
    g_schedulerVisits++;
    *time = g_now;
    return base;
}

void
timespec_diff(
    _In_  const struct timespec* a,
    _In_  const struct timespec* b,
    _Out_ struct timespec*       result)
{
    (void)a;
    (void)b;
    (void)result;
    __builtin_trap();
}

oserr_t
OSHandleCreate(
    _In_  OSHandle_t* registration,
    _In_  uint32_t    accessMask,
    _Out_ OSHandle_t* handle)
{
    CHECK(registration == NULL && accessMask == 0);
    handle->ID = ++g_handleCount;
    return OS_EOK;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID != 0);
    g_handlesDestroyed++;
    handle->ID = 0;
}

oserr_t
OSNotificationQueueCreate(
    _In_  unsigned int flags,
    _Out_ OSHandle_t*   handle)
{
    (void)flags;
    return OSHandleCreate(NULL, 0, handle);
}

oserr_t
OSNotificationQueueCtrl(
    _In_ OSHandle_t*         set,
    _In_ int                 operation,
    _In_ OSHandle_t*         handle,
    _In_ struct ioset_event* event)
{
    CHECK(set->ID && handle->ID);
    CHECK(operation == IOSET_ADD && event->events == IOSETSYN);
    CHECK(handle == &__usched_get_execution()->scheduler->notification_handle);
    return OS_EOK;
}

oserr_t
OSNotificationQueuePost(
    _In_ OSHandle_t*  handle,
    _In_ unsigned int flags)
{
    int i;

    (void)handle;
    CHECK(flags == IOSETSYN);
    for (i = 0; i < g_jobCount; i++) {
        if (!g_jobs[i].Freed && __JOB_STATE(g_jobs[i].Job) == JobState_READY) {
            CHECK(!__OnJobStack(&g_jobs[i]));
        }
    }
    return OS_EOK;
}
