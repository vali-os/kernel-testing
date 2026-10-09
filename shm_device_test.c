/**
 * Exercise the public kernel interface against the real SHM device context.
 * Only the heap is replaced, so failures and outstanding allocations can be
 * checked without booting a kernel or enabling a device.
 */
#include <shm_device.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint g_liveAllocations;
static unsigned int g_allocationCalls;
static bool g_failAllocation;

void*
kmalloc(
    _In_ size_t size)
{
    void* allocation;

    // Count attempts as well as successes to prove validation precedes storage.
    g_allocationCalls++;
    if (g_failAllocation) {
        return NULL;
    }
    allocation = malloc(size);
    assert(allocation != NULL);
    atomic_fetch_add(&g_liveAllocations, 1);
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    // Track actual destruction to catch both leaks and premature final release.
    assert(allocation != NULL);
    assert(atomic_fetch_sub(&g_liveAllocations, 1) > 0);
    free(allocation);
}

static void
__ExpectTranslation(
    _In_ const struct SHMDeviceContext* context,
    _In_ uint64_t                       physical,
    _In_ uint64_t                       length,
    _In_ uint64_t                       limit,
    _In_ oserr_t                        expectedStatus,
    _In_ uint64_t                       expectedAddress)
{
    uint64_t address = 0xfeed;

    // Failed lookups must not leave a plausible but incomplete device address.
    assert(SHMDeviceContextTranslate(context, physical, length, limit, &address) == expectedStatus);
    assert(address == (expectedStatus == OS_EOK ? expectedAddress : 0xfeed));
}

static void
__TestTranslation(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x1000, 0x2000 },
        { 0x702000, 0x3000, 0x1000 },
        { 0x704000, 0x5000, 0x1000 }
    };
    struct SHMDeviceContext* context = NULL;

    // Include touching ranges and a physical hole: a fitting first byte is
    // insufficient, even if a later range would cover the rest of the request.
    assert(SHMDeviceContextCreate(ranges, 3, SHMDeviceCacheNonCoherent, &context) == OS_EOK);
    __ExpectTranslation(context, 0x700300, 0x100, UINT64_MAX, OS_EOK, 0x1300);
    __ExpectTranslation(context, 0x700000, 0x2000, 0x2fff, OS_EOK, 0x1000);
    __ExpectTranslation(context, 0x701fff, 1, 0x2fff, OS_EOK, 0x2fff);
    __ExpectTranslation(context, 0x701fff, 2, UINT64_MAX, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x700000, 0x2001, UINT64_MAX, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x700300, 0x100, 0x13fe, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x700300, 0x100, 0x13ff, OS_EOK, 0x1300);
    __ExpectTranslation(context, 0x700300, 1, 0x12ff, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x6fffff, 2, UINT64_MAX, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x703000, 1, UINT64_MAX, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x705000, 1, UINT64_MAX, OS_ENOENT, 0);
    __ExpectTranslation(context, 0x704fff, 1, UINT64_MAX, OS_EOK, 0x5fff);
    __ExpectTranslation(context, 0x700000, 0, UINT64_MAX, OS_EINVALPARAMS, 0);
    __ExpectTranslation(context, UINT64_MAX, 2, UINT64_MAX, OS_EINVALPARAMS, 0);
    __ExpectTranslation(NULL, 0, 1, UINT64_MAX, OS_EINVALPARAMS, 0);
    assert(SHMDeviceContextTranslate(context, 0, 1, UINT64_MAX, NULL) == OS_EINVALPARAMS);
    SHMDeviceContextRelease(&context);
}

