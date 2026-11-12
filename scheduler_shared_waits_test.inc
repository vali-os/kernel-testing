// Shared wait tests use the handoff fixture's real jobs, stacks and synchronization.
static uuid_t g_completionHandle;
static int g_waitCompletions;
static struct usched_cnd g_otherCondition;

static void
__ExpireBeforeParking(void)
{
    struct usched_job* job;

    job = __usched_get_execution()->current;
    CHECK(__JOB_STATE(job) == JobState_BLOCKING);
    // Model a timer scan on another unit while the registering job is still
    // running. Only the native-stack adapter check is suppressed for this scan.
    g_remoteTimerScan = true;
    __update_timers(job->scheduler);
    g_remoteTimerScan = false;
    CHECK(__JOB_STATE(job) == JobState_RUNNING);
    CHECK(g_scheduler.ready == NULL);
}

static void
__EarlyTimeout(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec now = { 0, 0 };

    (void)argument;
    (void)token;
    usched_mtx_lock(&g_mutex);
    g_afterSchedulerUnlock = __ExpireBeforeParking;
    CHECK(usched_cnd_timedwait(&g_condition, &g_mutex, &now) == -1);
    CHECK(errno == ETIME && g_condition.queue == NULL && g_scheduler.Timers == NULL);
    usched_mtx_unlock(&g_mutex);
    CHECK(usched_job_sleep(NULL) == -1 && errno == EINVAL);
    g_waitCompletions++;
}

static void
__CompleteBeforeParking(void)
{
    struct usched_job* job;

    job = __usched_get_execution()->current;
    CHECK(__JOB_STATE(job) == JobState_BLOCKING);
    __atomic_store_n(&job->async_context.ErrorCode, OS_EOK, __ATOMIC_RELEASE);
    __process_syscall_completions(job->scheduler);
    CHECK(__JOB_STATE(job) == JobState_RUNNING);
    CHECK(g_scheduler.syscalls_pending == NULL && g_scheduler.ready == NULL);
}

static void
__MigratingWaits(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 10, 0 };
    struct usched_job* job;

    (void)argument;
    job = token;
    g_completionHandle = usched_notification_handle();
    CHECK(__usched_get_execution() == &g_executions[0]);
    usched_mtx_lock(&g_mutex);
    g_phase = 1;
    CHECK(usched_cnd_timedwait(&g_condition, &g_mutex, &until) == -1);
    CHECK(errno == ETIME && __usched_get_execution() == &g_executions[1]);
    CHECK(g_condition.queue == NULL && g_scheduler.Timers == NULL);
    CHECK(usched_notification_handle() == g_completionHandle);

    // Reuse the same wait frame after a timeout and migration. A successful
    // signal must remove the new timer before its deadline passes.
    until.tv_sec = 20;
    g_phase = 2;
    CHECK(usched_cnd_timedwait(&g_condition, &g_mutex, &until) == 0);
    CHECK(g_condition.queue == NULL && g_scheduler.Timers == NULL);
    usched_mtx_unlock(&g_mutex);

    g_phase = 3;
    OSAsyncContextInitialize(&job->async_context);
    CHECK(job->async_context.NotificationHandle == g_completionHandle);
    usched_wait_async();
    CHECK(__usched_get_execution() == &g_executions[2]);
    CHECK(g_scheduler.syscalls_pending == NULL);
    CHECK(job->async_context.ErrorCode == OS_EOK);
    CHECK(usched_notification_handle() == g_completionHandle);

    // Completion before registration must work even if its event was consumed.
    OSAsyncContextInitialize(&job->async_context);
    __atomic_store_n(&job->async_context.ErrorCode, OS_EOK, __ATOMIC_RELEASE);
    usched_wait_async();
    CHECK(g_scheduler.syscalls_pending == NULL);
    OSAsyncContextInitialize(&job->async_context);
    g_afterSchedulerUnlock = __CompleteBeforeParking;
    usched_wait_async();
    CHECK(g_scheduler.syscalls_pending == NULL);
    g_phase = 4;
    g_waitCompletions++;
}

