// Exercise production binding, signaling and cleanup with counted event references.
static const uuid_t g_eventHandle = 77;
static uuid_t       g_eventOwner = 2;
static int          g_eventGrant = 1;
static int          g_eventReferences = 1;
static unsigned int g_eventReleases;
static unsigned int g_eventSignals;
static HandleType_t g_eventType = HandleTypeUserEvent;
static atomic_int   g_eventCounter;
static UserEvent_t  g_testEvent = {
    .InitialValue = 1,
    .flags = EVT_RESET_EVENT,
    .KernelMapping = &g_eventCounter
};

static void*
__LookupTestEventHandle(
    _In_ uuid_t handle)
{
    if (handle != g_eventHandle || g_eventReferences == 0 || g_eventType != HandleTypeUserEvent) {
        return NULL;
    }
    return &g_testEvent;
}

void*
LookupHandle(
    _In_ uuid_t       handle,
    _In_ HandleType_t type)
{
    CHECK(type == HandleTypeUserEvent);
    if (!g_eventGrant || g_currentSpace != g_eventOwner) {
        return NULL;
    }
    return __LookupTestEventHandle(handle);
}

oserr_t
AcquireGlobalHandle(
    _In_  uuid_t       handle,
    _In_  HandleType_t type,
    _Out_ void**       resourceOut)
{
    if (handle != g_eventHandle || g_eventReferences == 0) {
        return OS_ENOENT;
    }
    if (type != g_eventType) {
        return OS_EUNKNOWN;
    }
    g_eventReferences++;
    if (resourceOut != NULL) {
        *resourceOut = &g_testEvent;
    }
    return OS_EOK;
}

oserr_t
AcquireHandleReference(
    _In_  uuid_t       handle,
    _In_  HandleType_t type,
    _Out_ void**       resourceOut)
{
    if (!g_eventGrant || g_currentSpace != g_eventOwner) {
        return OS_ENOENT;
    }
    return AcquireGlobalHandle(handle, type, resourceOut);
}

oserr_t
ReleaseGlobalHandle(
    _In_ uuid_t handle)
{
    CHECK(handle == g_eventHandle && g_eventReferences > 0);
    g_eventReferences--;
    g_eventReleases++;
    return g_eventReferences == 0 ? OS_EOK : OS_EINCOMPLETE;
}

oserr_t
FutexWake(
    _In_ atomic_int* futex,
    _In_ int         count,
    _In_ int         flags)
{
    CHECK(futex == &g_eventCounter && count == 1 && flags == 0);
    return OS_EOK;
}

oserr_t
MarkHandle(
    _In_ uuid_t handle,
    _In_ unsigned int flags)
{
    CHECK(handle == g_eventHandle && flags == IOSETSYN);
    CHECK(g_eventReferences > 0);
    g_eventSignals++;
    return OS_EOK;
}

static irqstatus_t
__SignalEventHandler(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                    context)
{
    InterruptResourceTable_t* resources = context;

    CHECK(functions->EventSignal(resources->HandleResource) == OS_EINCOMPLETE);
    return IRQSTATUS_HANDLED;
}