static void
__TestAliases(void)
{
    struct SHMDeviceRange ranges[] = {
        { 0x700000, 0x100000000ULL, 0x2000 },
        { 0x700000, 0x1000, 0x1000 },
        { 0x700800, 0x8000, 0x1000 }
    };
    struct SHMDeviceRange reversed[3];
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceContext* other = NULL;
    unsigned int i;

    // A 32-bit controller can use the low alias even when a high alias appears
    // first. A short low alias must not hide another alias covering more bytes.
    assert(SHMDeviceContextCreate(ranges, 3, SHMDeviceCacheCoherent, &context) == OS_EOK);
    for (i = 0; i < 3; i++) {
        reversed[i] = ranges[2 - i];
    }
    assert(SHMDeviceContextCreate(reversed, 3, SHMDeviceCacheCoherent, &other) == OS_EOK);
    __ExpectTranslation(context, 0x700300, 0x100, UINT32_MAX, OS_EOK, 0x1300);
    __ExpectTranslation(other, 0x700300, 0x100, UINT64_MAX, OS_EOK, 0x1300);
    __ExpectTranslation(context, 0x700800, 0x1000, UINT32_MAX, OS_EOK, 0x8000);
    __ExpectTranslation(other, 0x700800, 0x1000, UINT32_MAX, OS_EOK, 0x8000);
    __ExpectTranslation(context, 0x700000, 0x2000, UINT64_MAX, OS_EOK, 0x100000000ULL);
    __ExpectTranslation(context, 0x700000, 0x2000, UINT32_MAX, OS_ENOENT, 0);
    SHMDeviceContextRelease(&context);
    SHMDeviceContextRelease(&other);
}

static void
__TestAddressEdges(void)
{
    struct SHMDeviceRange ranges[] = {
        { UINT64_MAX - 15, UINT64_MAX - 15, 16 },
        { 0, 0, 1 }
    };
    struct SHMDeviceContext* context = NULL;

    // Inclusive end checks must accept the final representable byte and zero.
    assert(SHMDeviceContextCreate(ranges, 2, SHMDeviceCacheCoherent, &context) == OS_EOK);
    __ExpectTranslation(context, UINT64_MAX - 15, 16, UINT64_MAX, OS_EOK, UINT64_MAX - 15);
    __ExpectTranslation(context, UINT64_MAX, 1, UINT64_MAX, OS_EOK, UINT64_MAX);
    __ExpectTranslation(context, UINT64_MAX, 1, UINT64_MAX - 1, OS_ENOENT, 0);
    __ExpectTranslation(context, 0, 1, 0, OS_EOK, 0);
    __ExpectTranslation(context, 0, UINT64_MAX, UINT64_MAX, OS_ENOENT, 0);
    SHMDeviceContextRelease(&context);
}

static void
__TestCreationFailures(void)
{
    const struct SHMDeviceRange invalid[][2] = {
        { { 0, 0, 0 }, { 0, 10, 1 } },
        { { UINT64_MAX, 0, 2 }, { 0, 10, 1 } },
        { { 0, UINT64_MAX, 2 }, { 0, 10, 1 } },
        { { 0, 10, 10 }, { 100, 19, 10 } },
        { { 100, 19, 10 }, { 0, 10, 10 } },
        { { 0, 10, 10 }, { 100, 10, 10 } },
        { { 0, 10, 10 }, { 100, 12, 1 } },
        { { 0, 12, 1 }, { 100, 10, 10 } }
    };
    struct SHMDeviceRange valid = { 0, 0, 1 };
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceContext* saved;
    unsigned int calls = g_allocationCalls;
    size_t i;

    // Reject malformed descriptions before allocation, including an oversized
    // count with only one accessible entry (the count must be checked first).
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(SHMDeviceContextCreate(invalid[i], 2, SHMDeviceCacheCoherent, &context) == OS_EINVALPARAMS);
        assert(context == NULL);
    }
    assert(SHMDeviceContextCreate(NULL, 1, SHMDeviceCacheCoherent, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 0, SHMDeviceCacheCoherent, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 65, SHMDeviceCacheCoherent, &context) == OS_ENOTSUPPORTED);
    assert(SHMDeviceContextCreate(&valid, UINT32_MAX, SHMDeviceCacheCoherent, &context) == OS_ENOTSUPPORTED);
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheUnknown, &context) == OS_EINVALPARAMS);
    assert(SHMDeviceContextCreate(&valid, 1, (enum SHMDeviceCachePolicy)99, &context) == OS_EINVALPARAMS);
    assert(context == NULL && g_allocationCalls == calls);

    // Allocation failure leaves no owner behind and permits a later retry.
    g_failAllocation = true;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, &context) == OS_EOOM);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 0);
    g_failAllocation = false;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, &context) == OS_EOK);
    saved = context;
    assert(SHMDeviceContextCreate(&valid, 1, SHMDeviceCacheCoherent, &context) == OS_EBUSY);
    assert(context == saved);
    SHMDeviceContextRelease(&context);
}

