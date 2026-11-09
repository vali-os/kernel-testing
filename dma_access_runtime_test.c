// Real userspace SHM sessions, async wait wrapper and syscall marshalling.
#include <assert.h>
#include <stdio.h>
#include <time.h>
typedef int errno_t;
#include <internal/_syscalls.h>
#include <internal/_tls.h>
#include <os/shm.h>
#include <os/notification_queue.h>
#include <os/types/syscall.h>

static struct thread_storage g_tls;
static int g_calls;
static int g_waits;
static bool g_active;
static bool g_fail;
static bool g_fork;
static bool g_expectedWrite;
static enum SHMWaitMode g_expectedMode;
static oserr_t g_beginStatus;
static const OSTimestamp_t* g_expectedDeadline;

struct thread_storage* __tls_current(void) { return &g_tls; }
uuid_t usched_notification_handle(void) { return 42; }
void usched_wait_async(void)
{
    assert(g_tls.async_context != NULL);
    g_waits++;
    g_tls.async_context->ErrorCode = OS_ETIMEOUT;
}

SCTYPE
_syscall(SCTYPE function, SCTYPE a, SCTYPE b, SCTYPE c, SCTYPE d, SCTYPE e)
{
    struct SHMAccessBeginParameters* parameters;
    OSAsyncContext_t* async;
    (void)e;
    g_calls++;
    if (g_fail) return OS_EUNKNOWN;
    switch (function) {
        case 85:
            assert(a == 99 && b == 16 && c == 64);
            *(uuid_t*)(uintptr_t)d = 7;
            return OS_EOK;
        case 83:
            assert(a == 7 && b == SHMAccessCpu && c == 0 && d == 0 && g_active);
            g_active = false;
            return OS_EOK;
        case 84:
            assert(a == 7);
            return g_active ? OS_EBUSY : OS_EOK;
        case 69:
            assert(a == 55 && c != 0 && d != 0);
            async = (void*)(uintptr_t)b;
            *(int*)(uintptr_t)d = 0;
            if (g_fork) {
                assert(async != NULL && async == g_tls.async_context);
                return OS_EFORKED;
            }
            assert(async == NULL);
            return OS_EOK;
        case 82:
            async = (void*)(uintptr_t)a;
            parameters = (void*)(uintptr_t)c;
            assert(b == 7 && parameters->Kind == SHMAccessCpu && parameters->Write == g_expectedWrite);
            assert(parameters->Offset == 0 && parameters->Length == 0);
            assert(parameters->Mode == g_expectedMode);
            if (g_expectedDeadline != NULL) {
                assert(parameters->Deadline.Seconds == g_expectedDeadline->Seconds);
                assert(parameters->Deadline.Nanoseconds == g_expectedDeadline->Nanoseconds);
            }
            if (g_fork) {
                assert(async == g_tls.async_context);
                assert(async->NotificationHandle == 42 && async->ErrorCode == OS_ESCSTARTED);
                return OS_EFORKED;
            }
            assert(async == NULL);
            if (g_beginStatus != OS_EOK) {
                return g_beginStatus;
            }
            g_active = true;
            return OS_EOK;
        default:
            assert(0);
            return OS_EUNKNOWN;
    }
}

static void
__TestNotificationWait(void)
{
    OSHandle_t queue = { .ID = 55, .Type = OSHANDLE_HQUEUE };
    struct ioset_event event = { 0 };
    OSAsyncContext_t async = { 0 };
    int events = -1;
    int waits = g_waits;

    g_tls.async_context = NULL;
    g_fork = false;
    assert(OSNotificationQueueWait(&queue, &event, 1, 0, NULL, &events, NULL) == OS_EOK);
    assert(events == 0 && g_waits == waits);
    g_fail = true;
    assert(OSNotificationQueueWait(&queue, &event, 1, 0, NULL, &events, NULL) == OS_EUNKNOWN);
    assert(g_waits == waits);
    g_fail = false;
    g_fork = true;
    g_tls.async_context = &async;
    assert(OSNotificationQueueWait(&queue, &event, 1, 0, NULL, &events, &async) == OS_ETIMEOUT);
    assert(g_waits == waits + 1);
    g_tls.async_context = NULL;
    g_fork = false;
}

