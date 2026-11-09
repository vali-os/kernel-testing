// Exercise the production CPU syscalls and handle lifecycle together.
#include "../kernel/api/shm.c"
#undef __MODULE
#include "../kernel/api/handles.c"

static bool g_copyFails;
static bool g_beginForks;
static bool g_closeDuringBegin;
static uuid_t g_cpuHandle;
static uuid_t g_bufferHandle;
static unsigned int g_sessions;
static unsigned int g_bufferDestroyed;

static void
__DestroyTestBuffer(
    _In_ void* resource)
{
    (void)resource;
    g_bufferDestroyed++;
}

oserr_t
MemorySpaceCopyUser(
    _In_ void*  user,
    _In_ void*  kernel,
    _In_ size_t length,
    _In_ bool   write)
{
    if (write) {
        // The result may become visible to another thread during this copy.
        assert(ScResourceRelease(*(uuid_t*)kernel) == OS_EBUSY);
        assert(ScDestroyHandle(*(uuid_t*)kernel) == OS_EINVALPARAMS);
        if (g_copyFails) {
            return OS_EINVALPARAMS;
        }
        memcpy(user, kernel, length);
    } else {
        memcpy(kernel, user, length);
    }
    return OS_EOK;
}

oserr_t
SHMAccessCreate(
    _In_  uuid_t             shm,
    _In_  size_t             offset,
    _In_  size_t             length,
    _Out_ struct SHMAccess** accessOut)
{
    struct SHMAccess* access;
    void* buffer;
    oserr_t status;

    assert(offset == 16 && length == 64);
    status = AcquireHandleReference(shm, HandleTypeSHM, &buffer);
    if (status != OS_EOK) {
        return status;
    }
    access = calloc(1, sizeof(*access));
    assert(access != NULL);
    access->Length = length;
    access->Buffer = (struct SHMBuffer*)(uintptr_t)shm;
    *accessOut = access;
    g_sessions++;
    return OS_EOK;
}

oserr_t
SHMAccessDestroy(
    _InOut_ struct SHMAccess** access)
{
    if ((*access)->Phase != SHMAccessIdle) {
        return OS_EBUSY;
    }
    ReleaseGlobalHandle((uuid_t)(uintptr_t)(*access)->Buffer);
    free(*access);
    *access = NULL;
    g_sessions--;
    return OS_EOK;
}

void
SHMAccessEnd(
    _In_ struct SHMAccess* access)
{
    access->Phase = SHMAccessIdle;
    access->Holder = SHMAccessHolderNone;
}

oserr_t
SHMAccessBegin(
    _In_ struct SHMAccess*           access,
    _In_ size_t                      offset,
    _In_ size_t                      length,
    _In_ enum SHMAccessHolder        holder,
    _In_ bool                        write,
    _In_ const struct SHMAccessWait* wait)
{
    (void)write;
    assert(offset == 0 && length == 64 && holder == SHMAccessHolderCpu);
    if (g_closeDuringBegin) {
        assert(ScResourceRelease(g_cpuHandle) == OS_EBUSY);
    }
    if (access->Phase != SHMAccessIdle) {
        return OS_EBUSY;
    }
    access->Holder = holder;
    access->Phase = g_beginForks ? SHMAccessWaiting : SHMAccessActive;
    if (g_beginForks) {
        assert(wait->Async != NULL && wait->Blocking);
        return OS_EFORKED;
    }
    return OS_EOK;
}

