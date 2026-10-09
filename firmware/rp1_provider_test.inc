// Uses the real registry, publication group and RP1 provider callbacks. Firmware
// decoding is tested separately; these small inventories isolate ownership.
static const struct PciDevice* g_dmaParent;
static uint32_t g_dmaNode;
static uuid_t g_dmaHostId = 123;
static enum DmDmaCachePolicy g_dmaCachePolicy = DmDmaCacheNonCoherent;

oserr_t
Rp1GetDmaDescription(
    _In_  const struct PciDevice*   parent,
    _In_  uint32_t                  node,
    _Out_ struct PciDmaDescription* description)
{
    // Validate routing through the actual provider while the PCI fixture tests
    // the real configured query. An unexpected query fails this ownership test.
    CHECK(g_dmaParent != NULL && parent == g_dmaParent && node == g_dmaNode);
    CHECK(g_registryLocks == 0 && parent->ProviderReferences != 0);
    memset(description, 0, sizeof(*description));
    description->Host.HostId = g_dmaHostId;
    description->CachePolicy = g_dmaCachePolicy;
    description->Map.Count = 1;
    description->Map.Ranges[0].Length = 0x2000;
    return OS_EOK;
}

oserr_t
FdtEnumerateRp1Children(
    _In_ const struct FdtPciHost* host,
    _In_ const struct PciBar* bars,
    _In_ FdtRp1DeviceFn callback,
    _In_ void* context)
{
    // These registry tests supply inventory directly. Catch accidental discovery
    // rather than making this stub appear to validate any firmware.
    (void)host; (void)bars; (void)callback; (void)context;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

const struct PciHostIdentification*
PciHostGetIdentification(
    _In_ const PciHost_t* host)
{
    // Only present for the unused attachment constructor's link dependencies.
    (void)host;
    CHECK(0);
    return NULL;
}

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    // These ownership operations require no discovery or hardware diagnostics.
    (void)level; (void)format;
    CHECK(0);
}

static struct Rp1Bus*
__ProviderInventory(
    _In_ PciDevice_t* parent)
{
    struct Rp1Bus* bus = calloc(1, sizeof(*bus));
    struct Rp1Child* first = calloc(1, sizeof(*first));
    struct Rp1Child* second = calloc(1, sizeof(*second));

    // Two children expose mistakes where releasing one child releases the bus.
    CHECK(bus != NULL && first != NULL && second != NULL);
    bus->Parent = parent;
    atomic_init(&bus->ProviderReferences, 0);
    bus->Children = first;
    bus->ChildCount = 2;
    first->Bus = bus;
    first->Firmware.Name = "usb@200000";
    first->Firmware.NodeOffset = 100;
    first->Firmware.Compatible = "snps,dwc3";
    first->Firmware.CompatibleLength = sizeof("snps,dwc3");
    first->Firmware.RegisterCount = 1;
    first->Firmware.Registers[0].Base = 0x1f00200000ULL;
    first->Firmware.Registers[0].Length = 0x10000;
    *second = *first;
    second->Firmware.Name = "usb@300000";
    second->Firmware.NodeOffset = 200;
    second->Firmware.Registers[0].Base = 0x1f00300000ULL;
    first->Next = second;
    return bus;
}

static void
__ProviderParent(
    _InOut_ struct DmPublicationGroup* group,
    _InOut_ PciDevice_t* parent)
{
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Provider = { .Operations = &g_pciDeviceProviderOperations, .Context = parent }
    };

    // The parent registry reference and its child references share one PCI count.
    CHECK(DmPublicationAdd(group, &registration, 0, &parent->DeviceId) == OS_EOK);
    CHECK(parent->ProviderReferences == 1);
}

