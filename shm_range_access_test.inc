#include <shm/access.h>
extern _Thread_local atomic_int g_rangeWaitError;
extern _Thread_local bool g_rangeAdvanceOnError;
extern _Thread_local OSTimestamp_t* g_rangeWaitDeadline;
extern atomic_uint g_rangeWakeCalls;

struct __RangeWait {
    struct SHMAccess* Access;
    bool Write;
    atomic_int Entered;
    atomic_int Release;
    oserr_t Status;
};

static void*
__WaitRange(void* argument)
{
    struct __RangeWait* wait = argument;

    wait->Status = __TestAccessBeginCpu(wait->Access, wait->Write, &(struct SHMAccessWait) { .Blocking = true });
    atomic_store(&wait->Entered, 1);
    if (wait->Status == OS_EOK) {
        while (!atomic_load(&wait->Release)) {
            sched_yield();
        }
        assert(__TestAccessEndCpu(wait->Access) == OS_EOK);
    }
    return NULL;
}

static void
__WaitUntilQueued(struct SHMAccess* access)
{
    bool queued;

    do {
        MutexLock(&g_buffer->Mutex);
        queued = access->Phase != SHMAccessIdle;
        MutexUnlock(&g_buffer->Mutex);
        sched_yield();
    } while (!queued);
}

