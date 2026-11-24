/**
 * Verify the kernel stack cache in kernel/threads/stack_cache.c.
 *
 * Fake stacks are page-aligned host buffers of THREADING_KERNEL_STACK_SIZE.
 * The cache writes its list entry into the top bytes of a stack, so the tests
 * check that a refused stack is left unchanged and that a taken stack returns
 * the same top address that was given.
 */
#include <stack_cache.h>
#include <spinlock.h>
#include <threading.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PAGE_SIZE      0x1000
#define TEST_STACK_COUNT    (KERNEL_STACK_CACHE_LIMIT + 1)
#define TEST_THREAD_COUNT   4
#define TEST_THREAD_STACKS  8
#define TEST_THREAD_ROUNDS  20000
#define TEST_FILL_BYTE      0xa5

static uint8_t* g_stacks[TEST_STACK_COUNT];

// The cache uses only the IRQ variants. The host has no interrupts, so a
// plain test-and-set lock is enough to serialize the worker threads.
void
SpinlockAcquireIrq(
    _In_ Spinlock_t* lock)
{
    while (atomic_exchange(&lock->Current, 1) != 0) {
        sched_yield();
    }
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* lock)
{
    atomic_store(&lock->Current, 0);
}

static uintptr_t
__Top(
    _In_ uint8_t* stack)
{
    return (uintptr_t)stack + THREADING_KERNEL_STACK_SIZE;
}

static bool
__IsFilled(
    _In_ uint8_t* stack)
{
    for (size_t i = 0; i < THREADING_KERNEL_STACK_SIZE; i++) {
        if (stack[i] != TEST_FILL_BYTE) {
            return false;
        }
    }
    return true;
}

// Takes every cached stack, so each test starts and ends with an empty cache.
static int
__Drain(void)
{
    uintptr_t top;
    int       count = 0;

    while (StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &top) == OS_EOK) {
        count++;
    }
    return count;
}

static void
__TestParameters(void)
{
    uintptr_t top = 0xfeed;

    assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, NULL) == OS_EINVALPARAMS);
    assert(StackCacheRelease(0, THREADING_KERNEL_STACK_SIZE) == OS_EINVALPARAMS);

    // An empty cache leaves the output alone.
    assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &top) == OS_ENOENT);
    assert(top == 0xfeed);
}

static void
__TestOtherSizes(void)
{
    uintptr_t top = 0xfeed;

    // A stack of another size is refused and its memory is not written.
    memset(g_stacks[0], TEST_FILL_BYTE, THREADING_KERNEL_STACK_SIZE);
    assert(StackCacheRelease(__Top(g_stacks[0]), THREADING_KERNEL_STACK_SIZE / 2) == OS_ENOTSUPPORTED);
    assert(StackCacheRelease(__Top(g_stacks[0]), THREADING_KERNEL_STACK_SIZE * 2) == OS_ENOTSUPPORTED);
    assert(__IsFilled(g_stacks[0]));

    // A cached default-size stack is never handed out for another size.
    assert(StackCacheRelease(__Top(g_stacks[0]), THREADING_KERNEL_STACK_SIZE) == OS_EOK);
    assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE * 2, &top) == OS_ENOENT);
    assert(top == 0xfeed);
    assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &top) == OS_EOK);
    assert(top == __Top(g_stacks[0]));
    assert(__Drain() == 0);
}

static void
__TestLimitAndOrder(void)
{
    uintptr_t top;
    int       round;
    int       i;

    // Run twice, so a count that is not lowered by taking shows up as a
    // refused stack in the second round.
    for (round = 0; round < 2; round++) {
        for (i = 0; i < KERNEL_STACK_CACHE_LIMIT; i++) {
            assert(StackCacheRelease(__Top(g_stacks[i]), THREADING_KERNEL_STACK_SIZE) == OS_EOK);
        }

        // A full cache refuses the stack and leaves its memory unchanged.
        memset(g_stacks[KERNEL_STACK_CACHE_LIMIT], TEST_FILL_BYTE, THREADING_KERNEL_STACK_SIZE);
        assert(StackCacheRelease(__Top(g_stacks[KERNEL_STACK_CACHE_LIMIT]),
                                    THREADING_KERNEL_STACK_SIZE) == OS_EBUSY);
        assert(__IsFilled(g_stacks[KERNEL_STACK_CACHE_LIMIT]));

        // The most recently given stack comes back first.
        for (i = KERNEL_STACK_CACHE_LIMIT - 1; i >= 0; i--) {
            assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &top) == OS_EOK);
            assert(top == __Top(g_stacks[i]));
        }
        assert(StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &top) == OS_ENOENT);
    }
}

