/**
 * Verify asynchronous wait records with the real kernel/sync/async_queue.c and
 * kernel/sync/futex.c.
 *
 * Handles, SHM lookups and the scheduler are replaced. The completion ring is
 * a static buffer. A blocked thread is modeled by a list element with key 0,
 * which the SchedulerBlock replacement links into the futex queue, the same way
 * the real scheduler links a scheduler object. SchedulerQueueObject records
 * which thread a wake reached. A thread timing out is modeled by unlinking its
 * element from inside the SpinlockAcquireIrq replacement, just before the
 * futex code takes the list's own lock (see __ArmTimeout).
 */
#include <async_queue.h>
#include <futex.h>
#include <handles/types.h>
#include <memoryspace.h>
#include <scheduler.h>
#include <threading.h>
#include <component/cpu.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_QUEUE_ID   0x51
#define TEST_BUFFER_ID  0x52
#define TEST_OTHER_ID   0x53
#define TEST_MAX_THREADS 8

static _Alignas(64) uint8_t g_ringMemory[8192];
static size_t               g_ringLength;
static OSAsyncCompletionRing_t* const g_ring = (OSAsyncCompletionRing_t*)g_ringMemory;

static HandleDestructorFn g_queueDestroy;
static void*              g_queueResource;
static void*              g_deferredFree;
static int                g_queueReferences;
static int                g_bufferReferences;
static int                g_failCreateHandle;

static element_t g_threads[TEST_MAX_THREADS];
static int       g_blockedThreads;
static int       g_wokenThreads[TEST_MAX_THREADS];
static int       g_wokenCount;

static MemorySpace_t g_space;

// A thread timeout armed by __ArmTimeout. When the countdown reaches zero on
// an acquire of g_timeoutList's lock, g_timeoutThread is unlinked first.
static list_t*    g_lastBlockQueue;
static list_t*    g_timeoutList;
static element_t* g_timeoutThread;
static int        g_timeoutCountdown;

// Kernel replacements.

void*
kmalloc(
    _In_ size_t size)
{
    return malloc(size);
}

// The queue object is kept until the test ends, so a record that the queue
// destructor failed to remove shows up as a wrong wake instead of a crash.
void
kfree(
    _In_ void* memory)
{
    if (memory != NULL && memory == g_queueResource) {
        assert(g_deferredFree == NULL);
        g_deferredFree = memory;
        return;
    }
    free(memory);
}

irqstate_t InterruptDisable(void) { return 0; }
irqstate_t InterruptRestoreState(irqstate_t state) { return state; }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
// The scheduler's timeout path unlinks a blocked thread holding only the
// list's own lock. Doing the same here, right before the futex code takes that
// lock, lets a test remove a thread between the moment the futex code picks it
// and the moment it unlinks it.
void
SpinlockAcquireIrq(
    _In_ Spinlock_t* lock)
{
    if (g_timeoutList != NULL && lock == &g_timeoutList->lock && --g_timeoutCountdown == 0) {
        list_t* list = g_timeoutList;

        g_timeoutList = NULL;
        assert(list_remove(list, g_timeoutThread) == 0);
    }
    assert(atomic_exchange(&lock->Current, 1) == 0);
}

void SpinlockReleaseIrq(Spinlock_t* lock) { assert(atomic_exchange(&lock->Current, 0) == 1); }

uuid_t ArchGetProcessorCoreId(void) { return 0; }
SystemCpuCore_t* GetProcessorCore(uuid_t id) { (void)id; return (SystemCpuCore_t*)1; }
Thread_t* CpuCoreCurrentThread(SystemCpuCore_t* core) { (void)core; return (Thread_t*)1; }
SchedulerObject_t* ThreadSchedulerHandle(Thread_t* thread) { (void)thread; return (SchedulerObject_t*)1; }
MemorySpace_t* GetCurrentMemorySpace(void) { return &g_space; }
void ArchThreadYield(void) { }
oserr_t SchedulerGetTimeoutReason(void) { return OS_EOK; }

oserr_t
GetMemorySpaceMapping(
    _In_  MemorySpace_t* space,
    _In_  vaddr_t        address,
    _In_  int            count,
    _Out_ paddr_t*       result)
{
    assert(space == &g_space && count == 1);
    *result = address;
    return OS_EOK;
}

