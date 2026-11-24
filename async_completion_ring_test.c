/**
 * Verify the userspace side of asynchronous queues: librt/libos/async_queue.c
 * (queue setup and delivery of ring results) and the retry loop in
 * librt/libos/notification_queue.c.
 *
 * System calls go through the real syscall wrappers into _syscall below. SHM
 * and handle helpers are replaced, and the ring is a static buffer.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef int errno_t;
#include <internal/_syscalls.h>
#include <internal/_tls.h>
#include <os/async_queue.h>
#include <os/handle.h>
#include <os/notification_queue.h>
#include <os/shm.h>
#include <os/syslog.h>
#include <os/types/syscall.h>

#define TEST_QUEUE_ID  0x61
#define TEST_BUFFER_ID 0x62
#define TEST_SET_ID    0x63

static _Alignas(64) uint8_t     g_ringMemory[4096];
static OSAsyncCompletionRing_t* const g_ring = (OSAsyncCompletionRing_t*)g_ringMemory;
static struct thread_storage    g_tls;
static OSAsyncContext_t         g_async;

static oserr_t g_createStatus;
static int     g_buffersCreated;
static int     g_buffersDestroyed;
static int     g_queuesDestroyed;
static int     g_queueWaits;

// Scripted ListenHandleSet results. Each call returns the next status and
// writes the next count, unless the count is negative.
static oserr_t g_listenStatus[4];
static int     g_listenCount[4];
static int     g_listenCalls;

// Scripted completion of an OS_EFORKED call. A forked kernel thread stores
// events before it reports, which g_awaitCount models.
static oserr_t g_awaitStatus;
static int     g_awaitCount;
static int     g_awaits;
static int*    g_eventsOut;

// Replacements for libc and libos parts that are not under test.

struct thread_storage* __tls_current(void) { return &g_tls; }
uuid_t usched_notification_handle(void) { return TEST_QUEUE_ID; }

// The deadline the scheduler is given for the current await, or NULL.
static const OSTimestamp_t* g_expectedAwaitDeadline;
static int                  g_cancels;

void
usched_wait_async_until(
    _In_ const struct timespec* deadline)
{
    assert(g_tls.async_context == &g_async && g_async.ErrorCode == OS_ESCSTARTED);
    if (g_expectedAwaitDeadline == NULL) {
        assert(deadline == NULL);
    } else {
        assert(deadline != NULL && deadline->tv_sec == (time_t)g_expectedAwaitDeadline->Seconds);
        assert(deadline->tv_nsec == (long)g_expectedAwaitDeadline->Nanoseconds);
    }
    g_awaits++;
    if (g_awaitCount >= 0) {
        *g_eventsOut = g_awaitCount;
    }
    __atomic_store_n(&g_async.ErrorCode, g_awaitStatus, __ATOMIC_RELEASE);
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}

oserr_t
SHMCreate(
    _In_  SHM_t*      shm,
    _Out_ OSHandle_t* handleOut)
{
    assert(shm->Flags == SHM_ASYNC && shm->Size == sizeof(g_ringMemory));
    assert(shm->Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE) && shm->Key == NULL);
    g_buffersCreated++;
    *handleOut = (OSHandle_t) { .ID = TEST_BUFFER_ID, .Type = OSHANDLE_SHM, .Payload = g_ringMemory };
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    assert(handle->ID == TEST_BUFFER_ID);
    return handle->Payload;
}

oserr_t
OSHandleWrap(
    _In_ uuid_t            id,
    _In_ enum OSHandleType type,
    _In_ void*             payload,
    _In_ bool              ownership,
    _In_ struct OSHandle*  handle)
{
    assert(id == TEST_QUEUE_ID && type == OSHANDLE_NULL && payload == NULL && ownership);
    *handle = (OSHandle_t) { .ID = id, .Type = type };
    return OS_EOK;
}

void
OSHandleDestroy(
    _In_ struct OSHandle* handle)
{
    if (handle->ID == TEST_BUFFER_ID) {
        g_buffersDestroyed++;
    } else {
        assert(handle->ID == TEST_QUEUE_ID);
        g_queuesDestroyed++;
    }
    handle->ID = UUID_INVALID;
}

SCTYPE
_syscall(SCTYPE function, SCTYPE a, SCTYPE b, SCTYPE c, SCTYPE d, SCTYPE e)
{
    HandleSetWaitParameters_t* parameters;
    OSTimestamp_t*             deadline;
    (void)e;

    switch (function) {
        case 105:
            assert(a == TEST_BUFFER_ID && c == 0 && d == 0);
            if (g_createStatus == OS_EOK) {
                *(uuid_t*)(uintptr_t)b = TEST_QUEUE_ID;
            }
            return g_createStatus;
        case 106:
            deadline = (OSTimestamp_t*)(uintptr_t)c;
            assert(a == TEST_QUEUE_ID && (int)b == 41 && d == 0);
            assert(deadline == NULL || (deadline->Seconds == 3 && deadline->Nanoseconds == 4));
            g_queueWaits++;
            return OS_ETIMEOUT;
        case 107:
            assert(a == TEST_QUEUE_ID && b == (SCTYPE)(uintptr_t)&g_async && c == 0 && d == 0);
            g_cancels++;
            return OS_ENOENT;
        case 74:
            parameters = (HandleSetWaitParameters_t*)(uintptr_t)c;
            assert(a == TEST_SET_ID && parameters->MaxEvents == 4);
            assert(parameters->Deadline == g_expectedAwaitDeadline);
            assert(g_listenCalls < 4);
            assert((void*)(uintptr_t)b == g_tls.async_context);
            if (b != 0) {
                // Every call starts with a fresh context.
                assert(g_async.ErrorCode == OS_ESCSTARTED && g_async.NotificationHandle == TEST_QUEUE_ID);
            }

            // The wrapper clears the count, so a result that writes none reads 0.
            assert(*(int*)(uintptr_t)d == 0);
            g_eventsOut = (int*)(uintptr_t)d;
            if (g_listenCount[g_listenCalls] >= 0) {
                *g_eventsOut = g_listenCount[g_listenCalls];
            }
            return g_listenStatus[g_listenCalls++];
        default:
            assert(0);
            return OS_ENOTSUPPORTED;
    }
}

// Tests.

static void
__TestCreate(void)
{
    OSAsyncQueue_t queue;

    assert(OSAsyncQueueCreate(NULL) == OS_EINVALPARAMS);

    // A failed queue creation removes the ring buffer again.
    g_createStatus = OS_EOOM;
    assert(OSAsyncQueueCreate(&queue) == OS_EOOM);
    assert(g_buffersCreated == 1 && g_buffersDestroyed == 1);

    g_createStatus = OS_EOK;
    assert(OSAsyncQueueCreate(&queue) == OS_EOK);
    assert(queue.Handle.ID == TEST_QUEUE_ID && queue.Buffer.ID == TEST_BUFFER_ID);
    assert(queue.Ring == g_ring);

    // Waits forward the handle, the expected Sequence and the deadline.
    assert(OSAsyncQueueWait(&queue, 41, NULL) == OS_ETIMEOUT);
    assert(OSAsyncQueueWait(&queue, 41, &(OSTimestamp_t) { .Seconds = 3, .Nanoseconds = 4 }) == OS_ETIMEOUT);
    assert(OSAsyncQueueWait(NULL, 41, NULL) == OS_EINVALPARAMS);
    assert(g_queueWaits == 2);

    atomic_store(&g_ring->Sequence, 9);
    assert(OSAsyncQueueSequence(&queue) == 9);

    // A timed out wait is ended through the queue handle with its context.
    assert(OSAsyncQueueCancel(&queue, &g_async) == OS_ENOENT && g_cancels == 1);
    assert(OSAsyncQueueCancel(NULL, &g_async) == OS_EINVALPARAMS);
    assert(OSAsyncQueueCancel(&queue, NULL) == OS_EINVALPARAMS && g_cancels == 1);

    OSAsyncQueueDestroy(&queue);
    assert(g_queuesDestroyed == 1 && g_buffersDestroyed == 2 && queue.Ring == NULL);
}

static void
__WriteEntries(
    _In_ OSAsyncContext_t* contexts,
    _In_ uint32_t          first,
    _In_ uint32_t          start,
    _In_ uint32_t          count)
{
    for (uint32_t i = first; i < first + count; i++) {
        contexts[i] = (OSAsyncContext_t) { .NotificationHandle = TEST_QUEUE_ID, .ErrorCode = OS_ESCSTARTED };
        g_ring->Entries[(start + i) % g_ring->Capacity] = (OSAsyncCompletion_t) {
            .Cookie = i == 2 ? 0 : (uint64_t)(uintptr_t)&contexts[i],
            .Status = i == 5 ? OS_ETIMEOUT : OS_EOK
        };
    }
}

static void
__TestDeliver(void)
{
    OSAsyncContext_t contexts[6];
    OSAsyncQueue_t   queue = { .Ring = g_ring };
    uint32_t         start = UINT32_MAX - 1;

    memset(g_ringMemory, 0, sizeof(g_ringMemory));

    // A ring the kernel has not set up yet delivers nothing.
    assert(OSAsyncQueueDeliver(&queue) == 0);

    // Six results in a ring of four, starting two entries before the 32 bit
    // indexes wrap. The third result has no context and is skipped.
    g_ring->Capacity = 4;
    atomic_store(&g_ring->ReadIndex, start);
    __WriteEntries(contexts, 0, start, 3);
    atomic_store(&g_ring->WriteIndex, start + 3);
    assert(OSAsyncQueueDeliver(&queue) == 2);
    assert(atomic_load(&g_ring->ReadIndex) == start + 3);

    // The next three reuse entries that were just read.
    __WriteEntries(contexts, 3, start, 3);
    atomic_store(&g_ring->WriteIndex, start + 6);
    assert(OSAsyncQueueDeliver(&queue) == 3);
    assert(atomic_load(&g_ring->ReadIndex) == start + 6);
    for (int i = 0; i < 6; i++) {
        oserr_t expected = i == 2 ? OS_ESCSTARTED : i == 5 ? OS_ETIMEOUT : OS_EOK;
        assert(contexts[i].ErrorCode == expected);
    }

    // Nothing is delivered twice.
    contexts[5].ErrorCode = OS_ESCSTARTED;
    assert(OSAsyncQueueDeliver(&queue) == 0 && contexts[5].ErrorCode == OS_ESCSTARTED);
}

static void
__ExpectListen(
    _In_ OSAsyncContext_t* async,
    _In_ oserr_t           expected,
    _In_ int               expectedCount,
    _In_ int               expectedCalls,
    _In_ int               expectedAwaits)
{
    struct ioset_event events[4];
    OSHandle_t         set = { .ID = TEST_SET_ID };
    OSTimestamp_t*     deadline = (OSTimestamp_t*)g_expectedAwaitDeadline;
    int                count = -1;

    g_listenCalls = 0;
    g_awaits = 0;
    g_tls.async_context = async;
    if (async != NULL) {
        OSAsyncContextInitialize(async);
    }
    assert(OSNotificationQueueWait(&set, &events[0], 4, 0, deadline, &count, async) == expected);
    assert(count == expectedCount);
    assert(g_listenCalls == expectedCalls && g_awaits == expectedAwaits);
}

static void
__TestListenRetry(void)
{
    // A wait record reports OS_EOK without events. The wrapper calls again,
    // with a fresh context, and that call returns the events.
    g_listenStatus[0] = OS_EFORKED; g_listenCount[0] = -1;
    g_listenStatus[1] = OS_EOK;     g_listenCount[1] = 2;
    g_awaitStatus = OS_EOK; g_awaitCount = -1;
    __ExpectListen(&g_async, OS_EOK, 2, 2, 1);

    // Another waiter may take the events first, so the record can repeat.
    g_listenStatus[0] = OS_EFORKED; g_listenCount[0] = -1;
    g_listenStatus[1] = OS_EFORKED; g_listenCount[1] = -1;
    g_listenStatus[2] = OS_EOK;     g_listenCount[2] = 1;
    __ExpectListen(&g_async, OS_EOK, 1, 3, 2);

    // A forked kernel thread already stored the events. No second call.
    g_listenStatus[0] = OS_EFORKED; g_listenCount[0] = -1;
    g_awaitStatus = OS_EOK; g_awaitCount = 3;
    __ExpectListen(&g_async, OS_EOK, 3, 1, 1);

    // A failed or timed out wait is reported as it is.
    g_awaitStatus = OS_ETIMEOUT; g_awaitCount = -1;
    __ExpectListen(&g_async, OS_ETIMEOUT, 0, 1, 1);

    // A deadline goes to every call and to the scheduler, which ends a wait
    // record at that time. Its OS_ETIMEOUT is returned as it is.
    static const OSTimestamp_t deadline = { .Seconds = 7, .Nanoseconds = 8 };
    g_expectedAwaitDeadline = &deadline;
    g_listenStatus[0] = OS_EFORKED; g_listenCount[0] = -1;
    g_listenStatus[1] = OS_EOK;     g_listenCount[1] = 1;
    g_awaitStatus = OS_EOK; g_awaitCount = -1;
    __ExpectListen(&g_async, OS_EOK, 1, 2, 1);
    g_listenStatus[0] = OS_EFORKED; g_listenCount[0] = -1;
    g_awaitStatus = OS_ETIMEOUT;
    __ExpectListen(&g_async, OS_ETIMEOUT, 0, 1, 1);
    g_expectedAwaitDeadline = NULL;

    // A wait that did not continue asynchronously returns at once.
    g_listenStatus[0] = OS_EOK; g_listenCount[0] = 4;
    __ExpectListen(NULL, OS_EOK, 4, 1, 0);
    g_listenStatus[0] = OS_EINTERRUPTED; g_listenCount[0] = -1;
    __ExpectListen(&g_async, OS_EINTERRUPTED, 0, 1, 0);
}

int
main(void)
{
    __TestCreate();
    __TestDeliver();
    __TestListenRetry();
    puts("Asynchronous completion ring: queue setup, waits, cancels, delivery across wraps and handle set retries with deadlines passed");
    return 0;
}