static void
__TestRangeAccess(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x1000, 0x10000 };
    const paddr_t pages[] = { 0x700000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* writer = NULL;
    struct SHMDeviceMapping* disjoint = NULL;
    struct SHMDeviceMapping* stream = NULL;
    struct SHMDeviceMapping* readers[2] = { NULL, NULL };
    struct SHMAccess* cpu = NULL;
    struct SHMAccess* later = NULL;
    struct SHMAccess* adjacent = NULL;
    struct SHMAccess* streamAccess;
    struct SHMAccess* streamCpu = NULL;
    struct SHMAccess* readerCpu = NULL;
    struct __RangeWait wait = { 0 };
    OSTimestamp_t expired = { 0 };
    OSTimestamp_t infinite = *OS_DEADLINE_INFINITE;
    pthread_t thread;
    unsigned int allocations;

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    assert(SHMDeviceMap(context, 0x20, 0, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &writer) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 64, 64, DMAFromDevice, SHMDeviceMappingPersistent, NULL, &disjoint) == OS_EOK);
    assert(SHMAccessCreate(0x20, 1, 1, &cpu) == OS_EOK);
    assert(SHMAccessCreate(0x20, 63, 1, &later) == OS_EOK);
    assert(SHMAccessCreate(0x20, 128, 1, &adjacent) == OS_EOK);
    assert(SHMAccessCreate(0x20, 0, 128, &streamCpu) == OS_EOK);
    assert(SHMAccessCreate(0x20, 0, 64, &readerCpu) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 0, 128, DMAFromDevice, SHMDeviceMappingStreamingDirect, NULL, &stream) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 128, 64, DMAToDevice, SHMDeviceMappingPersistent, NULL, &readers[0]) == OS_EOK);
    assert(SHMDeviceMap(context, 0x20, 128, 64, DMAToDevice, SHMDeviceMappingPersistent, NULL, &readers[1]) == OS_EOK);
    allocations = g_allocationCalls;

    // CPU teardown accepts idle, active and suspended waits without exposing
    // reservation state to the syscall registry.
    assert(__TestAccessEndCpu(cpu) == OS_EINVALPARAMS);
    __TestAccessCancelCpu(cpu);
    assert(__TestAccessBeginCpu(cpu, true, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EBUSY);
    __TestAccessCancelCpu(cpu);
    assert(__TestAccessEndCpu(cpu) == OS_EINVALPARAMS);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    atomic_store(&g_rangeWaitError, OS_EFORKED);
    assert(__TestAccessBeginCpu(cpu, true, &(struct SHMAccessWait) { .Blocking = true }) == OS_EFORKED);
    assert(atomic_load(&g_buffer->Access.Waiting) == 1);
    assert(cpu->Phase == SHMAccessWaiting && cpu->Holder == SHMAccessHolderCpu);
    assert(__TestAccessEndCpu(cpu) == OS_EINVALPARAMS);
    assert(SHMAccessDestroy(&cpu) == OS_EBUSY);
    __TestAccessCancelCpu(cpu);
    assert(atomic_load(&g_buffer->Access.Waiting) == 0);
    assert(cpu->Phase == SHMAccessIdle && cpu->Holder == SHMAccessHolderNone);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);

    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(disjoint, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    assert(__TestAccessBeginCpu(adjacent, true, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(adjacent) == OS_EOK);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);
    assert(SHMDeviceEndUse(writer) == OS_EOK);

    // A forked admission keeps its submitted subrange until cancellation.
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    atomic_store(&g_rangeWaitError, OS_EFORKED);
    assert(SHMDeviceSyncForDevice(stream, 0, 64, &(struct SHMAccessWait) { .Blocking = true }) == OS_EFORKED);
    streamAccess = g_buffer->Access.Last;
    assert(streamAccess->Phase == SHMAccessWaiting);
    assert(streamAccess->First == 0 && streamAccess->Last == 63);
    assert(SHMDeviceUnmap(&stream) == OS_EBUSY);
    assert(SHMDeviceSyncForCpu(stream, 64) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForDevice(stream, 64, 64, NULL) == OS_EBUSY);
    assert(SHMDeviceBeginUse(disjoint, NULL) == OS_EOK);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);
    // The fixture cancels the suspended call before reusing the reservation.
    SHMAccessEnd(streamAccess);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    assert(SHMDeviceBeginUse(disjoint, NULL) == OS_EOK);
    assert(__TestAccessBeginCpu(streamCpu, false, NULL) == OS_EBUSY);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);

    assert(SHMDeviceSyncForDevice(stream, 0, 64, NULL) == OS_EOK);
    // An active stream reserves only its submitted subrange.
    streamAccess = g_buffer->Access.Last;
    assert(streamAccess->Phase == SHMAccessActive);
    assert(streamAccess->Holder == SHMAccessHolderDevice);
    assert(streamAccess->First == 0 && streamAccess->Last == 63);
    assert(SHMDeviceBeginUse(disjoint, NULL) == OS_EOK);
    assert(SHMDeviceSyncForCpu(stream, 65) == OS_EINVALPARAMS);
    assert(SHMDeviceSyncForCpu(stream, 32) == OS_EOK);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);

    // An earlier queued writer prevents new readers from starving it.
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    wait.Access = later;
    wait.Write = true;
    assert(pthread_create(&thread, NULL, __WaitRange, &wait) == 0);
    __WaitUntilQueued(later);
    assert(SHMDeviceBeginUse(disjoint, &(struct SHMAccessWait) { .Blocking = true, .Deadline = OS_DEADLINE_INFINITE }) == OS_EOK);
    assert(__TestAccessBeginCpu(readerCpu, false, NULL) == OS_EBUSY);
    assert(__TestAccessBeginCpu(readerCpu, false, &(struct SHMAccessWait) { .Blocking = true, .Deadline = &expired }) == OS_ETIMEOUT);
    atomic_store(&g_rangeWaitError, OS_EINTERRUPTED);
    assert(__TestAccessBeginCpu(readerCpu, false, &(struct SHMAccessWait) { .Blocking = true, .Deadline = &infinite }) == OS_EINTERRUPTED);
    assert(g_rangeWaitDeadline == &readerCpu->Deadline);
    // An interrupt racing a range wake retries and then reports the timeout.
    atomic_store(&g_rangeWaitError, OS_EINTERRUPTED);
    g_rangeAdvanceOnError = true;
    assert(__TestAccessBeginCpu(readerCpu, false, &(struct SHMAccessWait) { .Blocking = true, .Deadline = &expired }) == OS_ETIMEOUT);
    assert(SHMAccessDestroy(&later) == OS_EBUSY);
    assert(!atomic_load(&wait.Entered));
    assert(__TestAccessEndCpu(cpu) == OS_EOK);
    while (!atomic_load(&wait.Entered)) {
        sched_yield();
    }
    assert(wait.Status == OS_EOK);
    // Different bytes of the same cache line must still conflict.
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EBUSY);
    atomic_store(&wait.Release, 1);
    assert(pthread_join(thread, NULL) == 0);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);

    // A running persistent batch cannot keep extending ahead of a waiter.
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    atomic_store(&wait.Entered, 0);
    atomic_store(&wait.Release, 1);
    assert(pthread_create(&thread, NULL, __WaitRange, &wait) == 0);
    __WaitUntilQueued(later);
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EBUSY);
    assert(SHMDeviceBeginUse(writer, &(struct SHMAccessWait) { .Blocking = true, .Deadline = OS_DEADLINE_INFINITE }) == OS_EBUSY);
    assert(SHMDeviceEndUse(writer) == OS_EOK);
    assert(pthread_join(thread, NULL) == 0 && wait.Status == OS_EOK);

    // Death during DMA wakes blocked users with an error and keeps pages pinned.
    assert(SHMDeviceBeginUse(writer, NULL) == OS_EOK);
    atomic_store(&wait.Entered, 0);
    assert(pthread_create(&thread, NULL, __WaitRange, &wait) == 0);
    __WaitUntilQueued(later);
    assert(SHMDeviceAbandon(&writer) == OS_EOK && writer == NULL);
    assert(pthread_join(thread, NULL) == 0 && wait.Status == OS_EDEVFAULT);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EDEVFAULT);
    assert(SHMDeviceBeginUse(disjoint, NULL) == OS_EOK);
    assert(SHMDeviceEndUse(disjoint) == OS_EOK);
    assert(SHMDeviceBeginUse(readers[0], NULL) == OS_EOK);
    assert(SHMDeviceBeginUse(readers[1], NULL) == OS_EOK);
    assert(SHMDeviceAbandon(&readers[0]) == OS_EOK && readers[0] == NULL);
    assert(SHMDeviceBeginUse(readers[1], NULL) == OS_EDEVFAULT);
    assert(SHMDeviceEndUse(readers[1]) == OS_EOK);
    assert(SHMDeviceUnmap(&readers[0]) == OS_EOK);
    assert(SHMDeviceUnmap(&readers[1]) == OS_EOK);
    assert(g_allocationCalls == allocations);

    // Confirm a device stop, then allow CPU access to the formerly blocked range.
    SHMDeviceReleaseQuarantine(context);
    SHMDeviceReleaseQuarantine(context);
    assert(__TestAccessBeginCpu(cpu, false, NULL) == OS_EOK);
    assert(__TestAccessEndCpu(cpu) == OS_EOK);
    assert(SHMDeviceUnmap(&writer) == OS_EOK);
    assert(SHMDeviceUnmap(&disjoint) == OS_EOK);
    assert(SHMDeviceUnmap(&stream) == OS_EOK);
    assert(SHMAccessDestroy(&cpu) == OS_EOK);
    assert(SHMAccessDestroy(&later) == OS_EOK);
    assert(SHMAccessDestroy(&adjacent) == OS_EOK);
    assert(SHMAccessDestroy(&streamCpu) == OS_EOK);
    assert(SHMAccessDestroy(&readerCpu) == OS_EOK);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);
    puts("SHM ranges: disjoint DMA, CPU attachments, cache-line conflicts, fair waits, timeout and quarantine passed");
}

