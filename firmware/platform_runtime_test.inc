#include <ddk/types/interrupt-controller.h>
#include <ddk/interrupts/provider.h>
/* Kernel boundary model; kernel identity and attach semantics have separate tests. */
static oserr_t g_nativeCloseStatus;
static oserr_t g_nativeAttachStatus;
static unsigned int g_dependencyAcquires;
static unsigned int g_dependencyReleases;
static unsigned int g_dependencyFailAcquire;
static int g_dependencyFailRelease;

oserr_t DeviceInterruptLineSourceResolve(const InterruptLineSourceIdentity_t* identity, uuid_t* id)
{
    if (identity->Version != 1 ||
        identity->FirmwareController != 1 || identity->Line < 32 || identity->Line >= 96) {
        return OS_ENOTSUPPORTED;
    }
    *id = 1;
    return OS_EOK;
}
oserr_t DeviceInterruptProviderAttach(const struct InterruptProviderAttachRequest* request)
{
    CHECK(request->Count == 1 && request->ControllerID == 1);
    CHECK(request->Line == 72 || request->Line == 73);
    CHECK(request->Vectors[0].Completion == UUID_INVALID);
    return g_nativeAttachStatus;
}
oserr_t DeviceInterruptProviderClose(uuid_t allocation, uuid_t principal, uuid_t device)
{
    CHECK(allocation && principal && device);
    return g_nativeCloseStatus;
}
oserr_t DeviceInterruptProviderCheckEnabled(uuid_t allocation, uuid_t principal, uuid_t device)
{
    CHECK(allocation && principal && device);
    return OS_EOK;
}

static oserr_t __DependencyAcquire(void* context, const struct FdtPlatformDependency* dependency, void** token)
{
    CHECK(dependency->Kind == (enum FdtPlatformDependencyKind)g_dependencyAcquires);
    g_dependencyAcquires++;
    if (g_dependencyAcquires == g_dependencyFailAcquire) {
        return OS_EUNKNOWN;
    }
    *token = (void*)(uintptr_t)(dependency->Kind + 1);
    return OS_EOK;
}
static oserr_t __DependencyRelease(void* context, void* token)
{
    if (g_dependencyFailRelease) {
        return OS_EBUSY;
    }
    CHECK((uintptr_t)token == g_dependencyAcquires - g_dependencyReleases -
        (g_dependencyFailAcquire != 0));
    g_dependencyReleases++;
    return OS_EOK;
}
static const struct PlatformDependencyOperations g_testDependencyOperations = {
    .Acquire = __DependencyAcquire, .Release = __DependencyRelease
};