oserr_t
ThreadFork(
    _In_ OSAsyncContext_t* context)
{
    (void)context;
    abort();
}

oserr_t
SchedulerBlock(
    _In_ list_t*        queue,
    _In_ OSTimestamp_t* deadline)
{
    element_t* thread;

    assert(deadline == NULL && g_blockedThreads < TEST_MAX_THREADS);
    thread = &g_threads[g_blockedThreads++];
    ELEMENT_INIT(thread, 0, thread);
    list_append(queue, thread);
    g_lastBlockQueue = queue;
    return OS_EOK;
}

oserr_t
SchedulerQueueObject(
    _In_ SchedulerObject_t* object)
{
    element_t* thread = (element_t*)object;

    assert(thread >= &g_threads[0] && thread < &g_threads[TEST_MAX_THREADS]);
    g_wokenThreads[g_wokenCount++] = (int)(thread - &g_threads[0]);
    return OS_EOK;
}

oserr_t
SHMAcquireAsyncBufferReference(
    _In_  uuid_t  handle,
    _Out_ void**  bufferOut,
    _Out_ size_t* lengthOut)
{
    if (handle != TEST_BUFFER_ID) {
        return OS_EINVALPARAMS;
    }
    g_bufferReferences++;
    *bufferOut = g_ringMemory;
    *lengthOut = g_ringLength;
    return OS_EOK;
}

uuid_t
CreateHandle(
    _In_ HandleType_t       type,
    _In_ HandleDestructorFn grantCleanup,
    _In_ HandleDestructorFn globalCleanup,
    _In_ void*              resource)
{
    assert(type == HandleTypeAsyncQueue && grantCleanup == NULL && globalCleanup != NULL);
    if (g_failCreateHandle) {
        return UUID_INVALID;
    }
    g_queueDestroy = globalCleanup;
    g_queueResource = resource;
    return TEST_QUEUE_ID;
}

static oserr_t
__Acquire(
    _In_  uuid_t       handle,
    _In_  HandleType_t type,
    _Out_ void**       resourceOut)
{
    if (handle == TEST_OTHER_ID) {
        return OS_EINVALPARAMS;
    }
    if (handle != TEST_QUEUE_ID || type != HandleTypeAsyncQueue || g_queueResource == NULL) {
        return OS_ENOENT;
    }
    g_queueReferences++;
    *resourceOut = g_queueResource;
    return OS_EOK;
}

oserr_t AcquireHandleReference(uuid_t handle, HandleType_t type, void** resourceOut) { return __Acquire(handle, type, resourceOut); }
oserr_t AcquireGlobalHandle(uuid_t handle, HandleType_t type, void** resourceOut) { return __Acquire(handle, type, resourceOut); }

void
ReleaseGlobalHandle(
    _In_ uuid_t handle)
{
    if (handle == TEST_QUEUE_ID) {
        assert(g_queueReferences-- > 0);
    } else {
        assert(handle == TEST_BUFFER_ID && g_bufferReferences-- > 0);
    }
}

// Test helpers.

static void
__CreateQueue(
    _In_ size_t length)
{
    uuid_t handle = UUID_INVALID;

    memset(g_ringMemory, 0xa5, sizeof(g_ringMemory));
    g_ringLength = length;
    assert(AsyncQueueCreate(TEST_BUFFER_ID, &handle) == OS_EOK);
    assert(handle == TEST_QUEUE_ID && g_bufferReferences == 1);
}

static void
__DestroyQueue(void)
{
    assert(g_queueReferences == 0);
    g_queueDestroy(g_queueResource);
    assert(g_bufferReferences == 0 && g_deferredFree == g_queueResource);
    free(g_deferredFree);
    g_deferredFree = NULL;
    g_queueResource = NULL;
    g_blockedThreads = 0;
    g_wokenCount = 0;
}

static void
__ExpectEntry(
    _In_ uint32_t          index,
    _In_ OSAsyncContext_t* context)
{
    OSAsyncCompletion_t* entry = &g_ring->Entries[index % g_ring->Capacity];

    assert(entry->Cookie == (uint64_t)(uintptr_t)context && entry->Status == OS_EOK);
}