// Leave a suspended begin in the queue without starting a host thread. These
// tests cancel it after inspecting wake delivery; no async child can resume.
static void
__QueueRangeWait(
    _In_ struct SHMAccess* access,
    _In_ bool              write)
{
    atomic_store(&g_rangeWaitError, OS_EFORKED);
    assert(SHMAccessBegin(
        access, 0, access->Length,
        SHMAccessHolderDevice, write, &(struct SHMAccessWait) { .Blocking = true }
    ) == OS_EFORKED);
}

static void
__TestRangeWakeSelection(void)
{
    const paddr_t pages[] = { 0x700000 };
    struct SHMAccess holders[2];
    struct SHMAccess waiters[6];
    size_t i;

    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    for (i = 0; i < 2; i++) {
        SHMAccessInitialize(&holders[i], g_buffer, i * 64, 64);
        assert(SHMAccessBegin(
            &holders[i], 0, holders[i].Length,
            SHMAccessHolderDevice, true, NULL
        ) == OS_EOK);
    }
    for (i = 0; i < 6; i++) {
        SHMAccessInitialize(&waiters[i], g_buffer, (i % 2) * 64, 64);
        __QueueRangeWait(&waiters[i], true);
    }

    // Removing a middle writer redirects its successor without waking it.
    atomic_store(&g_rangeWakeCalls, 0);
    __TestAccessCancelCpu(&waiters[2]);
    assert(atomic_load(&g_rangeWakeCalls) == 0);
    SHMAccessEnd(&holders[0]);
    assert(atomic_load(&g_rangeWakeCalls) == 1);
    assert(atomic_load(&waiters[0].Event) == 1);
    for (i = 1; i < 6; i++) {
        assert(atomic_load(&waiters[i].Event) == 0);
    }

    // The second range progresses independently; each writer wakes one next.
    SHMAccessEnd(&holders[1]);
    assert(atomic_load(&g_rangeWakeCalls) == 2);
    assert(atomic_load(&waiters[1].Event) == 1);
    __TestAccessCancelCpu(&waiters[0]);
    assert(atomic_load(&g_rangeWakeCalls) == 3);
    assert(atomic_load(&waiters[4].Event) == 1);
    __TestAccessCancelCpu(&waiters[1]);
    assert(atomic_load(&g_rangeWakeCalls) == 4);
    assert(atomic_load(&waiters[3].Event) == 1);
    assert(atomic_load(&waiters[5].Event) == 0);
    __TestAccessCancelCpu(&waiters[3]);
    assert(atomic_load(&g_rangeWakeCalls) == 5);
    assert(atomic_load(&waiters[5].Event) == 1);
    __TestAccessCancelCpu(&waiters[4]);
    __TestAccessCancelCpu(&waiters[5]);
    assert(atomic_load(&g_rangeWakeCalls) == 5);

    // Admit a read batch together, but leave the next writer and reader asleep.
    SHMAccessInitialize(&holders[0], g_buffer, 0, 64);
    assert(SHMAccessBegin(
        &holders[0], 0, holders[0].Length,
        SHMAccessHolderDevice, true, NULL
    ) == OS_EOK);
    for (i = 0; i < 4; i++) {
        SHMAccessInitialize(&waiters[i], g_buffer, 0, 64);
        __QueueRangeWait(&waiters[i], i == 2);
    }
    atomic_store(&g_rangeWakeCalls, 0);
    SHMAccessEnd(&holders[0]);
    assert(atomic_load(&g_rangeWakeCalls) == 2);
    assert(atomic_load(&waiters[0].Event) == 1);
    assert(atomic_load(&waiters[1].Event) == 1);
    assert(atomic_load(&waiters[2].Event) == 0);
    assert(atomic_load(&waiters[3].Event) == 0);
    __TestAccessCancelCpu(&waiters[1]);
    assert(atomic_load(&g_rangeWakeCalls) == 2);
    __TestAccessCancelCpu(&waiters[0]);
    assert(atomic_load(&g_rangeWakeCalls) == 3);
    assert(atomic_load(&waiters[2].Event) == 1);
    __TestAccessCancelCpu(&waiters[2]);
    assert(atomic_load(&g_rangeWakeCalls) == 4);
    assert(atomic_load(&waiters[3].Event) == 1);
    __TestAccessCancelCpu(&waiters[3]);

    // A fault wakes all overlapping waiters, even those behind another waiter.
    for (i = 0; i < 2; i++) {
        SHMAccessInitialize(&holders[i], g_buffer, i * 64, 64);
        assert(SHMAccessBegin(
            &holders[i], 0, holders[i].Length,
            SHMAccessHolderDevice, false, NULL
        ) == OS_EOK);
    }
    SHMAccessInitialize(&waiters[0], g_buffer, 0, 64);
    SHMAccessInitialize(&waiters[1], g_buffer, 0, 64);
    SHMAccessInitialize(&waiters[2], g_buffer, 64, 64);
    __QueueRangeWait(&waiters[0], true);
    __QueueRangeWait(&waiters[1], false);
    __QueueRangeWait(&waiters[2], true);
    atomic_store(&g_rangeWakeCalls, 0);
    SHMAccessQuarantine(&holders[0]);
    assert(atomic_load(&g_rangeWakeCalls) == 2);
    assert(atomic_load(&waiters[0].Event) == 1);
    assert(atomic_load(&waiters[1].Event) == 1);
    assert(atomic_load(&waiters[2].Event) == 0);
    __TestAccessCancelCpu(&waiters[0]);
    __TestAccessCancelCpu(&waiters[1]);
    SHMAccessEnd(&holders[0]);
    SHMAccessEnd(&holders[1]);
    assert(atomic_load(&g_rangeWakeCalls) == 3);
    assert(atomic_load(&waiters[2].Event) == 1);
    __TestAccessCancelCpu(&waiters[2]);

    assert(g_buffer->Access.First == NULL && g_buffer->Access.Last == NULL);
    assert(atomic_load(&g_buffer->Access.Waiting) == 0);
    assert(atomic_load(&g_buffer->Access.Faults) == 0);
    assert(DestroyHandle(0x20) == OS_EOK);
    puts("SHM wake selection: writer order, disjoint progress, reader batches, cancellation and faults passed");
}

