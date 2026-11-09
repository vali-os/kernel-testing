static int g_cpuSessions;
static bool g_forkWait;
static bool g_cpuWrite;
static OSTimestamp_t* g_cpuDeadline;

oserr_t
SHMAccessCreate(uuid_t shm, size_t offset, size_t length, struct SHMAccess** access)
{
    CHECK(shm == 99 && offset == 16 && length == 64);
    if (g_engineFailure) {
        return OS_EOOM;
    }
    *access = calloc(1, sizeof(**access));
    (*access)->Offset = offset;
    (*access)->Length = length;
    g_cpuSessions++;
    return OS_EOK;
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
    CHECK(offset == 0 && length == access->Length && length == 64);
    g_cpuWrite = write;
    access->Deadline = wait != NULL && wait->Deadline != NULL ? *wait->Deadline : *OS_DEADLINE_INFINITE;
    g_cpuDeadline = &access->Deadline;
    if (access->Phase != SHMAccessIdle) return OS_EBUSY;
    access->Holder = holder;
    access->Phase = SHMAccessWaiting;
    if (wait != NULL && wait->Blocking && g_forkWait) {
        CHECK(wait->Async != NULL);
        return OS_EFORKED;
    }
    access->Phase = SHMAccessActive;
    return OS_EOK;
}

void
SHMAccessEnd(struct SHMAccess* access)
{
    access->Phase = SHMAccessIdle;
    access->Holder = SHMAccessHolderNone;
}

oserr_t
SHMAccessDestroy(struct SHMAccess** access)
{
    if (g_releaseRecord != NULL) {
        CHECK(g_releaseRecord->Object == NULL);
    }
    if ((*access)->Phase != SHMAccessIdle) return OS_EBUSY;
    free(*access);
    *access = NULL;
    g_cpuSessions--;
    return OS_EOK;
}

oserr_t
SHMDeviceAbandon(struct SHMDeviceMapping** mapping)
{
    if ((*mapping)->Owned || (*mapping)->Uses != 0) {
        (*mapping)->Next = (*mapping)->Context->Quarantine;
        (*mapping)->Context->Quarantine = *mapping;
        *mapping = NULL;
        return OS_EOK;
    }
    (*mapping)->Access.Phase = SHMAccessIdle;
    (*mapping)->Access.Holder = SHMAccessHolderNone;
    return SHMDeviceUnmap(mapping);
}

void
SHMDeviceReleaseQuarantine(struct SHMDeviceContext* context)
{
    struct SHMDeviceMapping* mapping;

    while (context->Quarantine != NULL) {
        mapping = context->Quarantine;
        context->Quarantine = mapping->Next;
        mapping->Owned = 0;
        mapping->Uses = 0;
        mapping->Access.Phase = SHMAccessIdle;
        mapping->Access.Holder = SHMAccessHolderNone;
        CHECK(SHMDeviceUnmap(&mapping) == OS_EOK);
    }
}

