// Production state machines with deterministic memory and security groups.
#ifndef HANDLES_SOURCE
#define HANDLES_SOURCE "../kernel/handle.c"
#endif
#include HANDLES_SOURCE
#ifndef GRANTS_SOURCE
#define GRANTS_SOURCE "../kernel/handle_grants.c"
#endif
#include GRANTS_SOURCE
#ifndef PROVIDER_SOURCE
#define PROVIDER_SOURCE "../kernel/handle_provider.c"
#endif
#include PROVIDER_SOURCE
#undef __MODULE
#ifndef HANDLE_SET_SOURCE
#define HANDLE_SET_SOURCE "../kernel/handle_set.c"
#endif
#include HANDLE_SET_SOURCE
#undef __MODULE
#ifndef HANDLES_API_SOURCE
#define HANDLES_API_SOURCE "../kernel/api/handles.c"
#endif
#include HANDLES_API_SOURCE
#include <stdio.h>
#include <stdlib.h>

static MSContextSecurity_t* g_current;
static int                  g_failAllocation = -1;
static int                  g_failCopy = -1;
static size_t               g_allocations;
static void __Drain(void);
static bool                 g_allowDestination;
static uuid_t               g_closeSpaceOnCopy = UUID_INVALID;
static uuid_t               g_closeBeforeSetLock = UUID_INVALID;
static uuid_t               g_closeAfterSetUnlock = UUID_INVALID;
static bool                 g_waitContinuation;
static bool                 g_waitFork;
static unsigned int         g_waitCalls;

void
TestBeforeNotificationLock(
    _In_ Spinlock_t* lock)
{
    uuid_t handle;

    if (lock == &g_handleSetsLock && g_closeBeforeSetLock != UUID_INVALID) {
        handle = g_closeBeforeSetLock;
        g_closeBeforeSetLock = UUID_INVALID;
        assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    }
}

void
TestAfterNotificationUnlock(
    _In_ Spinlock_t* lock)
{
    uuid_t handle;

    if (lock == &g_handleSetsLock && g_closeAfterSetUnlock != UUID_INVALID) {
        handle = g_closeAfterSetUnlock;
        g_closeAfterSetUnlock = UUID_INVALID;
        assert(DestroyHandle(handle) == OS_EOK);
    }
}

oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    assert(capabilities == CAPABILITY_MEMORYSPACE_MANAGEMENT);
    return g_allowDestination ? OS_EOK : OS_EPERMISSIONS;
}

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return g_current;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* p;
    if (g_failAllocation == 0) {
        return NULL;
    }
    if (g_failAllocation > 0) {
        g_failAllocation--;
    }
    p = malloc(size);
    if (p != NULL) {
        g_allocations++;
    }
    return p;
}

void
kfree(
    _In_ void* p)
{
    if (p != NULL) {
        assert(g_allocations);
        g_allocations--;
        free(p);
    }
}

void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_ void* p)
{
    free(p);
}

oserr_t
FutexWait(
    _In_ OSAsyncContext_t* async,
    _In_ _Atomic(int)*     value,
    _In_ int               expected,
    _In_ int               flags,
    _In_ _Atomic(int)*     other,
    _In_ int               count,
    _In_ int               operation,
    _In_ OSTimestamp_t*    deadline)
{
    if (g_waitFork) {
        assert(async != NULL);
        return OS_EFORKED;
    }
    if (g_waitContinuation) {
        if (g_waitCalls++ == 0) {
            assert(async != NULL);
            return OS_EOK;
        }
        assert(async == NULL);
    }
    (void)async;
    (void)value;
    (void)expected;
    (void)flags;
    (void)other;
    (void)count;
    (void)operation;
    (void)deadline;
    return OS_ETIMEOUT;
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* value,
    _In_ int count,
    _In_ int flags)
{
    (void)value;
    (void)count;
    (void)flags;
    return OS_EOK;
}

static oserr_t
__CopyProviderUser(
    _In_ void* user,
    _In_ void* kernel,
    _In_ size_t size,
    _In_ bool toUser)
{
    if (g_failCopy == 0) {
        return OS_EINVALPARAMS;
    }
    if (g_failCopy > 0) {
        g_failCopy--;
    }
    if (toUser) {
        memcpy(user, kernel, size);
    } else {
        memcpy(kernel, user, size);
    }
    return OS_EOK;
}

oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       kernelBuffer,
    _In_  const void* userBuffer,
    _In_  size_t      length)
{
    return __CopyProviderUser((void*)userBuffer, kernelBuffer, length, false);
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       userBuffer,
    _In_  const void* kernelBuffer,
    _In_  size_t      length)
{
    if (g_closeSpaceOnCopy != UUID_INVALID) {
        // Close the selector while the syscall is copying its result. The
        // destination context must still be valid for rollback afterwards.
        assert(DestroyHandle(g_closeSpaceOnCopy) == OS_EOK);
        g_closeSpaceOnCopy = UUID_INVALID;
        __Drain();
    }
    return __CopyProviderUser(userBuffer, (void*)kernelBuffer, length, true);
}

oserr_t
SHMReferenceForExport(
    _In_ uuid_t                     id,
    _In_ const GrantTokenOptions_t* options,
    _In_ HandleType_t*              type)
{
    (void)id;
    (void)options;
    (void)type;
    return OS_ENOTSUPPORTED;
}

oserr_t
MSContextAcquireSecurity(
    _In_ uint64_t group,
    _In_ MSContextSecurity_t** out)
{
    if (group == 0 || group > UINT32_MAX || (uuid_t)group == UUID_INVALID) {
        return OS_EINVALPARAMS;
    }
    return AcquireGlobalHandle((uuid_t)group, HandleTypeMSContext, (void**)out);
}

static void
__Drain(void)
{
    element_t* entry;
    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

static void
__NewGroup(
    _In_ MSContextSecurity_t* group)
{
    group->HandleTable = CreateHandleGrantsTable();
    group->GlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, NULL, group);
    assert(group->HandleTable != NULL);
}

