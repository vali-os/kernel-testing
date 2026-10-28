/** Failure injection at startup and host cleanup ownership boundaries. */
static void
__HostCleanupRecovery(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 61, .BusEnd = 0 },
        .Base = 0xb1000000,
        .IoResourcePolicy = PciIoResourceMemory,
        .Firmware = &g_host
    };
    struct FdtTree* tree = g_host.Tree;
    PciHost_t* host;
    PciHost_t* rejected;
    unsigned int references = atomic_load(&tree->References);
    unsigned int releases = g_releases;
    unsigned int destroys = g_destroys;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostAttach(host, tree) == OS_EOK);
    g_failIoReleaseBase = configuration.Base;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(host->Removing && host->IoAcquired && g_pciRoots.count == 1);
    CHECK(atomic_load(&tree->References) == references + 1);
    CHECK(g_releases == releases && g_destroys == destroys);
    CHECK(PciPublishDevice(host->RootDevice).Status == OS_EBUSY);
    CHECK(PciEcamHostCreate(&configuration, &rejected) == OS_EOK);
    CHECK(PciHostRegister(rejected) == OS_EEXISTS);

    g_failIoReleaseBase = 0;
    g_failIoDestroyBase = configuration.Base;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(!host->IoAcquired && g_releases == releases + 1);
    CHECK(PciHostDestroy(host) == OS_EBUSY && g_releases == releases + 1);
    CHECK(atomic_load(&tree->References) == references + 1);
    g_failIoDestroyBase = 0;
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_releases == releases + 1 && g_destroys == destroys + 1);
    CHECK(atomic_load(&tree->References) == references && g_pciRoots.count == 0);
    CHECK(PciHostDestroy(rejected) == OS_EOK);

    // Failed acquisition can leave a registration that the caller must destroy.
    g_failIoAcquisition = 1;
    g_failIoDestroyBase = configuration.Base;
    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOOM && host != NULL);
    CHECK(!host->IoAcquired && PciHostDestroy(host) == OS_EBUSY);
    g_failIoDestroyBase = 0;
    CHECK(PciHostDestroy(host) == OS_EOK);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    g_failPortDestroy = 1;
    CHECK(PciLegacyHostCreate(&host) == OS_EOOM && host != NULL);
    CHECK(!host->IoAcquired && PciHostDestroy(host) == OS_EBUSY);
    g_failPortDestroy = 0;
    CHECK(PciHostDestroy(host) == OS_EOK);
#endif
    g_failIoAcquisition = 0;
    puts("Host cleanup: release/destroy retries, firmware lifetime and failed constructor ownership passed");
}

