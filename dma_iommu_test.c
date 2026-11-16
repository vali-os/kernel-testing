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

#include "dma_iommu_sim.h"
#include "../kernel/memory/dma/private.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <spinlock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define __GRANULE 4096ULL
#define __TIMEOUT 100ULL

static atomic_size_t             g_liveAllocations;
static atomic_size_t             g_heapCalls;
static atomic_uint               g_liveMutexes;
static _Thread_local int         g_failHeapAfter = -1;
static _Thread_local bool        g_noHeap;
static _Thread_local Mutex_t*    g_lockedMutex;
static _Thread_local Spinlock_t* g_lockedTree;
static _Thread_local bool        g_failIOVARelease;
static DMAIOMMUBackendOps_t       g_originalOps;
static DMAIOMMUUnit_t*            g_faultUnit;
static bool                      g_failCreateAfter;
static bool                      g_failPrepareAfter;
static bool                      g_denyWriteOnly;
static bool                      g_pauseInstall;
static bool                      g_failRemoveAfter;
static atomic_uint               g_installCalls;
static atomic_uint               g_removeCalls;
static pthread_barrier_t         g_installReady;
static pthread_barrier_t         g_installContinue;

// Terminal failures intentionally keep resources alive until process exit.
// Child fixtures stay reachable here and assert the exact surviving records.
static DMAIOMMUSim_t*             g_failedSim;
static struct DMAAddressMapping* g_failedMappings[2];

void* kmalloc(size_t size)
{
    void* allocation;

    assert(!g_noHeap && g_lockedTree == NULL);
    atomic_fetch_add(&g_heapCalls, 1);
    if (g_failHeapAfter == 0) {
        g_failHeapAfter = -1;
        return NULL;
    }
    if (g_failHeapAfter > 0) {
        g_failHeapAfter--;
    }
    allocation = malloc(size);
    assert(allocation != NULL);
    atomic_fetch_add(&g_liveAllocations, 1);
    return allocation;
}

void kfree(void* allocation)
{
    assert(allocation != NULL && g_lockedTree == NULL);
    assert(atomic_fetch_sub(&g_liveAllocations, 1) != 0);
    free(allocation);
}

void MutexConstruct(Mutex_t* mutex, unsigned int flags)
{
    assert(flags == MUTEX_FLAG_PLAIN);
    assert(pthread_mutex_init(&mutex->Native, NULL) == 0);
    atomic_fetch_add(&g_liveMutexes, 1);
}

void MutexDestruct(Mutex_t* mutex)
{
    assert(g_lockedMutex == NULL);
    assert(pthread_mutex_destroy(&mutex->Native) == 0);
    assert(atomic_fetch_sub(&g_liveMutexes, 1) != 0);
}

void MutexLock(Mutex_t* mutex)
{
    // IOVA and backend locks must never nest, including during rollback.
    assert(g_lockedMutex == NULL && g_lockedTree == NULL);
    assert(pthread_mutex_lock(&mutex->Native) == 0);
    g_lockedMutex = mutex;
}

void MutexUnlock(Mutex_t* mutex)
{
    assert(g_lockedMutex == mutex && g_lockedTree == NULL);
    g_lockedMutex = NULL;
    assert(pthread_mutex_unlock(&mutex->Native) == 0);
}

void SpinlockConstruct(Spinlock_t* lock)
{
    atomic_init(&lock->Current, 0);
    atomic_init(&lock->Next, 0);
    lock->IrqState = 0;
}

void SpinlockAcquireIrq(Spinlock_t* lock)
{
    assert(g_lockedTree == NULL);
    while (atomic_exchange(&lock->Current, 1) != 0) {
        sched_yield();
    }
    g_lockedTree = lock;
}

void SpinlockReleaseIrq(Spinlock_t* lock)
{
    assert(g_lockedTree == lock);
    g_lockedTree = NULL;
    assert(atomic_exchange(&lock->Current, 0) == 1);
}

