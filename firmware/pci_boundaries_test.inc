/** Regression checks for construction, publication ownership and scan policy. */
static void
__HostConstruction(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 42, .BusStart = 4, .BusEnd = 5 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t* host = NULL;
    PciHost_t* rejected;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;
    unsigned int releases = g_releases;
    unsigned int destroys = g_destroys;
    uuid_t identity;

    PciInitialize();
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    g_failIoCreation = 1;
    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOOM && host == NULL);
    g_failIoCreation = 0;
    CHECK(g_releases == releases && g_destroys == destroys);
    g_selectedIo = NULL;
    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(host->RootDevice == NULL && host->Identification.HostId == UUID_INVALID);
    CHECK(g_selectedIo == NULL && g_publicationCount == publications && g_bindingCount == bindings);
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(host->IoSpace.Access.Memory.Length == 2 * 1024 * 1024);
    CHECK(PciHostRegister(host) == OS_EOK);
    identity = host->Identification.HostId;
    PciInitialize();
    BusEnumerate();
    CHECK(g_pciRoots.count == 1 && host->Identification.HostId == identity);
    CHECK(PciEcamHostCreate(&configuration, &rejected) == OS_EOK);
    CHECK(PciHostRegister(rejected) == OS_EEXISTS);
    CHECK(PciHostDestroy(rejected) == OS_EOK);
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 2);

    g_mappingFailureBase = configuration.Base;
    CHECK(PciEcamHostCreate(&configuration, &host) != OS_EOK && host == NULL);
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 3);
    g_mappingFailureBase = 0;
    configuration.Identification.BusEnd = 3;
    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EINVALPARAMS && host == NULL);
    configuration.Identification.BusEnd = 5;
    configuration.Base = UINT64_MAX - 1024;
    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOVERFLOW && host == NULL);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    g_failIoCreation = 1;
    CHECK(PciLegacyHostCreate(&host) == OS_EOOM && host == NULL);
    g_failIoCreation = 0;
    CHECK(g_destroys == destroys + 3);
    g_failIoAcquisition = 1;
    CHECK(PciLegacyHostCreate(&host) == OS_EOOM && host == NULL);
    g_failIoAcquisition = 0;
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 4);
    g_selectedIo = NULL;
    CHECK(PciLegacyHostCreate(&host) == OS_EOK);
    CHECK(host->RootDevice == NULL && host->Identification.HostId == UUID_INVALID);
    CHECK(g_selectedIo == NULL && g_pciDevices.count == 0);
    CHECK(PciHostDestroy(host) == OS_EOK);
#endif
    CHECK(g_publicationCount == publications && g_bindingCount == bindings);
    puts("PCI construction: no scanning/publication, repeated initialization, overlap and mapping cleanup passed");
}

static PciDevice_t*
__PublicationFunction(
    _In_ PciDevice_t* parent,
    _In_ int bridge)
{
    PciDevice_t* device = calloc(1, sizeof(*device));

    CHECK(device != NULL);
    device->Header = calloc(1, sizeof(*device->Header));
    CHECK(device->Header != NULL);
    device->Host = parent->Host;
    device->Parent = parent;
    device->IsBridge = bridge;
    list_construct(&device->children);
    ELEMENT_INIT(&device->list_header, 0, device);
    ELEMENT_INIT(&device->child_header, 0, device);
    list_append(&parent->children, &device->child_header);
    list_append(&g_pciDevices, &device->list_header);
    return device;
}