static void
__TestRp1Providers(void)
{
    struct DmPublicationGroup group = { 0 };
    struct DmPublicationGroup otherGroup = { 0 };
    PciDevice_t parent = { 0 };
    PciDevice_t otherParent = { 0 };
    struct Rp1Bus* bus = __ProviderInventory(&parent);
    struct Rp1Bus* other = __ProviderInventory(&otherParent);
    struct Rp1Child* first = bus->Children;
    struct Rp1Child* second = first->Next;
    struct DMDevice* request;
    struct DmDeviceProvider retained;
    struct OSIOCtlBusControl control = { 0 };
    size_t value = 0;
    unsigned int forgotten;

    __ProviderParent(&group, &parent);
    __ProviderParent(&otherGroup, &otherParent);
    CHECK(Rp1BusPublish(bus, &otherParent, &group) == OS_EINVALPARAMS);
    CHECK(parent.ProviderReferences == 1 && otherParent.ProviderReferences == 1);

    // A standalone firmware inventory must not supply a provider. Restoring its
    // parent afterward is test setup only; attached buses never change parents.
    bus->Parent = NULL;
    CHECK(__Rp1ProviderRetain(first) == OS_ENOTSUPPORTED);
    CHECK(atomic_load(&bus->ProviderReferences) == 0);
    bus->Parent = &parent;

    // Overflow must fail before RP1 increments its own count. A wrapped PCI
    // count could otherwise let host destruction proceed with live children.
    parent.ProviderReferences = UINT_MAX;
    CHECK(__Rp1ProviderRetain(first) == OS_EOVERFLOW);
    CHECK(parent.ProviderReferences == UINT_MAX && atomic_load(&bus->ProviderReferences) == 0);
    parent.ProviderReferences = 1;

    g_failPublicationAllocation = 1;
    CHECK(Rp1BusPublish(bus, &parent, &group) == OS_EOOM);
    g_failPublicationAllocation = 0;
    g_failAllocation = 1;
    CHECK(Rp1BusPublish(bus, &parent, &group) == OS_EOOM);
    g_failAllocation = 0;
    CHECK(parent.ProviderReferences == 1 && atomic_load(&bus->ProviderReferences) == 0);

    // Invalid parent registration fails after Retain. The real registry must
    // balance that retain, including when an earlier child is already published.
    CHECK(__Rp1PublishChild(&group, UINT_MAX, first) == OS_ENOENT);
    CHECK(parent.ProviderReferences == 1 && first->DeviceId == UUID_INVALID);
    CHECK(__Rp1PublishChild(&group, parent.DeviceId, first) == OS_EOK);
    CHECK(__Rp1PublishChild(&group, UINT_MAX, second) == OS_ENOENT);
    CHECK(parent.ProviderReferences == 2 && atomic_load(&bus->ProviderReferences) == 1);
    CHECK(second->DeviceId == UUID_INVALID);
    CHECK(Rp1BusDestroy(bus) == OS_EBUSY);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(parent.ProviderReferences == 0 && atomic_load(&bus->ProviderReferences) == 0);
    CHECK(first->DeviceId == UUID_INVALID);
    CHECK(DmPublicationReset(&group) == OS_EOK);
    __ProviderParent(&group, &parent);

    CHECK(Rp1BusPublish(bus, &parent, &group) == OS_EOK);
    CHECK(Rp1BusPublish(other, &otherParent, &otherGroup) == OS_EOK);
    CHECK(parent.ProviderReferences == 3 && otherParent.ProviderReferences == 3);
    CHECK(atomic_load(&bus->ProviderReferences) == 2);
    CHECK(__GetDeviceUnsafe(first->DeviceId)->Provider.Context == first);
    CHECK(((PlatformDevice_t*)__GetDeviceUnsafe(first->DeviceId)->device)->Pending ==
        (PLATFORM_DEVICE_PENDING_DMA | PLATFORM_DEVICE_PENDING_INTERRUPTS));
    CHECK(!DmDeviceIsBindable(first->DeviceId));
    CHECK(DmHandleIoctl(first->DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(first->DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0, 0, 4, &value) == OS_ENOTSUPPORTED);

    // Hold a real registry request while removal is attempted. Child entries,
    // parent references and the group must survive until that request finishes.
    CHECK(__AcquireDeviceRequest(second->DeviceId, &request) == OS_EOK);
    forgotten = g_forgotten;
    CHECK(DmPublicationRemove(&group) == OS_EBUSY);
    CHECK(g_forgotten == forgotten && parent.ProviderReferences == 3);
    CHECK(second->DeviceId != UUID_INVALID && parent.DeviceId != UUID_INVALID);
    CHECK(Rp1BusDestroy(bus) == OS_EBUSY);
    __ReleaseDeviceRequest(request);

    // A separate provider owner survives registry removal. Release must leave
    // the child allocated so the group can clear its saved ID after the callback.
    retained = __GetDeviceUnsafe(first->DeviceId)->Provider;
    CHECK(retained.Operations->Retain(retained.Context) == OS_EOK);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(first->DeviceId == UUID_INVALID && second->DeviceId == UUID_INVALID);
    CHECK(parent.ProviderReferences == 1 && atomic_load(&bus->ProviderReferences) == 1);
    CHECK(Rp1BusDestroy(bus) == OS_EBUSY);
    CHECK(otherParent.ProviderReferences == 3 && atomic_load(&other->ProviderReferences) == 2);
    retained.Operations->Release(retained.Context);
    CHECK(parent.ProviderReferences == 0 && atomic_load(&bus->ProviderReferences) == 0);
    CHECK(Rp1BusDestroy(bus) == OS_EOK);
    CHECK(DmPublicationRemove(&otherGroup) == OS_EOK);
    CHECK(otherParent.ProviderReferences == 0);
    CHECK(Rp1BusDestroy(other) == OS_EOK);
    CHECK(Rp1BusDestroy(NULL) == OS_EOK);
    puts("RP1 providers: exact contexts, failure rollback, active requests, retained children and independent parents passed");
}

static void
__TestRp1DmaLease(void)
{
    PciDevice_t parent = { 0 };
    struct Rp1Bus* bus = __ProviderInventory(&parent);
    struct DmPublicationGroup group = { 0 };
    struct DmDmaLease* lease = NULL;
    struct Rp1Child* child = bus->Children;

    // A surviving lease must also keep the saved child ID and PCI owner alive
    // when removal through the real publication group is retried later.
    __ProviderParent(&group, &parent);
    CHECK(Rp1BusPublish(bus, &parent, &group) == OS_EOK);
    g_dmaParent = &parent;
    g_dmaNode = child->Firmware.NodeOffset;
    // An anonymous host or unknown cache behavior must not produce a lease.
    g_dmaHostId = UUID_INVALID;
    CHECK(DmDevicePrepareDma(child->DeviceId, &lease) == OS_EBUSY && lease == NULL);
    g_dmaHostId = 123;
    g_dmaCachePolicy = DmDmaCacheUnknown;
    CHECK(DmDevicePrepareDma(child->DeviceId, &lease) == OS_ENOTSUPPORTED && lease == NULL);
    g_dmaCachePolicy = DmDmaCacheNonCoherent;
    CHECK(DmDevicePrepareDma(child->DeviceId, &lease) == OS_EOK);
    CHECK(DmDmaLeaseGetDescription(lease)->DeviceId == child->DeviceId);
    CHECK(((PlatformDevice_t*)__GetDeviceUnsafe(child->DeviceId)->device)->Pending ==
        (PLATFORM_DEVICE_PENDING_DMA | PLATFORM_DEVICE_PENDING_INTERRUPTS));
    CHECK(DmPublicationRemove(&group) == OS_EBUSY);
    CHECK(child->DeviceId != UUID_INVALID && parent.ProviderReferences != 0);
    CHECK(Rp1BusDestroy(bus) == OS_EBUSY);
    DmDmaLeaseRelease(&lease);
    CHECK(DmPublicationRemove(&group) == OS_EOK);
    CHECK(parent.ProviderReferences == 0);
    CHECK(DmPublicationReset(&group) == OS_EOK);
    CHECK(Rp1BusDestroy(bus) == OS_EOK);
    g_dmaParent = NULL;
}

