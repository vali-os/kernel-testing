/** Exercise kernel handles through growth, setup failure and concurrent operations. */
static void
__TestHandleIndex(
    _In_ const struct OSDmaOpenParameters* parameters,
    _In_ const struct OSDmaMapParameters*  map)
{
    uuid_t                    handles[128];
    uuid_t                    extra = UUID_INVALID;
    uuid_t                    generation;
    struct SHMRecord*         held;
    struct SHMRecord*         acquired = NULL;
    struct OSDmaMappingResult result = { 0 };
    unsigned int              i;

    CHECK(ScDmaOpen(7, parameters, &handles[0]) == OS_EOK);
    CHECK(SHMRecordBegin(handles[0], &g_contextOps, &held) == OS_EOK);
    for (i = 1; i < 128; i++) {
        CHECK(ScDmaOpen(7, parameters, &handles[i]) == OS_EOK);
    }
    CHECK(held->Handle == handles[0] && held->Object != NULL);
    CHECK(SHMRecordBegin(handles[0], &g_contextOps, &acquired) == OS_EBUSY);
    SHMRecordEnd(held);

    g_copyFailure = 1;
    CHECK(ScDmaMapShm(handles[0], 99, map, &result) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_live == 128 && result.Handle == UUID_INVALID);
    g_copyFailure = 0;

    // Operations on existing records need no resource allocation.
    g_oom = 1;
    for (i = 0; i < 128; i++) {
        CHECK(SHMRecordBegin(handles[i], &g_contextOps, &acquired) == OS_EOK);
        CHECK(acquired->Handle == handles[i]);
        SHMRecordEnd(acquired);
        CHECK(ScResourceRelease(handles[i]) == OS_EOK);
        CHECK(ScResourceRelease(handles[i]) == OS_ENOENT);
    }
    g_oom = 0;
    __TestHandlesDrain();
    CHECK(g_live == 0 && g_contexts == 0);

    // Setup is visible only as busy, even if another thread guesses the ID.
    CHECK(SHMRecordCreate(&g_contextOps, &held) == OS_EOK);
    CHECK(SHMRecordBegin(held->Handle, &g_contextOps, &acquired) == OS_EBUSY);
    CHECK(ScDestroyHandle(held->Handle) == OS_EINVALPARAMS);
    SHMRecordRemove(held);
    __TestHandlesDrain();
    CHECK(g_live == 0);

    generation = __TestNextHandleId(UUID_INVALID);
    CHECK(ScDmaOpen(7, parameters, &extra) == OS_EOOM);
    CHECK(extra == UUID_INVALID && g_live == 0 && g_contexts == 0);
    __TestNextHandleId(generation);
    puts("DMA handles: growth, stable payloads, setup rollback and allocation-free operations passed");
}

static unsigned int g_customCleanups;
static bool g_customCleanupFailure;

static oserr_t
__CleanupCustomRecord(
    _In_    struct SHMRecord* record,
    _InOut_ void**           object)
{
    CHECK(record->Object == NULL && *object == &g_customCleanups);
    if (g_customCleanupFailure) {
        return OS_EBUSY;
    }
    g_customCleanups++;
    *object = NULL;
    return OS_EOK;
}

static oserr_t
__BeginCustomAccess(
    _In_ struct SHMRecord*                      record,
    _In_ const struct SHMAccessBeginParameters* parameters,
    _In_ OSAsyncContext_t*                      async)
{
    CHECK(record->Object == &g_customCleanups);
    CHECK(async == NULL);
    return parameters->Kind == DMAAccessUse ? OS_EOK : OS_EINVALPARAMS;
}

static oserr_t
__EndCustomAccess(
    _In_ struct SHMRecord*                    record,
    _In_ const struct SHMAccessEndParameters* parameters)
{
    CHECK(record->Object == &g_customCleanups);
    CHECK(parameters->Length == 0);
    return parameters->Kind == DMAAccessUse ? OS_EOK : OS_EINVALPARAMS;
}