// Fault injection changes cached records only inside this fixture and restores
// each before returning. Production callers hold immutable borrowed records.
static void __NativeFirmwareFailures(struct PlatformBus* bus, struct PlatformEntry* entry)
{
    struct FdtResources* node = (void*)FdtTreeNode(bus->Tree, entry->Firmware.FirmwareNode);
    struct FdtResources* parent = (void*)FdtTreeParent(bus->Tree, node->NodeOffset);
    struct FdtResources savedNode = *node;
    struct FdtResources savedParent = *parent;
    struct FdtResources provider;
    struct FdtResources* clock;
    struct DmDmaMap map;
    struct DmDmaMap before;
    struct FdtPlatformDependencies dependencies;
    struct FdtPlatformDependencies savedDependencies;
    const uint8_t reference[] = { 0, 0, 0, 100, 0, 0, 0, 22 };
    struct FdtProperty property = { "iommus", reference, 4 };

    memset(&map, 0xa5, sizeof(map));
    before = map;
    parent->DmaRanges = NULL;
    parent->DmaRangesLength = 0;
    CHECK(FdtPlatformDma(bus->Tree, node->NodeOffset, &map) == OS_ENOTSUPPORTED);
    CHECK(!memcmp(&map, &before, sizeof(map)));
    *parent = savedParent;
    parent->DmaRangesLength--;
    CHECK(FdtPlatformDma(bus->Tree, node->NodeOffset, &map) == OS_EINVALPARAMS);
    CHECK(!memcmp(&map, &before, sizeof(map)));
    *parent = savedParent;
    parent->View.Properties = &property;
    parent->View.PropertyCount = 1;
    CHECK(FdtPlatformDma(bus->Tree, node->NodeOffset, &map) == OS_ENOTSUPPORTED);
    CHECK(!memcmp(&map, &before, sizeof(map)));
    *parent = savedParent;
    node->View.Properties = &property;
    node->View.PropertyCount = 1;
    CHECK(FdtPlatformDma(bus->Tree, node->NodeOffset, &map) == OS_ENOTSUPPORTED);
    *node = savedNode;

    memset(&dependencies, 0xa5, sizeof(dependencies));
    savedDependencies = dependencies;
    property.Name = "assigned-clocks";
    node->View.Properties = &property;
    node->View.PropertyCount = 1;
    CHECK(FdtPlatformDependencies(bus->Tree, node->NodeOffset, &dependencies) == OS_ENOTSUPPORTED);
    CHECK(!memcmp(&dependencies, &savedDependencies, sizeof(dependencies)));
    *node = savedNode;
    CHECK(FdtFindResources(bus->Tree, 10, &provider) == OS_EOK);
    clock = (void*)FdtTreeNode(bus->Tree, provider.NodeOffset);
    clock->Disabled = 1;
    CHECK(FdtPlatformDependencies(bus->Tree, node->NodeOffset, &dependencies) == OS_ENOENT);
    CHECK(!memcmp(&dependencies, &savedDependencies, sizeof(dependencies)));
    clock->Disabled = 0;
    property.Name = "clocks";
    property.Length = sizeof(reference);
    parent->View.Properties = &property;
    parent->View.PropertyCount = 1;
    CHECK(FdtPlatformDependencies(bus->Tree, node->NodeOffset, &dependencies) == OS_EOK);
    CHECK(dependencies.Count == 2 && dependencies.Entries[0].Arguments[0] == 22);
    CHECK(dependencies.Entries[1].Immutable);
    *parent = savedParent;
}

static void
__NativeInterruptPendingClose(
    _In_ struct PlatformEntry* entry)
{
    const oserr_t rejections[] = { OS_EPERMISSIONS, OS_ENOTSUPPORTED };
    struct DeviceInterruptSourceRequest request;
    enum DeviceActivation activation = DeviceStop;
    unsigned int i;

    for (i = 0; i < sizeof(rejections) / sizeof(rejections[0]); i++) {
        request = (struct DeviceInterruptSourceRequest) {
            .Operation = DeviceInterruptSourceReserve, .Allocation = 601 + i, .Index = 1
        };
        // A group update can report busy before validating its owner or kind.
        // Keep the uncertain lease until a later Close can give a final answer.
        g_nativeAttachStatus = g_nativeCloseStatus = OS_EBUSY;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == OS_EUNKNOWN);
        CHECK(entry->Interrupts[1].Allocation == request.Allocation);
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
            &activation, sizeof(activation)) == OS_EBUSY);
        request.Operation = DeviceInterruptSourceRelease;
        request.Index = 0;
        g_nativeCloseStatus = rejections[i];
        CHECK(DmHandleIoctl(2, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == OS_EPERMISSIONS);
        CHECK(entry->Interrupts[1].Allocation == request.Allocation);
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == OS_EOK);
        CHECK(entry->Interrupts[1].Allocation == UUID_INVALID);

        // A confirmed attachment must retain ownership on the same errors.
        // Reusing the source also proves that the pending lease was removed.
        request.Operation = DeviceInterruptSourceReserve;
        request.Index = 1;
        g_nativeAttachStatus = g_nativeCloseStatus = OS_EOK;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == OS_EOK);
        request.Operation = DeviceInterruptSourceRelease;
        request.Index = 0;
        g_nativeCloseStatus = rejections[i];
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == rejections[i]);
        CHECK(entry->Interrupts[1].Allocation == request.Allocation);
        g_nativeCloseStatus = OS_EOK;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
            &request, sizeof(request)) == OS_EOK);
    }
}