static GrantToken_t
__Token(
    _In_ uuid_t reference,
    _In_ uint32_t scope,
    _In_ uuid_t receiver)
{
    GrantTokenOptions_t options = {.ResourceType = GrantTokenResourceTypeProvider,
                                   .TargetType = receiver == UUID_INVALID
                                                     ? GrantTokenTargetTypeAny
                                                     : GrantTokenTargetTypeSpecific,
                                   .TargetGlobalHandleID = receiver,
                                   .Scope = (GrantTokenScope_t)scope};
    GrantToken_t        token;
    assert(GrantTokenCreate(reference, &options, &token) == OS_EOK);
    return token;
}

static void
__ExpectWakeup(
    _In_ uuid_t queue,
    _In_ uuid_t registration)
{
    struct ioset_event event;
    int               count;

    assert(WaitForHandleSet(queue, NULL, &event, 1, 0, NULL, &count) == OS_EOK);
    assert(count == 1 && event.events == IOSETIN);
    assert(event.data.handle == registration);
}

static void
__ExpectNoWakeup(
    _In_ uuid_t queue)
{
    struct ioset_event event;
    int               count;

    assert(WaitForHandleSet(queue, NULL, &event, 1, 0, NULL, &count) == OS_ETIMEOUT);
    assert(atomic_load(&((struct __HandleSet*)LookupHandle(queue, HandleTypeSet))->EventsPending) == 0);
}

