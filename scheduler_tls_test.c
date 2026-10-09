// Execute the real dispatch/resume paths with deterministic platform adapters.
#include "../librt/libos/uthreads/scheduler.c"
extern void test_exit(int);
extern int fail_allocation, live_allocations;
#define CHECK(x) do { if (!(x)) test_exit(__LINE__ % 200 + 1); } while (0)
static struct usched_scheduler schedulers[2];
static struct execution_unit_tls units[2];
static thread_storage_t scheduler_tls[2];
static uint64_t slots[2][12];
static struct usched_job jobs[2];
static _Alignas(16) char stacks[2][65536];
static unsigned long indices[4];
static const unsigned char templates[4][4] = {{11,12,13,14}, {21,22,23,24},
    {31,32,33,34}, {41,42,43,44}};
static int phase;
static uintptr_t original;
static void register_module(int n) {
    CHECK(!__tls_register_module((void*)templates[n], templates[n], 4, 60, 64, &indices[n]));
    CHECK(indices[n] == (unsigned)n);
}
static void check_job(struct usched_job* job, int count) {
    CHECK(__tls_current() == &job->tls);
    CHECK(job->tls.job_id == job->id);
    CHECK(job->tls.async_context == &job->async_context);
    CHECK(__get_reserved(1) == (uintptr_t)job->tls.tls_array);
    CHECK(__get_reserved(11) == (uintptr_t)job->tls.tls_array);
    CHECK(job->tls.tls_modules_prepared_count == (unsigned)count);
    for (int i = 0; i < count; ++i) {
        unsigned char* p = (void*)job->tls.tls_array[i];
        CHECK(p && !((uintptr_t)p & 63));
        // Module 0 is deliberately mutated by job 0 after its initial entry.
        if (i || job != &jobs[0] || !original) CHECK(p[0] == templates[i][0]);
        for (int j = 1; j < 4; ++j) CHECK(p[j] == templates[i][j]);
        for (int j = 4; j < 64; ++j) CHECK(p[j] == 0);
    }
    if (job == &jobs[0] && original) {
        CHECK(job->tls.tls_array[0] == original);
        CHECK(*(unsigned char*)original == 99);
    }
}
void __usched_task_main(struct usched_job* job) {
    job->state = JobState_RUNNING;
    if (job == &jobs[1]) {
        CHECK(phase == 1);
        check_job(job, 2); // Newly created job catches up to both modules.
        CHECK(job->tls.tls_array[0] != original);
        register_module(2);
        CHECK(jobs[0].tls.tls_modules_prepared_count == 2); // still dormant
        phase = 2;
        jobs[0].state = JobState_RUNNING;
        __usched_add_job_ready(&jobs[0]);
        job->state = JobState_BLOCKED;
        usched_yield(NULL);
        test_exit(201);
    }
    check_job(job, 1);
    original = job->tls.tls_array[0];
    *(unsigned char*)original = 99;
    register_module(1);
#ifdef FAIL_CATCHUP
    fail_allocation = 0;
#endif
    usched_yield(NULL); // No other ready job: same-job dispatch must catch up.
#ifdef FAIL_CATCHUP
    test_exit(202); // Must have trapped before returning to user code.
#endif
    check_job(job, 2);
    int allocations = live_allocations;
    usched_yield(NULL);
    CHECK(live_allocations == allocations); // No allocation without new modules.
    phase = 1;
    __usched_add_job_ready(&jobs[1]);
    job->state = JobState_BLOCKED;
    usched_yield(NULL);
    CHECK(phase == 2);
    check_job(job, 3); // Existing suspended job catches up on another-job resume.
    phase = 3;
    job->state = JobState_BLOCKED;
    usched_yield(NULL); // Return to idle scheduler; entry() publishes another module.
    CHECK(phase == 4);
    check_job(job, 4);

    CHECK(__usched_get_scheduler() == &schedulers[1]);
    CHECK(__get_reserved(2) == (uintptr_t)&units[1]);
    check_job(job, 4);
    __tls_release_modules();
    __tls_switch(&jobs[1].tls);
    __tls_release_modules();
    CHECK(live_allocations == 0);
    test_exit(0);
}
void entry(void) {
    for (int i = 0; i < 2; ++i) {
        units[i].scheduler = &schedulers[i];
        schedulers[i].tls = &scheduler_tls[i];
        slots[i][2] = (uintptr_t)&units[i];
        jobs[i].id = i + 10;
        jobs[i].stack = stacks[i];
        jobs[i].stack_size = sizeof(stacks[i]);
    }
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(slots[0]) : "memory");
    __tls_switch(&scheduler_tls[0]);
    register_module(0);
    g_globalQueue.ready = &jobs[0];
    struct timespec deadline;
    CHECK(usched_yield(&deadline) == -1);
    CHECK(phase == 3 && !schedulers[0].current);
    register_module(3);
    phase = 4;
    // Resume the suspended job on another execution unit at a cooperative
    // scheduling boundary. Signal-driven migration is deliberately not tested.
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(slots[1]) : "memory");
    __tls_switch(&scheduler_tls[1]);
    jobs[0].state = JobState_RUNNING;
    g_globalQueue.ready = &jobs[0];
    usched_yield(&deadline);
    test_exit(203);
}
// Only single-execution-unit queue operations are simulated here. No timers,
// pending syscalls or garbage jobs are installed; their adapters must trap.
oserr_t MutexLock(Mutex_t* m) { (void)m; return OS_EOK; }
oserr_t MutexUnlock(Mutex_t* m) { (void)m; return OS_EOK; }
int timespec_get(struct timespec* t, int base) { t->tv_sec=0; t->tv_nsec=0; return base; }
void timespec_diff(const struct timespec* a, const struct timespec* b, struct timespec* d) {
    (void)a; (void)b; (void)d; __builtin_trap();
}

struct execution_unit_tls* __usched_xunit_tls_current(void) { return (void*)__get_reserved(2); }
oserr_t OSNotificationQueuePost(OSHandle_t* h, unsigned int flags) { (void)h; (void)flags; return OS_EOK; }