static void
__DiscoveryCleanupRecovery(void)
{
    PciHost_t* sibling;
    PciHost_t* recovered;
    uuid_t siblingId;
    unsigned int resources;
    unsigned int queries;

    g_defaultConfigValue = UINT32_MAX;
    g_firmwarePrimary = OSFIRMWARE_ACPI;
    g_mcfgLength = sizeof(g_mcfg);
    g_mcfg.Entries[0] = (ACPI_MCFG_ALLOCATION) {
        .Address = 0xb2000000, .PciSegment = 62, .EndBusNumber = 0
    };
    g_mcfg.Entries[1] = (ACPI_MCFG_ALLOCATION) {
        .Address = 0xb3000000, .PciSegment = 63, .EndBusNumber = 0
    };
    g_hostsDiscovered = 0;
    g_policyInitialized = 0;
    g_acpiQueryFailure = OS_EOOM;
    CHECK(BusRecoverAndRepublish().Status == OS_EOOM && !g_policyInitialized);
    CHECK(g_pciRoots.count == 0);
    g_acpiQueryFailure = OS_EOK;
    g_firmwareQueryFailure = OS_EOOM;
    CHECK(BusRecoverAndRepublish().Status == OS_EOOM && g_pciRoots.count == 0);
    g_firmwareQueryFailure = OS_EOK;

    // Registration fails after construction, then release fails during rollback.
    g_failPciAllocation = 1;
    g_failIoReleaseBase = 0xb2000000;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(g_hostCleanup != NULL && g_pciRoots.count == 1);
    sibling = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    siblingId = sibling->RootDevice->DeviceId;
    CHECK(siblingId != UUID_INVALID && sibling->Identification.Segment == 63);
    resources = g_resources;
    queries = g_firmwareQueries;
    // Live policy is stable even if a later ACPI query would fail.
    g_acpiQueryFailure = OS_EOOM;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(g_resources == resources && g_firmwareQueries == queries);
    CHECK(sibling->RootDevice->DeviceId == siblingId);
    g_failIoReleaseBase = 0;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(g_hostCleanup == NULL && g_pciRoots.count == 2);
    g_acpiQueryFailure = OS_EOK;
    CHECK(sibling->RootDevice->DeviceId == siblingId);
    recovered = ((PciDevice_t*)g_pciRoots.tail->value)->Host;
    CHECK(recovered->Identification.Segment == 62);
    resources = g_resources;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK && g_resources == resources);
    CHECK(PciHostDestroy(recovered) == OS_EOK);
    CHECK(PciHostDestroy(sibling) == OS_EOK);
    g_mcfgLength = 0;
    g_defaultConfigValue = 0x1234;
    puts("Discovery recovery: firmware query, registration rollback, sibling isolation and idempotence passed");
}

static void
__ScanAllocationRecovery(void)
{
    static const struct PciHostOperations operations = {
        .Read = __BridgeRouteRead,
        .Write = __BridgeRouteWrite
    };
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 64, .BusEnd = 2 },
        .Base = 0xb4000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    struct __BridgeRoutes routes = {
        .Present = { { 1, 1 }, { 1, 0 }, { 0, 0 } },
        .Secondary = { { 1, 2 }, { 2, 0 }, { 0, 0 } }
    };
    PciHost_t* host;
    PciDevice_t* saved;
    unsigned int reads;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    host->Operations = &operations;
    host->OpContext = &routes;
    CHECK(PciHostRegister(host) == OS_EOK);
    g_failPciAllocation = 1;
    PciCheckBus(host->RootDevice, 0);
    CHECK(host->ScanStatus == OS_EOOM && g_pciDevices.count == 1);
    CHECK(PciPublishDevice(host->RootDevice).Status == OS_EOOM);
    CHECK(host->Publication.Group.State == DmPublicationCollecting);
    saved = host->RootDevice->Children.head->value;
    CHECK(saved->Slot == 1);
    CHECK(PciRecoverScan(host) == OS_EOK && g_pciDevices.count == 3);
    CHECK(PciFindDevice(64, 0, 1, 0) == saved);
    reads = routes.Reads;
    CHECK(PciRecoverScan(host) == OS_EOK && routes.Reads == reads);
    CHECK(PciHostDestroy(host) == OS_EOK && g_pciDevices.count == 0);
    puts("Scan recovery: allocation failure fills missing functions without replacing existing inventory passed");
}

