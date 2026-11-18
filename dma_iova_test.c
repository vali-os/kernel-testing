/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Compile the production allocator here to inspect its tree and inject count
// exhaustion without adding test hooks to the native API. The tree itself is
// linked from libds. Only heap, logging and kernel locking use host substitutes.
#include <os/osdefs.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The shared host osdefs shim omits these production convenience macros.
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define __VALI_MUTEX_H__
#define MUTEX_FLAG_PLAIN 0
typedef struct { pthread_mutex_t Native; } Mutex_t;

static _Thread_local Mutex_t* g_lockedMutex;
static _Thread_local bool     g_unpublished;
static _Thread_local bool     g_failHeap;
static _Thread_local int      g_failAppendAfter = -1;
static _Thread_local bool     g_failRemove;
static _Thread_local size_t   g_treeCalls;
static _Thread_local size_t   g_removeCalls;
static _Atomic(size_t)        g_liveMetadata;
static _Atomic(size_t)        g_heapCalls;
static unsigned int          g_errors;

static void MutexConstruct(Mutex_t* mutex, unsigned int flags)
{
    assert(flags == MUTEX_FLAG_PLAIN);
    assert(pthread_mutex_init(&mutex->Native, NULL) == 0);
}

static void MutexDestruct(Mutex_t* mutex)
{
    assert(g_lockedMutex == NULL);
    assert(pthread_mutex_destroy(&mutex->Native) == 0);
}

static void MutexLock(Mutex_t* mutex)
{
    assert(g_lockedMutex == NULL);
    assert(pthread_mutex_lock(&mutex->Native) == 0);
    g_lockedMutex = mutex;
}

static void MutexUnlock(Mutex_t* mutex)
{
    assert(g_lockedMutex == mutex);
    g_lockedMutex = NULL;
    assert(pthread_mutex_unlock(&mutex->Native) == 0);
}

static void __Error(const char* format, ...) __attribute__((format(printf, 1, 2)));
static void __Error(const char* format, ...)
{
    char    message[256];
    va_list arguments;

    va_start(arguments, format);
    assert(vsnprintf(message, sizeof(message), format, arguments) > 0);
    va_end(arguments);
    assert(strstr(message, "DMA IOVA:") != NULL && strstr(message, "0x") != NULL);
    g_errors++;
}

#define _DEBUG_H_
#define ERROR(...) __Error(__VA_ARGS__)
#define WARNING(...) ((void)0)
#ifndef DMA_IOVA_SOURCE
#define DMA_IOVA_SOURCE "../kernel/memory/dma/iova.c"
#endif
#include DMA_IOVA_SOURCE

void* kmalloc(size_t size)
{
    void* memory;

    assert(g_lockedMutex == NULL);
    atomic_fetch_add(&g_heapCalls, 1);
    if (g_failHeap) {
        return NULL;
    }
    memory = malloc(size);
    assert(memory != NULL);
    atomic_fetch_add(&g_liveMetadata, 1);
    return memory;
}

void kfree(void* memory)
{
    assert(g_lockedMutex == NULL && memory != NULL);
    assert(atomic_fetch_sub(&g_liveMetadata, 1) != 0);
    free(memory);
}

void SpinlockConstruct(Spinlock_t* lock)
{
    atomic_init(&lock->Current, 0);
    atomic_init(&lock->Next, 0);
    lock->IrqState = 0;
}

void SpinlockAcquireIrq(Spinlock_t* lock)
{
    // Every tree access after creation must be inside the allocator mutex.
    // This assertion catches missing locking deterministically, before races.
    assert(g_unpublished || g_lockedMutex != NULL);
    while (atomic_exchange(&lock->Current, 1) != 0) {
        sched_yield();
    }
    g_treeCalls++;
}

void SpinlockReleaseIrq(Spinlock_t* lock)
{
    assert(atomic_exchange(&lock->Current, 0) == 1);
}

int __real_rb_tree_append(rb_tree_t*, rb_leaf_t*);
int __wrap_rb_tree_append(rb_tree_t* tree, rb_leaf_t* node)
{
    if (g_failAppendAfter == 0) {
        return -1;
    }
    if (g_failAppendAfter > 0) {
        g_failAppendAfter--;
    }
    return __real_rb_tree_append(tree, node);
}

