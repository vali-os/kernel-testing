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

#include <shm/region_usage.h>
#include <arch/interrupts.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>

static _Thread_local bool g_irqsDisabled;
static _Thread_local SHMRegionAccess_t* g_endOnRestore;

irqstate_t
InterruptDisable(void)
{
    bool previous = g_irqsDisabled;
    g_irqsDisabled = true;
    return previous;
}

irqstate_t
InterruptRestoreState(
    irqstate_t state)
{
    SHMRegionAccess_t* access = g_endOnRestore;
    assert(g_irqsDisabled);
    g_irqsDisabled = state;
    if (access != NULL && !state) {
        g_endOnRestore = NULL;
        assert(access->State == SHMRegionAccessLive);
        assert(SHMRegionUsageEnd(access, access->Generation) == OS_EOK);
    }
    return state;
}

static Spinlock_t g_wrapLock;
static atomic_bool g_checkWrap;
static atomic_bool g_waiterSpinning;

void
MemorySpaceSyncProcessPending(void)
{
    if (atomic_load(&g_checkWrap)) {
        assert(atomic_load(&g_wrapLock.Next) == 1);
        atomic_store(&g_waiterSpinning, true);
    }
}

static void*
__WaitAcrossWrap(
    void* argument)
{
    (void)argument;
    SpinlockAcquireIrq(&g_wrapLock);
    assert(g_irqsDisabled);
    SpinlockReleaseIrq(&g_wrapLock);
    assert(!g_irqsDisabled);
    return NULL;
}

static void
__TestSpinlockWrap(void)
{
    pthread_t waiter;
    SpinlockConstruct(&g_wrapLock);
    atomic_store(&g_wrapLock.Current, UINT_MAX);
    atomic_store(&g_wrapLock.Next, UINT_MAX);
    SpinlockAcquireIrq(&g_wrapLock);
    atomic_store(&g_checkWrap, true);
    assert(pthread_create(&waiter, NULL, __WaitAcrossWrap, NULL) == 0);
    while (!atomic_load(&g_waiterSpinning)) {
    }
    SpinlockReleaseIrq(&g_wrapLock);
    assert(pthread_join(waiter, NULL) == 0);
    atomic_store(&g_checkWrap, false);
    assert(atomic_load(&g_wrapLock.Current) == 1 && atomic_load(&g_wrapLock.Next) == 1);
}


static void
__TestConflicts(void)
{
    SHMRegionUsage_t region;
    SHMRegionAccess_t cpu = { 0 }, device = { 0 }, writer = { 0 };
    SHMRegionUsageInitialize(&region);
    assert(SHMRegionUsageBegin(&region, &cpu, 1, false) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &device, 1, false) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &writer, 1, true) == OS_EBUSY);
    assert(writer.Generation == 0 && writer.State == SHMRegionAccessIdle);
    assert(SHMRegionUsageEnd(&cpu, 2) == OS_ENOENT && region.Readers == 2);
    assert(SHMRegionUsageEnd(&cpu, 1) == OS_EOK);
    assert(SHMRegionUsageEnd(&cpu, 1) == OS_ENOENT && region.Readers == 1);
    assert(SHMRegionUsageEnd(&device, 1) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &writer, 1, true) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &cpu, 2, false) == OS_EBUSY);
    assert(SHMRegionUsageBegin(&region, &device, 2, true) == OS_EBUSY);
    assert(SHMRegionUsageEnd(&writer, 1) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &cpu, 1, false) == OS_ENOENT);
    assert(SHMRegionUsageBegin(&region, &cpu, 0, false) == OS_EINVALPARAMS);
    cpu.Generation = UINT_MAX;
    assert(SHMRegionUsageBegin(&region, &cpu, 1, false) == OS_EOVERFLOW);
    region.Readers = UINT_MAX;
    assert(SHMRegionUsageBegin(&region, &device, 2, false) == OS_EOVERFLOW);
    region.Readers = 0;
    g_endOnRestore = &device;
    assert(SHMRegionUsageBegin(&region, &device, 2, false) == OS_EOK);
    assert(device.State == SHMRegionAccessIdle && region.Readers == 0);
}

