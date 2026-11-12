// Exercise subscription cleanup with the real handle table and grant references.
static uuid_t       g_closingAccess;
static unsigned int g_accessGrantCleanups;
static unsigned int g_accessGlobalCleanups;

static void
__CheckAccessGrantCleanup(
    _In_ void* resource)
{
    // The grant's global reference must still be valid until this callback ends.
    assert(LookupGlobalHandle(g_closingAccess, HandleTypeSHMAccess) == resource);
    assert(g_accessGlobalCleanups == 0);
    g_accessGrantCleanups++;
}

static void
__CheckAccessGlobalCleanup(
    _In_ void* resource)
{
    assert(resource == &g_closingAccess);
    assert(g_accessGrantCleanups == 1);
    g_accessGlobalCleanups++;
}

static void
__TestNotificationReferences(
    _In_ MSContextSecurity_t* context)
{
    struct ResourceHandle* record;
    struct ioset_event     watch = { .events = IOSETIN, .data.handle = UINT32_MAX };
    uuid_t                 handle;
    uuid_t                 queue;
    size_t                 before = g_allocations;

    g_current = context;

    // SHM access handles reject independent references. Removing subscriptions
    // must not consume the grant's reference when that check fails.
    g_closingAccess = CreateHandle(
        HandleTypeSHMAccess,
        __CheckAccessGrantCleanup,
        __CheckAccessGlobalCleanup,
        &g_closingAccess
    );
    assert(g_closingAccess != UUID_INVALID);
    assert(AcquireGlobalHandleUntyped(g_closingAccess, NULL, NULL) == OS_EPERMISSIONS);
    assert(DestroyHandle(g_closingAccess) == OS_EOK);
    assert(g_accessGrantCleanups == 1 && g_accessGlobalCleanups == 0);
    __Drain();
    assert(g_accessGlobalCleanups == 1);

    // A successful temporary reference must balance, and an overflow rejection
    // must leave the existing references unchanged.
    handle = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
    assert(handle != UUID_INVALID);
    record = __LookupGlobalSafe(handle);
    assert(record != NULL && record->References == 1);
    HandleSetsRemoveGrant(context->HandleTable, handle);
    assert(record->References == 1);
    record->References = INT_MAX;
    HandleSetsRemoveGrant(context->HandleTable, handle);
    assert(record->References == INT_MAX);
    record->References = 1;
    assert(DestroyHandle(handle) == OS_EOK);
    __Drain();

    // Non-handle notification IDs need no grant or temporary reference.
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(ControlHandleSet(queue, IOSET_ADD, UINT32_MAX, &watch) == OS_EOK);
    assert(MarkHandle(UINT32_MAX, IOSETIN) == OS_EOK);
    __ExpectNotification(queue, IOSETIN, UINT32_MAX);
    assert(ControlHandleSet(queue, IOSET_DEL, UINT32_MAX, NULL) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
    puts("Handle-set references: rejected access, deferred cleanup, overflow and raw notifications passed");
}