static void
__TestRangeSyscalls(const struct OSDmaOpenParameters* parameters)
{
    struct OSDmaMapParameters map = { .Offset = 16, .Length = 64, .Direction = DMAFromDevice,
        .Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT };
    struct OSDmaMappingResult active = { 0 };
    struct OSDmaMappingResult idle = { 0 };
    struct SHMAccessBeginParameters wait = { .Kind = SHMAccessCpu, .Write = true };
    OSAsyncContext_t async = { 0 };
    struct SHMRecord* parent;
    struct SHMDeviceContext* recovery;
    struct SHMRecord* session;
    HandleGrantsTable_t* ownerTable;
    HandleGrantsTable_t* otherTable;
    uuid_t cpu = 0;
    uuid_t pending = 0;
    uuid_t pendingInfinite[2] = { 0 };
    uuid_t other = 0;
    uuid_t context = 0;
    int i;

    cpu = 0xdead;
    g_engineFailure = 1;
    CHECK(ScShmAccessCreate(99, 16, 64, &cpu) == OS_EOOM);
    __TestHandlesDrain();
    CHECK(cpu == 0xdead && g_cpuSessions == 0 && g_live == 0);
    g_engineFailure = 0;
    CHECK(ScShmAccessCreate(99, 16, 64, &cpu) == OS_EOK);
    CHECK(__TestBeginUse(cpu) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(cpu) == OS_EOK && g_cpuSessions == 0);
    CHECK(ScShmAccessCreate(99, 16, 64, &cpu) == OS_EOK);
    CHECK(__TestBeginCpu(cpu, (enum SHMCpuAccess)0) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(cpu, (enum SHMCpuAccess)99) == OS_EINVALPARAMS);
    wait.Offset = 1;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Offset = 0;
    wait.Length = 1;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Length = 0;
    wait.Mode = (enum SHMWaitMode)99;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Mode = SHMWaitUntilAvailable;
    wait.Deadline.Nanoseconds = 1000000000;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Deadline = *OS_DEADLINE_INFINITE;
    wait.Mode = SHMWaitNonBlocking;
    wait.Kind = DMAAccessUse;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Kind = DMAAccessTransfer;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Kind = (unsigned int)99;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EINVALPARAMS);
    wait.Kind = SHMAccessCpu;
    wait.Write = true;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EOK);
    CHECK(g_cpuWrite);
    // Rejected end requests must leave CPU access active.
    CHECK(ScShmAccessEnd(cpu, (unsigned int)-1, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(cpu, (unsigned int)99, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(cpu, DMAAccessTransfer, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(cpu, DMAAccessUse, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(cpu, SHMAccessCpu, 1) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(cpu, SHMCpuAccessRead) == OS_EBUSY);
    CHECK(ScShmAccessEnd(cpu, SHMAccessCpu, 0) == OS_EOK);
    wait.Kind = SHMAccessCpu;
    wait.Write = false;
    CHECK(ScShmAccessBegin(NULL, cpu, &wait) == OS_EOK);
    CHECK(!g_cpuWrite);

    CHECK(ScResourceRelease(cpu) == OS_EBUSY);
    CHECK(__TestBeginCpu(cpu, SHMCpuAccessRead) == OS_EBUSY);
    CHECK(ScShmAccessEnd(cpu, SHMAccessCpu, 0) == OS_EOK);
    CHECK(ScShmAccessEnd(cpu, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(cpu) == OS_EOK && g_cpuSessions == 0);
    g_releaseRecord = NULL;
    g_copyFailure = 1;
    CHECK(ScShmAccessCreate(99, 16, 64, &cpu) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_cpuSessions == 0);
    g_copyFailure = 0;

    CHECK(ScDmaOpen(7, parameters, &context) == OS_EOK);
    CHECK(ScResourceRelease(context) == OS_EOK);
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    CHECK(ScDmaOpen(7, parameters, &context) == OS_EOK);
    CHECK(__TestBeginCpu(context, SHMCpuAccessRead) == OS_EINVALPARAMS);
    CHECK(ScShmAccessBegin(NULL, context, &wait) == OS_EINVALPARAMS);
    CHECK(__TestBeginUse(context) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(context, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(context, DMAAccessUse, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(context, DMAAccessTransfer, 64) == OS_EINVALPARAMS);
    map.Flags = DMA_MAP_REQUIRE_DIRECT;
    CHECK(ScDmaMapShm(context, 99, &map, &idle) == OS_EOK);
    CHECK(__TestBeginCpu(idle.Handle, SHMCpuAccessRead) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(idle.Handle, SHMCpuAccessWrite) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(context) == OS_EBUSY);
    wait.Kind = DMAAccessTransfer;
    wait.Offset = 0;
    wait.Length = 64;
    CHECK(ScShmAccessBegin(NULL, context, &wait) == OS_EINVALPARAMS);
    wait.Write = true;
    CHECK(ScShmAccessBegin(NULL, idle.Handle, &wait) == OS_EINVALPARAMS);
    wait.Write = false;
    CHECK(ScShmAccessBegin(NULL, idle.Handle, &wait) == OS_EOK);
    CHECK(ScShmAccessEnd(idle.Handle, DMAAccessUse, 0) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(idle.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(idle.Handle) == OS_EBUSY);
    CHECK(ScShmAccessEnd(idle.Handle, DMAAccessTransfer, 64) == OS_EOK);
    wait.Kind = DMAAccessUse;
    CHECK(ScShmAccessBegin(NULL, idle.Handle, &wait) == OS_EINVALPARAMS);
    wait.Kind = (unsigned int)-1;
    CHECK(ScShmAccessBegin(NULL, idle.Handle, &wait) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(idle.Handle) == OS_EOK);
    wait.Kind = SHMAccessCpu;
    wait.Write = true;
    wait.Offset = 0;
    wait.Length = 0;
    map.Flags |= DMA_MAP_PERSISTENT;
    CHECK(ScDmaMapShm(context, 99, &map, &active) == OS_EOK);
    CHECK(ScDmaMapShm(context, 99, &map, &idle) == OS_EOK);
    CHECK(__TestBeginCpu(idle.Handle, SHMCpuAccessRead) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(idle.Handle, SHMCpuAccessWrite) == OS_EINVALPARAMS);
    CHECK(__TestBeginUse(active.Handle) == OS_EOK);
    CHECK(__TestBeginCpu(idle.Handle, SHMCpuAccessRead) == OS_EINVALPARAMS);
    CHECK(ScShmAccessCreate(99, 16, 64, &cpu) == OS_EOK);
    CHECK(__TestBeginCpu(cpu, SHMCpuAccessWrite) == OS_EOK);
    CHECK(ScShmAccessCreate(99, 16, 64, &pending) == OS_EOK);
    g_forkWait = true;
    wait.Mode = SHMWaitUntilAvailable;
    wait.Deadline.Seconds = 123;
    CHECK(ScShmAccessBegin(&async, pending, &wait) == OS_EFORKED);
    CHECK(g_cpuDeadline != NULL && g_cpuDeadline != &wait.Deadline);
    wait.Deadline.Seconds = 0;
    CHECK(g_cpuDeadline->Seconds == 123);
    CHECK(ScResourceRelease(pending) == OS_EBUSY);

    // Both explicit and default far-future deadlines must survive the parent.
    for (i = 0; i < 2; i++) {
        CHECK(ScShmAccessCreate(99, 16, 64, &pendingInfinite[i]) == OS_EOK);
        wait.Mode = SHMWaitUntilAvailable;
        wait.Deadline = *OS_DEADLINE_INFINITE;
        CHECK(ScShmAccessBegin(&async, pendingInfinite[i], &wait) == OS_EFORKED);
        session = LookupHandle(pendingInfinite[i], HandleTypeSHMAccess);
        CHECK(session != NULL);
        CHECK(g_cpuDeadline == &((struct SHMAccess*)session->Object)->Deadline);
        wait.Deadline.Seconds = 0;
        CHECK(g_cpuDeadline->Seconds == INT64_MAX && g_cpuDeadline->Nanoseconds == 0);
        CHECK(ScResourceRelease(pendingInfinite[i]) == OS_EBUSY);
    }
    g_forkWait = false;
    ownerTable = g_security.HandleTable;
    otherTable = CreateHandleGrantsTable();
    CHECK(otherTable != NULL);
    g_security.HandleTable = otherTable;
    g_security.GlobalHandleID = 11;
    CHECK(ScShmAccessCreate(99, 16, 64, &other) == OS_EOK);
    CHECK(__TestBeginCpu(cpu, SHMCpuAccessRead) == OS_ENOENT);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable;
    CHECK(SHMRecordBegin(context, &g_contextOps, &parent) == OS_EOK);
    recovery = parent->Object;
    recovery->References++;
    SHMRecordEnd(parent);
    DestroyHandleGrantsTable(ownerTable);
    g_security.HandleTable = CreateHandleGrantsTable();
    __TestHandlesDrain();
    CHECK(g_cpuSessions == 1 && g_maps == 1 && g_contexts == 1);
    CHECK(__TestBeginUse(active.Handle) == OS_ENOENT);
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    CHECK(ScResourceRelease(cpu) == OS_ENOENT && ScResourceRelease(pending) == OS_ENOENT);
    CHECK(ScResourceRelease(idle.Handle) == OS_ENOENT);
    CHECK(g_maps == 1 && g_contexts == 1);
    ownerTable = g_security.HandleTable;
    g_security.HandleTable = otherTable;
    g_security.GlobalHandleID = 11;
    CHECK(__TestBeginCpu(other, SHMCpuAccessRead) == OS_EOK);
    CHECK(ScShmAccessEnd(other, SHMAccessCpu, 0) == OS_EOK);
    CHECK(ScResourceRelease(other) == OS_EOK);
    DestroyHandleGrantsTable(otherTable);
    g_security.HandleTable = ownerTable;
    __TestHandlesDrain();
    g_security.GlobalHandleID = 10;

    // Recovery no longer requires the exited group's registry records.
    SHMDeviceReleaseQuarantine(recovery);
    SHMDeviceContextRelease(&recovery);
    __TestHandlesDrain();
    CHECK(g_live == 0 && g_maps == 0 && g_contexts == 0 && g_cpuSessions == 0);
    puts("DMA range syscalls: plain CPU sessions, async pin, group isolation, idle cleanup and active quarantine passed");
}