static void
__PublicationLifetime(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 43, .BusEnd = 2 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t* host;
    PciDevice_t* bridge;
    PciDevice_t* endpoint;
    Device_t* descendant;
    uuid_t descendantId;
    uuid_t endpointId;
    unsigned int failure;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;
    unsigned int drivers = g_driverRegistrations;
    unsigned int releases;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    bridge = __PublicationFunction(host->RootDevice, 1);
    endpoint = __PublicationFunction(bridge, 0);
    endpoint->Resources.Bars[0] = (struct PciBar) {
        .State = PciBarAssigned, .Space = 2, .CpuAddress = 0xc0000000, .Size = 0x1000
    };
    for (failure = 1; failure <= 3; failure++) {
        g_failPublication = failure;
        CHECK(PciPublishDevice(host->RootDevice) == OS_EOOM);
        CHECK(g_publicationCount == publications && g_bindingCount == bindings);
        CHECK(host->RootDevice->DeviceId == UUID_INVALID);
        CHECK(bridge->DeviceId == UUID_INVALID && endpoint->DeviceId == UUID_INVALID);
    }
    g_failBinding = 1;
    CHECK(PciPublishDevice(host->RootDevice) == OS_EOOM);
    CHECK(g_publicationCount == publications + 3 && g_bindingCount == bindings);
    // Binding errors retain the staged tree; retry must not publish duplicates.
    CHECK(PciPublishDevice(host->RootDevice) == OS_EOK);
    endpointId = endpoint->DeviceId;
    CHECK(g_publicationCount == publications + 3 && g_bindingCount == bindings + 1);
    CHECK(g_published[bridge->DeviceId]->ParentId == host->RootDevice->DeviceId);
    CHECK(g_published[endpointId]->ParentId == bridge->DeviceId);
    CHECK(PciPublishDevice(host->RootDevice) == OS_EOK);
    CHECK(endpoint->DeviceId == endpointId && g_bindingCount == bindings + 1);

    // A peripheral-owned descendant prevents endpoint removal. The host must
    // retain its configuration mapping and IDs until the caller removes it.
    descendant = calloc(1, sizeof(*descendant));
    CHECK(descendant != NULL);
    descendant->Length = sizeof(*descendant);
    descendant->ParentId = endpointId;
    CHECK(DmDeviceCreate(descendant, 0, &descendantId) == OS_EOK);
    releases = g_releases;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(g_releases == releases && endpoint->DeviceId == endpointId);
    // A removal retry must not accidentally restart drivers on the surviving tree.
    CHECK(PciPublishDevice(host->RootDevice) == OS_EBUSY);
    CHECK(g_bindingCount == bindings + 1);
    CHECK(g_pciRoots.count == 1 && g_publicationCount == publications + 4);
    CHECK(DmDeviceDestroy(descendantId) == OS_EOK);
    CHECK(__PciProviderRetain(endpoint) == OS_EOK);
    CHECK(endpoint->ProviderReferences == 2);
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(endpoint->ProviderReferences == 1 && g_releases == releases);
    __PciProviderRelease(endpoint);
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(g_publicationCount == publications && g_releases == releases + 1);
    g_driverRegistrations = drivers;
    puts("PCI publication: topology IDs, rollback, idempotence and busy teardown retry passed");
}

#ifdef __OSCONFIG_HAS_LEGACY_PCI
static size_t
__LegacyRootRead(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t width)
{
    (void)host;
    (void)width;
    if (bus != 0 || slot != 0 || (function != 0 && function != 2)) {
        return (size_t)-1;
    }
    if (reg == 0) {
        return 0x12341234;
    }
    if (reg == 0x0e) {
        return 0x80;
    }
    if (reg == 0x0c) {
        return 0x00800000;
    }
    return 0;
}

static void
__LegacyRootWrite(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t value,
    _In_ size_t width)
{
    (void)host; (void)bus; (void)slot; (void)function;
    (void)reg; (void)value; (void)width;
}