static void __Barrier(pthread_barrier_t* barrier)
{
    int result = pthread_barrier_wait(barrier);

    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static oserr_t __CreateSpace(void* context, void** spaceOut)
{
    oserr_t status = g_originalOps.CreateSpace(context, spaceOut);

    if (status != OS_EOK) {
        return status;
    }
    if (g_failCreateAfter) {
        DMAIOMMUUnitFail(DMAIOMMUSimUnit(context), OS_EUNKNOWN);
    }
    return OS_EOK;
}

static oserr_t __PrepareMapping(void* space, const DMAIOMMUMappingConfig_t* config, void** mappingOut)
{
    oserr_t status;

    if (g_denyWriteOnly && config->Direction == DMAFromDevice) {
        return OS_ENOTSUPPORTED;
    }
    status = g_originalOps.PrepareMapping(space, config, mappingOut);
    if (status != OS_EOK) {
        return status;
    }
    if (g_failPrepareAfter) {
        DMAIOMMUUnitFail(g_faultUnit, OS_EUNKNOWN);
    }
    return OS_EOK;
}

static oserr_t __InstallMapping(void* space, void* mapping, uint64_t timeoutNs)
{
    oserr_t status;

    atomic_fetch_add(&g_installCalls, 1);
    status = g_originalOps.InstallMapping(space, mapping, timeoutNs);
    if (status != OS_EOK) {
        return status;
    }
    if (g_pauseInstall) {
        // The simulator lock is gone, but the core still holds the unit mutex
        // and has not performed its final health check.
        __Barrier(&g_installReady);
        __Barrier(&g_installContinue);
    }
    return OS_EOK;
}

static oserr_t __RemoveMapping(void* space, void* mapping, uint64_t timeoutNs)
{
    oserr_t status;

    atomic_fetch_add(&g_removeCalls, 1);
    status = g_originalOps.RemoveMapping(space, mapping, timeoutNs);
    if (status != OS_EOK) {
        return status;
    }
    if (g_failRemoveAfter) {
        DMAIOMMUUnitFail(g_faultUnit, OS_EUNKNOWN);
    }
    return OS_EOK;
}

oserr_t __real_DMAIOMMUUnitCreate(const DMAIOMMUUnitConfig_t*, const DMAIOMMUBackendOps_t*,
                                 void*, DMAIOMMUUnit_t**);
oserr_t __wrap_DMAIOMMUUnitCreate(const DMAIOMMUUnitConfig_t* config, const DMAIOMMUBackendOps_t* ops,
                                 void* context, DMAIOMMUUnit_t** unitOut)
{
    DMAIOMMUBackendOps_t wrapped;

    // Invalid API tests must reach the real callback-validation checks.
    if (ops == NULL || ops->CreateSpace == NULL || ops->DestroySpace == NULL ||
        ops->PrepareMapping == NULL || ops->InstallMapping == NULL ||
        ops->RemoveMapping == NULL || ops->DestroyMapping == NULL) {
        return __real_DMAIOMMUUnitCreate(config, ops, context, unitOut);
    }
    g_originalOps = *ops;
    wrapped = *ops;
    wrapped.CreateSpace = __CreateSpace;
    wrapped.PrepareMapping = __PrepareMapping;
    wrapped.InstallMapping = __InstallMapping;
    wrapped.RemoveMapping = __RemoveMapping;
    return __real_DMAIOMMUUnitCreate(config, &wrapped, context, unitOut);
}

oserr_t __real_DMAIOVAAllocationRelease(DMAIOVAAllocation_t**);
oserr_t __wrap_DMAIOVAAllocationRelease(DMAIOVAAllocation_t** allocation)
{
    if (g_failIOVARelease && allocation != NULL && *allocation != NULL) {
        return OS_EUNKNOWN;
    }
    return __real_DMAIOVAAllocationRelease(allocation);
}

static DMAIOMMUSim_t* __Simulator(uint64_t granule, uint32_t capacity)
{
    DMAIOMMUSimConfig_t config = { { granule, UINT64_MAX, UINT64_MAX, __TIMEOUT }, capacity };
    DMAIOMMUSim_t*      sim = NULL;

    assert(DMAIOMMUSimCreate(&config, &sim) == OS_EOK);
    return sim;
}

static struct DMAAddressSpace* __Space(DMAIOMMUSim_t* sim, uint64_t first, uint64_t pages)
{
    uint64_t granule = DMAIOMMUSimUnit(sim)->Config.Granularity;
    DMAIOVAConfig_t config = { { first, first + (pages - 1) * granule + granule - 1 }, granule, NULL, 0 };
    struct DMAAddressSpace* space = NULL;

    assert(DMAAddressSpaceCreateTranslated(DMAIOMMUSimUnit(sim), &config, &space) == OS_EOK);
    assert(DMAAddressSpaceCheck(space) == OS_EOK);
    return space;
}

static struct DMAAddressMapping* __Map(struct DMAAddressSpace* space, uintptr_t physical,
                                       size_t length, enum DMADirection direction)
{
    SHMSG_t extent = { physical, length };
    struct DMAAddressMapping* mapping = NULL;

    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX, direction, NULL, &mapping) == OS_EOK);
    return mapping;
}

static uint64_t __Address(struct DMAAddressMapping* mapping)
{
    uint32_t count;
    const struct DMASegment* segments = DMAAddressMappingSegments(mapping, &count);

    assert(count == 1);
    return segments[0].Address;
}

static void __Probe(DMAIOMMUSim_t* sim, struct DMAAddressSpace* space, uint64_t address,
                    enum DMADirection access, oserr_t expected, uintptr_t physical)
{
    SHMSG_t span = { 0xabcdef, 17 };
    uint32_t count = 99;

    assert(DMAIOMMUSimAccess(sim, space, address, 1, access, &span, 1, &count) == expected);
    if (expected == OS_EOK) {
        assert(count == 1 && span.Address == physical && span.Length == 1);
    } else {
        assert(count == 99 && span.Address == 0xabcdef && span.Length == 17);
    }
}

static void __Control(DMAIOMMUSim_t* sim, enum DMAIOMMUSimFailurePoint point,
                      uint64_t after, uint64_t delay, bool hold)
{
    DMAIOMMUSimControl_t control = { point, after, OS_EINCOMPLETE, false, delay, hold };

    assert(DMAIOMMUSimSetControl(sim, &control) == OS_EOK);
}

static void __Clean(void)
{
    assert(atomic_load(&g_liveAllocations) == 0);
    assert(atomic_load(&g_liveMutexes) == 0);
    assert(g_lockedMutex == NULL && g_lockedTree == NULL);
}