// Makes a blocked thread time out. countdown counts acquires of the list lock
// of the futex queue the thread waits on, and the thread is unlinked just
// before the acquire that brings it to zero. That queue must be the one the
// latest SchedulerBlock call used.
static void
__ArmTimeout(
    _In_ int thread,
    _In_ int countdown)
{
    assert(g_lastBlockQueue != NULL && thread < g_blockedThreads);
    g_timeoutThread = &g_threads[thread];
    g_timeoutCountdown = countdown;
    g_timeoutList = g_lastBlockQueue;
}

static OSAsyncContext_t
__Context(void)
{
    return (OSAsyncContext_t) { .NotificationHandle = TEST_QUEUE_ID, .ErrorCode = OS_ESCSTARTED };
}

// Tests.

static void
__TestCreate(void)
{
    uuid_t handle = UUID_INVALID;

    assert(AsyncQueueCreate(TEST_BUFFER_ID, NULL) == OS_EINVALPARAMS);
    assert(AsyncQueueCreate(TEST_OTHER_ID, &handle) == OS_EINVALPARAMS);

    // The header and one entry is the smallest usable ring.
    g_ringLength = sizeof(OSAsyncCompletionRing_t) + sizeof(OSAsyncCompletion_t) - 1;
    assert(AsyncQueueCreate(TEST_BUFFER_ID, &handle) == OS_EINVALPARAMS);
    assert(handle == UUID_INVALID && g_bufferReferences == 0);

    g_ringLength = 4096;
    g_failCreateHandle = 1;
    assert(AsyncQueueCreate(TEST_BUFFER_ID, &handle) == OS_EOOM);
    assert(handle == UUID_INVALID && g_bufferReferences == 0);
    g_failCreateHandle = 0;

    // The capacity is the largest power of two that fits: a page holds 255
    // entries, which gives 128, and a larger buffer is capped.
    __CreateQueue(sizeof(OSAsyncCompletionRing_t) + sizeof(OSAsyncCompletion_t));
    assert(g_ring->Capacity == 1);
    __DestroyQueue();
    __CreateQueue(4096);
    assert(g_ring->Capacity == 128 && g_ring->WriteIndex == 0 && g_ring->ReadIndex == 0);
    assert(g_ring->Sequence == 0);
    __DestroyQueue();
    __CreateQueue(sizeof(g_ringMemory));
    assert(g_ring->Capacity == ASYNC_QUEUE_MAX_CAPACITY);
    __DestroyQueue();
}