static void
__BcmCleanupRecovery(
    _In_ const struct FdtPciHost* firmware)
{
    PciHost_t* host;
    struct BcmPciHost* controller;
    unsigned int releases;
    unsigned int writes;
    uint64_t savedBridge = g_bridgeResetBase;
    uint64_t savedRescal = g_rescalBase;

    __Bcm2712Model();
    CHECK(BcmPciHostCreate(firmware, &host) == OS_EOK);
    controller = host->OpContext;
    g_failIoReleaseBase = g_bridgeResetBase;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(controller->BridgeReset != NULL && !controller->BridgeResetMapped);
    CHECK(g_bridgeResetIo != NULL && g_bcmIo == &host->IoSpace);
    writes = g_bcmWrites + g_bridgeWrites;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(g_bcmWrites + g_bridgeWrites == writes);
    g_failIoReleaseBase = 0;
    g_failIoDestroyBase = g_bridgeResetBase;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    releases = g_releases;
    CHECK(PciHostDestroy(host) == OS_EBUSY && g_releases == releases);
    CHECK(g_bcmWrites + g_bridgeWrites == writes);
    g_failIoDestroyBase = 0;
    CHECK(PciHostDestroy(host) == OS_EOK && g_bridgeResetIo == NULL);
    CHECK(g_bcm2712ResetProviders.count == 0);

    // A failed shutdown write must keep both register blocks for retry.
    __Bcm2712Model();
    CHECK(BcmPciHostCreate(firmware, &host) == OS_EOK);
    controller = host->OpContext;
    g_failedWrite = BCM2712_PCIE_CONTROL;
    writes = g_bridgeWrites;
    CHECK(PciHostDestroy(host) == OS_EUNKNOWN);
    CHECK(controller->BridgeResetMapped && !controller->Stopped);
    CHECK(g_bridgeWrites == writes + 1 && g_bridgeResetIo != NULL);
    CHECK(g_bcmIo == &host->IoSpace);
    g_failedWrite = SIZE_MAX;
    CHECK(PciHostDestroy(host) == OS_EOK && g_bridgeResetIo == NULL);

    // An uncertain calibration clear keeps its mapping, but still stops the host.
    __Bcm2712Model();
    g_failRescalClear = 1;
    CHECK(BcmPciHostCreate(firmware, &host) == OS_EUNKNOWN && host != NULL);
    controller = host->OpContext;
    CHECK(controller->RescalPendingClear && controller->RescalAcquired);
    CHECK(controller->Stopped && g_bridgeResetIo == NULL);
    writes = g_bcmWrites + g_bridgeWrites;
    CHECK(PciHostDestroy(host) == OS_EUNKNOWN);
    CHECK(g_bcmWrites + g_bridgeWrites == writes && g_resetIo != NULL);
    g_failRescalClear = 0;
    CHECK(PciHostDestroy(host) == OS_EOK && g_resetIo == NULL);

    // Calibration mapping cleanup fails during setup. Stop still runs, and the
    // failed constructor returns the owner needed to finish that cleanup.
    __Bcm2712Model();
    g_failIoDestroyBase = g_rescalBase;
    CHECK(BcmPciHostCreate(firmware, &host) == OS_EUNKNOWN && host != NULL);
    controller = host->OpContext;
    CHECK(controller != NULL && controller->Stopped && !controller->Ready);
    CHECK(controller->RescalIo.Type != DeviceIoInvalid && !controller->RescalAcquired);
    CHECK(g_bridgeResetIo == NULL && g_resetIo != NULL);
    writes = g_bcmWrites + g_bridgeWrites;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(g_bcmWrites + g_bridgeWrites == writes);
    g_failIoDestroyBase = 0;
    CHECK(PciHostDestroy(host) == OS_EOK && g_resetIo == NULL);

    // A reset registration survives failed acquisition and failed rollback.
    __Bcm2712Model();
    g_mappingFailureBase = g_bridgeResetBase;
    g_failIoDestroyBase = g_bridgeResetBase;
    CHECK(BcmPciHostCreate(firmware, &host) == OS_EUNKNOWN && host != NULL);
    CHECK(host->OpContext != NULL && g_bridgeResetIo != NULL);
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    g_mappingFailureBase = 0;
    g_failIoDestroyBase = 0;
    CHECK(PciHostDestroy(host) == OS_EOK && g_bridgeResetIo == NULL);
    CHECK(g_bcm2712ResetProviders.count == 0);
    g_model2712 = 0;
    g_bridgeResetBase = savedBridge;
    g_rescalBase = savedRescal;
    puts("Broadcom cleanup: reset and calibration mappings survive failed setup and teardown passed");
}