static void
__TestCustomAccessDispatch(void)
{
    static const struct SHMRecordOps ops = {
        .Release = __CleanupCustomRecord,
        .Abandon = __CleanupCustomRecord,
        .Begin = __BeginCustomAccess,
        .End = __EndCustomAccess
    };
    struct SHMRecord* record;
    struct SHMRecord* held;
    uuid_t            handle;
    unsigned int      cleanups = g_customCleanups;

    CHECK(SHMRecordCreate(&ops, &record) == OS_EOK);
    CHECK(SHMRecordRelease(record) == OS_EOK && g_customCleanups == cleanups);
    record->Object = &g_customCleanups;
    handle = record->Handle;
    atomic_store(&record->State, SHMRecordIdle);
    CHECK(SHMRecordBegin(handle, &g_contextOps, &held) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(handle, SHMCpuAccessRead) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(__TestBeginUse(handle) == OS_EOK);
    CHECK(ScShmAccessEnd(handle, DMAAccessUse, 0) == OS_EOK);
    g_customCleanupFailure = true;
    CHECK(ScResourceRelease(handle) == OS_EBUSY);
    CHECK(record->Object == &g_customCleanups && g_customCleanups == cleanups);
    CHECK(__TestBeginUse(handle) == OS_EOK);
    g_customCleanupFailure = false;

    // A caller delayed before entering Updating must see Closed without using Object.
    CHECK(AcquireHandle(handle, HandleTypeSHMAccess, (void**)&held) == OS_EOK);
    CHECK(ScResourceRelease(handle) == OS_EOK);
    __TestHandlesDrain();
    CHECK(atomic_load(&held->State) == SHMRecordClosed && held->Object == NULL);
    CHECK(ScResourceRelease(handle) == OS_ENOENT);
    CHECK(g_customCleanups == cleanups + 1);
    CHECK(DestroyHandle(handle) == OS_EOK);
    __TestHandlesDrain();
    CHECK(g_live == 0);
}

static void
__TestCustomRecordCleanup(void)
{
    static const struct SHMRecordOps ops = {
        .Release = __CleanupCustomRecord,
        .Abandon = __CleanupCustomRecord
    };
    struct SHMRecord* record;
    struct SHMRecord* held;
    HandleGrantsTable_t* table;
    unsigned int cleanups = g_customCleanups;

    CHECK(SHMRecordCreate(&ops, &record) == OS_EOK);
    record->Object = &g_customCleanups;
    atomic_store(&record->State, SHMRecordIdle);
    CHECK(SHMRecordBegin(record->Handle, NULL, &held) == OS_EOK);
    CHECK(ScResourceRelease(record->Handle) == OS_EBUSY);
    CHECK(ScDestroyHandle(record->Handle) == OS_EINVALPARAMS);
    // No call can resume after group exit, including this held operation.
    table = g_security.HandleTable;
    g_security.HandleTable = CreateHandleGrantsTable();
    DestroyHandleGrantsTable(table);
    __TestHandlesDrain();
    CHECK(g_customCleanups == cleanups + 1 && g_live == 0);
}

static void
__TestDmaCleanupOrder(
    _In_ const struct OSDmaOpenParameters* parameters,
    _In_ const struct OSDmaMapParameters*  map)
{
    struct OSDmaMappingResult result;
    HandleGrantsTable_t* other;
    uuid_t context;
    unsigned int order;

    other = CreateHandleGrantsTable();
    CHECK(other != NULL);
    for (order = 0; order < 2; order++) {
        CHECK(ScDmaOpen(7, parameters, &context) == OS_EOK);
        CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOK);
        CHECK(GrantHandle(&((MSContextSecurity_t){ .HandleTable = other }), context) == OS_EPERMISSIONS);
        CHECK(GrantHandle(&((MSContextSecurity_t){ .HandleTable = other }), result.Handle) == OS_EPERMISSIONS);
        CHECK(ScDestroyHandle(context) == OS_EINVALPARAMS);
        CHECK(ScDestroyHandle(result.Handle) == OS_EINVALPARAMS);
        // Exercise both possible grant cleanup orders with the real handle callbacks.
        if (order == 0) {
            CHECK(DestroyHandle(context) == OS_EOK);
            __TestHandlesDrain();
            CHECK(g_contexts == 1 && g_maps == 1);
            CHECK(DestroyHandle(result.Handle) == OS_EOK);
        } else {
            CHECK(DestroyHandle(result.Handle) == OS_EOK);
            __TestHandlesDrain();
            CHECK(g_contexts == 1 && g_maps == 0);
            CHECK(DestroyHandle(context) == OS_EOK);
        }
        __TestHandlesDrain();
        CHECK(g_contexts == 0 && g_maps == 0 && g_live == 0);
    }
    DestroyHandleGrantsTable(other);
}

static void
__TestDmaAsyncLifecycle(
    _In_ const struct OSDmaOpenParameters* parameters,
    _In_ const struct OSDmaMapParameters*  map)
{
    struct OSDmaMappingResult result = { 0 };
    struct SHMAccessBeginParameters begin = {
        .Kind = DMAAccessTransfer,
        .Mode = SHMWaitUntilAvailable,
        .Length = 64
    };
    OSAsyncContext_t async = { 0 };
    struct SHMRecord* record;
    HandleGrantsTable_t* table;
    uuid_t context;

    g_checkCopyVisibility = true;
    CHECK(ScDmaOpen(7, parameters, &context) == OS_EOK);
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOK);
    g_copyFailure = 1;
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EINVALPARAMS);
    g_copyFailure = 0;
    g_checkCopyVisibility = false;
    __TestHandlesDrain();
    CHECK(g_maps == 1 && g_live == 2);

    g_dmaForkWait = true;
    CHECK(ScShmAccessBegin(&async, result.Handle, &begin) == OS_EFORKED);
    CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
    CHECK(ScDestroyHandle(result.Handle) == OS_EINVALPARAMS);
    CHECK(ScDestroyHandle(result.Handle) == OS_EINVALPARAMS);
    CHECK(ScShmAccessBegin(&async, result.Handle, &begin) == OS_EBUSY);
    record = LookupHandle(result.Handle, HandleTypeSHMAccess);
    CHECK(record != NULL);
    CHECK(((struct SHMDeviceMapping*)record->Object)->Access.Phase == SHMAccessWaiting);
    // Model the child finishing a cancelled backend wait and dropping its reference.
    SHMAccessEnd(&((struct SHMDeviceMapping*)record->Object)->Access);
    SHMRecordEnd(record);
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_contexts == 1);

    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOK);
    CHECK(ScShmAccessBegin(&async, result.Handle, &begin) == OS_EFORKED);
    // A stopped child cannot finish its reference; grant cleanup must handle it.
    table = g_security.HandleTable;
    g_security.HandleTable = CreateHandleGrantsTable();
    DestroyHandleGrantsTable(table);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_contexts == 0 && g_live == 0);
    CHECK(ScResourceRelease(result.Handle) == OS_ENOENT);
    g_dmaForkWait = false;
}
