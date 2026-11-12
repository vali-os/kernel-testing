// Drive pool-control interleavings without creating native threads.
#include "../librt/libos/uthreads/xunit.c"

extern _Noreturn void test_exit(int status);
#define CHECK(condition) do { if (!(condition)) test_exit(__LINE__ % 200 + 1); } while (0)

static struct usched_execution_unit g_first;
static struct usched_execution_unit g_second;
static struct thread_storage g_nativeTls;
static uint64_t g_slots[12];
static int g_locked;
static int g_yields;
static int g_stopRequests;

static void
__SelectUnit(
    _In_ struct usched_execution_unit* unit)
{
    g_slots[2] = (uintptr_t)&unit->tls;
}

void
entry(void)
{
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(g_slots) : "memory");
    __tls_switch(&g_nativeTls);
    __execution_unit_construct(&g_executionManager.primary);
    __execution_unit_construct(&g_first);
    __execution_unit_construct(&g_second);
    g_executionManager.count = 3;
    g_executionManager.primary.next = &g_first;
    g_first.next = &g_second;

    // Unit B removes A; before A reaches its scheduler, A removes B. Neither
    // request may join the other kernel thread, which would deadlock the pair.
    __SelectUnit(&g_second);
    CHECK(usched_xunit_set_count(2) == 0);
    CHECK(atomic_load(&g_first.context.stop_requested));
    CHECK(!atomic_load(&g_second.context.stop_requested));
    __SelectUnit(&g_first);
    CHECK(usched_xunit_set_count(1) == 0);
    CHECK(atomic_load(&g_second.context.stop_requested));
    CHECK(g_executionManager.primary.next == NULL);
    CHECK(usched_xunit_count() == 1 && g_stopRequests == 2);
    CHECK(g_yields == 0 && g_locked == 0);

    // Self-retirement returns to the scheduler only after releasing the manager.
    memset(&g_first, 0, sizeof(g_first));
    __execution_unit_construct(&g_first);
    g_executionManager.primary.next = &g_first;
    g_executionManager.count = 2;
    __SelectUnit(&g_first);
    CHECK(usched_xunit_set_count(1) == 0);
    CHECK(g_stopRequests == 3 && g_yields == 1 && g_locked == 0);
    CHECK(g_executionManager.primary.next == NULL);
    test_exit(0);
}

oserr_t
MutexLock(
    _In_ Mutex_t* mutex)
{
    CHECK(mutex == &g_executionManager.lock && !g_locked);
    g_locked = 1;
    return OS_EOK;
}

oserr_t
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    CHECK(mutex == &g_executionManager.lock && g_locked);
    g_locked = 0;
    return OS_EOK;
}

void
__usched_request_stop(
    _InOut_ struct usched_execution_context* execution)
{
    CHECK(g_locked);
    g_stopRequests++;
    atomic_store(&execution->stop_requested, true);
}

void
usched_job_yield(void)
{
    CHECK(!g_locked);
    CHECK(atomic_load(&__usched_xunit_tls_current()->context->stop_requested));
    g_yields++;
}