rb_leaf_t* __real_rb_tree_remove(rb_tree_t*, void*);
rb_leaf_t* __wrap_rb_tree_remove(rb_tree_t* tree, void* key)
{
    g_removeCalls++;
    if (g_failRemove) {
        return NULL;
    }
    return __real_rb_tree_remove(tree, key);
}

static oserr_t
__TryCreate(const DMAIOVAConfig_t* config, DMAIOVAAllocator_t** allocator)
{
    oserr_t status;

    g_unpublished = true;
    status = DMAIOVAAllocatorCreate(config, allocator);
    g_unpublished = false;
    return status;
}

static DMAIOVAAllocator_t*
__Create(DMAIOVAConfig_t config)
{
    DMAIOVAAllocator_t* allocator = NULL;

    assert(__TryCreate(&config, &allocator) == OS_EOK && allocator != NULL);
    return allocator;
}

static DMAIOVAAllocation_t*
__Allocate(DMAIOVAAllocator_t* allocator, DMAIOVARequest_t request, uint64_t first)
{
    DMAIOVAAllocation_t*  allocation = NULL;
    const DMAIOVARange_t* range;

    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EOK);
    range = DMAIOVAAllocationRange(allocation);
    assert(range->First == first && range->Last - first == request.Length - 1);
    return allocation;
}

static void
__Release(DMAIOVAAllocation_t** allocation)
{
    bool   live = *allocation != NULL;
    size_t removals = g_removeCalls;

    assert(DMAIOVAAllocationRelease(allocation) == OS_EOK && *allocation == NULL);
    // Check unlinking before later tests inspect the tree. A mutation that
    // skips removal must fail here, before a freed node can be traversed.
    assert(g_removeCalls == removals + (live ? 1 : 0));
}

static void
__NoFit(DMAIOVAAllocator_t* allocator, DMAIOVARequest_t request)
{
    DMAIOVAAllocation_t* allocation = NULL;
    size_t               live = atomic_load(&g_liveMetadata);

    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_ENOENT);
    assert(allocation == NULL && atomic_load(&g_liveMetadata) == live);
}

static void
__TestCreation(void)
{
    DMAIOVAConfig_t invalid[] = {
        { { 0, 255 }, 0, NULL, 0 }, { { 0, 255 }, 3, NULL, 0 },
        { { 1, 255 }, 16, NULL, 0 }, { { 0, 254 }, 16, NULL, 0 },
        { { 256, 255 }, 16, NULL, 0 }, { { 0, 255 }, 16, NULL, 1 }
    };
    DMAIOVARange_t badRanges[][2] = {
        { { 16, 31 }, { 16, 31 } }, { { 16, 63 }, { 32, 47 } },
        { { 32, 47 }, { 16, 63 } }, { { 0, 31 }, { 16, 47 } },
        { { 1, 15 }, { 32, 47 } }, { { 16, 30 }, { 32, 47 } },
        { { 32, 31 }, { 48, 63 } }, { { 256, 271 }, { 32, 47 } }
    };
    DMAIOVARange_t       reserved[] = { { 128, 143 }, { 0, 15 }, { 16, 31 } };
    DMAIOVAConfig_t      config = { { 0, 255 }, 16, reserved, 3 };
    DMAIOVAAllocator_t*  allocator = NULL;
    DMAIOVAAllocator_t*  saved;
    DMAIOVAAllocation_t* allocation;

    assert(__TryCreate(NULL, &allocator) == OS_EINVALPARAMS);
    assert(__TryCreate(&config, NULL) == OS_EINVALPARAMS);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        assert(__TryCreate(&invalid[i], &allocator) == OS_EINVALPARAMS);
        assert(allocator == NULL && atomic_load(&g_liveMetadata) == 0);
    }
    for (size_t i = 0; i < sizeof(badRanges) / sizeof(*badRanges); i++) {
        DMAIOVAConfig_t bad = { { 0, 255 }, 16, badRanges[i], 2 };

        assert(__TryCreate(&bad, &allocator) == OS_EINVALPARAMS);
        assert(allocator == NULL && atomic_load(&g_liveMetadata) == 0);
    }
    g_failHeap = true;
    assert(__TryCreate(&config, &allocator) == OS_EOOM && allocator == NULL);
    g_failHeap = false;
    // Fail after each possible number of inserted fixed nodes. All storage
    // belongs to the unpublished allocator, so partial construction must free it.
    for (int i = 0; i < 3; i++) {
        g_failAppendAfter = i;
        assert(__TryCreate(&config, &allocator) == OS_EUNKNOWN);
        assert(allocator == NULL && atomic_load(&g_liveMetadata) == 0);
    }
    g_failAppendAfter = -1;
    allocator = __Create(config);
    saved = allocator;
    assert(__TryCreate(&config, &allocator) == OS_EBUSY && allocator == saved);
    memset(reserved, 0, sizeof(reserved));
    allocation = __Allocate(allocator, (DMAIOVARequest_t){ 16, 0, 0, 255 }, 32);
    __Release(&allocation);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK && allocator == NULL);
}