struct __WorkerContext {
    uint8_t*          Stacks[TEST_THREAD_STACKS];
    _Atomic(uint8_t)* Owners;
    int               Id;
};

static _Atomic(uint8_t) g_owners[TEST_THREAD_COUNT * TEST_THREAD_STACKS];
static uint8_t*         g_workerStacks[TEST_THREAD_COUNT * TEST_THREAD_STACKS];

static int
__WorkerIndexOf(
    _In_ uintptr_t top)
{
    for (int i = 0; i < TEST_THREAD_COUNT * TEST_THREAD_STACKS; i++) {
        if (__Top(g_workerStacks[i]) == top) {
            return i;
        }
    }
    return -1;
}

// Each worker gives its stacks to the cache and takes stacks back, and marks
// every stack it holds. Two workers holding the same stack at once would mean
// the list was corrupted by an unsynchronized update.
static void*
__Worker(
    _In_ void* argument)
{
    struct __WorkerContext* context = argument;
    uintptr_t               held[TEST_THREAD_STACKS];
    int                     heldCount = 0;
    int                     index;
    uint8_t                 expected;

    for (int i = 0; i < TEST_THREAD_STACKS; i++) {
        held[heldCount++] = __Top(context->Stacks[i]);
    }

    for (int round = 0; round < TEST_THREAD_ROUNDS; round++) {
        if (heldCount > 0 && (round & 1) == 0) {
            heldCount--;
            index = __WorkerIndexOf(held[heldCount]);
            assert(index >= 0);
            expected = (uint8_t)(context->Id + 1);
            assert(atomic_compare_exchange_strong(&g_owners[index], &expected, 0));
            assert(StackCacheRelease(held[heldCount], THREADING_KERNEL_STACK_SIZE) == OS_EOK);
        } else if (StackCacheAllocate(THREADING_KERNEL_STACK_SIZE, &held[heldCount]) == OS_EOK) {
            index = __WorkerIndexOf(held[heldCount]);
            assert(index >= 0);
            expected = 0;
            assert(atomic_compare_exchange_strong(&g_owners[index], &expected, (uint8_t)(context->Id + 1)));
            heldCount++;
        }
    }

    // Give back everything, so the main thread can count the stacks.
    while (heldCount > 0) {
        heldCount--;
        index = __WorkerIndexOf(held[heldCount]);
        expected = (uint8_t)(context->Id + 1);
        assert(atomic_compare_exchange_strong(&g_owners[index], &expected, 0));
        assert(StackCacheRelease(held[heldCount], THREADING_KERNEL_STACK_SIZE) == OS_EOK);
    }
    return NULL;
}

static void
__TestConcurrentUse(void)
{
    struct __WorkerContext contexts[TEST_THREAD_COUNT];
    pthread_t              threads[TEST_THREAD_COUNT];

    // All worker stacks together stay below the limit, so no give is refused.
    _Static_assert(TEST_THREAD_COUNT * TEST_THREAD_STACKS <= KERNEL_STACK_CACHE_LIMIT,
                   "worker stacks must fit in the cache");

    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
        contexts[i].Owners = g_owners;
        contexts[i].Id = i;
        for (int j = 0; j < TEST_THREAD_STACKS; j++) {
            contexts[i].Stacks[j] = g_workerStacks[i * TEST_THREAD_STACKS + j];
            atomic_store(&g_owners[i * TEST_THREAD_STACKS + j], (uint8_t)(i + 1));
        }
    }

    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
        assert(pthread_create(&threads[i], NULL, __Worker, &contexts[i]) == 0);
    }
    for (int i = 0; i < TEST_THREAD_COUNT; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }

    // Every stack is back in the cache exactly once.
    assert(__Drain() == TEST_THREAD_COUNT * TEST_THREAD_STACKS);
}

int
main(void)
{
    for (int i = 0; i < TEST_STACK_COUNT; i++) {
        g_stacks[i] = aligned_alloc(TEST_PAGE_SIZE, THREADING_KERNEL_STACK_SIZE);
        assert(g_stacks[i] != NULL);
    }
    for (int i = 0; i < TEST_THREAD_COUNT * TEST_THREAD_STACKS; i++) {
        g_workerStacks[i] = aligned_alloc(TEST_PAGE_SIZE, THREADING_KERNEL_STACK_SIZE);
        assert(g_workerStacks[i] != NULL);
    }

    __TestParameters();
    __TestOtherSizes();
    __TestLimitAndOrder();
    __TestConcurrentUse();

    for (int i = 0; i < TEST_STACK_COUNT; i++) {
        free(g_stacks[i]);
    }
    for (int i = 0; i < TEST_THREAD_COUNT * TEST_THREAD_STACKS; i++) {
        free(g_workerStacks[i]);
    }
    printf("kernel stack cache tests passed\n");
    return 0;
}