static void
__TestReferences(
    _In_ MSContextSecurity_t* provider,
    _In_ MSContextSecurity_t* client,
    _In_ MSContextSecurity_t* stranger)
{
    uuid_t              queue;
    uuid_t              registration, resource, readRef, writeRef, duplicate, result;
    uuid_t              pending[8];
    uint32_t            count;
    GrantToken_t        readToken, writeToken, reduced, changed;
    GrantTokenOptions_t options;
    size_t              before = g_allocations;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(ScHandleRegisterProvider(queue, &registration) == OS_EOK);
    assert(GrantHandle(client, registration) == OS_EPERMISSIONS);
    assert(ScCreateHandle(registration, 3, &resource) == OS_EOK);
    readToken = __Token(resource, 1, client->GlobalHandleID);
    writeToken = __Token(resource, 2, UUID_INVALID);
    assert(HandleProviderValidate(registration, provider->GlobalHandleID, resource, 3, &result) ==
           OS_EOK);
    assert(result == resource);
    assert(HandleProviderValidate(registration, stranger->GlobalHandleID, resource, 1, &result) ==
           OS_ENOENT);
    assert(HandleProviderValidate(
               registration, (1ULL << 32) | provider->GlobalHandleID, resource, 1, &result) ==
           OS_EINVALPARAMS);

    g_current = stranger;
    assert(HandleProviderImport(&readToken, NULL, &duplicate) == OS_EPERMISSIONS);
    assert(HandleProviderValidate(registration, provider->GlobalHandleID, resource, 1, &result) ==
           OS_ENOENT);
    assert(HandleProviderRegister(queue, &result) == OS_ENOENT);
    g_current = client;
    assert(ScHandleImport(&readToken, UUID_INVALID, &readRef) == OS_EOK);
    assert(ScHandleImport(&writeToken, UUID_INVALID, &writeRef) == OS_EOK);
    assert(ScHandleImport(&readToken, UUID_INVALID, &duplicate) == OS_EOK);
    assert(readRef != writeRef && readRef != resource && duplicate != readRef);
    options = readToken.Options;
    options.Scope = 3;
    assert(GrantTokenCreate(readRef, &options, &changed) == OS_EPERMISSIONS);
    options.Scope = 1;
    options.Data.Words[0] = 1;
    assert(GrantTokenCreate(readRef, &options, &changed) == OS_EINVALPARAMS);
    reduced = __Token(readRef, 1, UUID_INVALID);
    changed = readToken;
    changed.Options.Scope = 3;
    assert(HandleProviderImport(&changed, NULL, &result) == OS_EINVALPARAMS);

    g_current = provider;
    assert(HandleProviderValidate(registration, client->GlobalHandleID, readRef, 1, &result) ==
           OS_EOK);
    assert(result == resource);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, readRef, 2, &result) ==
           OS_EPERMISSIONS);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, writeRef, 1, &result) ==
           OS_EPERMISSIONS);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, writeRef, 2, &result) ==
           OS_EOK);
    assert(GrantTokenRevoke(readToken.ControlHandleID) == OS_EOK);
    assert(GrantTokenRevoke(writeToken.ControlHandleID) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    assert(HandleProviderGetEvents(registration, pending, 8, &count) == OS_EOK && count == 0);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, readRef, 1, &result) ==
           OS_EOK);

    g_current = client;
    assert(HandleProviderImport(&readToken, NULL, &result) != OS_EOK);
    assert(DestroyHandle(readRef) == OS_EOK);
    assert(DestroyHandle(writeRef) == OS_EOK);
    assert(DestroyHandle(duplicate) == OS_EOK);
    __Drain();
    g_current = provider;
    __ExpectNoWakeup(queue);
    g_current = client;
    assert(GrantTokenRevoke(reduced.ControlHandleID) == OS_EOK);
    __Drain();
    g_current = provider;
    __ExpectWakeup(queue, registration);

    // Both an ID-copy failure and a final count-copy failure preserve the queue.
    g_failCopy = 0;
    assert(ScHandleProviderGetEvents(registration, pending, 8, &count) == OS_EINVALPARAMS);
    g_failCopy = 1;
    assert(ScHandleProviderGetEvents(registration, pending, 8, &count) == OS_EINVALPARAMS);
    g_failCopy = -1;
    assert(ScHandleProviderGetEvents(registration, pending, 8, &count) == OS_EOK);
    assert(count == 1 && pending[0] == resource);
    __ExpectWakeup(queue, registration);
    __ExpectNoWakeup(queue);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestShutdown(
    _In_ MSContextSecurity_t* provider,
    _In_ MSContextSecurity_t* client)
{
    uuid_t       queue;
    uuid_t       registration;
    uuid_t       resource;
    uuid_t       imported;
    uuid_t       result;
    uuid_t       otherRegistration;
    uuid_t       otherResource;
    GrantToken_t token;
    size_t       before = g_allocations;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 0x80000001U, &resource) == OS_EOK);
    token = __Token(resource, 0x80000001U, UUID_INVALID);

    // Registrations in one group have independent resources and lifetimes.
    assert(HandleProviderRegister(queue, &otherRegistration) == OS_EOK);
    assert(otherRegistration != registration);
    assert(HandleProviderCreate(otherRegistration, 1, &otherResource) == OS_EOK);
    assert(HandleProviderValidate(
        otherRegistration, provider->GlobalHandleID, resource, 1, &result
    ) == OS_EPERMISSIONS);

    g_current = client;
    assert(HandleProviderImport(&token, NULL, &imported) == OS_EOK);
    g_current = provider;
    assert(DestroyHandle(registration) == OS_EOK);

    // Invalidation must not require the deferred janitor callback to run.
    g_current = client;
    assert(HandleProviderImport(&token, NULL, &result) == OS_ECANCELLED);
    assert(DestroyHandle(imported) == OS_EOK);
    g_current = provider;
    assert(HandleProviderValidate(
        otherRegistration, provider->GlobalHandleID, otherResource, 1, &result
    ) == OS_EOK);
    assert(result == otherResource);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    assert(DestroyHandle(otherResource) == OS_EOK);
    assert(DestroyHandle(otherRegistration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestFailures(
    _In_ MSContextSecurity_t* provider)
{
    uuid_t   queue;
    uuid_t   registration, resource, output = 0x1234;
    uint32_t count;
    int      failure;
    oserr_t  status;
    size_t   before;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    g_current = NULL;
    assert(HandleProviderRegister(queue, &output) == OS_EPERMISSIONS);
    g_current = provider;
    assert(HandleProviderRegister(UUID_INVALID, &output) == OS_ENOENT);
    assert(output == 0x1234);
    // A failed registration must release its queue reference and allocations,
    // leaving output unchanged until the handle can be delivered.
    __Drain();
    before = g_allocations;
    for (failure = 0; failure < 3; failure++) {
        g_failAllocation = failure;
        assert(ScHandleRegisterProvider(queue, &output) == OS_EOOM);
        g_failAllocation = -1;
        assert(output == 0x1234);
        __Drain();
        assert(g_allocations == before);
    }
    g_failCopy = 0;
    assert(ScHandleRegisterProvider(queue, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;
    __Drain();
    assert(g_allocations == before);

    assert(HandleProviderCreate(UUID_INVALID, 1, &output) == OS_EINVALPARAMS);
    assert(output == 0x1234);
    assert(HandleProviderCreate(UUID_INVALID, 0, &resource) == OS_EOK);
    assert(HandleProviderRegister(resource, &output) == OS_EUNKNOWN);
    assert(output == 0x1234);
    assert(DestroyHandle(resource) == OS_EOK);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    __Drain();
    before = g_allocations;
    for (failure = 0; failure < 4; failure++) {
        g_failAllocation = failure;
        status = ScCreateHandle(registration, 3, &output);
        g_failAllocation = -1;
        if (status == OS_EOK) {
            assert(DestroyHandle(output) == OS_EOK);
        } else {
            assert(status == OS_EOOM);
        }
        __Drain();
        assert(HandleProviderGetEvents(registration, &resource, 1, &count) == OS_EOK);
        __Drain();
        assert(g_allocations == before);
    }
    g_failCopy = 0;
    assert(ScCreateHandle(registration, 3, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;
    __Drain();
    assert(HandleProviderGetEvents(registration, &resource, 1, &count) == OS_EOK && count == 0);
    assert(g_allocations == before);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
}

static void
__TestCleanupWakeups(
    _In_ MSContextSecurity_t* provider)
{
    uuid_t       registration, queue, resource[3], ids[3];
    uint32_t     count;
    unsigned int i;
    size_t       before = g_allocations;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    for (i = 0; i < 3; i++) {
        assert(HandleProviderCreate(registration, 1, &resource[i]) == OS_EOK);
        assert(DestroyHandle(resource[i]) == OS_EOK);
    }
    // Final close queues both cleanup and its wakeup without allocating memory.
    g_failAllocation = 0;
    __Drain();
    g_failAllocation = -1;
    __ExpectWakeup(queue, registration);
    __ExpectNoWakeup(queue);

    // Even an invalid capacity must replace the consumed wakeup while work
    // remains pending on a valid registration.
    assert(HandleProviderGetEvents(registration, ids, 0, &count) == OS_EINVALPARAMS);
    __ExpectWakeup(queue, registration);
    __ExpectNoWakeup(queue);

    assert(HandleProviderGetEvents(registration, ids, 1, &count) == OS_EOK);
    assert(count == 1 && ids[0] == resource[0]);
    __ExpectWakeup(queue, registration);
    __ExpectNoWakeup(queue);

    g_failAllocation = 0;
    assert(HandleProviderGetEvents(registration, ids, 3, &count) == OS_EOOM);
    g_failAllocation = -1;
    __ExpectWakeup(queue, registration);
    __ExpectNoWakeup(queue);
    g_failCopy = 1;
    assert(HandleProviderGetEvents(registration, ids, 3, &count) == OS_EINVALPARAMS);
    g_failCopy = -1;
    __ExpectWakeup(queue, registration);
    assert(HandleProviderGetEvents(registration, ids, 3, &count) == OS_EOK);
    assert(count == 2 && ids[0] == resource[1] && ids[1] == resource[2]);
    __ExpectNoWakeup(queue);

    // A new empty-to-nonempty transition posts again. Direct draining leaves at
    // most one stale wakeup, even when partial reads request more notifications.
    assert(HandleProviderCreate(registration, 1, &resource[0]) == OS_EOK);
    assert(DestroyHandle(resource[0]) == OS_EOK);
    __Drain();
    assert(HandleProviderGetEvents(registration, ids, 3, &count) == OS_EOK && count == 1);
    __ExpectWakeup(queue, registration);
    assert(HandleProviderGetEvents(registration, ids, 3, &count) == OS_EOK && count == 0);
    __ExpectNoWakeup(queue);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestSharedQueueAndClose(
    _In_ MSContextSecurity_t* provider)
{
    uuid_t             queue, first, second, resource, watched, ids[1];
    uint32_t           count;
    int                eventsRead;
    struct ioset_event events[2];
    struct ioset_event watch = {.events = IOSETIN};
    size_t             before = g_allocations;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &first) == OS_EOK);
    assert(HandleProviderRegister(queue, &second) == OS_EOK);
    assert(HandleProviderCreate(first, 1, &resource) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    assert(HandleProviderCreate(second, 1, &resource) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    assert(DestroyHandle(first) == OS_EOK);
    __Drain();

    // A queued event contains only the ID and survives registration close. Each
    // registration has its own event slot even when both use the same queue.
    __ExpectWakeup(queue, first);
    __ExpectWakeup(queue, second);
    __ExpectNoWakeup(queue);
    assert(HandleProviderGetEvents(second, ids, 1, &count) == OS_EOK && count == 1);

    // Direct notifications and watched events share output capacity. Existing
    // userspace poll results must also be preserved.
    assert(HandleProviderCreate(UUID_INVALID, 0, &watched) == OS_EOK);
    watch.data.handle = watched;
    assert(ControlHandleSet(queue, IOSET_ADD, watched, &watch) == OS_EOK);
    assert(MarkHandle(watched, IOSETIN) == OS_EOK);
    assert(HandleProviderCreate(second, 1, &resource) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    events[0] = (struct ioset_event){.events = IOSETOUT, .data.handle = UUID_INVALID};
    assert(WaitForHandleSet(queue, NULL, events, 2, 1, NULL, &eventsRead) == OS_EOK);
    assert(eventsRead == 2 && events[0].events == IOSETOUT);
    assert(events[0].data.handle == UUID_INVALID && events[1].data.handle == watched);
    __ExpectWakeup(queue, second);
    __ExpectNoWakeup(queue);

    // A partial cleanup read must append its new wakeup behind an IPC/device
    // event already waiting, even when the consumer requests one event at a time.
    assert(HandleProviderCreate(second, 1, &resource) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    assert(MarkHandle(watched, IOSETIN) == OS_EOK);
    assert(HandleProviderGetEvents(second, ids, 1, &count) == OS_EOK && count == 1);
    __ExpectWakeup(queue, watched);
    __ExpectWakeup(queue, second);
    __ExpectNoWakeup(queue);
    assert(ControlHandleSet(queue, IOSET_DEL, watched, NULL) == OS_EOK);
    assert(DestroyHandle(watched) == OS_EOK);

    // Queue close does not end a registration. Its independent queue reference
    // remains until registration close, including with an undelivered wakeup.
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(HandleProviderGetEvents(second, ids, 1, &count) == OS_EOK && count == 1);
    assert(HandleProviderCreate(second, 1, &resource) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    assert(DestroyHandle(second) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestSingleUseReferences(
    _In_ MSContextSecurity_t* provider,
    _In_ MSContextSecurity_t* client)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeProvider,
        .TargetType = GrantTokenTargetTypeSpecific,
        .Scope = GrantTokenScopeRead,
        .Flags = GrantTokenFlagsSingleUse
    };
    GrantToken_t token;
    uuid_t       queue, registration, resource, imported, output, pending;
    uint32_t     count;
    int          failure;
    size_t       before = g_allocations;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 1, &resource) == OS_EOK);
    options.TargetGlobalHandleID = client->GlobalHandleID;
    assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    g_current = client;

    // A failed token copy happens before the import, so the same token must
    // still produce the first reference.
    g_failCopy = 0;
    assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;
    assert(ScHandleImport(&token, UUID_INVALID, &imported) == OS_EOK);
    assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_ECANCELLED);
    __Drain();
    g_current = provider;
    __ExpectNoWakeup(queue);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, imported, 1, &output) == OS_EOK);
    assert(output == resource);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    g_current = client;
    assert(DestroyHandle(imported) == OS_EOK);
    __Drain();
    g_current = provider;
    __ExpectWakeup(queue, registration);
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK);
    assert(count == 1 && pending == resource);
    __Drain();

    // A failed reference allocation still uses up the token. The source
    // reference handed out by the import is released, so the resource is
    // queued for cleanup without any reference reaching the client.
    for (failure = 0; failure < 2; failure++) {
        assert(HandleProviderCreate(registration, 1, &resource) == OS_EOK);
        assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
        assert(DestroyHandle(resource) == OS_EOK);
        g_current = client;
        output = 0x1234;
        g_failAllocation = failure;
        assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_EOOM);
        g_failAllocation = -1;
        assert(output == 0x1234);
        assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_ECANCELLED);
        __Drain();
        g_current = provider;
        __ExpectWakeup(queue, registration);
        assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK);
        assert(count == 1 && pending == resource);
        assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
        __Drain();
    }

    // Output-copy failure happens after successful reference creation. It closes
    // that reference, but must neither restore the token nor delay final cleanup
    // until the creator explicitly closes the consumed token's control handle.
    assert(HandleProviderCreate(registration, 1, &resource) == OS_EOK);
    assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);
    g_current = client;
    g_failCopy = 1;
    assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;
    assert(ScHandleImport(&token, UUID_INVALID, &output) == OS_ECANCELLED);
    __Drain();
    g_current = provider;
    __ExpectWakeup(queue, registration);
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK);
    assert(count == 1 && pending == resource);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestConsumerCleanup(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* parent,
    _In_ bool                 exitChild)
{
    MSContextSecurity_t child;
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeProvider,
        .Scope = GrantTokenScopeRead,
        .TargetType = GrantTokenTargetTypeSpecific,
        .Flags = GrantTokenFlagsSingleUse
    };
    GrantToken_t initialToken;
    GrantToken_t childToken;
    uuid_t       queue;
    uuid_t       registration;
    uuid_t       resource;
    uuid_t       parentReference;
    uuid_t       childReference;
    uuid_t       resolved;
    uuid_t       pending;
    uint32_t     count;
    size_t       before = g_allocations;

    __NewGroup(&child);
    g_current = service;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 1, &resource) == OS_EOK);
    options.TargetGlobalHandleID = parent->GlobalHandleID;
    assert(GrantTokenCreate(resource, &options, &initialToken) == OS_EOK);
    assert(DestroyHandle(resource) == OS_EOK);

    // File and socket services use this same handoff. The service keeps the
    // resource ID for its object table without keeping a consumer reference.
    g_current = parent;
    assert(ScHandleImport(&initialToken, UUID_INVALID, &parentReference) == OS_EOK);
    options.TargetType = GrantTokenTargetTypeAny;
    options.TargetGlobalHandleID = UUID_INVALID;
    assert(GrantTokenCreate(parentReference, &options, &childToken) == OS_EOK);
    g_current = &child;
    assert(ScHandleImport(&childToken, UUID_INVALID, &childReference) == OS_EOK);
    assert(AcquireHandle(childReference, HandleTypeProviderReference, NULL) == OS_EOK);
    __Drain();

    g_current = service;
    assert(HandleProviderValidate(registration, parent->GlobalHandleID,
        parentReference, 1, &resolved) == OS_EOK && resolved == resource);
    assert(HandleProviderValidate(registration, child.GlobalHandleID,
        childReference, 1, &resolved) == OS_EOK && resolved == resource);
    __ExpectNoWakeup(queue);

    g_current = parent;
    assert(DestroyHandle(parentReference) == OS_EOK);
    __Drain();
    g_current = service;
    __ExpectNoWakeup(queue);
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK && count == 0);
    assert(HandleProviderValidate(registration, child.GlobalHandleID,
        childReference, 1, &resolved) == OS_EOK && resolved == resource);

    // Dropping a temporary use of the child's grant cannot produce cleanup.
    // Its final close, or destruction of its whole grant table, produces one
    // resource ID. Libos descriptor aliases are tested in inheritance_test.
    g_current = &child;
    assert(DestroyHandle(childReference) == OS_EINCOMPLETE);
    __Drain();
    g_current = service;
    __ExpectNoWakeup(queue);
    if (exitChild) {
        DestroyHandleGrantsTable(child.HandleTable);
    } else {
        g_current = &child;
        assert(DestroyHandle(childReference) == OS_EOK);
        DestroyHandleGrantsTable(child.HandleTable);
    }
    g_current = service;
    ReleaseGlobalHandle(child.GlobalHandleID);
    __Drain();
    __ExpectWakeup(queue, registration);
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK);
    assert(count == 1 && pending == resource);
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK && count == 0);
    __ExpectNoWakeup(queue);

    // Consumed export controls need closing, but do not delay final-reference
    // cleanup. This is the existing token behavior, with no extra socket hook.
    assert(GrantTokenRevoke(initialToken.ControlHandleID) == OS_EOK);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    g_current = parent;
    assert(GrantTokenRevoke(childToken.ControlHandleID) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestGroupExit(
    _In_ MSContextSecurity_t* client)
{
    uuid_t              queue;
    MSContextSecurity_t provider;
    uuid_t              registration, resource, imported, result;
    GrantToken_t        token;
    size_t              before = g_allocations;

    __NewGroup(&provider);
    g_current = &provider;
    queue = CreateHandleSet(0);
    assert(queue != UUID_INVALID);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 0, &resource) == OS_EOK);
    token = __Token(resource, 0, UUID_INVALID);
    g_current = client;
    assert(HandleProviderImport(&token, NULL, &imported) == OS_EOK);
    g_current = &provider;
    assert(HandleProviderValidate(registration, client->GlobalHandleID, imported, 0, &result) ==
           OS_EOK);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, imported, 1, &result) ==
           OS_EPERMISSIONS);

    // A client-created export can outlive the provider group. Simulate the
    // production ordering: the context enters destruction before grant cleanup.
    g_current = client;
    token = __Token(imported, 0, UUID_INVALID);
    ReleaseGlobalHandle(provider.GlobalHandleID);
    assert(HandleProviderImport(&token, NULL, &result) == OS_ECANCELLED);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    DestroyHandleGrantsTable(provider.HandleTable);
    g_current = client;
    assert(HandleProviderImport(&token, NULL, &result) != OS_EOK);
    assert(DestroyHandle(imported) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__DeleteTestMemorySpace(
    _In_ void* resource)
{
    MemorySpace_t* memorySpace = resource;

    if (memorySpace->GroupGlobalHandleID != UUID_INVALID) {
        ReleaseGlobalHandle(memorySpace->GroupGlobalHandleID);
    }
    kfree(memorySpace);
}

static uuid_t
__TestMemorySpace(
    _In_ MSContextSecurity_t* group,
    _In_ unsigned int        flags)
{
    MemorySpace_t* memorySpace;
    uuid_t         id;

    memorySpace = kmalloc(sizeof(*memorySpace));
    assert(memorySpace != NULL);
    memorySpace->Flags = flags;
    memorySpace->GroupGlobalHandleID = group != NULL ? group->GlobalHandleID : UUID_INVALID;
    if (group != NULL) {
        assert(AcquireGlobalHandle(group->GlobalHandleID, HandleTypeMSContext, NULL) == OS_EOK);
    }
    id = CreateHandle(HandleTypeMemorySpace, NULL, __DeleteTestMemorySpace, memorySpace);
    assert(id != UUID_INVALID);
    return id;
}

static void
__DeleteTestReceiver(
    _In_ void* resource)
{
    MSContextSecurity_t* receiver = resource;

    // Output-copy rollback must have removed the new grant before the final
    // group reference ends, even when the memory space closed during the copy.
    assert(receiver->HandleTable->Table.element_count == 0);
    DestroyHandleGrantsTable(receiver->HandleTable);
    kfree(receiver);
}

static void
__TestDestinationImport(
    _In_ MSContextSecurity_t* provider,
    _In_ MSContextSecurity_t* manager,
    _In_ MSContextSecurity_t* receiver)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeProvider,
        .TargetType = GrantTokenTargetTypeSpecific,
        .Scope = 0x10,
        .Flags = GrantTokenFlagsSingleUse
    };
    GrantToken_t        token;
    GrantToken_t        wrongReceiver;
    MSContextSecurity_t* temporary;
    uuid_t             queue;
    uuid_t             registration;
    uuid_t             resource;
    uuid_t             memorySpace;
    uuid_t             inheritedSpace;
    uuid_t             kernelSpace;
    uuid_t             ownSpace;
    uuid_t             temporarySpace;
    uuid_t             output;
    uuid_t             imported;
    uuid_t             pending;
    uint32_t           count;
    size_t             receiverGrants;
    size_t             managerGrants;
    size_t             before = g_allocations;
    int                failure;

    g_current = provider;
    queue = CreateHandleSet(0);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 0x30, &resource) == OS_EOK);
    options.TargetGlobalHandleID = receiver->GlobalHandleID;
    assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
    wrongReceiver = __Token(resource, 0x10, manager->GlobalHandleID);

    g_current = manager;
    memorySpace = __TestMemorySpace(receiver, MEMORY_SPACE_APPLICATION);
    inheritedSpace = __TestMemorySpace(receiver, MEMORY_SPACE_APPLICATION | MEMORY_SPACE_INHERIT);
    kernelSpace = __TestMemorySpace(NULL, 0);
    ownSpace = __TestMemorySpace(manager, MEMORY_SPACE_APPLICATION);
    managerGrants = manager->HandleTable->Table.element_count;
    receiverGrants = receiver->HandleTable->Table.element_count;
    output = 0x1234;

    // A known global ID is insufficient. Capability and destination-grant
    // failures leave the single-use token available for an authorized retry.
    g_allowDestination = false;
    assert(ScHandleImport(&token, memorySpace, &output) == OS_EPERMISSIONS);
    g_allowDestination = true;
    g_current = receiver;
    assert(ScHandleImport(&token, memorySpace, &output) == OS_ENOENT);
    g_current = manager;
    assert(ScHandleImport(&token, ownSpace, &output) == OS_EPERMISSIONS);
    assert(ScHandleImport(&token, kernelSpace, &output) == OS_EINVALPARAMS);
    assert(ScHandleImport(&token, UINT32_MAX - 1, &output) == OS_ENOENT);
    assert(ScHandleImport(&token, wrongReceiver.ControlHandleID, &output) == OS_ENOENT);
    assert(ScHandleImport(&wrongReceiver, memorySpace, &output) == OS_EPERMISSIONS);
    assert(output == 0x1234);
    g_failCopy = 0;
    assert(ScHandleImport(&token, memorySpace, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;

    // Neither an inherited space nor a running group needs a startup-state
    // exception. The group, not the particular address space, receives the grant.
    assert(ScHandleImport(&token, inheritedSpace, &imported) == OS_EOK);
    assert(ScHandleImport(&token, memorySpace, &output) == OS_ECANCELLED);
    assert(LookupHandle(imported, HandleTypeProviderReference) == NULL);
    assert(manager->HandleTable->Table.element_count == managerGrants);
    assert(receiver->HandleTable->Table.element_count == receiverGrants + 1);
    g_current = provider;
    assert(HandleProviderValidate(registration, receiver->GlobalHandleID, imported, 0x10, &output) == OS_EOK);
    assert(output == resource);
    assert(HandleProviderValidate(registration, receiver->GlobalHandleID, imported, 0x20, &output) == OS_EPERMISSIONS);
    assert(HandleProviderValidate(registration, manager->GlobalHandleID, imported, 0, &output) == OS_ENOENT);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    g_current = receiver;
    assert(DestroyHandle(imported) == OS_EOK);
    __Drain();

    // Explicitly selecting the caller's own group is permitted with the same
    // authorization. The ordinary NULL-destination wrapper requires no capability.
    g_current = manager;
    assert(ScHandleImport(&wrongReceiver, ownSpace, &imported) == OS_EOK);
    assert(DestroyHandle(imported) == OS_EOK);
    g_allowDestination = false;
    assert(ScHandleImport(&wrongReceiver, UUID_INVALID, &imported) == OS_EOK);
    assert(DestroyHandle(imported) == OS_EOK);
    g_allowDestination = true;
    g_current = provider;
    assert(GrantTokenRevoke(wrongReceiver.ControlHandleID) == OS_EOK);
    __Drain();

    // The token has already been consumed when reference allocation or output
    // publication fails. Neither failure may add a grant to the manager.
    for (failure = 0; failure < 3; failure++) {
        g_current = provider;
        assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
        g_current = manager;
        output = 0x1234;
        if (failure < 2) {
            g_failAllocation = failure;
            assert(ScHandleImport(&token, memorySpace, &output) == OS_EOOM);
            g_failAllocation = -1;
        } else {
            g_failCopy = 1;
            assert(ScHandleImport(&token, memorySpace, &output) == OS_EINVALPARAMS);
            g_failCopy = -1;
        }
        assert(output == 0x1234);
        assert(ScHandleImport(&token, memorySpace, &output) == OS_ECANCELLED);
        assert(receiver->HandleTable->Table.element_count == receiverGrants);
        assert(manager->HandleTable->Table.element_count == managerGrants);
        g_current = provider;
        assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
        __Drain();
    }

    // Let the destination's last memory space close while copying out a new
    // reference. The syscall's group reference must outlive the failing copy.
    temporary = kmalloc(sizeof(*temporary));
    assert(temporary != NULL);
    temporary->HandleTable = CreateHandleGrantsTable();
    temporary->GlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, __DeleteTestReceiver, temporary);
    options.TargetGlobalHandleID = temporary->GlobalHandleID;
    assert(GrantTokenCreate(resource, &options, &token) == OS_EOK);
    g_current = manager;
    temporarySpace = __TestMemorySpace(temporary, MEMORY_SPACE_APPLICATION);
    ReleaseGlobalHandle(temporary->GlobalHandleID);
    g_closeSpaceOnCopy = temporarySpace;
    g_failCopy = 1;
    assert(ScHandleImport(&token, temporarySpace, &output) == OS_EINVALPARAMS);
    g_failCopy = -1;
    __Drain();
    g_current = provider;
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);

    assert(DestroyHandle(resource) == OS_EOK);
    __Drain();
    assert(HandleProviderGetEvents(registration, &pending, 1, &count) == OS_EOK);
    assert(count == 1 && pending == resource);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(queue) == OS_EOK);
    g_current = manager;
    assert(DestroyHandle(memorySpace) == OS_EOK);
    assert(DestroyHandle(inheritedSpace) == OS_EOK);
    assert(DestroyHandle(kernelSpace) == OS_EOK);
    assert(DestroyHandle(ownSpace) == OS_EOK);
    g_allowDestination = false;
    __Drain();
    assert(g_allocations == before);
}