static void
__TestWaitAndWake(void)
{
    OSAsyncContext_t context = __Context();
    OSAsyncContext_t other = __Context();
    _Atomic(int)     futex = 0;

    __CreateQueue(4096);
    assert(FutexWaitAsync(&context, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EFORKED);
    assert(g_queueReferences == 0 && g_ring->WriteIndex == 0);

    // The wake writes the result into the ring and advances Sequence. The
    // context in userspace memory is only written later, by the scheduler.
    assert(FutexWake(&futex, 1, FUTEX_FLAG_PRIVATE) == OS_EOK);
    assert(g_ring->WriteIndex == 1 && g_ring->Sequence == 1);
    __ExpectEntry(0, &context);
    assert(context.ErrorCode == OS_ESCSTARTED);
    assert(FutexWake(&futex, 1, FUTEX_FLAG_PRIVATE) == OS_ENOENT);

    // A changed value is reported at once and keeps no record.
    atomic_store(&futex, 1);
    assert(FutexWaitAsync(&other, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EINTERRUPTED);
    assert(FutexWake(&futex, 1, FUTEX_FLAG_PRIVATE) == OS_ENOENT);
    assert(g_ring->WriteIndex == 1 && g_queueReferences == 0);

    // A handle that is not a queue makes the caller fall back to a fork.
    other.NotificationHandle = TEST_OTHER_ID;
    assert(FutexWaitAsync(&other, &futex, 1, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);
    other.NotificationHandle = 0x99;
    assert(FutexWaitAsync(&other, &futex, 1, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);
    assert(FutexWaitAsync(NULL, &futex, 1, FUTEX_FLAG_PRIVATE) == OS_EINVALPARAMS);
    __DestroyQueue();
}

static void
__TestRingSpace(void)
{
    static OSAsyncContext_t contexts[256];
    OSAsyncContext_t        extra = __Context();
    _Atomic(int)            futex = 0;
    uint32_t                capacity;
    uint32_t                i;

    __CreateQueue(4096);
    capacity = g_ring->Capacity;
    for (i = 0; i < capacity; i++) {
        contexts[i] = __Context();
        assert(FutexWaitAsync(&contexts[i], &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EFORKED);
    }

    // Every record will need a ring entry, so no more can be queued.
    assert(FutexWaitAsync(&extra, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);
    assert(g_queueReferences == 0);

    // The results arrive in queue order.
    assert(FutexWake(&futex, INT_MAX, FUTEX_FLAG_PRIVATE) == OS_EOK);
    assert(g_ring->WriteIndex == capacity && g_ring->Sequence == (int)capacity);
    for (i = 0; i < capacity; i++) {
        __ExpectEntry(i, &contexts[i]);
    }

    // Unread entries still hold the space.
    assert(FutexWaitAsync(&extra, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);

    // A ReadIndex ahead of WriteIndex can only come from userspace and counts
    // as a full ring. Queued records must not make the unread count wrap into
    // a small number.
    atomic_store(&g_ring->ReadIndex, capacity);
    for (i = 0; i < 10; i++) {
        assert(FutexWaitAsync(&contexts[i], &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EFORKED);
    }
    atomic_store(&g_ring->ReadIndex, capacity + 5);
    assert(FutexWaitAsync(&extra, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);
    atomic_store(&g_ring->ReadIndex, 0);
    assert(FutexWaitAsync(&extra, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);

    // With the ring read, the ten records complete after the earlier results.
    atomic_store(&g_ring->ReadIndex, capacity);
    assert(FutexWake(&futex, INT_MAX, FUTEX_FLAG_PRIVATE) == OS_EOK);
    assert(g_ring->WriteIndex == capacity + 10);
    for (i = 0; i < 10; i++) {
        __ExpectEntry(capacity + i, &contexts[i]);
    }

    // Leave one slot free. The next result goes into it, past the end of the
    // ring and back to its start.
    atomic_store(&g_ring->ReadIndex, 11);
    assert(FutexWaitAsync(&extra, &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EFORKED);
    assert(FutexWaitAsync(&contexts[0], &futex, 0, FUTEX_FLAG_PRIVATE) == OS_ENOTSUPPORTED);
    assert(FutexWake(&futex, 1, FUTEX_FLAG_PRIVATE) == OS_EOK);
    assert(g_ring->WriteIndex == capacity + 11);
    __ExpectEntry(capacity + 10, &extra);

    // Reading everything makes all records available again.
    atomic_store(&g_ring->ReadIndex, capacity + 11);
    for (i = 0; i < capacity; i++) {
        assert(FutexWaitAsync(&contexts[i], &futex, 0, FUTEX_FLAG_PRIVATE) == OS_EFORKED);
    }
    assert(FutexWake(&futex, INT_MAX, FUTEX_FLAG_PRIVATE) == OS_EOK);
    assert(g_ring->WriteIndex == 2 * capacity + 11);
    __DestroyQueue();
}

static void
__TestWaiterOrder(void)
{
    OSAsyncContext_t first = __Context();
    OSAsyncContext_t second = __Context();
    _Atomic(int)     futex = 0;

    __CreateQueue(4096);

    // Thread 0, record, thread 1: one wake at a time takes them in that order.
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    assert(FutexWaitAsync(&first, &futex, 0, 0) == OS_EFORKED);
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_wokenCount == 1 && g_wokenThreads[0] == 0 && g_ring->WriteIndex == 0);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_wokenCount == 1 && g_ring->WriteIndex == 1);
    __ExpectEntry(0, &first);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_wokenCount == 2 && g_wokenThreads[1] == 1);

    // A threads-only wake passes over the record at the front.
    assert(FutexWaitAsync(&second, &futex, 0, 0) == OS_EFORKED);
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    assert(FutexWakeThreads(&futex, INT_MAX, 0) == OS_EOK);
    assert(g_wokenCount == 3 && g_wokenThreads[2] == 2 && g_ring->WriteIndex == 1);
    assert(FutexWakeThreads(&futex, INT_MAX, 0) == OS_ENOENT);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_EOK);
    assert(g_ring->WriteIndex == 2);
    __ExpectEntry(1, &second);
    __DestroyQueue();
}

static void
__TestTimedOutWaiter(void)
{
    OSAsyncContext_t context = __Context();
    _Atomic(int)     futex = 0;

    __CreateQueue(4096);

    // Threads 0, 1 and 2 wait. Thread 0 times out after the wake found it at
    // the front (list_front is the first acquire of the list lock) and before
    // the wake unlinks it (the second acquire). The wake must go on with
    // thread 1, and the thread that left must not use up the wake count.
    for (int i = 0; i < 3; i++) {
        assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    }
    __ArmTimeout(0, 2);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_timeoutList == NULL);
    assert(g_wokenCount == 1 && g_wokenThreads[0] == 1);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_EOK);
    assert(g_wokenCount == 2 && g_wokenThreads[1] == 2);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_ENOENT);

    // The same for a threads-only wake: thread 3, a record, thread 4. The
    // wake finds thread 3 without the list lock, and thread 3 times out just
    // before the removal (the first acquire). The wake passes over the record
    // and reaches thread 4.
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    assert(FutexWaitAsync(&context, &futex, 0, 0) == OS_EFORKED);
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
    __ArmTimeout(3, 1);
    assert(FutexWakeThreads(&futex, 1, 0) == OS_EOK);
    assert(g_timeoutList == NULL);
    assert(g_wokenCount == 3 && g_wokenThreads[2] == 4 && g_ring->WriteIndex == 0);
    assert(FutexWakeThreads(&futex, INT_MAX, 0) == OS_ENOENT);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_EOK);
    assert(g_ring->WriteIndex == 1);
    __ExpectEntry(0, &context);
    __DestroyQueue();
}

static void
__TestSequence(void)
{
    OSAsyncContext_t context = __Context();
    OSAsyncContext_t onSequence = __Context();
    _Atomic(int)     futex = 0;
    int              sequence;

    __CreateQueue(4096);

    // A changed Sequence returns at once. An unchanged one blocks.
    assert(AsyncQueueWait(TEST_QUEUE_ID, 1, NULL) == OS_EINTERRUPTED);
    assert(AsyncQueueWait(TEST_QUEUE_ID, 0, NULL) == OS_EOK);
    assert(g_blockedThreads == 1 && g_queueReferences == 0);
    assert(AsyncQueueWait(TEST_OTHER_ID, 0, NULL) == OS_EINVALPARAMS);

    // A post advances Sequence and wakes the blocked thread.
    assert(AsyncQueueSignal(TEST_QUEUE_ID) == OS_EOK);
    assert(g_ring->Sequence == 1 && g_wokenCount == 1 && g_queueReferences == 0);
    assert(AsyncQueueSignal(TEST_OTHER_ID) == OS_EINVALPARAMS);

    // A record on Sequence itself is never completed by the queue's own wake.
    // Otherwise a completion could start another completion inside it.
    sequence = atomic_load(&g_ring->Sequence);
    assert(FutexWaitAsync(&onSequence, &g_ring->Sequence, sequence, 0) == OS_EFORKED);
    assert(AsyncQueueWait(TEST_QUEUE_ID, sequence, NULL) == OS_EOK);
    assert(FutexWaitAsync(&context, &futex, 0, 0) == OS_EFORKED);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_ring->WriteIndex == 1 && g_wokenCount == 2);
    __ExpectEntry(0, &context);
    __DestroyQueue();
}

static void
__ExpectStatus(
    _In_ uint32_t          index,
    _In_ OSAsyncContext_t* context,
    _In_ oserr_t           status)
{
    OSAsyncCompletion_t* entry = &g_ring->Entries[index % g_ring->Capacity];

    assert(entry->Cookie == (uint64_t)(uintptr_t)context && entry->Status == status);
}

static void
__TestCancel(void)
{
    OSAsyncContext_t timed = __Context();
    OSAsyncContext_t other = __Context();
    _Atomic(int)     futex = 0;
    _Atomic(int)     second = 0;
    int              sequence;

    __CreateQueue(4096);

    // A queued record ends with OS_ETIMEOUT through the ring, and is gone
    // from the futex afterwards. Threads in AsyncQueueWait are woken.
    assert(FutexWaitAsync(&timed, &futex, 0, 0) == OS_EFORKED);
    assert(FutexWaitAsync(&other, &futex, 0, 0) == OS_EFORKED);
    sequence = atomic_load(&g_ring->Sequence);
    assert(AsyncQueueWait(TEST_QUEUE_ID, sequence, NULL) == OS_EOK);
    assert(AsyncQueueCancel(TEST_QUEUE_ID, (uint64_t)(uintptr_t)&timed) == OS_EOK);
    assert(g_ring->WriteIndex == 1 && g_ring->Sequence == sequence + 1);
    assert(g_wokenCount == 1 && g_queueReferences == 0);
    __ExpectStatus(0, &timed, OS_ETIMEOUT);

    // The other record is still queued and is the only one a wake finds.
    assert(FutexWake(&futex, INT_MAX, 0) == OS_EOK);
    assert(g_ring->WriteIndex == 2);
    __ExpectStatus(1, &other, OS_EOK);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_ENOENT);

    // A finished wait, an unknown cookie and a wrong handle change nothing.
    assert(AsyncQueueCancel(TEST_QUEUE_ID, (uint64_t)(uintptr_t)&timed) == OS_ENOENT);
    assert(AsyncQueueCancel(TEST_QUEUE_ID, 0x1234) == OS_ENOENT);
    assert(AsyncQueueCancel(TEST_OTHER_ID, (uint64_t)(uintptr_t)&other) == OS_EINVALPARAMS);
    assert(g_ring->WriteIndex == 2 && g_queueReferences == 0);

    // Both records were freed, so they can be reused on another futex. A
    // cancel for one context leaves the record of the other alone.
    assert(FutexWaitAsync(&other, &second, 0, 0) == OS_EFORKED);
    assert(FutexWaitAsync(&timed, &second, 0, 0) == OS_EFORKED);
    assert(AsyncQueueCancel(TEST_QUEUE_ID, (uint64_t)(uintptr_t)&timed) == OS_EOK);
    __ExpectStatus(2, &timed, OS_ETIMEOUT);
    assert(FutexWake(&second, INT_MAX, 0) == OS_EOK);
    __ExpectStatus(3, &other, OS_EOK);
    assert(g_ring->WriteIndex == 4);
    __DestroyQueue();
}

static void
__TestDestroy(void)
{
    OSAsyncContext_t contexts[3] = { __Context(), __Context(), __Context() };
    _Atomic(int)     futex = 0;

    __CreateQueue(4096);
    for (int i = 0; i < 3; i++) {
        assert(FutexWaitAsync(&contexts[i], &futex, 0, 0) == OS_EFORKED);
    }
    assert(FutexWait(NULL, &futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);

    // Destroying the queue removes its records, so the next wake reaches the
    // blocked thread behind them and nothing is written to the ring.
    g_queueDestroy(g_queueResource);
    assert(g_bufferReferences == 0);
    assert(FutexWake(&futex, 1, 0) == OS_EOK);
    assert(g_wokenCount == 1 && g_wokenThreads[0] == 0 && g_ring->WriteIndex == 0);
    assert(FutexWake(&futex, INT_MAX, 0) == OS_ENOENT);

    free(g_deferredFree);
    g_deferredFree = NULL;
    g_queueResource = NULL;
    g_blockedThreads = 0;
    g_wokenCount = 0;
}

int
main(void)
{
    FutexInitialize();
    __TestCreate();
    __TestWaitAndWake();
    __TestRingSpace();
    __TestWaiterOrder();
    __TestCancel();
    __TestTimedOutWaiter();
    // __TestSequence leaves a record queued at teardown. Run __TestDestroy
    // first, so a destructor that leaves records behind fails its own checks.
    __TestDestroy();
    __TestSequence();
    puts("Asynchronous queue: creation, wait records, ring space, waiter order, timeouts, timed out threads, Sequence wakes and teardown passed");
    return 0;
}