static void
__TestCopiedStorageAndLifetime(void)
{
    struct SHMDeviceRange* ranges;
    struct SHMDeviceContext* context = NULL;
    struct SHMDeviceContext* retained;
    struct SHMDeviceContext* other = NULL;
    unsigned int i;

    // Fill the maximum map in reverse order to exercise bounded unsorted input.
    ranges = malloc(SHM_DEVICE_MAX_RANGES * sizeof(*ranges));
    assert(ranges != NULL);
    for (i = 0; i < SHM_DEVICE_MAX_RANGES; i++) {
        ranges[i] = (struct SHMDeviceRange){ i * 0x1000, (64 - i) * 0x1000, 0x1000 };
    }
    assert(SHMDeviceContextCreate(ranges, 64, SHMDeviceCacheNonCoherent, &context) == OS_EOK);
    ranges[0].DeviceBase = 0;
    assert(SHMDeviceContextCreate(ranges, 1, SHMDeviceCacheCoherent, &other) == OS_EOK);
    memset(ranges, 0, SHM_DEVICE_MAX_RANGES * sizeof(*ranges));
    free(ranges);

    // Neither source destruction nor another context's rules may change the
    // original. A retained reference must also outlive the creating owner.
    assert(SHMDeviceContextRetain(context) == OS_EOK);
    retained = context;
    SHMDeviceContextRelease(&context);
    assert(context == NULL && atomic_load(&g_liveAllocations) == 2);
    SHMDeviceContextRelease(&context);
    assert(atomic_load(&g_liveAllocations) == 2);
    __ExpectTranslation(retained, 0, 0x1000, UINT64_MAX, OS_EOK, 0x40000);
    __ExpectTranslation(retained, 63 * 0x1000, 0x1000, UINT64_MAX, OS_EOK, 0x1000);
    __ExpectTranslation(other, 0, 1, 0, OS_EOK, 0);
    assert(SHMDeviceContextGetCachePolicy(retained) == SHMDeviceCacheNonCoherent);
    assert(SHMDeviceContextGetCachePolicy(other) == SHMDeviceCacheCoherent);
    SHMDeviceContextRelease(&retained);
    assert(atomic_load(&g_liveAllocations) == 1);
    SHMDeviceContextRelease(&other);
    assert(atomic_load(&g_liveAllocations) == 0);
    SHMDeviceContextRelease(NULL);
    assert(SHMDeviceContextRetain(NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceContextGetCachePolicy(NULL) == SHMDeviceCacheUnknown);
}

static void*
__ReferenceWorker(
    _In_ void* argument)
{
    struct SHMDeviceContext* context = argument;
    struct SHMDeviceContext* temporary;
    unsigned int i;

    // Each worker already owns a reference, allowing safe retain/release and
    // translation while other workers and the creator drop their references.
    for (i = 0; i < 1000; i++) {
        assert(SHMDeviceContextRetain(context) == OS_EOK);
        temporary = context;
        __ExpectTranslation(temporary, 0x700300, 0x100, UINT64_MAX, OS_EOK, 0x1300);
        SHMDeviceContextRelease(&temporary);
    }
    SHMDeviceContextRelease(&context);
    return NULL;
}

static void
__TestConcurrentOwners(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x2000 };
    struct SHMDeviceContext* context = NULL;
    pthread_t workers[4];
    unsigned int i;

    // Pass actual owning references to threads, then let the creator leave.
    assert(SHMDeviceContextCreate(&range, 1, SHMDeviceCacheCoherent, &context) == OS_EOK);
    for (i = 0; i < 4; i++) {
        assert(SHMDeviceContextRetain(context) == OS_EOK);
        assert(pthread_create(&workers[i], NULL, __ReferenceWorker, context) == 0);
    }
    SHMDeviceContextRelease(&context);
    for (i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(atomic_load(&g_liveAllocations) == 0);
}

int
main(void)
{
    // Keep the tests ordered so each case must finish with no live contexts.
    __TestTranslation();
    __TestAliases();
    __TestAddressEdges();
    __TestCreationFailures();
    __TestCopiedStorageAndLifetime();
    __TestConcurrentOwners();
    assert(atomic_load(&g_liveAllocations) == 0);
    puts("SHM device context: translation, validation, copied storage and lifetime passed");
    return 0;
}
