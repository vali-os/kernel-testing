static void
__TestPersistentSyscalls(
    _In_ uuid_t context)
{
    struct OSDmaMapParameters map = {
        .Offset = 16, .Length = 64, .Direction = DMAFromDevice,
        .Flags = DMA_MAP_PERSISTENT
    };
    struct OSDmaMappingResult result = { .Handle = 0xdead };
    struct SHMAccessBeginParameters wait = { .Kind = DMAAccessUse };
    struct SHMRecord* record;
    unsigned int calls = g_persistentMapCalls;
    unsigned int allocations;
    HandleGrantsTable_t* ownerTable = g_security.HandleTable;
    HandleGrantsTable_t* otherTable = CreateHandleGrantsTable();
    int i;

    // Persistent mappings cannot select a bounce-capable creation mode.
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(result.Handle == 0xdead && g_persistentMapCalls == calls && g_live == 1);
    map.Flags |= DMA_MAP_REQUIRE_DIRECT;
    g_engineFailure = 1;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOOM);
    g_engineFailure = 0;
    g_copyFailure = 1;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(result.Handle == 0xdead && g_live == 1 && g_maps == 0);
    g_copyFailure = 0;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    __TestHandlesDrain();
    CHECK(g_persistentMapCalls == calls + 3 && g_live == 2 && g_maps == 1);
    CHECK(__TestBeginTransfer(result.Handle, 0, 64) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 64) == OS_EINVALPARAMS);
    CHECK(__TestBeginUse(context) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(context, DMAAccessUse, 0) == OS_EINVALPARAMS);
    CHECK(__TestBeginUse(0xdead) == OS_ENOENT);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EINVALPARAMS);
    CHECK(SHMRecordBegin(result.Handle, &g_mappingOps, &record) == OS_EOK);
    CHECK(__TestBeginUse(result.Handle) == OS_EBUSY);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EBUSY);
    CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
    SHMRecordEnd(record);
    g_security.GlobalHandleID = 11;
    g_security.HandleTable = otherTable;
    CHECK(__TestBeginUse(result.Handle) == OS_ENOENT);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_ENOENT);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable;
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(result.Handle, (enum SHMCpuAccess)99) == OS_EINVALPARAMS);
    CHECK(__TestBeginCpu(result.Handle, SHMCpuAccessWrite) == OS_EINVALPARAMS);
    g_security.GlobalHandleID = 11;
    g_security.HandleTable = otherTable;
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_ENOENT);
    CHECK(__TestBeginCpu(result.Handle, SHMCpuAccessRead) == OS_ENOENT);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable;
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    wait.Offset = 1;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EINVALPARAMS);
    wait.Offset = 0;
    wait.Length = 1;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EINVALPARAMS);
    wait.Length = 0;
    wait.Write = true;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EINVALPARAMS);
    wait.Write = false;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EOK);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 1) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EOK);
    wait.Kind = SHMAccessCpu;
    wait.Write = false;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    wait.Kind = SHMAccessCpu;
    wait.Write = true;
    CHECK(ScShmAccessBegin(NULL, result.Handle, &wait) == OS_EINVALPARAMS);
    CHECK(ScShmAccessEnd(result.Handle, SHMAccessCpu, 0) == OS_EINVALPARAMS);
    allocations = __TestHandleAllocationCalls();
    g_oom = 1;
    for (i = 0; i < 1000; i++) {
        CHECK(__TestBeginUse(result.Handle) == OS_EOK);
        CHECK(__TestBeginUse(result.Handle) == OS_EOK);
        CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
        CHECK(ScResourceRelease(context) == OS_EBUSY);
        g_engineFailure = 1;
        CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EUNKNOWN);
        CHECK(__TestBeginUse(result.Handle) == OS_EUNKNOWN);
        g_engineFailure = 0;
        CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EOK);
        CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
        CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EOK);
        CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_EINVALPARAMS);
    }
    g_oom = 0;
    __TestHandlesDrain();
    CHECK(__TestHandleAllocationCalls() == allocations && g_live == 2);
    __TestHandlesDrain();
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    __TestHandlesDrain();
    CHECK(g_live == 1 && g_maps == 0);
    CHECK(__TestBeginUse(result.Handle) == OS_ENOENT);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessUse, 0) == OS_ENOENT);
    DestroyHandleGrantsTable(otherTable);
}