static void
__TestRequests(void)
{
    DMAIOVAAllocator_t*  allocator = __Create((DMAIOVAConfig_t){ { 0, 255 }, 4, NULL, 0 });
    DMAIOVAAllocation_t* allocation = NULL;
    DMAIOVAAllocation_t* saved;
    DMAIOVARequest_t     request = { 4, 0, 0, 255 };
    DMAIOVARequest_t bad[] = {
        { 0, 0, 0, 255 }, { 3, 0, 0, 255 },
        { 4, 3, 0, 255 }, { 4, 0, 3, 255 }
    };

    assert(DMAIOVAAllocate(NULL, &request, &allocation) == OS_EINVALPARAMS);
    assert(DMAIOVAAllocate(allocator, NULL, &allocation) == OS_EINVALPARAMS);
    assert(DMAIOVAAllocate(allocator, &request, NULL) == OS_EINVALPARAMS);
    g_failHeap = true;
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        assert(DMAIOVAAllocate(allocator, &bad[i], &allocation) == OS_EINVALPARAMS);
        assert(allocation == NULL);
    }
    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EOOM);
    request.Boundary = 1;
    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EOOM);
    g_failHeap = false;
    __NoFit(allocator, request);
    request.Boundary = 0;
    allocator->ActiveCount = SIZE_MAX;
    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EOVERFLOW);
    assert(allocation == NULL);
    allocator->ActiveCount = 0;
    g_failAppendAfter = 0;
    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EUNKNOWN);
    assert(allocation == NULL && allocator->ActiveCount == 0);
    g_failAppendAfter = -1;
    allocation = __Allocate(allocator, request, 0);
    saved = allocation;
    assert(DMAIOVAAllocate(allocator, &request, &allocation) == OS_EBUSY && allocation == saved);
    __Release(&allocation);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
}