static void __TestLayout(void)
{
    for (int direction = DMAToDevice; direction <= DMABidirectional; direction++) {
        DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 4);
        struct DMAAddressSpace* space = __Space(sim, 0x10000, 32);
        struct DMAAddressMapping* mapping = NULL;
        SHMSG_t extents[] = { { 0x20000, __GRANULE }, { 0x90000, __GRANULE } };
        SHMSG_t spans[2] = { { 77, 88 }, { 99, 100 } };
        uint32_t count = 55;
        DMAIOMMUSimStats_t stats;

        assert(DMAAddressMappingPrepare(space, extents, 2, UINT64_MAX, direction, NULL, &mapping) == OS_EOK);
        assert(atomic_load(&space->References) == 2);
        assert(__Address(mapping) == 0x10000);
        __Probe(sim, space, 0x10000, direction, OS_ENOENT, 0);
        extents[0].Address = 0xa0000;
        extents[1].Length = 0;
        g_noHeap = true;
        assert(DMAAddressMappingInstall(mapping) == OS_EOK);
        assert(DMAAddressMappingInstall(mapping) == OS_EBUSY);
        assert(DMAIOMMUSimAccess(sim, space, 0x10ffe, 4, direction, spans, 1, &count) == OS_EBUFFER);
        assert(count == 2 && spans[0].Address == 77 && spans[1].Address == 99);
        assert(DMAIOMMUSimAccess(sim, space, 0x10000, 8192, direction, NULL, 0, &count) == OS_EBUFFER);
        assert(count == 2);
        assert(DMAIOMMUSimAccess(sim, space, 0x10ffe, 4, direction, spans, 2, &count) == OS_EOK);
        assert(count == 2 && spans[0].Address == 0x20ffe && spans[0].Length == 2);
        assert(spans[1].Address == 0x90000 && spans[1].Length == 2);
        for (int access = DMAToDevice; access <= DMABidirectional; access++) {
            oserr_t expected = direction == DMABidirectional || direction == access ? OS_EOK : OS_EPERMISSIONS;

            __Probe(sim, space, 0x10010, access, expected, 0x20010);
        }
        assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
        assert(DMAIOMMUSimAdvance(sim, 1000) == OS_EOK);
        __Probe(sim, space, 0x10000, direction, OS_ENOENT, 0);
        DMAIOMMUSimSnapshot(sim, &stats);
        assert(stats.Mappings == 0 && stats.TablePages == 1 && stats.PendingCommands == 0);
        assert(stats.SubmittedCommands == 2 && stats.CompletedCommands == 2 && stats.CachedTranslations == 0);
        DMAAddressSpaceRelease(&space);
        assert(DMAIOMMUSimDestroy(&sim) == OS_EOK && sim == NULL);
        g_noHeap = false;
        __Clean();
    }
}

static void __TestValidation(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    DMAIOMMUUnit_t* unit = DMAIOMMUSimUnit(sim);
    DMAIOMMUUnit_t* invalidUnit = NULL;
    DMAIOMMUUnitConfig_t config = unit->Config;
    DMAIOMMUBackendOps_t missing = { 0 };
    struct DMAAddressSpace* space = __Space(sim, 0, 32);
    struct DMAAddressSpace* other = NULL;
    struct DMAAddressMapping* mapping = NULL;
    struct DMAMapRequirements requirements = { 3, 0 };
    SHMSG_t invalid[] = { { 1, 4096 }, { 4096, 0 }, { 4096, 4095 }, { UINTPTR_MAX, 4096 } };
    SHMSG_t extent = { 0x1000, __GRANULE };
    SHMSG_t overflow[] = { { 0, UINT64_C(1) << 63 }, { 0, UINT64_C(1) << 63 } };
    DMAIOVAConfig_t bounds = { { 0, 0xffff }, __GRANULE, NULL, 0 };
    uint32_t segmentCount = 77;
    size_t baseline = atomic_load(&g_liveAllocations);
    size_t calls = atomic_load(&g_heapCalls);

    assert(DMAIOMMUUnitCreate(&config, &missing, NULL, &invalidUnit) == OS_EINVALPARAMS);
    config.Granularity = 8193;
    assert(DMAIOMMUUnitCreate(&config, &g_originalOps, NULL, &invalidUnit) == OS_EINVALPARAMS);
    config = unit->Config;
    config.CompletionTimeoutNs = 0;
    assert(DMAIOMMUUnitCreate(&config, &g_originalOps, NULL, &invalidUnit) == OS_EINVALPARAMS);
    assert(invalidUnit == NULL);
    assert(DMAAddressSpaceCheck(NULL) == OS_EINVALPARAMS);
    assert(DMADirectWalkSegments(space, UINT64_MAX, &extent, 1, NULL,
                                 NULL, &segmentCount) == OS_ENOTSUPPORTED);
    assert(segmentCount == 77);
    assert(DMAAddressMappingInstall(NULL) == OS_EINVALPARAMS);
    assert(DMAAddressSpaceCreateTranslated(unit, &bounds, &space) == OS_EBUSY);
    bounds.Granularity = 65536;
    assert(DMAAddressSpaceCreateTranslated(unit, &bounds, &other) == OS_EINVALPARAMS && other == NULL);
    for (unsigned int i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(DMAAddressMappingPrepare(space, &invalid[i], 1, UINT64_MAX,
                                       DMAToDevice, NULL, &mapping) == OS_EINVALPARAMS);
    }
    assert(DMAAddressMappingPrepare(space, overflow, 2, UINT64_MAX,
                                   DMAToDevice, NULL, &mapping) == OS_EOVERFLOW);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   -1, NULL, &mapping) == OS_EINVALPARAMS);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, &requirements, &mapping) == OS_EINVALPARAMS);
    requirements = (struct DMAMapRequirements){ 0, 8192 };
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, &requirements, &mapping) == OS_ENOTSUPPORTED);
    assert(atomic_load(&g_heapCalls) == calls);
    assert(DMAAddressMappingPrepare(space, &extent, 1, 4094,
                                   DMAToDevice, NULL, &mapping) == OS_ENOENT);
    g_denyWriteOnly = true;
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAFromDevice, NULL, &mapping) == OS_ENOTSUPPORTED);
    g_denyWriteOnly = false;
    atomic_store(&space->References, UINT_MAX);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &mapping) == OS_EOVERFLOW);
    atomic_store(&space->References, 1);
    bounds.Granularity = __GRANULE;
    unit->Spaces = UINT_MAX;
    assert(DMAAddressSpaceCreateTranslated(unit, &bounds, &other) == OS_EOVERFLOW && other == NULL);
    unit->Spaces = 1;
    assert(mapping == NULL && atomic_load(&g_liveAllocations) == baseline);
    mapping = __Map(space, 0x1000, __GRANULE, DMAToDevice);
    assert(__Address(mapping) == 0);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &mapping) == OS_EBUSY);
    assert(DMAIOMMUUnitDestroy(&unit) == OS_EBUSY && unit == DMAIOMMUSimUnit(sim));
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(DMAAddressMappingRelease(NULL) == OS_EOK);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
}

