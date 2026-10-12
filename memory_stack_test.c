/** Verify range allocation against an independent page-level reference model. */
#include <utils/memory_stack.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 0x1000
#define PAGES 65
#define BASE 0x301000

static unsigned int g_random = 0x32ac8901;

static unsigned int
__Random(void)
{
    g_random ^= g_random << 13;
    g_random ^= g_random >> 17;
    g_random ^= g_random << 5;
    return g_random;
}

static void
__ExpectAllocation(
    _In_ MemoryStack_t* stack,
    _In_ size_t         length,
    _In_ size_t         alignment,
    _In_ size_t         boundary,
    _In_ oserr_t        expected,
    _In_ uintptr_t      address)
{
    uintptr_t output = 0xfeed;
    size_t before = MemoryStackFreeBlockCount(stack);

    assert(MemoryStackPopContiguous(stack, length, alignment, boundary, &output) == expected);
    assert(output == (expected == OS_EOK ? address : 0xfeed));
    assert(MemoryStackFreeBlockCount(stack) ==
           (expected == OS_EOK ? before - DIVUP(length, PAGE) : before));
}

static void
__TestConstraints(void)
{
    MemoryStack_t stack;
    MemoryStack_t saved;
    uintptr_t storage[128];
    uintptr_t address;
    uintptr_t top = UINTPTR_MAX & ~(uintptr_t)(PAGE - 1);
    int count;

    memset(&stack, 0, sizeof(stack));
    saved = stack;
    assert(MemoryStackCalculateSize(0) == 0);
    assert(MemoryStackCalculateSize(SIZE_MAX) == 0);
    assert(MemoryStackConstruct(NULL, PAGE, (uintptr_t)storage, sizeof(storage)) == OS_EINVALPARAMS);
    assert(MemoryStackConstruct(&stack, 0, (uintptr_t)storage, sizeof(storage)) == OS_EINVALPARAMS);
    assert(MemoryStackConstruct(&stack, 3, (uintptr_t)storage, sizeof(storage)) == OS_EINVALPARAMS);
    assert(MemoryStackConstruct(&stack, PAGE, 0, sizeof(storage)) == OS_EINVALPARAMS);
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage + 1, sizeof(storage)) == OS_EINVALPARAMS);
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, 1) == OS_EINVALPARAMS);
    assert(memcmp(&stack, &saved, sizeof(stack)) == 0);
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, sizeof(storage)) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x701000, 16) == OS_EOK);
    __ExpectAllocation(&stack, PAGE, 0x4000, 0, OS_EOK, 0x704000);
    __ExpectAllocation(&stack, PAGE, 0x100, 0, OS_EOK, 0x701000);
    __ExpectAllocation(&stack, 0x2000, 0, 0x2000, OS_EOK, 0x702000);
    __ExpectAllocation(&stack, 0x2000, 0, 0x2000, OS_EOK, 0x706000);
    __ExpectAllocation(&stack, 0x800, 0, 0x800, OS_EOK, 0x705000);
    __ExpectAllocation(&stack, 0, 0, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&stack, SIZE_MAX, 0, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&stack, PAGE, 3, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&stack, PAGE, 0, 3, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&stack, 0x2000, 0, PAGE, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&stack, 0x20000, 0, 0, OS_EOOM, 0);
    assert(MemoryStackPopContiguous(&stack, PAGE, 0, 0, NULL) == OS_EINVALPARAMS);
    assert(MemoryStackPush(&stack, 0x700001, 1) == OS_EINVALPARAMS);
    assert(MemoryStackPush(&stack, 0x700000, 0) == OS_EINVALPARAMS);
    assert(MemoryStackPush(&stack, 0x700000, -1) == OS_EINVALPARAMS);
    assert(MemoryStackPush(&stack, top, 2) == OS_EINVALPARAMS);

    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, sizeof(storage)) == OS_EOK);
    assert(MemoryStackPush(&stack, top, 1) == OS_EOK);
    __ExpectAllocation(&stack, PAGE, 2 * PAGE, 0, OS_EOOM, 0);
    __ExpectAllocation(&stack, PAGE, 0, PAGE, OS_EOK, top);
    __ExpectAllocation(&stack, 1, 0, 0, OS_EOOM, 0);
    assert(MemoryStackPush(&stack, top, 1) == OS_EOK);
    assert(MemoryStackPush(&stack, top - PAGE, 1) == OS_EOK);
    assert(stack.index == 1);
    __ExpectAllocation(&stack, 2 * PAGE, 0, 0, OS_EOK, top - PAGE);
    assert(MemoryStackPush(&stack, 0, 1) == OS_EOK);
    __ExpectAllocation(&stack, 1, 0, 0, OS_EOK, 0);
    count = 1;
    address = 0xfeed;
    assert(MemoryStackPop(&stack, &count, &address) == OS_EOOM);
    assert(count == 1 && address == 0xfeed);
}