static void
__TestArithmetic(void)
{
    const uint64_t high = UINT64_C(1) << 63;
    struct __Case {
        DMAIOVARange_t   Bounds;
        uint64_t         Granularity;
        DMAIOVARequest_t Request;
        bool             Fits;
        uint64_t         First;
    } cases[] = {
        { { 0, 255 }, 1, { 1, 0, 0, 0 }, true, 0 },
        { { 16, 255 }, 16, { 16, 0, 0, 15 }, false, 0 },
        { { 16, 255 }, 16, { 16, 0, 0, 30 }, false, 0 },
        { { 16, 255 }, 16, { 16, 1, 0, 31 }, true, 16 },
        { { 1, 255 }, 1, { 8, 16, 0, 255 }, true, 16 },
        { { 15, 255 }, 1, { 8, 0, 16, 255 }, true, 16 },
        { { 17, 255 }, 1, { 8, 64, 16, 255 }, true, 64 },
        { { 0, 255 }, 16, { 16, 0, 8, 255 }, false, 0 },
        { { 0, 255 }, 1, { 17, 0, 16, 255 }, false, 0 },
        { { UINT64_C(0xfffffff0), UINT64_C(0x1000000ff) }, 16,
            { 32, 0, 0, UINT64_MAX }, true, UINT64_C(0xfffffff0) },
        { { UINT64_C(0x100000000), UINT64_C(0x100000fff) }, 4096,
            { 4096, 0, 0, UINT64_MAX }, true, UINT64_C(0x100000000) },
        { { UINT64_MAX - 15, UINT64_MAX }, 16, { 16, 0, 0, UINT64_MAX }, true, UINT64_MAX - 15 },
        { { UINT64_MAX, UINT64_MAX }, 1, { 1, 0, 0, UINT64_MAX }, true, UINT64_MAX },
        { { UINT64_MAX, UINT64_MAX }, 1, { 1, 2, 0, UINT64_MAX }, false, 0 },
        { { UINT64_MAX, UINT64_MAX }, 1, { 2, 0, high, UINT64_MAX }, false, 0 },
        { { high + 1, UINT64_MAX }, 1, { 1, high, 0, UINT64_MAX }, false, 0 },
        { { high - 1, UINT64_MAX }, 1, { 2, 0, high, UINT64_MAX }, true, high },
        { { 0, UINT64_MAX }, high, { high, high, high, UINT64_MAX }, true, 0 },
        { { high, UINT64_MAX }, high, { high, 0, 0, UINT64_MAX }, true, high },
        { { 0, UINT64_MAX }, 1, { UINT64_MAX, 0, 0, UINT64_MAX }, true, 0 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        DMAIOVAAllocator_t* allocator = __Create((DMAIOVAConfig_t){ cases[i].Bounds, cases[i].Granularity, NULL, 0 });

        if (cases[i].Fits) {
            DMAIOVAAllocation_t* allocation = __Allocate(allocator, cases[i].Request, cases[i].First);

            __Release(&allocation);
        } else {
            __NoFit(allocator, cases[i].Request);
        }
        assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
    }

    // Occupy the entire 64-bit space with two representable lengths. This
    // checks ordering of high keys, the final byte, and reuse after removal.
    {
        DMAIOVAAllocator_t*   allocator = __Create((DMAIOVAConfig_t){ { 0, UINT64_MAX }, 1, NULL, 0 });
        DMAIOVAAllocation_t*  first = __Allocate(allocator, (DMAIOVARequest_t){ UINT64_MAX, 0, 0, UINT64_MAX }, 0);
        const DMAIOVARange_t* range = DMAIOVAAllocationRange(first);
        DMAIOVAAllocation_t*  last = __Allocate(allocator, (DMAIOVARequest_t){ 1, 0, 0, UINT64_MAX }, UINT64_MAX);

        assert(range == DMAIOVAAllocationRange(first) && range->First == 0 && range->Last == UINT64_MAX - 1);
        __NoFit(allocator, (DMAIOVARequest_t){ 1, 0, 0, UINT64_MAX });
        __Release(&first);
        first = __Allocate(allocator, (DMAIOVARequest_t){ high, high, high, UINT64_MAX }, 0);
        __Release(&last);
        last = __Allocate(allocator, (DMAIOVARequest_t){ high, high, high, UINT64_MAX }, high);
        __Release(&first);
        __Release(&last);
        assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
    }
}

static void
__TestLifetime(void)
{
    DMAIOVARange_t       fixed = { 4, 7 };
    DMAIOVAAllocator_t*  allocator = __Create((DMAIOVAConfig_t){ { 0, 15 }, 1, &fixed, 1 });
    DMAIOVAAllocator_t*  other = __Create((DMAIOVAConfig_t){ { 0, 15 }, 1, NULL, 0 });
    DMAIOVAAllocator_t*  saved = allocator;
    DMAIOVAAllocation_t* first = __Allocate(allocator, (DMAIOVARequest_t){ 4, 0, 0, 15 }, 0);
    DMAIOVAAllocation_t* second = __Allocate(allocator, (DMAIOVARequest_t){ 8, 0, 0, 15 }, 8);
    DMAIOVAAllocation_t* independent = __Allocate(other, (DMAIOVARequest_t){ 4, 0, 0, 15 }, 0);
    DMAIOVAAllocation_t  fake = *first;
    DMAIOVAAllocation_t* mismatched = &fake;
    DMAIOVAAllocation_t* empty = NULL;
    size_t               heapCalls;

    assert(DMAIOVAAllocationRelease(NULL) == OS_EOK);
    assert(DMAIOVAAllocationRelease(&empty) == OS_EOK);
    assert(DMAIOVAAllocatorDestroy(NULL) == OS_EOK);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EBUSY && allocator == saved);
    assert(DMAIOVAAllocationRelease(&mismatched) == OS_EUNKNOWN && mismatched == &fake);
    // Model incomplete invalidation by keeping first live. No allocator API
    // can tell whether hardware completed. Only release permits address reuse.
    g_failRemove = true;
    assert(DMAIOVAAllocationRelease(&first) == OS_EUNKNOWN && first != NULL);
    g_failRemove = false;
    __NoFit(allocator, (DMAIOVARequest_t){ 1, 0, 0, 15 });
    g_failHeap = true;
    heapCalls = atomic_load(&g_heapCalls);
    __Release(&second);
    assert(atomic_load(&g_heapCalls) == heapCalls);
    g_failHeap = false;
    second = __Allocate(allocator, (DMAIOVARequest_t){ 8, 0, 0, 15 }, 8);
    __Release(&first);
    first = __Allocate(allocator, (DMAIOVARequest_t){ 4, 0, 0, 15 }, 0);
    __Release(&first);
    __Release(&second);
    __Release(&independent);
    g_failHeap = true;
    heapCalls = atomic_load(&g_heapCalls);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK && allocator == NULL);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
    assert(DMAIOVAAllocatorDestroy(&other) == OS_EOK && other == NULL);
    assert(atomic_load(&g_heapCalls) == heapCalls);
    g_failHeap = false;

    fixed = (DMAIOVARange_t){ 0, UINT64_MAX };
    allocator = __Create((DMAIOVAConfig_t){ fixed, 1, &fixed, 1 });
    g_failHeap = true;
    assert(DMAIOVAAllocate(allocator, &(DMAIOVARequest_t){ 1, 0, 0, 0 }, &empty) == OS_EOOM);
    g_failHeap = false;
    __NoFit(allocator, (DMAIOVARequest_t){ 1, 0, 0, UINT64_MAX });
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
}

