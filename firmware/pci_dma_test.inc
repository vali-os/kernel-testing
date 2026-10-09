// Uses the existing PCI register model so query tests exercise real host setup.
static void
__PciDmaFailure(PciHost_t* host, oserr_t expected)
{
    struct PciDmaDescription result;
    struct PciDmaDescription saved;

    // A failed query must not leave a usable-looking fragment in the output.
    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    CHECK(PciHostGetDmaDescription(host, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
}

static void
__Rp1ConfiguredFailure(const PciDevice_t* parent, uint32_t child, oserr_t expected)
{
    struct PciDmaDescription result;
    struct PciDmaDescription saved;

    // Child composition must preserve the same all-or-nothing output contract.
    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    CHECK(Rp1GetDmaDescription(parent, child, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
}

static void
__PciDmaHosts(void)
{
    struct FdtPciHost firmware = {
        .Type = FdtPciHostBcm2711, .BusEnd = 255, .WindowCount = 1,
        .Windows = {{ .Space = 2, .BusBase = 0xc0000000,
            .PhysicalBase = 0x600000000ULL, .Length = 0x40000000 }},
        .DmaWindowCount = 1,
        .DmaWindows = {{ .Kind = FdtDmaWindowRam, .Space = 2,
            .Attributes = 0x02000000, .Length = 0xc0000000 }}
    };
    PciHost_t* first = calloc(1, sizeof(*first));
    PciHost_t* second = calloc(1, sizeof(*second));
    struct BcmPciHost controllers[2];
    struct PciDmaDescription result;
    struct PciDmaDescription saved;
    uint64_t address;
    unsigned int writes;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;

    // Two registered hosts establish that copied identity and ranges are local
    // to one controller, including after a sibling fails or is destroyed.
    CHECK(first != NULL && second != NULL);
    PciInitialize();
    first->IoSpace.Type = DeviceIoMemoryBased;
    first->IoSpace.Access.Memory.Length = 0x9310;
    second->IoSpace = first->IoSpace;
    g_bcmIo = &first->IoSpace;
    g_bcmSecondIo = &second->IoSpace;
    memset(g_bcmRegisters, 0, sizeof(g_bcmRegisters));
    memset(g_bcmSecondRegisters, 0, sizeof(g_bcmSecondRegisters));
    g_bcmRegisters[0x4068 / 4] = 0xb0;
    g_bcmSecondRegisters[0x4068 / 4] = 0xb0;

    __PciDmaFailure(NULL, OS_EINVALPARAMS);
    CHECK(PciHostGetDmaDescription(first, NULL) == OS_EINVALPARAMS);
    __PciDmaFailure(first, OS_ENOTSUPPORTED);
    first->Operations = &g_pciDtEcamOperations;
    __PciDmaFailure(first, OS_ENOTSUPPORTED);
    first->Operations = &g_pciAcpiEcamOperations;
    __PciDmaFailure(first, OS_ENOTSUPPORTED);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    first->Operations = &g_pciLegacyOperations;
    __PciDmaFailure(first, OS_ENOTSUPPORTED);
#endif
    first->Operations = NULL;
    CHECK(BcmPciInitialize(first, &controllers[0], &firmware) == OS_EOK);
    CHECK(PciHostGetDmaDescription(first, &result) == OS_EOK);
    CHECK(result.Host.HostId == UUID_INVALID);
    CHECK(PciHostRegister(first) == OS_EOK);
    writes = g_bcmWrites;
    CHECK(PciHostGetDmaDescription(first, &result) == OS_EOK);
    CHECK(result.Host.HostId == first->Identification.HostId);
    CHECK(result.Host.HostId != UUID_INVALID && result.Host.BusEnd == 255);
    CHECK(result.CachePolicy == DmDmaCacheNonCoherent);
    CHECK(result.Map.Count == 1 && result.Map.Ranges[0].DeviceBase == 0);
    CHECK(result.Map.Ranges[0].PhysicalBase == 0);

    // Setup rounded 3 GiB to 4 GiB; only the original three are described.
    CHECK(g_bcmRegisters[0x4034 / 4] == 17);
    CHECK(result.Map.Ranges[0].Length == 0xc0000000);
    CHECK(BcmPciDmaAddress(&controllers[0], 0xc0000000, 1, &address) == OS_ENOENT);
    CHECK(g_bcmWrites == writes);
    saved = result;
    firmware.DmaWindows[0].Length = 0x1000;
    CHECK(PciHostGetDmaDescription(first, &result) == OS_EOK);
    CHECK(result.Map.Ranges[0].Length == saved.Map.Ranges[0].Length);
    firmware.DmaWindows[0].Length = 0xc0000000;

    // Ready only covers configuration access. It must not bypass the separate
    // record of successful inbound-window programming.
    controllers[0].InboundConfigured = 0;
    __PciDmaFailure(first, OS_EBUSY);
    CHECK(BcmPciDmaAddress(&controllers[0], 0x1000, 4, &address) == OS_EINVALPARAMS);
    controllers[0].InboundConfigured = 1;

    firmware.Segment = 1;
    firmware.DmaWindows[0].BusBase = 0x100000000ULL;
    g_failedWrite = 0x4034;
    CHECK(BcmPciInitialize(second, &controllers[1], &firmware) != OS_EOK);
    CHECK(!controllers[1].InboundConfigured);
    __PciDmaFailure(second, OS_ENOTSUPPORTED);
    // Also fail after inbound programming, during bridge setup.
    g_failedWrite = 0x18;
    CHECK(BcmPciInitialize(second, &controllers[1], &firmware) != OS_EOK);
    CHECK(!controllers[1].InboundConfigured);
    __PciDmaFailure(second, OS_ENOTSUPPORTED);
    g_failedWrite = SIZE_MAX;
    CHECK(BcmPciInitialize(second, &controllers[1], &firmware) == OS_EOK);
    CHECK(PciHostRegister(second) == OS_EOK);
    CHECK(PciHostGetDmaDescription(second, &result) == OS_EOK);
    CHECK(result.Host.HostId != saved.Host.HostId && result.Host.Segment == 1);
    CHECK(result.Map.Ranges[0].DeviceBase == 0x100000000ULL);
    CHECK(saved.Map.Ranges[0].DeviceBase == 0);

    // Teardown withdraws the query before removing mappings. A saved copy is
    // still plain data; it cannot keep a stopped controller usable.
    BcmPciDestroy(first, &controllers[0]);
    CHECK(!controllers[0].InboundConfigured);
    __PciDmaFailure(first, OS_ENOTSUPPORTED);
    CHECK(PciHostDestroy(first) == OS_EOK);
    CHECK(PciHostGetDmaDescription(second, &result) == OS_EOK);
    CHECK(result.Map.Ranges[0].DeviceBase == 0x100000000ULL);
    CHECK(PciHostDestroy(second) == OS_EOK);
    CHECK(!controllers[1].InboundConfigured);
    CHECK(g_publicationCount == publications && g_bindingCount == bindings);
    puts("PCI DMA: configured ranges, exact lengths, identity, independent hosts, setup failure and teardown passed");
}

static void
__Rp1ConfiguredDma(const struct FdtPciHost* description)
{
    struct FdtPciHost configured = *description;
    struct FdtPciDependencies dependencies;
    struct BcmPciHost controller;
    PciHost_t* host = calloc(1, sizeof(*host));
    PciNativeHeader_t header = {
        .VendorId = RP1_VENDOR_ID, .DeviceId = RP1_DEVICE_ID, .Revision = RP1_REVISION_C0
    };
    PciDevice_t parent = { .Host = host, .Header = &header, .Bus = 1 };
    struct PciFunctionResources resources = { .Firmware = description };
    struct Rp1Bus* inventory;
    struct Rp1Child* usb0;
    struct Rp1Child* usb1;
    struct PciDmaDescription result;
    struct FdtDmaMap described;
    struct DmDmaDescription prepared = { 0 };
    unsigned int writes;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;

    // Program a smaller, relocated RAM destination while keeping the original
    // firmware inventory. The child query must follow this effective host map.
    CHECK(host != NULL);
    CHECK(FdtResolvePciDependencies(description, &dependencies) == OS_EOK);
    g_bridgeResetBase = dependencies.BridgeResetBase;
    g_rescalBase = dependencies.ResetBase;
    __Bcm2712Model();
    g_bcmIo = &host->IoSpace;
    host->IoSpace.Type = DeviceIoMemoryBased;
    host->IoSpace.Access.Memory.Length = 0x9310;
    configured.DmaWindows[1].Length = 0x2000;
    configured.DmaWindows[1].PhysicalBase = 0x700000;
    CHECK(BcmPciInitialize(host, &controller, &configured) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    CHECK(PciHostGetDmaDescription(host, &result) == OS_EOK);
    CHECK(result.Map.Count == 1 && result.Map.Ranges[0].DeviceBase == 0x1000000000ULL);
    CHECK(result.Map.Ranges[0].PhysicalBase == 0x700000 && result.Map.Ranges[0].Length == 0x2000);
    CHECK(host->DriversBlocked);

    resources.Bars[1] = (struct PciBar) {
        .CpuAddress = 0x1f00000000ULL, .Size = 0x400000, .Space = 2, .State = PciBarAssigned
    };
    resources.Bars[2] = (struct PciBar) {
        .BusAddress = 0x400000, .CpuAddress = 0x1f00400000ULL,
        .Size = 0x10000, .Space = 2, .State = PciBarAssigned
    };
    parent.Resources = resources;
    __Rp1ConfiguredFailure(&parent, 0, OS_ENOTSUPPORTED);
    parent.Handler = &g_rp1PciHandler;
    CHECK(parent.Handler->Attach(&parent, &resources, &parent.Attachment) == OS_EOK);
    inventory = parent.Attachment;
    usb0 = __Rp1Find(inventory, "usb@200000");
    usb1 = __Rp1Find(inventory, "usb@300000");
    CHECK(usb0 != NULL && usb1 != NULL);
    CHECK(FdtResolveRp1Dma(description, usb0->Firmware.NodeOffset, &described) == OS_EOK);
    CHECK(described.Ranges[0].PhysicalBase == 0 && described.Ranges[0].Length == 0x1000000000ULL);
    writes = g_bcmWrites;
    CHECK(Rp1GetDmaDescription(&parent, usb0->Firmware.NodeOffset, &result) == OS_EOK);
    CHECK(result.Host.HostId == host->Identification.HostId);
    CHECK(result.CachePolicy == DmDmaCacheNonCoherent && result.Map.Count == 2);
    CHECK(result.Map.Ranges[0].DeviceBase == 0 && result.Map.Ranges[1].DeviceBase == 0x1000000000ULL);
    CHECK(result.Map.Ranges[0].PhysicalBase == 0x700000 && result.Map.Ranges[0].Length == 0x2000);
    CHECK(result.Map.Ranges[1].PhysicalBase == 0x700000 && result.Map.Ranges[1].Length == 0x2000);
    CHECK(Rp1GetDmaDescription(&parent, usb1->Firmware.NodeOffset, &result) == OS_EOK);
    CHECK(result.Map.Count == 2 && result.Map.Ranges[0].Length == 0x2000);
    // The typed provider must return that same configured path, without writes.
    CHECK(g_rp1ChildProviderOperations.PrepareDma(usb0, &prepared) == OS_EOK);
    CHECK(prepared.HostId == result.Host.HostId && prepared.CachePolicy == result.CachePolicy);
    CHECK(memcmp(&prepared.Map, &result.Map, sizeof(result.Map)) == 0);

    __Rp1ConfiguredFailure(&parent, UINT32_MAX, OS_ENOENT);
    __Rp1ConfiguredFailure(NULL, 0, OS_EINVALPARAMS);
    CHECK(Rp1GetDmaDescription(&parent, usb0->Firmware.NodeOffset, NULL) == OS_EINVALPARAMS);
    CHECK(g_bcmWrites == writes && host->DriversBlocked && parent.Handler->BlockActivation);
    CHECK(g_publicationCount == publications && g_bindingCount == bindings);

    // Stop the parent before destroying the inventory to prove that copied
    // child descriptions cannot fall back to firmware once the host is gone.
    BcmPciDestroy(host, &controller);
    __Rp1ConfiguredFailure(&parent, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    CHECK(g_rp1ChildProviderOperations.PrepareDma(usb0, &prepared) == OS_ENOTSUPPORTED);
    parent.Handler->Destroy(parent.Attachment);
    CHECK(PciHostDestroy(host) == OS_EOK);
    g_model2712 = 0;
    g_bridgeResetBase = 0;
    g_rescalBase = 0;
    puts("RP1 configured DMA: effective host map, RAM-only filtering, both aliases, child ownership and stopped-host rejection passed");
}
