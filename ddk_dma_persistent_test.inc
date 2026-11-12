static void
__TestPersistentUses(void)
{
    struct DMAContext* context = NULL;
    struct DMAMapping* mapping = NULL;
    struct DMAMapping* saved;
    struct DMAUse first = { 0 };
    struct DMAUse second = { 0 };
    struct DMAUse rollback = { 0 };
    OSHandle_t buffer = { .ID = 303, .Type = OSHANDLE_SHM };
    const struct DMASegment* segments;
    OSTimestamp_t deadline = { .Seconds = 123, .Nanoseconds = 456 };
    OSTimestamp_t infinite = *OS_DEADLINE_INFINITE;
    OSTimestamp_t expired = { 0 };
    uint32_t count;
    int calls;
    int allocations;
    int queries;
    int logs;
    int i;

    g_expectedMapFlags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    g_resultSegmentCount = 2;
    for (i = 0; i < 2; i++) {
        g_failAfter = i;
        assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EOOM);
        assert(mapping == NULL && g_mappings == 0 && g_allocations == 1);
    }
    g_failAfter = -1;
    g_failSyscall = 81;
    g_failStatus = OS_ENOTSUPPORTED;
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_ENOTSUPPORTED);
    g_failSyscall = 82;
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_ENOTSUPPORTED);
    assert(mapping == NULL && g_mappings == 0);
    g_failSyscall = -1;
    g_expectedMapRequirements = (struct DMAMapRequirements){2, 2};
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice,
        &g_expectedMapRequirements, &mapping) == OS_EOK);
    g_expectedMapRequirements = (struct DMAMapRequirements){0};
    segments = DMAMappingSegments(mapping, &count);
    assert(count == 2 && segments[0].Address == 0x100001234ULL && segments[1].Address == 0x200001234ULL);
    saved = mapping;
    calls = g_calls;
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EBUSY);
    assert(DMABeginUse(NULL, &first, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(DMABeginUse(mapping, NULL, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(DMAEndUse(NULL) == OS_EINVALPARAMS);
    assert(DMAEndUse(&first) == OS_EINVALPARAMS);
    g_persistentUses = UINT_MAX;
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EOVERFLOW && first.Mapping == NULL);
    g_persistentUses = 0;
    assert(g_calls == calls + 1);
    g_failSyscall = 84;
    g_failStatus = OS_EUNKNOWN;
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EUNKNOWN);
    assert(first.Mapping == NULL && g_persistentUses == 0);
    g_failSyscall = -1;
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EOK);
    assert(DMABeginUse(mapping, &second, SHMWaitNonBlocking, NULL) == OS_EOK);
    calls = g_calls;
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EBUSY);
    assert(DMAUnmap(&mapping) == OS_EBUSY && mapping == saved);
    assert(DMAClose(&context) == OS_EBUSY);
    assert(g_calls == calls + 1);
    g_failSyscall = 85;
    assert(DMAEndUse(&first) == OS_EUNKNOWN);
    assert(first.Mapping == mapping && g_persistentUses == 2);
    g_failSyscall = -1;
    // Complete the second job first, then roll back a job that was not submitted.
    assert(DMAEndUse(&second) == OS_EOK && second.Mapping == NULL);
    assert(DMABeginUse(mapping, &rollback, SHMWaitNonBlocking, NULL) == OS_EOK);
    assert(DMAEndUse(&rollback) == OS_EOK && rollback.Mapping == NULL);
    assert(DMAUnmap(&mapping) == OS_EBUSY);
    assert(DMAEndUse(&first) == OS_EOK && first.Mapping == NULL);
    calls = g_calls;
    assert(DMAEndUse(&first) == OS_EINVALPARAMS && g_calls == calls);
    assert(g_persistentUses == 0);

    calls = g_calls;
    allocations = g_allocations;
    queries = g_queries;
    logs = g_logs;
    g_failAfter = 0;
    for (i = 0; i < 1000; i++) {
        assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EOK);
        assert(DMABeginUse(mapping, &second, SHMWaitNonBlocking, NULL) == OS_EOK);
        assert(DMAEndUse(&first) == OS_EOK);
        assert(DMAEndUse(&second) == OS_EOK);
        assert(DMAMappingSegments(mapping, &count) == segments && count == 2);
    }
    g_failAfter = -1;
    assert(g_calls == calls + 4000 && g_allocations == allocations);
    assert(g_queries == queries && g_logs == logs);
    g_failSyscall = 86;
    assert(DMAUnmap(&mapping) == OS_EUNKNOWN && mapping == saved);
    g_failSyscall = -1;
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL);
    assert(DMAUnmap(&mapping) == OS_EOK && DMAUnmap(NULL) == OS_EOK);

    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EOK);
    assert(DMABeginTransfer(mapping, 0, 64, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(DMAEndTransfer(mapping, 0) == OS_EINVALPARAMS);
    calls = g_calls;
    assert(DMABeginUse(mapping, &first, (enum SHMWaitMode)99, NULL) == OS_EINVALPARAMS);
    assert(g_calls == calls && first.Mapping == NULL);

    g_waitStatus = OS_EBUSY;
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, &deadline) == OS_EBUSY);
    assert(g_waitParameters.Mode == SHMWaitNonBlocking && first.Mapping == NULL);
    g_waitStatus = OS_ETIMEOUT;
    assert(DMABeginUse(mapping, &first, SHMWaitUntilAvailable, &expired) == OS_ETIMEOUT);
    assert(g_waitParameters.Mode == SHMWaitUntilAvailable && first.Mapping == NULL);
    assert(g_waitParameters.Deadline.Seconds == 0 && g_waitParameters.Deadline.Nanoseconds == 0);
    assert(DMABeginUse(mapping, &first, SHMWaitUntilAvailable, &deadline) == OS_ETIMEOUT);
    assert(first.Mapping == NULL && g_persistentUses == 0);
    assert(g_waitParameters.Mode == SHMWaitUntilAvailable);
    assert(g_waitParameters.Deadline.Seconds == deadline.Seconds);
    assert(g_waitParameters.Deadline.Nanoseconds == deadline.Nanoseconds);
    g_waitStatus = OS_EOK;
    assert(DMABeginUse(mapping, &first, SHMWaitUntilAvailable, &infinite) == OS_EOK);
    assert(g_waitParameters.Kind == DMAAccessUse && g_waitParameters.Mode == SHMWaitUntilAvailable);
    assert(g_waitParameters.Deadline.Seconds == infinite.Seconds);
    assert(g_waitParameters.Deadline.Nanoseconds == infinite.Nanoseconds);
    assert(DMAEndUse(&first) == OS_EOK);
    assert(DMAUnmap(&mapping) == OS_EOK);

    g_expectedMapFlags = DMA_MAP_REQUIRE_DIRECT;
    assert(DMAMapSHMDirect(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EOK);
    assert(DMABeginUse(mapping, &first, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    calls = g_calls;
    assert(DMABeginTransfer(mapping, 0, 64, (enum SHMWaitMode)99, NULL) == OS_EINVALPARAMS);
    assert(g_calls == calls && !g_deviceOwned);
    g_waitStatus = OS_ETIMEOUT;
    assert(DMABeginTransfer(mapping, 0, 64, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_ETIMEOUT && !g_deviceOwned);
    g_waitStatus = OS_EOK;
    assert(DMABeginTransfer(mapping, 0, 64, SHMWaitUntilAvailable, OS_DEADLINE_INFINITE) == OS_EOK && g_deviceOwned && g_transferLength == 64);
    assert(DMAEndTransfer(mapping, 32) == OS_EOK);
    assert(DMAUnmap(&mapping) == OS_EOK);
    g_expectedMapFlags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;

    // A failed segment query can leave cleanup pending under the context.
    g_badCount = 1;
    g_failRollback = 1;
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EUNKNOWN);
    assert(mapping == NULL && context->Pending != NULL && g_mappings == 1);
    g_badCount = 0;
    g_failRollback = 0;
    assert(DMAClose(&context) == OS_EOK && g_allocations == 0 && g_mappings == 0);
    g_resultSegmentCount = 1;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    calls = g_calls;
    assert(DMAMapSHMPersistent(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EOK);
    assert(g_calls == calls + 1);
    assert(DMAMappingSegments(mapping, &count)->Length == 512 && count == 1);
    assert(DMAUnmap(&mapping) == OS_EOK);
    assert(DMAClose(&context) == OS_EOK && g_allocations == 0);
    g_expectedMapFlags = 0;
}