static void
__TimedMutexWaiter(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 30, 0 };

    (void)argument;
    CHECK(usched_mtx_timedlock(&g_mutex, &until) == -1);
    CHECK(errno == ETIME && g_mutex.owner != token);
    g_waitCompletions++;
}

static void
__LiveMutexWaiter(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 40, 0 };

    (void)argument;
    CHECK(usched_mtx_timedlock(&g_mutex, &until) == 0);
    CHECK(g_mutex.owner == token);
    usched_mtx_unlock(&g_mutex);
    g_waitCompletions++;
}

static void
__MutexOwner(
    _In_ void* argument,
    _In_ void* token)
{
    (void)argument;
    (void)token;
    usched_mtx_lock(&g_mutex);
    __Queue(__TimedMutexWaiter);
    __Queue(__LiveMutexWaiter);
    usched_job_yield();
    CHECK(g_mutex.queue != NULL && g_mutex.queue->queue_next != NULL);

    // Expire the first waiter, leaving it on the mutex queue. Unlock must skip
    // that completed wait and transfer the mutex to the second waiter.
    g_now.tv_sec = 30;
    g_remoteTimerScan = true;
    __update_timers(&g_scheduler);
    g_remoteTimerScan = false;
    CHECK(g_mutex.queue->Result == UschedWaitStatus_TIMEOUT);
    usched_mtx_unlock(&g_mutex);
    CHECK(g_mutex.owner != NULL && g_mutex.queue == NULL);
    CHECK(g_scheduler.Timers == NULL);
    g_now.tv_sec = 40;
}

static void
__TimedConditionWaiter(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 50, 0 };

    (void)argument;
    (void)token;
    usched_mtx_lock(&g_mutex);
    CHECK(usched_cnd_timedwait(&g_condition, &g_mutex, &until) == -1);
    CHECK(errno == ETIME);
    usched_mtx_unlock(&g_mutex);
    g_waitCompletions++;
}

static void
__LiveConditionWaiter(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 60, 0 };

    (void)argument;
    (void)token;
    usched_mtx_lock(&g_mutex);
    CHECK(usched_cnd_timedwait(&g_condition, &g_mutex, &until) == 0);
    usched_mtx_unlock(&g_mutex);
    g_waitCompletions++;
}

static void
__OtherConditionWaiter(
    _In_ void* argument,
    _In_ void* token)
{
    struct timespec until = { 55, 0 };

    (void)argument;
    (void)token;
    usched_mtx_lock(&g_mutex);
    CHECK(usched_cnd_timedwait(&g_otherCondition, &g_mutex, &until) == 0);
    usched_mtx_unlock(&g_mutex);
    g_waitCompletions++;
}

static void
__PendingSyscall(
    _In_ void* argument,
    _In_ void* token)
{
    struct usched_job* job;

    (void)argument;
    job = token;
    OSAsyncContextInitialize(&job->async_context);
    usched_wait_async();
    CHECK(job->async_context.ErrorCode == OS_EOK);
    g_waitCompletions++;
}