static void
__ExpectNotification(
    _In_ uuid_t       queue,
    _In_ unsigned int flags,
    _In_ uuid_t       context)
{
    struct ioset_event event;
    int               count;

    assert(WaitForHandleSet(queue, NULL, &event, 1, 0, NULL, &count) == OS_EOK);
    assert(count == 1 && event.events == flags && event.data.handle == context);
}

static void
__TestServiceSubscriptionLifetime(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* client)
{
    uuid_t             queue, registration, resource, localReference, clientReference;
    GrantToken_t       token;
    struct ioset_event watch;
    size_t             before = g_allocations;

    g_current = service;
    queue = CreateHandleSet(0);
    assert(HandleProviderRegister(queue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 1, &resource) == OS_EOK);
    token = __Token(resource, 1, UUID_INVALID);
    watch = (struct ioset_event) {.events = IOSETOUT, .data.handle = resource};
    assert(ControlHandleSet(queue, IOSET_ADD, resource, &watch) == OS_EOK);

    // An imported reference in the service process is still a client reference.
    // Only its original resource selector follows registration lifetime.
    assert(HandleProviderImport(&token, NULL, &localReference) == OS_EOK);
    watch = (struct ioset_event) {.events = IOSETIN, .data.handle = localReference};
    assert(ControlHandleSet(queue, IOSET_ADD, localReference, &watch) == OS_EOK);
    assert(MarkHandle(resource, IOSETIN) == OS_EOK);
    assert(DestroyHandle(localReference) == OS_EOK);
    __ExpectNoWakeup(queue);

    g_current = client;
    assert(HandleProviderImport(&token, NULL, &clientReference) == OS_EOK);
    g_current = service;
    assert(DestroyHandle(resource) == OS_EOK);
    assert(GrantTokenRevoke(token.ControlHandleID) == OS_EOK);
    __Drain();
    g_current = client;
    assert(MarkHandle(clientReference, IOSETOUT) == OS_EOK);
    g_current = service;
    assert(DestroyHandle(registration) == OS_EOK);
    __ExpectNoWakeup(queue);
    g_current = client;
    assert(MarkHandle(clientReference, IOSETOUT) == OS_ENOENT);
    assert(DestroyHandle(clientReference) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestNotificationRaces(
    _In_ MSContextSecurity_t* group)
{
    struct ioset_event watch = {.events = IOSETIN, .data.handle = 77};
    struct ioset_event event;
    OSAsyncContext_t   async = {0};
    uuid_t             queue, handle;
    int                count;
    int                references;
    size_t             before = g_allocations;

    g_current = group;
    queue = CreateHandleSet(0);
    handle = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);
    g_closeBeforeSetLock = handle;
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    assert(g_closeBeforeSetLock == UUID_INVALID);
    assert(LookupHandle(handle, HandleTypeGeneric) == NULL);
    assert(MarkHandle(handle, IOSETIN) == OS_ENOENT);
    __ExpectNoWakeup(queue);

    // Removal after taking an event must not invalidate the copied value.
    // Ordinary event handles follow grant cleanup just like shared resources.
    handle = CreateHandle(HandleTypeUserEvent, NULL, NULL, NULL);
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    assert(MarkHandle(handle, IOSETIN) == OS_EOK);
    g_closeAfterSetUnlock = handle;
    __ExpectNotification(queue, IOSETIN, 77);
    assert(g_closeAfterSetUnlock == UUID_INVALID);
    assert(MarkHandle(handle, IOSETIN) == OS_ENOENT);
    __ExpectNoWakeup(queue);

    // Model an asynchronous child resuming after another waiter consumed its
    // event. Its second wait must not create another asynchronous completion.
    references = __LookupGlobalSafe(queue)->References;
    g_waitContinuation = true;
    g_waitCalls = 0;
    assert(WaitForHandleSet(queue, &async, &event, 1, 0, NULL, &count) == OS_ETIMEOUT);
    g_waitContinuation = false;
    assert(g_waitCalls == 2 && __LookupGlobalSafe(queue)->References == references);

    // The parent hands its queue reference to the forked child. The real fork
    // is not executed by this host test; explicitly finish the child's release.
    g_waitFork = true;
    assert(WaitForHandleSet(queue, &async, &event, 1, 0, NULL, &count) == OS_EFORKED);
    g_waitFork = false;
    assert(__LookupGlobalSafe(queue)->References == references + 1);
    assert(ReleaseGlobalHandle(queue) == OS_EINCOMPLETE);
    assert(DestroyHandle(queue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestNotificationRouting(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* client,
    _In_ MSContextSecurity_t* child)
{
    uuid_t             serviceQueue, clientQueue, childQueue;
    uuid_t             registration, resource, readRef, writeRef, childRef, result;
    uint32_t           count;
    GrantToken_t       readToken, writeToken;
    struct ioset_event watch;
    size_t             before = g_allocations;

    g_current = service;
    serviceQueue = CreateHandleSet(0);
    assert(HandleProviderRegister(serviceQueue, &registration) == OS_EOK);
    assert(HandleProviderCreate(registration, 3, &resource) == OS_EOK);
    watch = (struct ioset_event) {.events = IOSETOUT | IOSETCREDIT, .data.handle = resource};
    assert(ControlHandleSet(serviceQueue, IOSET_ADD, resource, &watch) == OS_EOK);
    readToken = __Token(resource, 1, UUID_INVALID);
    writeToken = __Token(resource, 2, UUID_INVALID);

    g_current = client;
    assert(HandleProviderImport(&readToken, NULL, &readRef) == OS_EOK);
    assert(HandleProviderImport(&writeToken, NULL, &writeRef) == OS_EOK);
    clientQueue = CreateHandleSet(0);
    watch = (struct ioset_event) {.events = IOSETIN, .data.handle = 11};
    assert(ControlHandleSet(clientQueue, IOSET_ADD, readRef, &watch) == OS_EOK);
    watch = (struct ioset_event) {.events = IOSETCTL, .data.handle = 22};
    assert(ControlHandleSet(clientQueue, IOSET_ADD, writeRef, &watch) == OS_EOK);

    g_current = child;
    assert(HandleProviderImport(&readToken, NULL, &childRef) == OS_EOK);
    assert(childRef != resource && childRef != readRef && childRef != writeRef);

    g_current = service;
    assert(DestroyHandle(resource) == OS_EOK);
    assert(GrantTokenRevoke(readToken.ControlHandleID) == OS_EOK);
    assert(GrantTokenRevoke(writeToken.ControlHandleID) == OS_EOK);
    __Drain();
    assert(LookupGlobalHandle(resource, HandleTypeProviderReference) == NULL);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, readRef, 2, &result) ==
           OS_EPERMISSIONS);
    assert(HandleProviderValidate(registration, client->GlobalHandleID, writeRef, 1, &result) ==
           OS_EPERMISSIONS);

    // An unsubscribed poster must reach the service after its initial
    // reference has gone. A subscription cannot be the source of the mapping.
    g_current = child;
    assert(MarkHandle(childRef, IOSETOUT | IOSETCREDIT) == OS_EOK);
    g_current = service;
    __ExpectNotification(serviceQueue, IOSETOUT | IOSETCREDIT, resource);
    assert(MarkHandle(resource, IOSETIN | IOSETCTL) == OS_EOK);
    g_current = client;
    __ExpectNotification(clientQueue, IOSETIN, 11);
    __ExpectNotification(clientQueue, IOSETCTL, 22);

    // MOD and DEL select R, even when both entries dispatch on S. Removing a
    // queued entry must also remove its contribution to the pending count.
    watch = (struct ioset_event) {.events = IOSETIN, .data.handle = 33};
    assert(ControlHandleSet(clientQueue, IOSET_MOD, writeRef, &watch) == OS_EOK);
    assert(MarkHandle(readRef, IOSETIN) == OS_EOK);
    assert(ControlHandleSet(clientQueue, IOSET_DEL, readRef, NULL) == OS_EOK);
    __ExpectNotification(clientQueue, IOSETIN, 33);
    assert(MarkHandle(readRef, IOSETIN) == OS_EOK);
    assert(DestroyHandle(writeRef) == OS_EOK);
    __ExpectNoWakeup(clientQueue);

    // The same reference may be granted to two groups. Closing one group's
    // grant removes only its subscription, including events already pending.
    watch = (struct ioset_event) {.events = IOSETIN, .data.handle = 44};
    assert(ControlHandleSet(clientQueue, IOSET_ADD, readRef, &watch) == OS_EOK);
    assert(GrantHandle(child, readRef) == OS_EOK);
    g_current = child;
    childQueue = CreateHandleSet(0);
    watch.data.handle = 55;
    assert(ControlHandleSet(childQueue, IOSET_ADD, readRef, &watch) == OS_EOK);
    assert(GrantHandle(service, childQueue) == OS_EOK);
    assert(MarkHandle(childRef, IOSETIN) == OS_EOK);
    assert(DestroyHandle(childRef) == OS_EOK);
    g_current = client;
    assert(DestroyHandle(readRef) == OS_EOK);
    __ExpectNoWakeup(clientQueue);
    g_current = child;
    __ExpectNotification(childQueue, IOSETIN, 55);
    assert(MarkHandle(readRef, IOSETIN) == OS_EOK);

    // Group exit must remove watches even when another group keeps the queue
    // alive. No subscription may delay the final service cleanup record.
    g_current = NULL;
    DestroyHandleGrantsTable(child->HandleTable);
    child->HandleTable = CreateHandleGrantsTable();
    __Drain();
    g_current = service;
    __ExpectNoWakeup(childQueue);
    __ExpectWakeup(serviceQueue, registration);
    assert(HandleProviderGetEvents(registration, &result, 1, &count) == OS_EOK);
    assert(count == 1 && result == resource);
    assert(ControlHandleSet(serviceQueue, IOSET_DEL, resource, NULL) == OS_EOK);
    assert(DestroyHandle(registration) == OS_EOK);
    assert(DestroyHandle(serviceQueue) == OS_EOK);
    assert(DestroyHandle(childQueue) == OS_EOK);
    g_current = client;
    assert(DestroyHandle(clientQueue) == OS_EOK);
    __Drain();
    assert(g_allocations == before);
}

#include "handle_set_reference_test.inc"

int
main(void)
{
    MSContextSecurity_t provider, client, stranger;
    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    // Reserve zero; IPC principals use zero to mean missing credentials.
    (void)AllocateHandleId();
    __NewGroup(&provider);
    __NewGroup(&client);
    __NewGroup(&stranger);
    __TestNotificationReferences(&client);
    __TestReferences(&provider, &client, &stranger);
    __TestShutdown(&provider, &client);
    __TestFailures(&provider);
    __TestCleanupWakeups(&provider);
    __TestSharedQueueAndClose(&provider);
    __TestSingleUseReferences(&provider, &client);
    __TestConsumerCleanup(&provider, &client, false);
    __TestConsumerCleanup(&provider, &client, true);
    __TestGroupExit(&client);
    __TestDestinationImport(&provider, &client, &stranger);
    __TestNotificationRouting(&provider, &client, &stranger);
    __TestNotificationRaces(&client);
    __TestServiceSubscriptionLifetime(&provider, &client);
    g_current = NULL;
    DestroyHandleGrantsTable(provider.HandleTable);
    DestroyHandleGrantsTable(client.HandleTable);
    DestroyHandleGrantsTable(stranger.HandleTable);
    ReleaseGlobalHandle(provider.GlobalHandleID);
    ReleaseGlobalHandle(client.GlobalHandleID);
    ReleaseGlobalHandle(stranger.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0);
    assert(g_allocations == 0);
    puts("Provider handles: local/destination import, rights, validation, cleanup, shutdown and failures passed");
    return 0;
}

// Restricted SHM imports are exercised with the real VM code in shm_restricted_test.
oserr_t
SHMAttachToken(
    _In_  const GrantToken_t*  token,
    _In_  MSContextSecurity_t* receiver,
    _Out_ SHMHandle_t*         handle)
{
    return OS_ENOTSUPPORTED;
}


