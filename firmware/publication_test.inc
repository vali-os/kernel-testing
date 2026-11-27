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
    CHECK(group.State == DmPublicationCollecting);
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
    registration.Bus.Operations = &g_firstOperations;
    registration.Bus.Context = &provider;
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

// DmPublicationAddCopy gives the registry its own copy of a description and
// of a name, and keeps nothing when the group refuses the device. The address
// sanitizer's leak check sees whether the refused copies are freed.
static void
__TestPublicationCopies(void)
{
    struct DmPublicationGroup group = { 0 };
    char  callerName[] = "caller's name";
    BusDevice_t description = {
        .Base = { .Length = sizeof(BusDevice_t), .VendorId = 0x1234, .ProductId = 0x5678 },
        .Slot = 3,
        .Function = 7,
        .InterruptLine = 9
    };
    struct DmDeviceRegistration registration = {
        .Description = &description.Base,
        .Kind = DmDeviceDescriptionBus
    };
    const BusDevice_t* copy;
    DmDevice_t*        registered;
    uuid_t       rootId = UUID_INVALID;
    uuid_t       refusedId = UUID_INVALID;

    // The caller's own strings stay the caller's. The copy holds the whole
    // structure, the name, and no other identification string.
    description.Base.Identification.Manufacturer = callerName;
    CHECK(DmPublicationAddCopy(&group, &registration, "copied device", 0, &rootId) == OS_EOK);
    CHECK(rootId != UUID_INVALID);
    registered = DmDeviceFindLocked(rootId);
    CHECK(registered != NULL && registered->Device != &description.Base);
    copy = (const BusDevice_t*)registered->Device;
    CHECK(copy->Base.Length == sizeof(BusDevice_t));
    CHECK(copy->Base.VendorId == 0x1234 && copy->Base.ProductId == 0x5678);
    CHECK(copy->Slot == 3 && copy->Function == 7 && copy->InterruptLine == 9);
    CHECK(!strcmp(registered->Device->Identification.Description, "copied device"));
    CHECK(registered->Device->Identification.Manufacturer == NULL);
    CHECK(description.Base.Identification.Manufacturer == callerName);
    CHECK(description.Base.Identification.Description == NULL);

    // A description shorter than Device_t cannot be copied.
    description.Base.Length = sizeof(Device_t) - 1;
    CHECK(DmPublicationAddCopy(&group, &registration, "short", 0, &refusedId) == OS_EINVALPARAMS);
    CHECK(refusedId == UUID_INVALID);
    description.Base.Length = sizeof(BusDevice_t);

    // The group cannot record the device: the copies are freed and the
    // caller's description is as it was.
    g_failPublicationAllocation = 1;
    CHECK(DmPublicationAddCopy(&group, &registration, "refused", 0, &refusedId) == OS_EOOM);
    g_failPublicationAllocation = 0;
    CHECK(refusedId == UUID_INVALID && description.Function == 7);

    // A group that no longer collects refuses the device as well.
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationAddCopy(&group, &registration, "late", 0, &refusedId) == OS_EBUSY);
    CHECK(refusedId == UUID_INVALID);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(rootId == UUID_INVALID);
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
    registration.Bus.Operations = &g_firstOperations;
    registration.Bus.Context = &provider;
    CHECK(DmPublicationAdd(&group, &registration, 1, &provider.DeviceId) == OS_EOK);
    registration.Description = __NewDescription();
    registration.Description->ParentId = rootId;
    memset(&registration.Bus, 0, sizeof(registration.Bus));
    CHECK(DmPublicationAdd(&group, &registration, 1, &siblingId) == OS_EOK);
    CHECK(!DmDeviceIsBindable(provider.DeviceId) && !DmDeviceIsBindable(siblingId));

    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
    g_failPublicationDmaAt = g_publicationDmaCalls + 2;
    CHECK(DmPublicationPrepareDma(&group) == OS_EOOM && group.State != DmPublicationDmaReady);
    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
    // The first entry succeeded. Retrying visits only the failed child and its
    // sibling, preserving registration IDs that may already exist in the kernel.
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    CHECK(g_publicationDmaCalls == g_failPublicationDmaAt + 2 && group.State == DmPublicationDmaReady);
    CHECK(DmPublicationIsFinished(&group));
    registration.Description = __NewDescription();
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EBUSY);
    CHECK(rejectedId == UUID_INVALID);
    g_failPublicationBindingAt = calls + 2;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOOM);
    CHECK(DmPublicationIsFinished(&group) && g_devices.count == 3);
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
    {
        DmDevice_t* active;
        // Hold a registry request while another removal pass walks siblings.
        // This models concurrent removal without re-entering lifecycle control
        // from inside a bus callback, which the production lock order forbids.
        CHECK(DmDeviceRequestAcquire(provider.DeviceId, &active) == OS_EOK);
        __RemoveDuringRequest(&provider);
        DmDeviceRequestRelease(active);
    }
    CHECK(group.State == DmPublicationRemoving);
    CHECK(siblingId == UUID_INVALID && provider.DeviceId != UUID_INVALID);
    CHECK(rootId != UUID_INVALID && g_devices.count == 2);
    CHECK(provider.References == 1 && provider.Releases == 0);
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EBUSY);
    CHECK(DmPublicationFinish(&group) == OS_EBUSY);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(group.State == DmPublicationCollecting && g_devices.count == 0);
    CHECK(rootId == UUID_INVALID && provider.DeviceId == UUID_INVALID);
    CHECK(provider.References == 0 && provider.Releases == 1);

    // Completed removal allows new descriptions with no previous binding state.
    CHECK(group.State == DmPublicationCollecting);
    CHECK(DmPublicationAdd(&group, &registration, 1, &rejectedId) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(g_publicationBindingCalls == calls + 4);
    CHECK(DmPublicationRemove(&group) == OS_EOK && rejectedId == UUID_INVALID);
    CHECK(group.State == DmPublicationCollecting);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    puts("Publication groups: ownership, allocation failures, binding retries and busy removal passed");
}