static void
__TestCoalescingAndCapacity(void)
{
    MemoryStack_t stack;
    uintptr_t storage[32];
    uintptr_t pages[8];
    size_t oneEntry = MemoryStackCalculateSize(1);
    int count;

    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, oneEntry) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x3000, 1) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x2000, 1) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x1000, 1) == OS_EOK);
    assert(stack.index == 1);
    __ExpectAllocation(&stack, 0x3000, 0, 0, OS_EOK, 0x1000);

    // With no descriptor left, an interior split fails without losing either
    // side. A later run that can be taken from its edge remains usable.
    assert(MemoryStackPush(&stack, 0x1000, 3) == OS_EOK);
    __ExpectAllocation(&stack, PAGE, 0x2000, 0, OS_EOOM, 0);
    assert(stack.index == 1);
    assert(MemoryStackPush(&stack, 0x8000, 1) == OS_EOOM);
    __ExpectAllocation(&stack, 0x3000, 0, 0, OS_EOK, 0x1000);
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, 2 * oneEntry) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x1000, 3) == OS_EOK);
    assert(MemoryStackPush(&stack, 0x8000, 1) == OS_EOK);
    __ExpectAllocation(&stack, PAGE, 0x2000, 0, OS_EOK, 0x8000);
    __ExpectAllocation(&stack, PAGE, 0x2000, 0, OS_EOK, 0x2000);
    assert(stack.index == 2);
    assert(MemoryStackPush(&stack, 0x2000, 1) == OS_EOK);
    assert(stack.index == 1);
    assert(MemoryStackPush(&stack, 0x2000, 1) == OS_EINVALPARAMS);
    assert(MemoryStackPush(&stack, 0, 2) == OS_EINVALPARAMS);
    assert(MemoryStackFreeBlockCount(&stack) == 3);

    // Roll back a batch that first bridges two ranges, inserts a new range,
    // then runs out of metadata. Undoing the bridge must restore both sides.
    __ExpectAllocation(&stack, PAGE, 0x2000, 0, OS_EOK, 0x2000);
    pages[0] = 0x2000;
    pages[1] = 0x9000;
    pages[2] = 0xb000;
    assert(MemoryStackPushMultiple(&stack, pages, 3) == OS_EOOM);
    assert(stack.index == 2 && MemoryStackFreeBlockCount(&stack) == 2);
    pages[1] = pages[0];
    assert(MemoryStackPushMultiple(&stack, pages, 2) == OS_EINVALPARAMS);
    assert(stack.index == 2 && MemoryStackFreeBlockCount(&stack) == 2);
    __ExpectAllocation(&stack, PAGE, 0, 0, OS_EOK, 0x1000);
    __ExpectAllocation(&stack, PAGE, 0, 0, OS_EOK, 0x3000);

    pages[0] = 0x4000;
    pages[1] = 0x2000;
    pages[2] = 0x3000;
    pages[3] = 0x1000;
    assert(MemoryStackPushMultiple(&stack, pages, 4) == OS_EOK);
    assert(stack.index == 1);
    count = 6;
    assert(MemoryStackPop(&stack, &count, pages) == OS_EINCOMPLETE);
    assert(count == 4 && stack.index == 0);
    for (int i = 0; i < 4; i++) {
        assert(pages[i] == (uintptr_t)(i + 1) * PAGE);
    }
}

static void
__CheckModel(
    _In_ MemoryStack_t* stack,
    _In_ const bool*    available)
{
    MemoryStack_t copy = *stack;
    uintptr_t storage[256];
    uintptr_t pages[PAGES];
    bool seen[PAGES] = { false };
    size_t freeCount = 0;
    int runs = 0;
    int count = PAGES;
    oserr_t status;

    for (int i = 0; i < PAGES; i++) {
        if (available[i]) {
            freeCount++;
            if (i == 0 || !available[i - 1]) {
                runs++;
            }
        }
    }
    assert(MemoryStackFreeBlockCount(stack) == freeCount);
    assert(stack->index == runs);
    assert(stack->data_size <= sizeof(storage));
    memcpy(storage, stack->items, stack->data_size);
    MemoryStackRelocate(&copy, (uintptr_t)storage);
    status = MemoryStackPop(&copy, &count, pages);
    if (freeCount == 0) {
        assert(status == OS_EOOM && count == PAGES);
        return;
    }
    assert(status == (freeCount == PAGES ? OS_EOK : OS_EINCOMPLETE));
    assert((size_t)count == freeCount && copy.index == 0);
    for (int i = 0; i < count; i++) {
        size_t page = (pages[i] - BASE) / PAGE;
        assert(pages[i] >= BASE && page < PAGES);
        assert(available[page] && !seen[page]);
        seen[page] = true;
    }
}

/** A live allocation in the independent reference model. */
struct __ModelAllocation {
    uintptr_t Base;
    size_t Count;
};