int main(void)
{
    struct SHMAccessSession session = { 0 };
    OSHandle_t buffer = { .ID = 99, .Type = OSHANDLE_SHM };
    OSAsyncContext_t async = { 0 };
    OSTimestamp_t deadline = { .Seconds = 123 };
    OSTimestamp_t expired = { 0 };
    OSTimestamp_t infinite = *OS_DEADLINE_INFINITE;
    OSTimestamp_t distant = { .Seconds = INT64_MAX, .Nanoseconds = 1 };
    int calls;

    assert(SHMAccessOpen(NULL, 0, 64, &session) == OS_EINVALPARAMS);
    assert(SHMAccessClose(&session) == OS_EOK);
    g_fail = true;
    assert(SHMAccessOpen(&buffer, 16, 64, &session) == OS_EUNKNOWN && session.Handle == 0);
    g_fail = false;
    assert(SHMAccessOpen(&buffer, 16, 64, &session) == OS_EOK && session.Handle == 7);
    calls = g_calls;
    assert(SHMAccessOpen(&buffer, 16, 64, &session) == OS_EBUSY && g_calls == calls);
    g_expectedWrite = true;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, SHMWaitNonBlocking, NULL) == OS_EOK);
    assert(SHMAccessClose(&session) == OS_EBUSY && session.Handle == 7);
    assert(SHMAccessEndCpu(&session) == OS_EOK);
    // Invalid modes must fail before submitting a syscall.
    calls = g_calls;
    assert(SHMAccessBeginCpu(&session, (enum SHMCpuAccess)0, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(SHMAccessBeginCpu(&session, (enum SHMCpuAccess)99, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, (enum SHMWaitMode)99, NULL) == OS_EINVALPARAMS);
    assert(g_calls == calls && !g_active);

    // Nonblocking access ignores timestamps and preserves the busy result.
    g_beginStatus = OS_EBUSY;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, SHMWaitNonBlocking, &deadline) == OS_EBUSY);
    assert(!g_active && g_waits == 0);

    // A zero timestamp still uses waiting mode and reports a timeout on conflict.
    g_expectedMode = SHMWaitUntilAvailable;
    g_expectedDeadline = &expired;
    g_beginStatus = OS_ETIMEOUT;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, SHMWaitUntilAvailable, &expired) == OS_ETIMEOUT);
    assert(!g_active);
    g_beginStatus = OS_EOK;
    g_expectedWrite = false;
    g_expectedDeadline = &infinite;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessRead, SHMWaitUntilAvailable, NULL) == OS_EOK);
    assert(SHMAccessEndCpu(&session) == OS_EOK);
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessRead, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_EOK);
    assert(SHMAccessEndCpu(&session) == OS_EOK);
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessRead, SHMWaitUntilAvailable, &infinite) == OS_EOK);
    assert(SHMAccessEndCpu(&session) == OS_EOK);
    g_expectedDeadline = &distant;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessRead, SHMWaitUntilAvailable, &distant) == OS_EOK);
    assert(SHMAccessEndCpu(&session) == OS_EOK);
    g_expectedWrite = true;
    g_tls.async_context = &async;
    g_fork = true;
    g_expectedDeadline = &deadline;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, SHMWaitUntilAvailable, &deadline) == OS_ETIMEOUT);
    g_expectedDeadline = &infinite;
    assert(SHMAccessBeginCpu(&session, SHMCpuAccessWrite, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_ETIMEOUT);
    assert(g_waits == 2 && !g_active);
    g_fail = true;
    assert(SHMAccessClose(&session) == OS_EUNKNOWN && session.Handle == 7);
    g_fail = false;
    assert(SHMAccessClose(&session) == OS_EOK && session.Handle == 0);
    __TestNotificationWait();
    puts("SHM runtime: session lifetime, syscall arguments, native wait and repeated cooperative completion passed");
    return 0;
}

#include "../librt/libos/shm_access.c"