static void
__TestCpuSessions(void)
{
    MSContextSecurity_t context = { .GlobalHandleID = 10 };
    MSContextSecurity_t other = { .GlobalHandleID = 11 };
    struct SHMAccessBeginParameters parameters = {
        .Kind = SHMAccessCpu,
        .Mode = SHMWaitNonBlocking
    };
    struct SHMRecord* session;
    OSAsyncContext_t async = { 0 };
    uuid_t pending;
    uuid_t unpublished = 123;
    size_t live;

    context.HandleTable = CreateHandleGrantsTable();
    other.HandleTable = CreateHandleGrantsTable();
    g_currentContext = &context;
    g_bufferHandle = CreateHandle(HandleTypeSHM, NULL, __DestroyTestBuffer, (void*)42);
    assert(g_bufferHandle != UUID_INVALID);
    live = g_live;
    g_copyFails = true;
    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &unpublished) == OS_EINVALPARAMS);
    assert(unpublished == 123);
    DrainJanitor();
    assert(g_sessions == 0 && g_live == live);
    g_copyFails = false;
    g_failRecords = true;
    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &unpublished) == OS_EOOM);
    g_failRecords = false;
    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &g_cpuHandle) == OS_EOK);
    assert(GrantHandle(&other, g_cpuHandle) == OS_EPERMISSIONS);
    assert(ScShmAccessBegin(NULL, g_bufferHandle, &parameters) == OS_EINVALPARAMS);
    g_currentContext = &other;
    assert(ScShmAccessBegin(NULL, g_cpuHandle, &parameters) == OS_ENOENT);
    assert(ScResourceRelease(g_cpuHandle) == OS_ENOENT);
    g_currentContext = &context;
    g_closeDuringBegin = true;
    assert(ScShmAccessBegin(NULL, g_cpuHandle, &parameters) == OS_EOK);
    g_closeDuringBegin = false;
    assert(ScResourceRelease(g_cpuHandle) == OS_EBUSY);
    assert(ScShmAccessEnd(g_cpuHandle, DMAAccessUse, 0) == OS_EINVALPARAMS);
    assert(ScShmAccessEnd(g_cpuHandle, SHMAccessCpu, 0) == OS_EOK);
    assert(ScShmAccessEnd(g_cpuHandle, SHMAccessCpu, 0) == OS_EINVALPARAMS);

    // A call can have a reference but not yet have entered the session state.
    assert(AcquireHandle(g_cpuHandle, HandleTypeSHMAccess, (void**)&session) == OS_EOK);
    assert(ScResourceRelease(g_cpuHandle) == OS_EOK);
    DrainJanitor();
    assert(atomic_load(&session->State) == SHMRecordClosed);
    assert(ScShmAccessBegin(NULL, g_cpuHandle, &parameters) == OS_ENOENT);
    assert(ScResourceRelease(g_cpuHandle) == OS_ENOENT);
    assert(DestroyHandle(g_cpuHandle) == OS_EOK);
    DrainJanitor();
    assert(g_sessions == 0);

    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &g_cpuHandle) == OS_EOK);
    parameters.Mode = SHMWaitUntilAvailable;
    g_beginForks = true;
    assert(ScShmAccessBegin(&async, g_cpuHandle, &parameters) == OS_EFORKED);
    assert(ScResourceRelease(g_cpuHandle) == OS_EBUSY);
    assert(ScDestroyHandle(g_cpuHandle) == OS_EINVALPARAMS);
    assert(ScDestroyHandle(g_cpuHandle) == OS_EINVALPARAMS);
    session = LookupHandle(g_cpuHandle, HandleTypeSHMAccess);
    assert(session != NULL && ((struct SHMAccess*)session->Object)->Phase == SHMAccessWaiting);
    // Model the suspended child returning from the backend begin call.
    ((struct SHMAccess*)session->Object)->Phase = SHMAccessActive;
    SHMRecordEnd(session);
    assert(ScShmAccessEnd(g_cpuHandle, SHMAccessCpu, 0) == OS_EOK);
    assert(ScResourceRelease(g_cpuHandle) == OS_EOK);
    DrainJanitor();

    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &pending) == OS_EOK);
    assert(ScShmAccessBegin(&async, pending, &parameters) == OS_EFORKED);
    g_beginForks = false;
    assert(ScShmAccessCreate(g_bufferHandle, 16, 64, &g_cpuHandle) == OS_EOK);
    assert(ScShmAccessBegin(NULL, g_cpuHandle, &parameters) == OS_EOK);
    // Exit drops the buffer grant before deferred session destruction runs.
    g_currentContext = &other;
    DestroyHandleGrantsTable(context.HandleTable);
    assert(g_sessions == 0 && g_bufferDestroyed == 0);
    DrainJanitor();
    assert(g_sessions == 0 && g_bufferDestroyed == 1);
    DestroyHandleGrantsTable(other.HandleTable);
    g_currentContext = NULL;
    DrainJanitor();
    assert(g_live == 0 && g_handles.element_count == 0);
    puts("CPU sessions: publication, close, async references and context exit passed");
}
