// These tests use the real registry. Only allocation and binding failures are
// injected so ownership, request lifetime, and parent checks remain exercised.
static int          g_failPublicationAllocation;
static unsigned int g_publicationBindingCalls;
static unsigned int g_failPublicationBindingAt;

static void*
__PublicationCalloc(
    _In_ size_t count,
    _In_ size_t size)
{
    // Fail only the group's bookkeeping allocation, leaving the registry intact.
    if (g_failPublicationAllocation) {
        return NULL;
    }
    return calloc(count, size);
}

static oserr_t
__PublicationBind(
    _In_ uuid_t id)
{
    g_publicationBindingCalls++;
    // Fail the chosen attempt to verify that earlier successes are not repeated.
    if (g_publicationBindingCalls == g_failPublicationBindingAt) {
        return OS_EOOM;
    }
    return DmDeviceEnableDriverBinding(id);
}

static void
__TestPublicationFailures(void)
{
    struct DmPublicationGroup group = { 0 };
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric
    };
    uuid_t rootId = UUID_INVALID;
    uuid_t childId = UUID_INVALID;
    uuid_t earlierChildren[3] = { 0 };
    unsigned int index;

    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
    CHECK(DmPublicationReset(&group) == OS_EBUSY);
    CHECK(DmPublicationAdd(&group, &registration, 0, &rootId) == OS_EOK);
    CHECK(!DmDeviceIsBindable(rootId));

    // Keep several successful children ahead of each injected failure. Cleanup
    // must remove all of them through the real registry's parent checks.
    for (index = 0; index < 3; index++) {
        registration.Description = __NewDescription();
        registration.Description->ParentId = rootId;
        CHECK(DmPublicationAdd(&group, &registration, 1, &earlierChildren[index]) == OS_EOK);
        CHECK(!DmDeviceIsBindable(earlierChildren[index]));
    }

    registration.Description = __NewDescription();
    registration.Description->ParentId = rootId;
    registration.Provider.Operations = &g_firstOperations;
    registration.Provider.Context = &provider;
    g_failPublicationAllocation = 1;
    CHECK(DmPublicationAdd(&group, &registration, 1, &childId) == OS_EOOM);
    CHECK(childId == UUID_INVALID && provider.Retains == 0);
    CHECK(g_devices.count == 4 && group.First != group.Last);
    g_failPublicationAllocation = 0;

    g_failAllocation = 1;
    CHECK(DmPublicationAdd(&group, &registration, 1, &childId) == OS_EOOM);
    CHECK(childId == UUID_INVALID && provider.Retains == 0);
    CHECK(g_devices.count == 4 && group.First != group.Last);
    g_failAllocation = 0;

    provider.FailRetain = 1;
    CHECK(DmPublicationAdd(&group, &registration, 1, &childId) == OS_EUNKNOWN);
    CHECK(provider.References == 0 && provider.Releases == 0);
    provider.FailRetain = 0;
    CHECK(DmPublicationAdd(&group, &registration, 1, &rootId) == OS_EBUSY);
    CHECK(rootId != UUID_INVALID && provider.Retains == 1);

    // A bad parent fails after retaining the provider. That reference must be
    // released while the caller keeps the same description for another attempt.
    registration.Description->ParentId = 0x7fffffff;
    CHECK(DmPublicationAdd(&group, &registration, 1, &childId) == OS_ENOENT);
    CHECK(provider.References == 0 && provider.Releases == 1);
    registration.Description->ParentId = rootId;
    CHECK(DmPublicationAdd(&group, &registration, 1, &childId) == OS_EOK);
    CHECK(!DmDeviceIsBindable(childId));
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(rootId == UUID_INVALID && childId == UUID_INVALID);
    for (index = 0; index < 3; index++) {
        CHECK(earlierChildren[index] == UUID_INVALID);
    }
    CHECK(provider.References == 0 && provider.Releases == 2);
    CHECK(group.First == NULL && group.Last == NULL && g_devices.count == 0);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
}

static void
__TestPublicationLifecycle(void)
{
    struct DmPublicationGroup group = { 0 };
    struct __TestProvider provider = { .Group = &group, .RemoveDuringRequest = 1 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric
    };
    struct OSIOCtlBusControl control = { 0 };
    uuid_t rootId = UUID_INVALID;
    uuid_t siblingId = UUID_INVALID;
    uuid_t rejectedId = UUID_INVALID;
    unsigned int calls = g_publicationBindingCalls;

    CHECK(DmPublicationAdd(&group, &registration, 0, &rootId) == OS_EOK);
    registration.Description = __NewDescription();
    registration.Description->ParentId = rootId;
    registration.Provider.Operations = &g_firstOperations;
    registration.Provider.Context = &provider;
    CHECK(DmPublicationAdd(&group, &registration, 1, &provider.DeviceId) == OS_EOK);
    registration.Description = __NewDescription();
    registration.Description->ParentId = rootId;
    memset(&registration.Provider, 0, sizeof(registration.Provider));
    CHECK(DmPublicationAdd(&group, &registration, 1, &siblingId) == OS_EOK);
    CHECK(!DmDeviceIsBindable(provider.DeviceId) && !DmDeviceIsBindable(siblingId));

    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(group.State == DmPublicationReady);
    registration.Description = __NewDescription();
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EBUSY);
    CHECK(rejectedId == UUID_INVALID);
    g_failPublicationBindingAt = calls + 2;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOOM);
    CHECK(group.State == DmPublicationBinding && g_devices.count == 3);
    CHECK(!DmDeviceIsBindable(rootId));
    CHECK(DmDeviceIsBindable(provider.DeviceId) && !DmDeviceIsBindable(siblingId));
    CHECK(DmPublicationFinish(&group) == OS_EBUSY);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(g_publicationBindingCalls == calls + 3);
    CHECK(DmDeviceIsBindable(siblingId));
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(g_publicationBindingCalls == calls + 3);

    // Removal from inside a request can remove the newer sibling, but the
    // provider and its parent must survive until the request returns.
    CHECK(DmHandleIoctl(provider.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    CHECK(group.State == DmPublicationRemoving);
    CHECK(siblingId == UUID_INVALID && provider.DeviceId != UUID_INVALID);
    CHECK(rootId != UUID_INVALID && g_devices.count == 2);
    CHECK(provider.References == 1 && provider.Releases == 0);
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EBUSY);
    CHECK(DmPublicationFinish(&group) == OS_EBUSY);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(group.State == DmPublicationRemoved && g_devices.count == 0);
    CHECK(rootId == UUID_INVALID && provider.DeviceId == UUID_INVALID);
    CHECK(provider.References == 0 && provider.Releases == 1);

    // Explicit reuse starts with new descriptions and no previous binding state.
    CHECK(DmPublicationReset(&group) == OS_EOK);
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(g_publicationBindingCalls == calls + 4);
    CHECK(DmPublicationRemove(&group) == OS_EOK && rejectedId == UUID_INVALID);
    CHECK(DmPublicationReset(&group) == OS_EOK);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    puts("Publication groups: ownership, allocation failures, binding retries and busy removal passed");
}