static void __TestHeapFailures(void)
{
    bool succeeded = false;
    DMAIOMMUSimConfig_t config = { { __GRANULE, UINT64_MAX, UINT64_MAX, __TIMEOUT }, 2 };
    DMAIOVAConfig_t bounds = { { 0, 32 * __GRANULE - 1 }, __GRANULE, NULL, 0 };
    SHMSG_t extent = { 0x100000, 17 * __GRANULE };

    for (int failure = 0; !succeeded && failure < 12; failure++) {
        DMAIOMMUSim_t* sim = NULL;
        oserr_t status;

        g_failHeapAfter = failure;
        status = DMAIOMMUSimCreate(&config, &sim);
        g_failHeapAfter = -1;
        assert(status == OS_EOOM || status == OS_EOK);
        succeeded = status == OS_EOK;
        if (!succeeded) {
            assert(sim == NULL);
            __Clean();
        }
        assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
        __Clean();
    }
    assert(succeeded);
    for (int phase = 0; phase < 2; phase++) {
        DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
        struct DMAAddressSpace* space = phase == 0 ? NULL : __Space(sim, 0, 32);
        size_t baseline = atomic_load(&g_liveAllocations);

        succeeded = false;
        for (int failure = 0; !succeeded && failure < 24; failure++) {
            struct DMAAddressMapping* mapping = NULL;
            DMAIOMMUSimStats_t stats;
            oserr_t status;

            g_failHeapAfter = failure;
            if (phase == 0) {
                status = DMAAddressSpaceCreateTranslated(DMAIOMMUSimUnit(sim), &bounds, &space);
            } else {
                status = DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                                  DMAToDevice, NULL, &mapping);
            }
            g_failHeapAfter = -1;
            assert(status == OS_EOOM || status == OS_EOK);
            succeeded = status == OS_EOK;
            if (!succeeded) {
                assert(mapping == NULL && (phase != 0 || space == NULL));
                assert(atomic_load(&g_liveAllocations) == baseline);
                assert(DMAIOMMUSimUnit(sim)->Spaces == (unsigned int)phase);
                assert(space == NULL || atomic_load(&space->References) == 1);
                DMAIOMMUSimSnapshot(sim, &stats);
                assert(stats.Mappings == 0 && stats.TablePages == 0);
            }
            g_noHeap = true;
            assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
            if (phase == 0) {
                DMAAddressSpaceRelease(&space);
            }
            g_noHeap = false;
            DMAIOMMUSimSnapshot(sim, &stats);
            assert(stats.TablePages == 0 && stats.Mappings == 0 && stats.PendingCommands == 0);
            assert(atomic_load(&g_liveAllocations) == baseline);
        }
        assert(succeeded);
        DMAAddressSpaceRelease(&space);
        assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
        __Clean();
    }
}

