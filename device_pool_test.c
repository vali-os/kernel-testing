/**
 * Exercise the real device pool allocator. Only the spinlock is replaced, so
 * the tests also check that every allocation and free releases its lock.
 */
#include <device_pool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int g_lockHeld;

void
SpinlockConstruct(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
    g_lockHeld = 0;
}

void
SpinlockAcquireIrq(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
    assert(g_lockHeld == 0);
    g_lockHeld = 1;
}

void
SpinlockReleaseIrq(
    _In_ Spinlock_t* spinlock)
{
    (void)spinlock;
    assert(g_lockHeld == 1);
    g_lockHeld = 0;
}

static uint32_t g_bitmap[4];

static struct DevicePool
__CreatePool(
    _In_ paddr_t base,
    _In_ size_t  pageCount)
{
    struct DevicePool pool;

    assert(DEVICE_POOL_BITMAP_BYTES(pageCount) <= sizeof(g_bitmap));
    assert(DevicePoolConstruct(&pool, base, pageCount * 0x1000, 0x1000, g_bitmap) == OS_EOK);
    return pool;
}

static void
__ExpectAllocation(
    _In_ struct DevicePool* pool,
    _In_ size_t             length,
    _In_ size_t             alignment,
    _In_ size_t             boundary,
    _In_ oserr_t            expectedStatus,
    _In_ paddr_t            expectedBase)
{
    paddr_t base = 0xfeed;

    // A failed allocation must not hand out a plausible address, and the lock
    // must be released on every path.
    assert(DevicePoolAllocate(pool, length, alignment, boundary, &base) == expectedStatus);
    assert(base == (expectedStatus == OS_EOK ? expectedBase : 0xfeed));
    assert(g_lockHeld == 0);
}

static void
__TestConstruct(void)
{
    struct DevicePool pool;
    paddr_t           lastPage = (paddr_t)~(paddr_t)0 & ~(paddr_t)0xfff;

    // The pool must sit on page boundaries and inside the physical address
    // space; a pool ending exactly at the last address is fine.
    assert(DevicePoolConstruct(NULL, 0, 0x1000, 0x1000, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0, 0x1000, 0x1000, NULL) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0, 0x1000, 0, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0, 0x1000, 0x1800, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0x800, 0x1000, 0x1000, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0, 0, 0x1000, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, 0, 0x1800, 0x1000, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, lastPage, 0x2000, 0x1000, g_bitmap) == OS_EINVALPARAMS);
    assert(DevicePoolConstruct(&pool, lastPage, 0x1000, 0x1000, g_bitmap) == OS_EOK);
    __ExpectAllocation(&pool, 1, 0, 0, OS_EOK, lastPage);
}

static void
__TestFirstFit(void)
{
    struct DevicePool pool = __CreatePool(0x700000, 16);

    // Pages are handed out in order, freed pages are reused first, and a
    // length is rounded up to whole pages.
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOK, 0x700000);
    __ExpectAllocation(&pool, 1, 0, 0, OS_EOK, 0x701000);
    assert(DevicePoolFree(&pool, 0x700000, 0x1000) == OS_EOK);
    __ExpectAllocation(&pool, 0x800, 0, 0, OS_EOK, 0x700000);

    // A run of neighbouring pages skips holes too small to hold it.
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOK, 0x702000);
    assert(DevicePoolFree(&pool, 0x701000, 0x1000) == OS_EOK);
    __ExpectAllocation(&pool, 0x2800, 0, 0, OS_EOK, 0x703000);
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOK, 0x701000);
    assert(g_lockHeld == 0);
}

static void
__TestAlignmentAndBoundary(void)
{
    struct DevicePool pool = __CreatePool(0x701000, 16);

    // The pool does not start on a 16 KiB boundary, so the first aligned run
    // starts three pages in. Alignments up to a page are met by every page.
    __ExpectAllocation(&pool, 0x1000, 0x4000, 0, OS_EOK, 0x704000);
    __ExpectAllocation(&pool, 0x1000, 0x100, 0, OS_EOK, 0x701000);

    // Two pages that must not cross an 8 KiB boundary: the run from 0x702000
    // stays inside one, but the run from 0x705000 would cross 0x706000, so
    // the next such request skips it. The skipped page stays free.
    __ExpectAllocation(&pool, 0x2000, 0, 0x2000, OS_EOK, 0x702000);
    __ExpectAllocation(&pool, 0x2000, 0, 0x2000, OS_EOK, 0x706000);
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOK, 0x705000);

    // A boundary smaller than a page only limits the requested bytes.
    __ExpectAllocation(&pool, 0x800, 0, 0x800, OS_EOK, 0x708000);
}

static void
__TestInvalidAndFull(void)
{
    struct DevicePool pool = __CreatePool(0x700000, 4);
    paddr_t           base;

    // Requests no memory could meet are refused as mistakes, not shortages.
    __ExpectAllocation(&pool, 0, 0, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&pool, 0x1000, 3, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&pool, 0x1000, 0, 0x3000, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&pool, 0x2000, 0, 0x1000, OS_EINVALPARAMS, 0);
    __ExpectAllocation(&pool, SIZE_MAX, 0, 0, OS_EINVALPARAMS, 0);
    __ExpectAllocation(NULL, 0x1000, 0, 0, OS_EINVALPARAMS, 0);
    assert(DevicePoolAllocate(&pool, 0x1000, 0, 0, NULL) == OS_EINVALPARAMS);

    // A full pool, or a request larger than the pool, fails without falling
    // back to other memory; an alignment no page in the pool meets does too.
    __ExpectAllocation(&pool, 0x5000, 0, 0, OS_EOOM, 0);
    __ExpectAllocation(&pool, 0x1000, 0x200000, 0, OS_EOOM, 0);
    assert(DevicePoolAllocate(&pool, 0x4000, 0, 0, &base) == OS_EOK && base == 0x700000);
    __ExpectAllocation(&pool, 1, 0, 0, OS_EOOM, 0);

    // Freeing outside the pool, off a page, past its end, or twice is refused
    // and changes nothing.
    assert(DevicePoolFree(&pool, 0x6ff000, 0x1000) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x700800, 0x1000) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x703000, 0x2000) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x700000, 0) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x700000, SIZE_MAX) == OS_EINVALPARAMS);
    assert(DevicePoolFree(NULL, 0x700000, 0x1000) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x701000, 0x1000) == OS_EOK);
    assert(DevicePoolFree(&pool, 0x701000, 0x1000) == OS_EINVALPARAMS);
    assert(DevicePoolFree(&pool, 0x700000, 0x2000) == OS_EINVALPARAMS);
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOK, 0x701000);
    __ExpectAllocation(&pool, 0x1000, 0, 0, OS_EOOM, 0);
    assert(g_lockHeld == 0);
}

int
main(void)
{
    __TestConstruct();
    __TestFirstFit();
    __TestAlignmentAndBoundary();
    __TestInvalidAndFull();
    puts("Device pool: construction, first fit, alignment, boundaries and frees passed");
    return 0;
}
