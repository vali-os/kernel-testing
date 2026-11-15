// Reuse the SHM backing, scope and allocation harness. Include the region engine
// to exercise generation and admission counter exhaustion without billions of jobs.
#include "../kernel/memory/shm/region.c"
#define main __RunSHMDeviceTests
#include "shm_device_test.c"
#undef main
#include "../kernel/api/shm_region.c"
#include <os/shm_region.h>


static _Thread_local bool g_interruptsDisabled;
static _Thread_local struct SHMRegionCpuSession** g_abandonOnRestore;

irqstate_t
InterruptDisable(void)
{
    irqstate_t previous = g_interruptsDisabled;

    g_interruptsDisabled = true;
    return previous;
}

irqstate_t
InterruptRestoreState(
    _In_ irqstate_t state)
{
    struct SHMRegionCpuSession** abandoned = g_abandonOnRestore;

    assert(g_interruptsDisabled);
    g_interruptsDisabled = state;
    if (abandoned != NULL && !state) {
        g_abandonOnRestore = NULL;
        assert(SHMRegionCpuDestroy(abandoned, true) == OS_EOK);
    }
    return state;
}

static struct SHMRecord g_records[8];
static bool g_copyFailure;
static unsigned int g_syscalls;

oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    if (g_copyFailure) {
        return OS_EINVALPARAMS;
    }
    memcpy(destination, source, length);
    return OS_EOK;
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       destination,
    _In_  const void* source,
    _In_  size_t      length)
{
    return MemorySpaceCopyFromUser(destination, source, length);
}

oserr_t
SHMRecordCreate(
    _In_  const struct SHMRecordOps* ops,
    _Out_ struct SHMRecord**         recordOut)
{
    unsigned int i;

    for (i = 0; i < 8; i++) {
        if (g_records[i].Handle == 0) {
            memset(&g_records[i], 0, sizeof(g_records[i]));
            g_records[i].Handle = i + 1;
            g_records[i].Ops = ops;
            atomic_init(&g_records[i].State, SHMRecordUpdating);
            *recordOut = &g_records[i];
            return OS_EOK;
        }
    }
    return OS_EOOM;
}

oserr_t
SHMRecordBegin(
    _In_  uuid_t                     handle,
    _In_  const struct SHMRecordOps* ops,
    _Out_ struct SHMRecord**         recordOut)
{
    struct SHMRecord* record;

    g_syscalls++;
    if (handle == 0 || handle > 8) {
        return OS_ENOENT;
    }
    record = &g_records[handle - 1];
    if (record->Handle != handle || (ops != NULL && ops != record->Ops)) {
        return OS_ENOENT;
    }
    if (atomic_load(&record->State) != SHMRecordIdle) {
        return OS_EBUSY;
    }
    atomic_store(&record->State, SHMRecordUpdating);
    *recordOut = record;
    return OS_EOK;
}

void
SHMRecordEnd(
    _In_ struct SHMRecord* record)
{
    atomic_store(&record->State, SHMRecordIdle);
}

oserr_t
SHMRecordRelease(
    _In_ struct SHMRecord* record)
{
    if (record->Object == NULL) {
        return OS_EOK;
    }
    return record->Ops->Release(record, &record->Object);
}

void
SHMRecordRemove(
    _In_ struct SHMRecord* record)
{
    assert(record->Object == NULL);
    record->Handle = 0;
}

static oserr_t
__ReleaseResource(
    _In_ uuid_t handle)
{
    struct SHMRecord* record;
    oserr_t           status;

    status = SHMRecordBegin(handle, NULL, &record);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMRecordRelease(record);
    if (status == OS_EOK) {
        SHMRecordRemove(record);
    } else {
        SHMRecordEnd(record);
    }
    return status;
}

#define __INTERNAL_CRT_SYSCALLS__
#define Syscall_ShmRegionsRegister ScShmRegionsRegister
#define Syscall_ShmRegionSessionOpen ScShmRegionSessionOpen
#define Syscall_ShmRegionBeginCpu ScShmRegionBeginCpu
#define Syscall_ShmRegionEndCpu ScShmRegionEndCpu
#define Syscall_ResourceRelease __ReleaseResource
#include "../librt/libos/shm_region.c"