static void __TestDomainsAndTables(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 4);
    struct DMAAddressSpace* a = __Space(sim, 0x10000, 32);
    struct DMAAddressSpace* b = __Space(sim, 0x40000, 32);
    struct DMAAddressSpace* c = __Space(sim, 0x10000, 32);
    struct DMAAddressMapping* first = __Map(a, 0x90000, __GRANULE, DMABidirectional);
    struct DMAAddressMapping* second = __Map(a, 0xa0000, __GRANULE, DMABidirectional);
    struct DMAAddressMapping* discarded = __Map(a, 0xb0000, __GRANULE, DMABidirectional);
    struct DMAAddressMapping* independent = __Map(b, 0x90000, __GRANULE, DMABidirectional);
    struct DMAAddressMapping* overlapping = __Map(c, 0xc0000, __GRANULE, DMABidirectional);
    DMAIOMMUSimStats_t stats;

    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.TablePages == 5);
    assert(DMAAddressMappingInstall(second) == OS_EOK);
    assert(DMAAddressMappingInstall(first) == OS_EOK);
    assert(DMAAddressMappingInstall(independent) == OS_EOK);
    assert(DMAAddressMappingInstall(overlapping) == OS_EOK);
    assert(__Address(first) == 0x10000 && __Address(independent) == 0x40000);
    assert(DMAAddressMappingRelease(&discarded) == OS_EOK);
    __Probe(sim, b, 0x40000, DMAToDevice, OS_EOK, 0x90000);
    __Probe(sim, a, 0x10000, DMAToDevice, OS_EOK, 0x90000);
    __Probe(sim, c, 0x10000, DMAToDevice, OS_EOK, 0xc0000);
    assert(DMAAddressMappingRelease(&first) == OS_EOK);
    __Probe(sim, a, 0x11000, DMAToDevice, OS_EOK, 0xa0000);
    __Probe(sim, b, 0x40000, DMAToDevice, OS_EOK, 0x90000);
    __Probe(sim, c, 0x10000, DMAToDevice, OS_EOK, 0xc0000);
    DMAAddressSpaceRelease(&a);
    DMAAddressSpaceRelease(&b);
    DMAAddressSpaceRelease(&c);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    assert(DMAAddressMappingRelease(&second) == OS_EOK);
    assert(DMAAddressMappingRelease(&independent) == OS_EOK);
    assert(DMAAddressMappingRelease(&overlapping) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
}

static void __TestGranulesAndLimits(void)
{
    for (int i = 0; i < 3; i++) {
        uint64_t granule = i == 1 ? 65536 : __GRANULE;
        uint64_t first = i == 2 ? UINT64_MAX - granule + 1 : 0;
        DMAIOMMUSim_t* sim = __Simulator(granule, 2);
        struct DMAAddressSpace* space = __Space(sim, first, 1);
        struct DMAAddressMapping* mapping = __Map(space, 2 * granule, granule, DMABidirectional);

        assert(__Address(mapping) == first);
        assert(DMAAddressMappingInstall(mapping) == OS_EOK);
        __Probe(sim, space, first + granule - 1, DMAFromDevice, OS_EOK, 3 * granule - 1);
        assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
        DMAAddressSpaceRelease(&space);
        assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
        __Clean();
    }
    {
        DMAIOMMUSimConfig_t config = { { __GRANULE, 0xffff, 0x2fff, __TIMEOUT }, 2 };
        DMAIOMMUSim_t* sim = NULL;
        struct DMAAddressSpace* space;
        struct DMAAddressMapping* mapping = NULL;
        SHMSG_t extent = { 0x2000, 8192 };
        DMAIOVARange_t reserved = { 0, 0x1fff };
        DMAIOVAConfig_t bounds = { { 0, 0xffff }, __GRANULE, &reserved, 1 };
        struct DMAMapRequirements requirements = { 0x4000, __GRANULE };

        assert(DMAIOMMUSimCreate(&config, &sim) == OS_EOK);
        space = NULL;
        assert(DMAAddressSpaceCreateTranslated(DMAIOMMUSimUnit(sim), &bounds, &space) == OS_EOK);
        assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                       DMAToDevice, NULL, &mapping) == OS_EINVALPARAMS);
        extent.Length = __GRANULE;
        assert(DMAAddressMappingPrepare(space, &extent, 1, 0x4fff,
                                       DMAToDevice, &requirements, &mapping) == OS_EOK);
        assert(__Address(mapping) == 0x4000);
        assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
        DMAAddressSpaceRelease(&space);
        assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
        __Clean();
    }
}

static void __TestPartialProgress(void)
{
    for (int removing = 0; removing < 2; removing++) {
        for (unsigned int prefix = 0; prefix <= 3; prefix++) {
            DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
            struct DMAAddressSpace* space = __Space(sim, 0, 3);
            struct DMAAddressMapping* mapping = __Map(space, 0x10000, 3 * __GRANULE, DMAToDevice);
            struct DMAAddressMapping* saved = mapping;
            enum DMAIOMMUSimFailurePoint point = removing ? DMAIOMMUSimFailureRemoveEntry : DMAIOMMUSimFailureInstallEntry;
            size_t allocations = atomic_load(&g_liveAllocations);

            if (removing) {
                SHMSG_t span;
                uint32_t count;

                assert(DMAAddressMappingInstall(mapping) == OS_EOK);
                assert(DMAIOMMUSimAccess(sim, space, 0, 3 * __GRANULE,
                                        DMAToDevice, &span, 1, &count) == OS_EOK);
                assert(count == 1 && span.Address == 0x10000 && span.Length == 3 * __GRANULE);
            }
            __Control(sim, prefix == 3 ? DMAIOMMUSimFailureSubmit : point, prefix == 3 ? 0 : prefix, 0, false);
            g_noHeap = true;
            if (removing) {
                assert(DMAAddressMappingRelease(&mapping) == OS_EINCOMPLETE);
            } else {
                assert(DMAAddressMappingInstall(mapping) == OS_EINCOMPLETE);
            }
            assert(mapping == saved && atomic_load(&g_liveAllocations) == allocations);
            assert(DMAAddressMappingInstall(mapping) == OS_ECANCELLED);
            __Control(sim, DMAIOMMUSimFailureNone, 0, 0, false);
            assert(DMAAddressMappingRelease(&mapping) == OS_EOK && mapping == NULL);
            assert(DMAIOMMUSimAdvance(sim, 1000) == OS_EOK);
            __Probe(sim, space, 0, DMAToDevice, OS_ENOENT, 0);
            DMAAddressSpaceRelease(&space);
            assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
            g_noHeap = false;
            __Clean();
        }
    }
}