static void
__TestInterruptEvents(void)
{
    InterruptDescriptor_t descriptor = { 0 };
    uuid_t                id;
    uuid_t                callback;
    unsigned int          releases;

    CHECK(g_eventReferences == 1);
    descriptor.Line = g_testIndex;
    descriptor.ResourceTable.Handler = __SignalEventHandler;
    descriptor.ResourceTable.HandleResource = g_eventHandle;
    g_identityClone = 1;
    CHECK(InterruptReserve(&descriptor, 0, &id) == OS_EOK);

    // Binding rejects another process's event and a handle of the wrong type.
    g_eventOwner = 3;
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_ENOENT);
    CHECK(g_eventReferences == 1 && g_liveMappings == 0);
    g_eventOwner = 2;
    g_eventType = HandleTypeSHM;
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EUNKNOWN);
    CHECK(g_eventReferences == 1 && g_liveMappings == 0);
    g_eventType = HandleTypeUserEvent;

    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EOK);
    CHECK(g_eventReferences == 2);
    CHECK(InterruptSetEnabled(id, 1) == OS_EOK);
    g_eventSignals = 0;
    g_currentSpace = 3;
    CHECK(LookupHandle(g_eventHandle, HandleTypeUserEvent) == NULL);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_eventSignals == 1 && atomic_load(&g_eventCounter) == 1);

    // Closing the driver's grant cannot free an event still bound to a handler.
    g_eventGrant = 0;
    g_eventReferences--;
    atomic_store(&g_eventCounter, 0);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_eventReferences == 1 && g_eventSignals == 2);
    g_currentSpace = 2;
    CHECK(InterruptSetEnabled(id, 0) == OS_EOK);
    CHECK(InterruptUnbind(id) == OS_EOK);
    CHECK(g_eventReferences == 0);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    // Both an early mapping failure and retryable partial cleanup balance the reference.
    g_eventReferences = 1;
    g_eventGrant = 1;
    g_cloneCalls = 0;
    g_failClone = 1;
    CHECK(InterruptReserve(&descriptor, 0, &id) == OS_EOK);
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EUNKNOWN);
    CHECK(g_eventReferences == 1 && g_liveMappings == 0);
    descriptor.ResourceTable.MemoryResources[0].Address = 0x2000;
    descriptor.ResourceTable.MemoryResources[0].Length = 64;
    g_cloneCalls = 0;
    g_failClone = 2;
    g_failUnmap = 1;
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EUNKNOWN);
    CHECK(g_eventReferences == 2 && g_liveMappings == 1);
    CHECK(InterruptUnbind(id) == OS_EUNKNOWN);
    CHECK(g_eventReferences == 2);
    g_failUnmap = 0;
    g_failClone = 0;
    CHECK(InterruptUnbind(id) == OS_EOK);
    CHECK(g_eventReferences == 1);
    releases = g_eventReleases;
    CHECK(InterruptUnbind(id) == OS_EOK && g_eventReleases == releases);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();
    memset(descriptor.ResourceTable.MemoryResources, 0, sizeof(descriptor.ResourceTable.MemoryResources));

    // Bus completion callbacks use the same event access and lifetime checks.
    CHECK(InterruptBusHandlerCreate(&callback) == OS_EOK);
    g_eventOwner = 3;
    CHECK(InterruptBusHandlerBind(callback, &descriptor.ResourceTable) == OS_ENOENT);
    CHECK(g_eventReferences == 1);
    g_eventOwner = 2;
    CHECK(InterruptBusHandlerBind(callback, &descriptor.ResourceTable) == OS_EOK);
    CHECK(g_eventReferences == 2);
    g_failUnmap = 1;
    CHECK(InterruptBusHandlerRelease(callback) == OS_EUNKNOWN);
    CHECK(g_eventReferences == 2);
    g_failUnmap = 0;
    CHECK(InterruptBusHandlerRelease(callback) == OS_EOK);
    CHECK(g_eventReferences == 1);
    __CheckEmpty();

    // Kernel handlers receive their own context, so an event handle is rejected
    // before any reference is taken. Binding without one works as before.
    g_currentSpace = 3;
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL, &id) == OS_EOK);
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EINVALPARAMS);
    CHECK(g_eventReferences == 1);
    descriptor.ResourceTable.HandleResource = UUID_INVALID;
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EOK);
    CHECK(g_eventReferences == 1);
    CHECK(InterruptUnregister(id) == OS_EOK);
    CHECK(g_eventReferences == 1);
    g_currentSpace = 2;
    g_identityClone = 0;
    __CheckEmpty();
    puts("Interrupt events: grant checks, cross-process signal, close, rollback, bus cleanup and kernel rejection passed");
}