// Verify the production tree's ordering, parent links and red-black balance.
// The caller is single-threaded here. Interval contents remain opaque to drivers.
static unsigned int
__CheckNode(rb_tree_t* tree, rb_leaf_t* node, size_t* count, uint64_t* last)
{
    unsigned int              left;
    unsigned int              right;
    struct __DMAIOVAInterval* interval;

    if (node == &tree->nil) {
        return 1;
    }
    assert(node != NULL && node->left != NULL && node->right != NULL);
    if (node->left != &tree->nil) {
        assert(node->left->parent == node);
    }
    if (node->right != &tree->nil) {
        assert(node->right->parent == node);
    }
    assert(node->color == 0 || node->color == 1);
    if (node->color == 1) {
        assert(node->left->color == 0 && node->right->color == 0);
    }
    left = __CheckNode(tree, node->left, count, last);
    interval = node->value;
    assert(node->key == &interval->Range.First);
    assert(interval->Range.First <= interval->Range.Last);
    assert(*count == 0 || *last < interval->Range.First);
    *last = interval->Range.Last;
    (*count)++;
    right = __CheckNode(tree, node->right, count, last);
    assert(left == right);
    return left + (node->color == 0);
}

static void
__CheckTree(DMAIOVAAllocator_t* allocator, size_t fixedCount)
{
    size_t     count = 0;
    uint64_t   last = 0;
    rb_tree_t* tree = &allocator->Intervals;

    assert(tree->root->color == 0 && tree->nil.color == 0);
    if (tree->root != &tree->nil) {
        assert(tree->root->parent == &tree->nil);
    }
    __CheckNode(tree, tree->root, &count, &last);
    assert(count == fixedCount + allocator->ActiveCount);
}

