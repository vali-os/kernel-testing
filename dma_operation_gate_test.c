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

#define main __RunBoundTests
#include "dma_bound_test.c"
#undef main
#include <arch/interrupts.h>
#include <limits.h>

static _Thread_local bool g_irqDisabled;
static _Thread_local bool g_fastPath;
static SHMDeviceContext_t* g_collectRace;
static bool g_raceOperation;

void
TestGateMutex(void)
{
    assert(!g_irqDisabled && !g_fastPath);
}

irqstate_t
InterruptDisable(void)
{
    bool previous = g_irqDisabled;
    g_irqDisabled = true;
    return previous;
}

irqstate_t
InterruptRestoreState(
    irqstate_t state)
{
    assert(g_irqDisabled);
    g_irqDisabled = state;
    return state;
}

void MemorySpaceSyncProcessPending(void) {}

oserr_t __real_SHMDeviceContextAcquire(SHMDeviceContext_t* context);
oserr_t
__wrap_SHMDeviceContextAcquire(
    SHMDeviceContext_t* context)
{
    if (context == g_collectRace) {
        bool stopped = false;
        g_collectRace = NULL;
        assert(DMABindingBeginOperation(context->Binding, true, &stopped) == OS_EOK && stopped);
        g_raceOperation = true;
    }
    return __real_SHMDeviceContextAcquire(context);
}

static void
__RejectAllocation(void)
{
    assert(!"healthy gate must not allocate");
}

static void*
__RunGate(
    void* argument)
{
    DMABinding_t* binding = argument;
    bool stopped = true;
    g_fastPath = true;
    for (unsigned int i = 0; i < 4000; i++) {
        assert(DMABindingBeginOperation(binding, false, &stopped) == OS_EOK && !stopped);
        assert(DMABindingCheckOperation(binding) == OS_EOK);
        DMABindingEndOperation(binding);
    }
    g_fastPath = false;
    return NULL;
}

int
main(void)
{
    MSContextSecurity_t service, driver;
    __BoundFixture_t fixture;
    DMARecoveryGroup_t* group;
    DMABinding_t* binding;
    OSDmaStopTicket_t ticket;
    pthread_t workers[4];
    bool stopped = true;
    size_t allocations;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    fixture = __PrepareBoundFixture(&service, &driver);
    __OpenBoundFixture(&fixture);
    binding = fixture.Context->Binding;
    group = binding->Group;
    allocations = g_allocations;
    g_constructionHook = __RejectAllocation;
    for (unsigned int i = 0; i < 4; i++) {
        assert(pthread_create(&workers[i], NULL, __RunGate, binding) == 0);
    }
    for (unsigned int i = 0; i < 4; i++) {
        assert(pthread_join(workers[i], NULL) == 0);
    }
    assert(group->Access.Operations == 0 && g_allocations == allocations);
    g_constructionHook = NULL;

    g_fastPath = true;
    group->Access.Operations = UINT_MAX;
    assert(DMABindingBeginOperation(binding, false, &stopped) == OS_EOVERFLOW && stopped);
    group->Access.Operations = 0;
    g_fastPath = false;

    // Stop refuses cleanup until an already admitted operation leaves the gate.
    assert(DMABindingBeginOperation(binding, false, &stopped) == OS_EOK);
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY);
    assert(DMABindingBeginOperation(binding, false, &stopped) == OS_ECANCELLED);
    assert(DMABindingBeginOperation(binding, true, &stopped) == OS_EBUSY);
    DMABindingEndOperation(binding);

    // Simulate a failed cleanup pass. Releases may now start while the manager
    // collects context references for the retry. The final gate check must catch it.
    group->Access.State = DMARecoveryStopConfirmed;
    g_collectRace = fixture.Context;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY && g_raceOperation);
    assert(!group->Access.Cleaning && group->Access.Operations == 1);
    DMABindingEndOperation(binding);
    group->Access.Cleaning = true;
    assert(DMABindingBeginOperation(binding, true, &stopped) == OS_EBUSY);
    group->Access.Cleaning = false;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(group->Access.State == DMARecoveryStopCleaned && !group->Access.Cleaning);
    g_fastPath = true;
    assert(DMABindingBeginOperation(binding, true, &stopped) == OS_EOK && stopped);
    DMABindingEndOperation(binding);
    g_fastPath = false;
    group->Access.ManagerClosed = true;
    assert(DMABindingBeginOperation(binding, true, &stopped) == OS_ECANCELLED);
    group->Access.ManagerClosed = false;
    __FinishBoundFixture(&fixture);
    puts("DMA operation gate: no fast-path mutex/allocation, parallel counts, stop drain and cleanup retry exclusion passed");
    return 0;
}