static void __TestStaleTranslation(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* space = __Space(sim, 0, 1);
    struct DMAAddressMapping* mapping = __Map(space, 0x20000, __GRANULE, DMAToDevice);
    struct DMAAddressMapping* another = NULL;
    struct DMAAddressMapping* saved = mapping;
    SHMSG_t extent = { 0x90000, __GRANULE };
    DMAIOMMUSimStats_t stats;

    assert(DMAAddressMappingInstall(mapping) == OS_EOK);
    __Probe(sim, space, 0, DMAToDevice, OS_EOK, 0x20000);
    __Control(sim, DMAIOMMUSimFailureNone, 0, 0, true);
    assert(DMAAddressMappingRelease(&mapping) == OS_ETIMEOUT && mapping == saved);
    __Probe(sim, space, 0, DMAToDevice, OS_EOK, 0x20000);
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &another) == OS_ENOENT && another == NULL);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.PendingCommands == 1 && stats.CachedTranslations == 1 && stats.Mappings == 1);
    assert(DMAAddressMappingRelease(&mapping) == OS_ETIMEOUT);
    __Control(sim, DMAIOMMUSimFailureNone, 0, 0, false);
    g_noHeap = true;
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(DMAIOMMUSimAdvance(sim, 1000) == OS_EOK);
    __Probe(sim, space, 0, DMAToDevice, OS_ENOENT, 0);
    g_noHeap = false;
    another = __Map(space, 0x90000, __GRANULE, DMAToDevice);
    assert(__Address(another) == 0 && DMAAddressMappingInstall(another) == OS_EOK);
    __Probe(sim, space, 0, DMAToDevice, OS_EOK, 0x90000);
    assert(DMAAddressMappingRelease(&another) == OS_EOK);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
}

static void __TestCommandRetry(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* space = __Space(sim, 0, 1);
    struct DMAAddressMapping* mapping = __Map(space, 0x10000, __GRANULE, DMAToDevice);
    DMAIOMMUSimStats_t stats;
    unsigned int calls;

    __Control(sim, DMAIOMMUSimFailureNone, 0, 150, false);
    assert(DMAAddressMappingInstall(mapping) == OS_ETIMEOUT);
    __Control(sim, DMAIOMMUSimFailureNone, 0, 80, false);
    assert(DMAAddressMappingRelease(&mapping) == OS_ETIMEOUT);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.SubmittedCommands == 2 && stats.CompletedCommands == 1 && stats.PendingCommands == 1);
    assert(DMAIOMMUSimAdvance(sim, 30) == OS_EOK);
    g_failIOVARelease = true;
    assert(DMAAddressMappingRelease(&mapping) == OS_EUNKNOWN);
    assert(mapping != NULL && mapping->State == DMAAddressMappingRemoved);
    calls = atomic_load(&g_removeCalls);
    g_failIOVARelease = false;
    DMAIOMMUUnitFail(DMAIOMMUSimUnit(sim), OS_EUNKNOWN);
    g_noHeap = true;
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(atomic_load(&g_removeCalls) == calls);
    assert(DMAIOMMUSimAdvance(sim, 1000) == OS_EOK);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.SubmittedCommands == 2 && stats.CompletedCommands == 2 && stats.PendingCommands == 0);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    g_noHeap = false;
    __Clean();
}

static void __TestQueuePressure(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 1);
    struct DMAAddressSpace* space = __Space(sim, 0, 2);
    struct DMAAddressMapping* a = __Map(space, 0x10000, __GRANULE, DMAToDevice);
    struct DMAAddressMapping* b = __Map(space, 0x20000, __GRANULE, DMAToDevice);
    DMAIOMMUSimStats_t stats;

    __Control(sim, DMAIOMMUSimFailureNone, 0, 0, true);
    g_noHeap = true;
    assert(DMAAddressMappingInstall(a) == OS_ETIMEOUT);
    assert(DMAAddressMappingInstall(b) == OS_EBUSY);
    assert(DMAAddressMappingRelease(&b) == OS_EBUSY);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.PendingCommands == 1 && stats.SubmittedCommands == 1 && stats.Mappings == 2);
    __Control(sim, DMAIOMMUSimFailureNone, 0, 0, false);
    assert(DMAIOMMUSimAdvance(sim, 0) == OS_EOK);
    assert(DMAAddressMappingRelease(&b) == OS_EOK);
    assert(DMAAddressMappingRelease(&a) == OS_EOK);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    g_noHeap = false;
    __Clean();
}