static uint32_t
__Random(uint32_t* state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int
__Oracle(const bool* used, uint64_t granule, const DMAIOVARequest_t* request)
{
    uint64_t alignment = request->Alignment > granule ? request->Alignment : granule;

    for (unsigned int first = 0; first < 256; first++) {
        uint64_t last = first + request->Length - 1;
        bool     available = true;

        if (first % alignment != 0 || last >= 256 || last > request->Limit ||
            (request->Boundary != 0 && first / request->Boundary != last / request->Boundary)) {
            continue;
        }
        for (unsigned int i = first; i <= last; i++) {
            if (used[i]) {
                available = false;
                break;
            }
        }
        if (available) {
            return (int)first;
        }
    }
    return -1;
}

static void
__Mark(bool* used, const DMAIOVARange_t* range, bool value)
{
    for (uint64_t i = range->First; i <= range->Last; i++) {
        assert(used[i] != value);
        used[i] = value;
    }
}

static void
__TestRandom(uint32_t seed, uint64_t granule)
{
    DMAIOVARange_t       fixed[] = { { 32, 47 }, { 128, 143 } };
    DMAIOVAAllocator_t*  allocator = __Create((DMAIOVAConfig_t){ { 0, 255 }, granule, fixed, 2 });
    DMAIOVAAllocation_t* slots[64] = { 0 };
    bool                 used[256] = { 0 };

    __Mark(used, &fixed[0], true);
    __Mark(used, &fixed[1], true);
    for (unsigned int operation = 0; operation < 12000; operation++) {
        unsigned int slot = __Random(&seed) % 64;

        if (slots[slot] != NULL) {
            __Mark(used, DMAIOVAAllocationRange(slots[slot]), false);
            __Release(&slots[slot]);
        } else {
            DMAIOVARequest_t request = {
                (1 + __Random(&seed) % 16) * granule,
                (__Random(&seed) % 2) ? 0 : UINT64_C(1) << (__Random(&seed) % 8),
                (__Random(&seed) % 2) ? 0 : UINT64_C(1) << (__Random(&seed) % 8),
                __Random(&seed) % 300
            };
            int expected = __Oracle(used, granule, &request);

            if (expected < 0) {
                __NoFit(allocator, request);
            } else {
                slots[slot] = __Allocate(allocator, request, (uint64_t)expected);
                __Mark(used, DMAIOVAAllocationRange(slots[slot]), true);
            }
        }
        __CheckTree(allocator, 2);
    }
    for (size_t i = 0; i < 64; i++) {
        __Release(&slots[i]);
    }
    __CheckTree(allocator, 2);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
}

static void
__TestRemovalOrder(void)
{
    DMAIOVAAllocator_t*  allocator = __Create((DMAIOVAConfig_t){ { 0, 30 }, 1, NULL, 0 });
    DMAIOVAAllocation_t* slots[31] = { 0 };

    for (size_t i = 0; i < 31; i++) {
        slots[i] = __Allocate(allocator, (DMAIOVARequest_t){ 1, 0, 0, 30 }, i);
    }
    // Repeatedly remove the root, including nodes with two children and the
    // final node. Indices follow addresses, independent of the tree shape.
    for (size_t i = 0; i < 31; i++) {
        uint64_t first = *(uint64_t*)allocator->Intervals.root->key;

        __Release(&slots[first]);
        __CheckTree(allocator, 0);
    }
    slots[0] = __Allocate(allocator, (DMAIOVARequest_t){ 31, 0, 0, 30 }, 0);
    __Release(&slots[0]);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
}

struct __Concurrent {
    DMAIOVAAllocator_t* Allocator;
    pthread_mutex_t     Lock;
    bool                Used[256];
};

static void*
__AllocationThread(void* argument)
{
    struct __Concurrent* fixture = argument;

    for (unsigned int i = 0; i < 2000; i++) {
        DMAIOVARequest_t     request = { 1 + i % 8, 0, 0, 255 };
        DMAIOVAAllocation_t* allocation = NULL;
        DMAIOVARange_t       range;

        assert(DMAIOVAAllocate(fixture->Allocator, &request, &allocation) == OS_EOK);
        range = *DMAIOVAAllocationRange(allocation);
        assert(pthread_mutex_lock(&fixture->Lock) == 0);
        __Mark(fixture->Used, &range, true);
        assert(pthread_mutex_unlock(&fixture->Lock) == 0);
        sched_yield();
        // Keep oracle removal and actual release together. A newly allocated
        // interval cannot be registered before the old entry is removed.
        assert(pthread_mutex_lock(&fixture->Lock) == 0);
        __Mark(fixture->Used, &range, false);
        __Release(&allocation);
        assert(pthread_mutex_unlock(&fixture->Lock) == 0);
    }
    return NULL;
}

static void
__TestConcurrency(void)
{
    DMAIOVARange_t fixed = { 64, 127 };
    struct __Concurrent fixture = {
        .Allocator = __Create((DMAIOVAConfig_t){ { 0, 255 }, 1, &fixed, 1 })
    };
    pthread_t threads[4];

    assert(pthread_mutex_init(&fixture.Lock, NULL) == 0);
    __Mark(fixture.Used, &fixed, true);
    for (size_t i = 0; i < 4; i++) {
        assert(pthread_create(&threads[i], NULL, __AllocationThread, &fixture) == 0);
    }
    for (size_t i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    __CheckTree(fixture.Allocator, 1);
    __Mark(fixture.Used, &fixed, false);
    for (size_t i = 0; i < 256; i++) {
        assert(!fixture.Used[i]);
    }
    assert(DMAIOVAAllocatorDestroy(&fixture.Allocator) == OS_EOK);
    assert(pthread_mutex_destroy(&fixture.Lock) == 0);
}

struct __ReleaseBatch {
    pthread_barrier_t    Start;
    DMAIOVAAllocation_t* Allocations[32];
};

struct __ReleaseThread {
    struct __ReleaseBatch* Batch;
    unsigned int           Index;
};

static void*
__ReleaseThread(void* argument)
{
    struct __ReleaseThread* thread = argument;
    int                     status;

    status = pthread_barrier_wait(&thread->Batch->Start);
    assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned int i = thread->Index; i < 32; i += 4) {
        __Release(&thread->Batch->Allocations[i]);
    }
    return NULL;
}

static void
__TestConcurrentRelease(void)
{
    DMAIOVAAllocator_t*    allocator = __Create((DMAIOVAConfig_t){ { 0, 255 }, 1, NULL, 0 });
    struct __ReleaseBatch  batch;
    struct __ReleaseThread arguments[4];
    pthread_t              threads[4];

    // The mixed-operation fixture serializes releases with its oracle mutex.
    // Here distinct releases start together and share only the allocator lock.
    assert(pthread_barrier_init(&batch.Start, NULL, 4) == 0);
    for (size_t i = 0; i < 32; i++) {
        batch.Allocations[i] = __Allocate(allocator, (DMAIOVARequest_t){ 8, 0, 0, 255 }, i * 8);
    }
    for (unsigned int i = 0; i < 4; i++) {
        arguments[i] = (struct __ReleaseThread){ &batch, i };
        assert(pthread_create(&threads[i], NULL, __ReleaseThread, &arguments[i]) == 0);
    }
    for (size_t i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    __CheckTree(allocator, 0);
    batch.Allocations[0] = __Allocate(allocator, (DMAIOVARequest_t){ 256, 0, 0, 255 }, 0);
    __Release(&batch.Allocations[0]);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
    assert(pthread_barrier_destroy(&batch.Start) == 0);
}

static void
__TestFragmentedSearch(void)
{
    DMAIOVARange_t       fixed[512];
    DMAIOVAAllocator_t*  allocator;
    DMAIOVAAllocation_t* allocation;
    size_t               treeCalls;

    for (size_t i = 0; i < 512; i++) {
        fixed[i] = (DMAIOVARange_t){ i * 2 + 1, i * 2 + 1 };
    }
    allocator = __Create((DMAIOVAConfig_t){ { 0, 1025 }, 1, fixed, 512 });
    treeCalls = g_treeCalls;
    allocation = __Allocate(allocator, (DMAIOVARequest_t){ 2, 0, 0, 1025 }, 1024);
    printf("IOVA fragmented search: 512 fixed intervals, %zu tree calls\n", g_treeCalls - treeCalls);
    __Release(&allocation);
    assert(DMAIOVAAllocatorDestroy(&allocator) == OS_EOK);
}

int main(void)
{
    __TestCreation();
    __TestRequests();
    __TestArithmetic();
    __TestLifetime();
    __TestRandom(0x12345678, 1);
    __TestRandom(0x24681357, 4);
    __TestRandom(0xdeadbeef, 16);
    __TestRemovalOrder();
    __TestConcurrency();
    __TestConcurrentRelease();
    __TestFragmentedSearch();
    assert(g_errors == 6);
    assert(atomic_load(&g_liveMetadata) == 0);
    puts("DMA IOVA tests passed");
    return 0;
}