static void
__TestRandomized(void)
{
    struct __ModelAllocation live[PAGES] = { 0 };
    MemoryStack_t stack;
    uintptr_t storage[256];
    uintptr_t batch[PAGES];
    bool available[PAGES];
    uintptr_t expected;
    uintptr_t actual;
    size_t length;
    size_t alignment;
    size_t boundary;
    size_t count;
    size_t first;
    unsigned int slot;
    oserr_t status;
    bool fits;

    memset(available, true, sizeof(available));
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, MemoryStackCalculateSize(PAGES)) == OS_EOK);
    assert(MemoryStackPush(&stack, BASE, PAGES) == OS_EOK);
    for (int iteration = 0; iteration < 20000; iteration++) {
        slot = __Random() % PAGES;
        if (live[slot].Count != 0) {
            count = live[slot].Count;
            first = (live[slot].Base - BASE) / PAGE;
            for (size_t i = 0; i < count; i++) {
                assert(!available[first + i]);
                available[first + i] = true;
                batch[count - 1 - i] = live[slot].Base + i * PAGE;
            }
            if (__Random() & 1) {
                assert(MemoryStackPushMultiple(&stack, batch, (int)count) == OS_EOK);
            } else {
                assert(MemoryStackPush(&stack, live[slot].Base, (int)count) == OS_EOK);
            }
            live[slot].Count = 0;
        } else {
            count = 1 + __Random() % 9;
            length = count * PAGE - __Random() % PAGE;
            alignment = PAGE << (__Random() % 5);
            boundary = (__Random() & 1) ? PAGE << (__Random() % 6) : 0;
            expected = UINTPTR_MAX;
            if (boundary == 0 || length <= boundary) {
                for (size_t i = 0; i + count <= PAGES; i++) {
                    actual = BASE + i * PAGE;
                    if (actual % alignment != 0) {
                        continue;
                    }
                    if (boundary != 0 && actual / boundary != (actual + length - 1) / boundary) {
                        continue;
                    }
                    fits = true;
                    for (size_t j = 0; j < count; j++) {
                        fits = fits && available[i + j];
                    }
                    if (fits) {
                        expected = actual;
                        break;
                    }
                }
            }
            actual = 0xfeed;
            status = MemoryStackPopContiguous(&stack, length, alignment, boundary, &actual);
            if (boundary != 0 && length > boundary) {
                assert(status == OS_EINVALPARAMS && actual == 0xfeed);
            } else if (expected == UINTPTR_MAX) {
                assert(status == OS_EOOM && actual == 0xfeed);
            } else {
                assert(status == OS_EOK && actual == expected);
                live[slot] = (struct __ModelAllocation){actual, count};
                first = (actual - BASE) / PAGE;
                for (size_t i = 0; i < count; i++) {
                    available[first + i] = false;
                }
            }
        }
        __CheckModel(&stack, available);
    }
    for (int i = 0; i < PAGES; i++) {
        if (live[i].Count != 0) {
            assert(MemoryStackPush(&stack, live[i].Base, (int)live[i].Count) == OS_EOK);
        }
    }
    assert(stack.index == 1);
    __ExpectAllocation(&stack, PAGES * PAGE, 0, 0, OS_EOK, BASE);
}

static void
__TestWorstCaseStorage(
    _In_ int count)
{
    MemoryStack_t stack;
    size_t bytes = MemoryStackCalculateSize((size_t)count);
    void* storage = malloc(bytes);
    uintptr_t* pages = malloc((size_t)count * sizeof(*pages));
    int requested = count;

    assert(storage != NULL && pages != NULL);
    assert(MemoryStackConstruct(&stack, PAGE, (uintptr_t)storage, bytes) == OS_EOK);
    assert(MemoryStackPush(&stack, PAGE, count) == OS_EOK);
    assert(MemoryStackPop(&stack, &requested, pages) == OS_EOK);
    // This exceeds the old one-page metadata capacity. Even with every other
    // page freed and all descriptors occupied, bridging frees still succeed.
    for (int i = 0; i < count; i += 2) {
        assert(MemoryStackPush(&stack, pages[i], 1) == OS_EOK);
    }
    assert(stack.index == (count + 1) / 2 && stack.index == stack.capacity);
    for (int i = count - 1; i >= 0; i--) {
        if (i & 1) {
            assert(MemoryStackPush(&stack, pages[i], 1) == OS_EOK);
        }
    }
    assert(stack.index == 1);
    __ExpectAllocation(&stack, (size_t)count * PAGE, 0, 0, OS_EOK, PAGE);
    free(pages);
    free(storage);
}

int
main(void)
{
    __TestConstraints();
    __TestCoalescingAndCapacity();
    __TestRandomized();
    __TestWorstCaseStorage(1024);
    __TestWorstCaseStorage(1025);
    puts("Memory stack: coalescing, constraints, overflow, metadata exhaustion, batch rollback and 20000 model operations passed");
    return 0;
}