static void __NativeRuntime(struct PlatformBus* bus, struct PlatformEntry* entry)
{
    struct DeviceInterruptSourceQuery support = { .Index = 1 };
    struct DeviceInterruptSourceRequest irq = { .Operation = DeviceInterruptSourceReserve,
        .Allocation = 501, .Index = 1 };
    struct DmDmaLease* lease = NULL;
    const struct DmDmaDescription* dma;
    enum DeviceActivation activation = DeviceStart;

    __NativeFirmwareFailures(bus, entry);
    CHECK(DmDevicePrepareDma(entry->DeviceId, &lease) == OS_EBUSY && lease == NULL);
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EBUSY);
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EBUSY);
    activation = DevicePrepare;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EOK);
    CHECK(DmHandleIoctl(2, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EPERMISSIONS);
    CHECK(DmDevicePrepareDmaForPrincipal(entry->DeviceId, 2, &lease) == OS_EPERMISSIONS && lease == NULL);
    CHECK(DmDevicePrepareDmaForPrincipal(entry->DeviceId, 1, &lease) == OS_EOK);
    dma = DmDmaLeaseGetDescription(lease);
    CHECK(dma->HostId == bus->DeviceId && dma->CachePolicy == DmDmaCacheNonCoherent);
    CHECK(dma->Map.Count == 1 && dma->Map.Ranges[0].PhysicalBase == 0);
    CHECK(dma->Map.Ranges[0].DeviceBase == 0x40000000 && dma->Map.Ranges[0].Length == 0x10000000);
    CHECK(PlatformBusDestroy(bus) == OS_EBUSY && !bus->Removing);
    activation = DeviceStop;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EBUSY);
    DmDmaLeaseRelease(&lease);
    CHECK(entry->DmaReferences == 0);
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(support.Index == 1 && support.Line.Line == 73);
    CHECK(support.Kind == InterruptSourceFixedLine);
    CHECK(support.ControllerID == 1);
    CHECK(!(support.Line.AcpiConform & INTERRUPT_ACPICONFORM_TRIGGERMODE));
    support = (struct DeviceInterruptSourceQuery) { .Index = 0 };
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(support.Line.Line == 72 && (support.Line.AcpiConform & INTERRUPT_ACPICONFORM_TRIGGERMODE));
    __NativeInterruptPendingClose(entry);
    g_nativeAttachStatus = OS_EUNKNOWN;
    g_nativeCloseStatus = OS_EBUSY;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
        &irq, sizeof(irq)) == OS_EUNKNOWN);
    activation = DeviceStop;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EBUSY);
    g_nativeAttachStatus = g_nativeCloseStatus = OS_EOK;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
        &irq, sizeof(irq)) == OS_EOK);
    CHECK(DmHandleIoctl(2, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
        &irq, sizeof(irq)) == OS_EPERMISSIONS);
    irq.Operation = DeviceInterruptSourceRelease;
    irq.Index = 0;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_INTERRUPT_CONTROL,
        &irq, sizeof(irq)) == OS_EOK);
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EOK);
}

static void __NativeDependencies(struct PlatformBus* bus, struct PlatformEntry* entry, uint32_t provider)
{
    enum DeviceActivation activation = DevicePrepare;
    unsigned int failure;

    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_ENOTSUPPORTED);
    CHECK(PlatformDependencyRegister(bus, provider, &g_testDependencyOperations, NULL) == OS_EOK);
    CHECK(PlatformDependencyRegister(bus, provider, &g_testDependencyOperations, NULL) == OS_EEXISTS);
    for (failure = 1; failure <= 6; failure++) {
        g_dependencyAcquires = g_dependencyReleases = 0;
        g_dependencyFailAcquire = failure;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
            &activation, sizeof(activation)) == OS_EUNKNOWN);
        CHECK(entry->Activation == NULL && g_dependencyReleases == failure - 1);
    }
    g_dependencyAcquires = g_dependencyReleases = g_dependencyFailAcquire = 0;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EOK);
    CHECK(g_dependencyAcquires == 6);
    CHECK(PlatformDependencyUnregister(bus, provider) == OS_EBUSY);
    activation = DeviceStart;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EOK);
    activation = DeviceStop;
    g_dependencyFailRelease = 1;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EBUSY);
    CHECK(PlatformDependencyUnregister(bus, provider) == OS_EBUSY);
    g_dependencyFailRelease = 0;
    CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
        &activation, sizeof(activation)) == OS_EOK);
    CHECK(g_dependencyReleases == 6);
    CHECK(PlatformDependencyUnregister(bus, provider) == OS_EOK);
}