static void
__LegacyRootScanning(void)
{
    static const struct PciHostOperations operations = {
        .Read = __LegacyRootRead, .Write = __LegacyRootWrite
    };
    PciHost_t* host;

    CHECK(PciLegacyHostCreate(&host) == OS_EOK);
    host->Operations = &operations;
    CHECK(PciHostRegister(host) == OS_EOK);
    PciScanLegacyRoots(host);
    CHECK(host->ScannedBuses[0] == ((1U << 0) | (1U << 2)));
    CHECK(g_pciDevices.count == 2);
    PciScanLegacyRoots(host);
    CHECK(g_pciDevices.count == 2);
    CHECK(PciHostDestroy(host) == OS_EOK);
    puts("PCI legacy roots: present functions, sparse roots and duplicate-scan guard passed");
}
#endif

/** @brief Small bridge layouts with deliberately broken secondary bus numbers. */
struct __BridgeRoutes {
    unsigned int Present[3][2];
    unsigned int Secondary[3][2];
    unsigned int Reads;
};

static size_t
__BridgeRouteRead(
    _In_ PciHost_t*  host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t       reg,
    _In_ size_t       width)
{
    struct __BridgeRoutes* routes = host->OpContext;

    (void)width;
    // A fixed upper bound makes an accidental scan loop fail promptly.
    CHECK(++routes->Reads < 1000);
    // This model contains only function zero and two slots on each of three buses.
    if (bus >= 3 || slot >= 2 || function != 0) {
        return (size_t)-1;
    }
    // Absent slots must look empty to the real PCI scanner.
    if (!routes->Present[bus][slot]) {
        return (size_t)-1;
    }
    switch (reg) {
        case 0:
            return 0x12341234;
        case 0x08:
            return 0x06040000;
        case 0x0c:
            return 0x00010000;
        case 0x18:
            return routes->Secondary[bus][slot] << 8;
        default:
            return 0;
    }
}

static void
__BridgeRouteWrite(
    _In_ PciHost_t*  host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t       reg,
    _In_ size_t       value,
    _In_ size_t       width)
{
    // These bridges expose no writable resources needed by the scan.
    (void)host; (void)bus; (void)slot; (void)function;
    (void)reg; (void)value; (void)width;
}

static void
__CheckBridgeRoutes(
    _InOut_ struct __BridgeRoutes* routes,
    _In_    unsigned int           expectedDevices,
    _In_    unsigned int           expectedBuses)
{
    static const struct PciHostOperations operations = {
        .Read = __BridgeRouteRead,
        .Write = __BridgeRouteWrite
    };
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 47, .BusEnd = 2 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t* host;
    unsigned int reads;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    host->Operations = &operations;
    host->OpContext = routes;
    CHECK(PciHostRegister(host) == OS_EOK);
    PciCheckBus(host->RootDevice, 0);
    CHECK(g_pciDevices.count == expectedDevices);
    CHECK(host->ScannedBuses[0] == expectedBuses);
    reads = routes->Reads;
    PciCheckBus(host->RootDevice, 0);
    CHECK(routes->Reads == reads && g_pciDevices.count == expectedDevices);
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_pciDevices.count == 0 && g_pciRoots.count == 0);
}

static void
__MalformedBridgeRoutes(void)
{
    struct __BridgeRoutes self = { .Present = {{1, 0}}, .Secondary = {{0, 0}} };
    struct __BridgeRoutes cycle = {
        .Present = {{1, 0}, {1, 0}, {1, 0}},
        .Secondary = {{1, 0}, {2, 0}, {1, 0}}
    };
    struct __BridgeRoutes duplicate = {
        .Present = {{1, 1}, {1, 0}},
        .Secondary = {{1, 1}, {1, 0}}
    };
    struct __BridgeRoutes outside = { .Present = {{1, 0}}, .Secondary = {{3, 0}} };

    __CheckBridgeRoutes(&self, 1, 1);
    __CheckBridgeRoutes(&cycle, 3, 7);
    __CheckBridgeRoutes(&duplicate, 3, 3);
    __CheckBridgeRoutes(&outside, 1, 1);
    puts("PCI scanning: self loops, bridge cycles, duplicate routes and out-of-range buses passed");
}