static void
__SharedWaitsMain(void)
{
    struct timespec deadline;
    int i;
    int destroyed;

    for (i = 0; i < 3; i++) {
        g_units[i].context = &g_executions[i];
        g_slots[i][2] = (uintptr_t)&g_units[i];
    }
    __SelectUnit(0);
    __usched_startup();
    for (i = 0; i < 3; i++) {
        __SelectUnit(i);
        __usched_init(&g_executions[i], &(struct usched_init_params) { NULL });
        CHECK(usched_notification_handle() == g_scheduler.notification_handle.ID);
    }
    usched_mtx_init(&g_mutex, USCHED_MUTEX_PLAIN);
    usched_cnd_init(&g_condition);
    usched_cnd_init(&g_otherCondition);

    __SelectUnit(0);
    __Queue(__EarlyTimeout);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_waitCompletions == 1);
    __Queue(__MigratingWaits);
    CHECK(usched_yield(&deadline) == -1 && errno == EWOULDBLOCK);
    CHECK(g_phase == 1 && deadline.tv_sec == 10);
    CHECK(g_scheduler.Timers != NULL && g_condition.queue != NULL);
    __usched_request_stop(&g_executions[0]);
    CHECK(usched_yield(&deadline) == -1 && errno == ESHUTDOWN);
    destroyed = g_handlesDestroyed;
    __usched_destroy(&g_executions[0]);
    CHECK(g_handlesDestroyed == destroyed + 1);
    memset(&g_executions[0], 0xa5, sizeof(g_executions[0]));

    __SelectUnit(1);
    CHECK(usched_yield(&deadline) == -1 && errno == EWOULDBLOCK);
    CHECK(deadline.tv_sec == 10);
    g_now.tv_sec = 10;
    __update_timers(&g_scheduler);
    __update_timers(&g_scheduler);
    usched_cnd_notify_one(&g_condition);
    CHECK(g_scheduler.ready != NULL && g_scheduler.ready->next == NULL);
    CHECK(usched_yield(&deadline) == -1 && errno == EWOULDBLOCK);
    CHECK(g_phase == 2 && deadline.tv_sec == 20);
    usched_cnd_notify_one(&g_condition);
    g_now.tv_sec = 20;
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_phase == 3 && g_scheduler.syscalls_pending != NULL);
    __usched_request_stop(&g_executions[1]);
    CHECK(usched_yield(&deadline) == -1 && errno == ESHUTDOWN);
    __usched_destroy(&g_executions[1]);
    memset(&g_executions[1], 0xa5, sizeof(g_executions[1]));

    __SelectUnit(2);
    __atomic_store_n(&g_scheduler.syscalls_pending->async_context->ErrorCode, OS_EOK, __ATOMIC_RELEASE);
    OSNotificationQueuePost(&g_scheduler.notification_handle, IOSETSYN);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_phase == 4 && g_waitCompletions == 2 && g_freedJobs == 2);

    __Queue(__MutexOwner);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_mutex.owner == NULL && g_mutex.queue == NULL);
    CHECK(g_waitCompletions == 4);

    // An unrelated timer must not be removed when another condition is signalled.
    __Queue(__TimedConditionWaiter);
    __Queue(__OtherConditionWaiter);
    __Queue(__LiveConditionWaiter);
    CHECK(usched_yield(&deadline) == -1 && errno == EWOULDBLOCK);
    CHECK(deadline.tv_sec == 50);
    g_now.tv_sec = 50;
    __update_timers(&g_scheduler);
    usched_cnd_notify_one(&g_condition);
    CHECK(g_condition.queue == NULL);
    CHECK(__get_next_deadline(&g_scheduler, &deadline) == -1);
    CHECK(deadline.tv_sec == 55);
    usched_cnd_notify_all(&g_otherCondition);
    g_now.tv_sec = 60;
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_waitCompletions == 7 && g_freedJobs == g_jobCount);

    // Remove a completed node from the middle, then tail, then head. Every
    // resumed job exits and frees its stack before the following scan.
    for (i = 0; i < 3; i++) {
        __Queue(__PendingSyscall);
    }
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_scheduler.syscalls_pending->next->next != NULL);
    __atomic_store_n(&g_scheduler.syscalls_pending->next->async_context->ErrorCode,
                    OS_EOK, __ATOMIC_RELEASE);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_waitCompletions == 8 && g_scheduler.syscalls_pending->next->next == NULL);
    __atomic_store_n(&g_scheduler.syscalls_pending->next->async_context->ErrorCode,
                    OS_EOK, __ATOMIC_RELEASE);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_waitCompletions == 9 && g_scheduler.syscalls_pending->next == NULL);
    __atomic_store_n(&g_scheduler.syscalls_pending->async_context->ErrorCode,
                    OS_EOK, __ATOMIC_RELEASE);
    CHECK(usched_yield(&deadline) == -1 && errno == ENOENT);
    CHECK(g_waitCompletions == 10 && g_freedJobs == g_jobCount);
    CHECK(g_scheduler.Timers == NULL && g_scheduler.syscalls_pending == NULL);
    CHECK(g_lockDepth == 0);
    __usched_destroy(&g_executions[2]);
    test_exit(0);
}