static void
__TestRegionSetup(void)
{
    const paddr_t pages[] = { 0x700000 };
    struct SHMRegionRange ranges[] = { { 0, 128 }, { 128, 128 } };
    struct SHMRegionRange invalid[] = { { 64, 128 }, { 128, 128 } };
    struct SHMRegionSession session = { 0 };
    struct SHMAccess* legacy = NULL;
    struct SHMAccess initialized;
    OSHandle_t buffer = { .ID = 0x20, .Type = OSHANDLE_SHM };
    unsigned int live;
    struct SHMDeviceRange deviceRange = { 0x700000, 0x1000, 0x10000 };
    struct SHMDeviceContext* context;
    struct SHMDeviceMapping* mapping = NULL;

    __CreateBuffer(pages, 1, 0, 4096, SHM_COMMIT | SHM_REGIONS);
    live = atomic_load(&g_liveAllocations);
    assert(SHMRegionsRegister(&buffer, ranges, 0) == OS_EINVALPARAMS);
    assert(SHMRegionsRegister(&buffer, ranges, SHM_REGION_LIMIT + 1) == OS_EINVALPARAMS);
    assert(SHMRegionsRegister(&buffer, invalid, 2) == OS_EINVALPARAMS);
    invalid[0] = (struct SHMRegionRange) { 1, 64 };
    assert(SHMRegionsRegister(&buffer, invalid, 1) == OS_EINVALPARAMS);
    invalid[0] = (struct SHMRegionRange) { SIZE_MAX - 63, 128 };
    assert(SHMRegionsRegister(&buffer, invalid, 1) == OS_EINVALPARAMS);
    g_copyFailure = true;
    assert(SHMRegionsRegister(&buffer, ranges, 2) == OS_EINVALPARAMS);
    g_copyFailure = false;
    g_failAtCall = g_allocationCalls + 2;
    assert(SHMRegionsRegister(&buffer, ranges, 2) == OS_EOOM);
    g_failAtCall = 0;
    assert(g_buffer->Regions == NULL);
    assert(atomic_load(&g_liveAllocations) == live);
    assert(SHMRegionSessionOpen(&buffer, 0, 2, &session) == OS_ENOENT);
    assert(SHMRegionsRegister(&buffer, ranges, 2) == OS_EOK);
    assert(SHMRegionsRegister(&buffer, ranges, 2) == OS_EBUSY);
    live = atomic_load(&g_liveAllocations);
    g_copyFailure = true;
    assert(SHMRegionSessionOpen(&buffer, 0, 2, &session) == OS_EINVALPARAMS);
    g_copyFailure = false;
    g_failAllocation = true;
    assert(SHMRegionSessionOpen(&buffer, 0, 2, &session) == OS_EOOM);
    g_failAllocation = false;
    assert(SHMRegionSessionOpen(&buffer, 0, 0, &session) == OS_EINVALPARAMS);
    assert(SHMRegionSessionOpen(&buffer, 2, 2, &session) == OS_ENOENT);
    assert(session.Handle == 0 && g_bufferReferences == 1);
    assert(atomic_load(&g_liveAllocations) == live);
    assert(SHMAccessCreate(0x20, 0, 64, &legacy) == OS_ENOTSUPPORTED);
    context = __CreateContext(&deviceRange, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    assert(SHMDeviceMap(NULL, context, 0x20, 0, 128, DMAToDevice,
        SHMDeviceMappingPersistent, NULL, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL);
    SHMDeviceContextRelease(&context);
    SHMAccessInitialize(&initialized, g_buffer, 0, 64);
    assert(SHMAccessBegin(&initialized, 0, 64, SHMAccessHolderCpu, false, NULL) == OS_ENOTSUPPORTED);
}

static void
__TestRegionTokens(void)
{
    OSHandle_t buffer = { .ID = 0x20, .Type = OSHANDLE_SHM };
    OSHandle_t restricted = { .ID = 0x21, .Type = OSHANDLE_SHM };
    struct SHMRegionSession first = { 0 };
    struct SHMRegionSession second = { 0 };
    struct SHMRegionSession other = { 0 };
    struct SHMRegionCpuToken a = { 0 };
    struct SHMRegionCpuToken b = { 0 };
    struct SHMRegionCpuToken c = { 0 };
    struct SHMRegionCpuToken slot = { .Slot = 1 };
    struct SHMRegionCpuSession* engine;
    struct SHMView view = { .Buffer = g_buffer, .Offset = 0, .Length = 128, .Scope = GrantTokenScopeRead };
    unsigned int allocations;
    unsigned int calls;

    g_restricted = &view;
    assert(SHMRegionsRegister(&restricted, &(struct SHMRegionRange) { 0, 128 }, 1) == OS_EPERMISSIONS);
    view.Length = 64;
    assert(SHMRegionSessionOpen(&restricted, 0, 2, &second) == OS_EPERMISSIONS);
    view.Length = 128;
    assert(SHMRegionSessionOpen(&restricted, 1, 2, &second) == OS_EPERMISSIONS);
    assert(SHMRegionSessionOpen(&restricted, 0, 2, &second) == OS_EOK);
    assert(SHMRegionSessionOpen(&buffer, 0, 2, &first) == OS_EOK);
    assert(SHMRegionSessionOpen(&buffer, 1, 1, &other) == OS_EOK);
    assert(memcmp(&first.Identity, &second.Identity, sizeof(first.Identity)) == 0);
    assert(first.Identity.Buffer == 0x20 && first.Identity.Generation == 1);
    engine = g_records[first.Handle - 1].Object;
    allocations = g_allocationCalls;
    calls = g_syscalls;
    g_failAllocation = true;
    assert(SHMRegionTryBeginCpu(&first, &a, SHMCpuAccessRead) == OS_EOK);
    assert(SHMRegionEndCpu(&first, &a) == OS_EOK);
    assert(g_syscalls == calls + 2);
    assert(SHMRegionTryBeginCpu(&first, &a, SHMCpuAccessWrite) == OS_EOK);
    assert(SHMRegionTryBeginCpu(&second, &b, SHMCpuAccessRead) == OS_EBUSY);
    assert(b.Generation == 0 && !b.Active);
    assert(SHMRegionTryBeginCpu(&other, &c, SHMCpuAccessWrite) == OS_EOK);
    assert(SHMRegionSessionClose(&first) == OS_EBUSY);
    assert(ScShmRegionEndCpu(first.Handle, a.Slot, a.Generation - 1) == OS_ENOENT);
    assert(SHMRegionTryBeginCpu(&second, &b, SHMCpuAccessRead) == OS_EBUSY);
    assert(SHMRegionEndCpu(&first, &a) == OS_EOK);
    assert(ScShmRegionEndCpu(first.Handle, a.Slot, a.Generation) == OS_ENOENT);
    assert(SHMRegionTryBeginCpu(&second, &b, SHMCpuAccessWrite) == OS_EPERMISSIONS);
    assert(SHMRegionTryBeginCpu(&first, &a, SHMCpuAccessRead) == OS_EOK);
    assert(SHMRegionTryBeginCpu(&second, &b, SHMCpuAccessRead) == OS_EOK);
    assert(SHMRegionTryBeginCpu(&first, &slot, SHMCpuAccessWrite) == OS_EBUSY);
    assert(SHMRegionEndCpu(&first, &a) == OS_EOK);
    assert(SHMRegionTryBeginCpu(&first, &slot, SHMCpuAccessWrite) == OS_EBUSY);
    assert(SHMRegionEndCpu(&second, &b) == OS_EOK);
    assert(SHMRegionTryBeginCpu(&first, &slot, SHMCpuAccessWrite) == OS_EOK);
    assert(SHMRegionEndCpu(&first, &slot) == OS_EOK);
    assert(SHMRegionEndCpu(&other, &c) == OS_EOK);
    assert(ScShmRegionBeginCpu(first.Handle, 2, 1, SHMCpuAccessRead) == OS_EINVALPARAMS);
    assert(ScShmRegionBeginCpu(first.Handle, 0, 0, SHMCpuAccessRead) == OS_EINVALPARAMS);
    assert(ScShmRegionBeginCpu(first.Handle, 0, 1, SHMCpuAccessRead) == OS_ENOENT);
    atomic_store(&engine->Region->Users, UINT_MAX - 1);
    assert(SHMRegionTryBeginCpu(&first, &a, SHMCpuAccessRead) == OS_EOVERFLOW);
    atomic_store(&engine->Region->Users, 0);
    engine->Tokens[0].Generation = UINT_MAX;
    assert(ScShmRegionBeginCpu(first.Handle, 0, 1, SHMCpuAccessRead) == OS_EOVERFLOW);
    assert(g_allocationCalls == allocations);
    g_failAllocation = false;
    assert(SHMRegionSessionClose(&first) == OS_EOK);
    assert(SHMRegionSessionClose(&second) == OS_EOK);
    assert(SHMRegionSessionClose(&other) == OS_EOK);
    assert(g_restrictedReferences == 0);
    g_restricted = NULL;
}

/** Each worker uses its own session, like independent queues or processes. */
struct __RegionWorker {
    struct SHMRegionCpuSession* Session;
    unsigned int*              Value;
};

static void*
__RegionWorkerRun(
    _In_ void* argument)
{
    struct __RegionWorker* worker = argument;
    unsigned int           i;
    oserr_t                status;

    for (i = 1; i <= 1000; i++) {
        do {
            status = SHMRegionCpuBegin(worker->Session, 0, i, SHMCpuAccessWrite);
            if (status == OS_EBUSY) {
                sched_yield();
            }
        } while (status == OS_EBUSY);
        assert(status == OS_EOK);
        (*worker->Value)++;
        assert(SHMRegionCpuEnd(worker->Session, 0, i) == OS_EOK);
    }
    return NULL;
}

static void
__TestConcurrentRegions(void)
{
    struct __RegionWorker workers[4];
    OSBufferIdentity_t identity;
    pthread_t threads[4];
    unsigned int value = 0;
    unsigned int i;
    unsigned int allocations;
    struct SHMRegionCpuSession* interrupted;

    // Termination at the first enabled interrupt sees either a complete active
    // token or a fully ended one. Neither case may strand or double-release it.
    assert(SHMRegionCpuCreate(0x20, 0, 1, &interrupted, &identity) == OS_EOK);
    g_abandonOnRestore = &interrupted;
    assert(SHMRegionCpuBegin(interrupted, 0, 1, SHMCpuAccessWrite) == OS_EOK);
    assert(interrupted == NULL && atomic_load(&g_buffer->Regions->Entries[0].Users) == 0);
    assert(SHMRegionCpuCreate(0x20, 0, 1, &interrupted, &identity) == OS_EOK);
    assert(SHMRegionCpuBegin(interrupted, 0, 1, SHMCpuAccessRead) == OS_EOK);
    g_abandonOnRestore = &interrupted;
    assert(SHMRegionCpuEnd(interrupted, 0, 1) == OS_EOK);
    assert(interrupted == NULL && atomic_load(&g_buffer->Regions->Entries[0].Users) == 0);

    for (i = 0; i < 4; i++) {
        assert(SHMRegionCpuCreate(0x20, 0, 1, &workers[i].Session, &identity) == OS_EOK);
        workers[i].Value = &value;
    }
    allocations = g_allocationCalls;
    for (i = 0; i < 4; i++) {
        assert(pthread_create(&threads[i], NULL, __RegionWorkerRun, &workers[i]) == 0);
    }
    for (i = 0; i < 4; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    assert(value == 4000 && g_allocationCalls == allocations);
    assert(SHMRegionCpuBegin(workers[0].Session, 0, 1001, SHMCpuAccessWrite) == OS_EOK);
    assert(SHMRegionCpuDestroy(&workers[0].Session, false) == OS_EBUSY);
    assert(SHMRegionCpuDestroy(&workers[0].Session, true) == OS_EOK);
    assert(SHMRegionCpuBegin(workers[1].Session, 0, 1001, SHMCpuAccessWrite) == OS_EOK);
    assert(SHMRegionCpuEnd(workers[1].Session, 0, 1001) == OS_EOK);
    for (i = 1; i < 4; i++) {
        assert(SHMRegionCpuDestroy(&workers[i].Session, false) == OS_EOK);
    }
}

int
main(void)
{
    assert(__RunSHMDeviceTests() == 0);
    __TestRegionSetup();
    __TestRegionTokens();
    __TestConcurrentRegions();
    assert(g_bufferReferences == 1);
    kfree(g_buffer->Regions);
    g_buffer->Regions = NULL;
    assert(DestroyHandle(0x20) == OS_EOK);
    assert(atomic_load(&g_liveAllocations) == 0);
    puts("SHM regions: setup rollback, aliases, tokens, wrappers and concurrency passed");
    return 0;
}