static void
__TestStoppedReaders(void)
{
    SHMRegionUsage_t region, other;
    SHMRegionAccess_t first = { 0 }, second = { 0 }, cpu = { 0 };
    SHMRegionFault_t firstFault = { 0 }, secondFault = { 0 };
    SHMRegionUsageInitialize(&region);
    SHMRegionUsageInitialize(&other);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EINVALPARAMS);
    assert(SHMRegionUsageBegin(&region, &first, 1, false) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &second, 1, false) == OS_EOK);
    assert(SHMRegionUsageStop(&first, 1) == OS_EINVALPARAMS);
    assert(SHMRegionUsageFault(&region, &firstFault) == OS_EOK);
    assert(SHMRegionUsageFault(&region, &firstFault) == OS_EOK && region.Faults == 1);
    assert(SHMRegionUsageFault(&other, &firstFault) == OS_EINVALPARAMS);
    assert(SHMRegionUsageFault(&region, &secondFault) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &cpu, 1, false) == OS_EDEVFAULT);
    assert(SHMRegionUsageStop(&first, 2) == OS_ENOENT && region.Readers == 2);
    assert(SHMRegionUsageStop(&first, 1) == OS_EOK);
    assert(SHMRegionUsageStop(&first, 1) == OS_EOK && region.Readers == 1 && region.Stopped == 1);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EBUSY);
    assert(SHMRegionUsageStop(&second, 1) == OS_EOK);
    assert(region.Readers == 0 && region.Stopped == 2);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EOK);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EBUSY);
    assert(SHMRegionUsageEnd(&first, 1) == OS_EOK);
    assert(SHMRegionUsageClearFault(&firstFault) == OS_EOK);
    assert(SHMRegionUsageClearFault(&secondFault) == OS_EBUSY);
    SHMRegionUsageEndRecovery(&region);
    assert(SHMRegionUsageClearFault(&secondFault) == OS_EBUSY);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EOK);
    assert(SHMRegionUsageEnd(&second, 1) == OS_EOK);
    assert(SHMRegionUsageClearFault(&secondFault) == OS_EBUSY);
    SHMRegionUsageEndRecovery(&region);
    assert(SHMRegionUsageClearFault(&secondFault) == OS_EOK);
    assert(SHMRegionUsageClearFault(&secondFault) == OS_EOK);
    assert(SHMRegionUsageBegin(&region, &cpu, 1, true) == OS_EOK);
    assert(SHMRegionUsageFault(&region, &firstFault) == OS_EOK);
    assert(SHMRegionUsageBeginRecovery(&region) == OS_EBUSY);
    region.Stopped = UINT_MAX;
    assert(SHMRegionUsageStop(&cpu, 1) == OS_EOVERFLOW && region.Writer);
    region.Stopped = 0;
    assert(SHMRegionUsageStop(&cpu, 1) == OS_EOK && !region.Writer);
    assert(SHMRegionUsageEnd(&cpu, 1) == OS_EOK);
    assert(SHMRegionUsageClearFault(&firstFault) == OS_EOK);
    region.Faults = UINT_MAX;
    assert(SHMRegionUsageFault(&region, &firstFault) == OS_EOVERFLOW && firstFault.Region == NULL);
}

static SHMRegionUsage_t g_regions[2];
static int g_values[2];
static void*
__RunQueue(
    void* argument)
{
    unsigned int first = (uintptr_t)argument % 2;
    unsigned int second = 1 - first;
    SHMRegionAccess_t tokens[2] = { 0 };
    for (unsigned int i = 0; i < 2000; i++) {
        oserr_t status;
        do {
            status = SHMRegionUsageBegin(&g_regions[first], &tokens[first], tokens[first].Generation + 1, true);
            assert(status == OS_EOK || status == OS_EBUSY);
        } while (status == OS_EBUSY);
        status = SHMRegionUsageBegin(&g_regions[second], &tokens[second], tokens[second].Generation + 1, true);
        if (status == OS_EOK) {
            assert(g_values[0] == g_values[1]);
            g_values[0]++;
            g_values[1]++;
            assert(SHMRegionUsageEnd(&tokens[second], tokens[second].Generation) == OS_EOK);
        } else {
            assert(status == OS_EBUSY);
        }
        assert(SHMRegionUsageEnd(&tokens[first], tokens[first].Generation) == OS_EOK);
    }
    return NULL;
}

int
main(void)
{
    pthread_t queues[4];
    __TestSpinlockWrap();
    __TestConflicts();
    __TestStoppedReaders();
    SHMRegionUsageInitialize(&g_regions[0]);
    SHMRegionUsageInitialize(&g_regions[1]);
    for (uintptr_t i = 0; i < 4; i++) {
        assert(pthread_create(&queues[i], NULL, __RunQueue, (void*)i) == 0);
    }
    for (unsigned int i = 0; i < 4; i++) {
        assert(pthread_join(queues[i], NULL) == 0);
    }
    assert(g_values[0] > 0 && g_values[0] == g_values[1]);
    puts("Region usage: shared readers, exclusive writers, faults, stop transfer, IRQ retirement and queue contention passed");
    return 0;
}