static void __TestPreparationFailure(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    DMAIOVAConfig_t bounds = { { 0, 0xffff }, __GRANULE, NULL, 0 };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressMapping* mapping = NULL;
    DMAIOMMUSimStats_t stats;
    SHMSG_t extent = { 0x10000, __GRANULE };
    size_t baseline;

    g_failCreateAfter = true;
    assert(DMAAddressSpaceCreateTranslated(DMAIOMMUSimUnit(sim), &bounds, &space) == OS_ECANCELLED);
    assert(space == NULL);
    g_failCreateAfter = false;
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
    sim = __Simulator(__GRANULE, 2);
    space = __Space(sim, 0, 16);
    baseline = atomic_load(&g_liveAllocations);
    g_faultUnit = DMAIOMMUSimUnit(sim);
    g_failPrepareAfter = true;
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &mapping) == OS_ECANCELLED);
    g_failPrepareAfter = false;
    assert(mapping == NULL && atomic_load(&g_liveAllocations) == baseline);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.Mappings == 0 && stats.TablePages == 0 && stats.PendingCommands == 0);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
}

static void __TestTerminalFailure(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* a = __Space(sim, 0, 16);
    struct DMAAddressSpace* b = __Space(sim, 0x10000, 16);
    struct DMAAddressMapping* active = __Map(a, 0x20000, __GRANULE, DMAToDevice);
    struct DMAAddressMapping* pending = __Map(b, 0x30000, __GRANULE, DMAToDevice);
    struct DMAAddressMapping* prepared;
    struct DMAAddressMapping* rejected = NULL;
    SHMSG_t extent = { 0x40000, __GRANULE };
    DMAIOMMUSimStats_t stats;
    DMAIOMMUSimControl_t control = { DMAIOMMUSimFailureComplete, 0, OS_EINCOMPLETE, true, 0, false };
    size_t before;
    unsigned int calls;

    assert(DMAAddressMappingInstall(active) == OS_EOK);
    __Probe(sim, a, 0, DMAToDevice, OS_EOK, 0x20000);
    __Control(sim, DMAIOMMUSimFailureNone, 0, 0, true);
    assert(DMAAddressMappingInstall(pending) == OS_ETIMEOUT);
    prepared = __Map(a, 0x40000, __GRANULE, DMAToDevice);
    before = atomic_load(&g_liveAllocations);
    assert(DMAIOMMUSimSetControl(sim, &control) == OS_EOK);
    // A callback's own error takes precedence over the unit cancellation it
    // also reported. Later calls see the permanent unit failure.
    assert(DMAAddressMappingRelease(&pending) == OS_EINCOMPLETE);
    DMAIOMMUUnitFail(DMAIOMMUSimUnit(sim), OS_EUNKNOWN);
    assert(atomic_load(&DMAIOMMUSimUnit(sim)->Failure) == OS_EINCOMPLETE);
    assert(DMAAddressSpaceCheck(a) == OS_ECANCELLED && DMAAddressSpaceCheck(b) == OS_ECANCELLED);
    calls = atomic_load(&g_installCalls);
    assert(DMAAddressMappingInstall(prepared) == OS_ECANCELLED);
    assert(prepared->State == DMAAddressMappingPrepared && atomic_load(&g_installCalls) == calls);
    assert(DMAAddressMappingPrepare(b, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &rejected) == OS_ECANCELLED && rejected == NULL);
    assert(DMAAddressMappingRelease(&active) == OS_ECANCELLED);
    assert(DMAAddressMappingRelease(&pending) == OS_ECANCELLED);
    assert(DMAAddressMappingRelease(&prepared) == OS_EOK);
    // No new leaf was needed for prepared: core, extent copy, IOVA and backend
    // record are the four software allocations discarded after the failure.
    assert(atomic_load(&g_liveAllocations) == before - 4);
    __Probe(sim, a, 0, DMAToDevice, OS_EOK, 0x20000);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.Spaces == 2 && stats.Mappings == 2 && stats.TablePages == 2);
    assert(stats.PendingCommands == 1 && stats.CachedTranslations == 1);
    {
        DMAIOMMUSim_t* healthy = __Simulator(__GRANULE, 2);
        struct DMAAddressSpace* space = __Space(healthy, 0, 1);
        struct DMAAddressMapping* mapping = __Map(space, 0x50000, __GRANULE, DMAToDevice);

        assert(DMAAddressMappingInstall(mapping) == OS_EOK);
        assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
        DMAAddressSpaceRelease(&space);
        assert(DMAIOMMUSimDestroy(&healthy) == OS_EOK);
    }
    DMAAddressSpaceRelease(&a);
    DMAAddressSpaceRelease(&b);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    g_failedSim = sim;
    g_failedMappings[0] = active;
    g_failedMappings[1] = pending;
    assert(atomic_load(&g_liveAllocations) == before - 4 && atomic_load(&g_liveMutexes) == 3);
}

struct __InstallThread {
    struct DMAAddressMapping* Mapping;
    oserr_t                   Status;
};

static void* __InstallThread(void* argument)
{
    struct __InstallThread* thread = argument;

    thread->Status = DMAAddressMappingInstall(thread->Mapping);
    return NULL;
}