static void
__TestAccessStates(void)
{
    const paddr_t pages[] = { 0x700000 };
    struct SHMAccess holder;
    struct SHMAccess waiter;
    int event;

    __CreateBuffer(pages, 1, 0, 0x1000, 0);
    SHMAccessInitialize(&holder, g_buffer, 0, 64);
    SHMAccessInitialize(&waiter, g_buffer, 0, 256);
    assert(holder.Phase == SHMAccessIdle && waiter.Phase == SHMAccessIdle);
    assert(holder.Holder == SHMAccessHolderNone && waiter.Holder == SHMAccessHolderNone);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderDevice, true, NULL
    ) == OS_EOK);
    assert(holder.Phase == SHMAccessActive && holder.Holder == SHMAccessHolderDevice);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EBUSY);
    assert(holder.Phase == SHMAccessActive && holder.Holder == SHMAccessHolderDevice);

    __QueueRangeWait(&waiter, true);
    assert(waiter.Phase == SHMAccessWaiting && g_buffer->Access.Waiting == 1);
    assert(waiter.Holder == SHMAccessHolderDevice);
    // The fixture stops the suspended caller before quarantining its request.
    SHMAccessQuarantine(&waiter);
    SHMAccessQuarantine(&waiter);
    assert(waiter.Phase == SHMAccessQuarantined && waiter.Holder == SHMAccessHolderDevice);
    assert(g_buffer->Access.Waiting == 0 && g_buffer->Access.Faults == 1);
    SHMAccessEnd(&holder);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EDEVFAULT);
    assert(holder.Phase == SHMAccessIdle && holder.Holder == SHMAccessHolderNone);

    SHMAccessEnd(&waiter);
    SHMAccessEnd(&waiter);
    assert(waiter.Phase == SHMAccessIdle && g_buffer->Access.Faults == 0);
    assert(waiter.Holder == SHMAccessHolderNone);
    // Reusing a previously quarantined slot must not preserve fault state.
    assert(SHMAccessBegin(
        &waiter, 0, waiter.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EOK);
    assert(waiter.Phase == SHMAccessActive && waiter.Holder == SHMAccessHolderCpu);
    SHMAccessEnd(&waiter);
    assert(waiter.Phase == SHMAccessIdle && waiter.Holder == SHMAccessHolderNone);
    // Each begin selects its range without resetting the futex sequence.
    // Different bytes on the same cache line still conflict.
    event = atomic_load(&waiter.Event);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderDevice, true, NULL
    ) == OS_EOK);
    assert(SHMAccessBegin(&waiter, 128, 64, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    assert(atomic_load(&waiter.Event) == event);
    assert(SHMAccessBegin(&waiter, 0, 64, SHMAccessHolderCpu, true, NULL) == OS_EBUSY);
    assert(waiter.First == 128 && waiter.Last == 191);
    SHMAccessEnd(&waiter);
    assert(SHMAccessBegin(
        &waiter, 63, 2,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EBUSY);
    SHMAccessEnd(&holder);

    // Shared storage can be quarantined before any begin. Include the fixed
    // offset and cover its tail, without blocking a disjoint reservation.
    SHMAccessInitialize(&waiter, g_buffer, 64, 128);
    SHMAccessQuarantine(&waiter);
    assert(waiter.Phase == SHMAccessQuarantined && waiter.Holder == SHMAccessHolderDevice);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EOK);
    SHMAccessEnd(&holder);
    SHMAccessInitialize(&holder, g_buffer, 128, 64);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EDEVFAULT);
    SHMAccessEnd(&waiter);
    assert(SHMAccessBegin(
        &holder, 0, holder.Length,
        SHMAccessHolderCpu, false, NULL
    ) == OS_EOK);
    SHMAccessEnd(&holder);
    assert(g_buffer->Access.First == NULL && g_buffer->Access.Last == NULL);
    assert(DestroyHandle(0x20) == OS_EOK);
}