static void __TestFailureAtCompletion(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* space = __Space(sim, 0, 1);
    struct __InstallThread thread = { __Map(space, 0x20000, __GRANULE, DMAToDevice), OS_EOK };
    pthread_t worker;
    DMAIOMMUSimStats_t stats;
    size_t before = atomic_load(&g_liveAllocations);

    assert(pthread_barrier_init(&g_installReady, NULL, 2) == 0);
    assert(pthread_barrier_init(&g_installContinue, NULL, 2) == 0);
    g_pauseInstall = true;
    assert(pthread_create(&worker, NULL, __InstallThread, &thread) == 0);
    __Barrier(&g_installReady);
    DMAIOMMUUnitFail(DMAIOMMUSimUnit(sim), OS_EUNKNOWN);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.CompletedCommands == 1);
    __Barrier(&g_installContinue);
    assert(pthread_join(worker, NULL) == 0);
    g_pauseInstall = false;
    assert(pthread_barrier_destroy(&g_installReady) == 0);
    assert(pthread_barrier_destroy(&g_installContinue) == 0);
    assert(thread.Status == OS_ECANCELLED && thread.Mapping->State == DMAAddressMappingInstallStarted);
    assert(DMAAddressMappingRelease(&thread.Mapping) == OS_ECANCELLED);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    assert(atomic_load(&g_liveAllocations) == before);
    g_failedSim = sim;
    g_failedMappings[0] = thread.Mapping;
}

static void __TestFailureAfterRemoval(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* space = __Space(sim, 0, 1);
    struct DMAAddressMapping* mapping = __Map(space, 0x20000, __GRANULE, DMAToDevice);
    DMAIOMMUSimStats_t stats;
    size_t before = atomic_load(&g_liveAllocations);

    assert(DMAAddressMappingInstall(mapping) == OS_EOK);
    g_faultUnit = DMAIOMMUSimUnit(sim);
    g_failRemoveAfter = true;
    assert(DMAAddressMappingRelease(&mapping) == OS_ECANCELLED);
    g_failRemoveAfter = false;
    assert(mapping != NULL && mapping->State == DMAAddressMappingRemoving);
    assert(atomic_load(&g_liveAllocations) == before);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.CompletedCommands == 2 && stats.PendingCommands == 0 && stats.Mappings == 1);
    assert(DMAAddressMappingRelease(&mapping) == OS_ECANCELLED);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    g_failedSim = sim;
    g_failedMappings[0] = mapping;
}

static void __TestUnpublishedCleanupDefect(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 2);
    struct DMAAddressSpace* space = __Space(sim, 0, 1);
    struct DMAAddressMapping* mapping = NULL;
    SHMSG_t extent = { 0x20000, __GRANULE };
    DMAIOMMUSimStats_t stats;
    size_t before = atomic_load(&g_liveAllocations);

    g_faultUnit = DMAIOMMUSimUnit(sim);
    g_failPrepareAfter = true;
    g_failIOVARelease = true;
    assert(DMAAddressMappingPrepare(space, &extent, 1, UINT64_MAX,
                                   DMAToDevice, NULL, &mapping) == OS_ECANCELLED);
    g_failPrepareAfter = false;
    g_failIOVARelease = false;
    assert(mapping == NULL && atomic_load(&space->References) == 1);
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.Mappings == 0 && stats.TablePages == 0);
    assert(atomic_load(&g_liveAllocations) == before + 1);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    assert(atomic_load(&g_liveAllocations) == before + 1);
    g_failedSim = sim;
}

struct __MapThread {
    DMAIOMMUSim_t*          Sim;
    struct DMAAddressSpace* Space;
    uintptr_t               Physical;
};

static void* __MapThread(void* argument)
{
    struct __MapThread* thread = argument;

    for (int i = 0; i < 250; i++) {
        struct DMAAddressMapping* mapping = __Map(thread->Space, thread->Physical, __GRANULE, DMAToDevice);
        uint64_t address = __Address(mapping);

        assert(DMAAddressMappingInstall(mapping) == OS_EOK);
        __Probe(thread->Sim, thread->Space, address, DMAToDevice, OS_EOK, thread->Physical);
        assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    }
    return NULL;
}

static void __TestConcurrentMappings(void)
{
    DMAIOMMUSim_t* sim = __Simulator(__GRANULE, 4);
    struct DMAAddressSpace* space = __Space(sim, 0, 16);
    struct __MapThread threads[4];
    pthread_t workers[4];
    DMAIOMMUSimStats_t stats;

    for (unsigned int i = 0; i < 4; i++) {
        threads[i] = (struct __MapThread){ sim, space, 0x10000 + i * __GRANULE };
        assert(pthread_create(&workers[i], NULL, __MapThread, &threads[i]) == 0);
    }
    for (unsigned int i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    DMAIOMMUSimSnapshot(sim, &stats);
    assert(stats.SubmittedCommands == 2000 && stats.CompletedCommands == 2000);
    assert(stats.Mappings == 0 && stats.PendingCommands == 0 && stats.CachedTranslations == 0);
    assert(atomic_load(&space->References) == 1);
    DMAAddressSpaceRelease(&space);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __Clean();
}

static void __Child(void (*test)(void))
{
    pid_t child = fork();
    int status;

    assert(child >= 0);
    if (child == 0) {
        test();
        assert(g_failedSim != NULL);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    __Clean();
}

int main(void)
{
    __TestLayout();
    __TestValidation();
    __TestHeapFailures();
    __TestDomainsAndTables();
    __TestGranulesAndLimits();
    __TestPartialProgress();
    __TestStaleTranslation();
    __TestCommandRetry();
    __TestQueuePressure();
    __TestPreparationFailure();
    __TestConcurrentMappings();
    __Child(__TestTerminalFailure);
    __Child(__TestFailureAtCompletion);
    __Child(__TestFailureAfterRemoval);
    __Child(__TestUnpublishedCleanupDefect);
    __Clean();
    puts("DMA IOMMU: layouts, permissions, partial cleanup, stale translations and unit failure passed");
    return 0;
}
