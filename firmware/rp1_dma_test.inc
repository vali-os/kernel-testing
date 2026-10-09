// Included after the RP1 property helpers; uses the real Pi 5 tree and PCI fixtures.
static void
__Rp1DmaFailure(
    _In_ const struct FdtPciHost* host,
    _In_ uint32_t child,
    _In_ oserr_t expected)
{
    struct FdtDmaMap result;
    struct FdtDmaMap saved;

    memset(&result, 0xa5, sizeof(result));
    memcpy(&saved, &result, sizeof(saved));
    CHECK(FdtResolveRp1Dma(host, child, &result) == expected);
    CHECK(!memcmp(&result, &saved, sizeof(result)));
}

static uint32_t
__Rp1DmaNameOffset(
    _In_ const struct FdtPciHost* host,
    _In_ const char* name)
{
    const uint8_t* bytes = host->Blob;
    const char* strings = (const char*)bytes + FdtReadBe32(bytes + 12);
    uint32_t size = FdtReadBe32(bytes + 32);
    uint32_t offset = 0;

    while (offset < size) {
        if (!strcmp(strings + offset, name)) {
            return offset;
        }
        offset += strlen(strings + offset) + 1;
    }
    CHECK(0);
    return 0;
}

static void
__Rp1DmaAcceptance(
    _In_ const struct FdtPciHost* host,
    _In_ struct Rp1Bus* bus)
{
    struct Rp1Child* usb0 = __Rp1Find(bus, "usb@200000");
    struct Rp1Child* usb1 = __Rp1Find(bus, "usb@300000");
    struct FdtPciHost changed = *host;
    struct FdtResources rp1;
    struct FdtDmaMap result;
    struct FdtDmaMap other;
    uint8_t saved[96];
    uint8_t* property;
    uint32_t nameOffset;
    uint32_t interrupt;
    uint32_t i;
    unsigned int writes = g_bcmWrites;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;

    CHECK(usb0 != NULL && usb1 != NULL);
    CHECK(FdtResolveRp1Dma(host, usb0->Firmware.NodeOffset, &result) == OS_EOK);
    CHECK(FdtResolveRp1Dma(host, usb1->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(result.Count == 2 && other.Count == 2);
    for (i = 0; i < 2; i++) {
        CHECK(result.Ranges[i].DeviceBase == (i ? 0x1000000000ULL : 0));
        CHECK(result.Ranges[i].PhysicalBase == 0 && result.Ranges[i].Length == 0x1000000000ULL);
        CHECK(other.Ranges[i].DeviceBase == result.Ranges[i].DeviceBase);
        CHECK(other.Ranges[i].PhysicalBase == result.Ranges[i].PhysicalBase);
        CHECK(other.Ranges[i].Length == result.Ranges[i].Length);
    }
    CHECK(result.Ranges[0].DeviceBase + 0x200000 == 0x200000);
    CHECK(result.Ranges[1].DeviceBase + 0x200000 == 0x1000200000ULL);
    CHECK(FdtContainsRange(result.Ranges[0].PhysicalBase, result.Ranges[0].Length,
        0xfffffffffULL, 1));
    CHECK(!FdtContainsRange(result.Ranges[0].PhysicalBase, result.Ranges[0].Length,
        0xfffffffffULL, 2));

    // Both operations use the same topology checks, but a late invalid
    // interrupt must prevent all enumeration callbacks without blocking DMA.
    g_rp1Children = 0;
    CHECK(FdtEnumerateRp1Children(host, bus->Bars, __Rp1Count, NULL) == OS_EOK);
    CHECK(g_rp1Children == bus->ChildCount);
    property = __Rp1Property(host, usb1->Firmware.NodeOffset, "interrupts");
    CHECK(property != NULL);
    interrupt = FdtReadBe32(property);
    __Word(property, FDT_RP1_INTERRUPT_COUNT);
    g_rp1Children = 0;
    CHECK(FdtEnumerateRp1Children(host, bus->Bars, __Rp1Count, NULL) == OS_ENOTSUPPORTED);
    CHECK(g_rp1Children == 0);
    CHECK(FdtResolveRp1Dma(host, usb1->Firmware.NodeOffset, &other) == OS_EOK);
    __Word(property, interrupt);

    changed.Type = FdtPciHostEcam;
    CHECK(FdtEnumerateRp1Children(&changed, bus->Bars, __Rp1Count, NULL) == OS_ENOTSUPPORTED);
    CHECK(g_rp1Children == 0);
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    changed = *host;

    // A second description has a different physical target. Neither resolution
    // may retain process-global state or change the first host's result.
    changed.DmaWindows[1].PhysicalBase = 0x200000000ULL;
    CHECK(FdtResolveRp1Dma(&changed, usb0->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(other.Ranges[0].PhysicalBase == 0x200000000ULL);
    CHECK(FdtResolveRp1Dma(host, usb0->Firmware.NodeOffset, &result) == OS_EOK);
    CHECK(result.Ranges[0].PhysicalBase == 0);
    changed = *host;
    changed.NodeOffset++;
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_ENOENT);
    __Rp1DmaFailure(host, UINT32_MAX, OS_ENOENT);
    changed = *host;
    changed.BlobLength--;
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_EINVALPARAMS);
    changed = *host;
    changed.DmaWindowCount = 17;
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    changed = *host;
    changed.DmaWindows[1].Kind = FdtDmaWindowUnknown;
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_ENOENT);
    changed = *host;
    changed.DmaWindows[2].BusBase = changed.DmaWindows[1].BusBase;
    __Rp1DmaFailure(&changed, usb0->Firmware.NodeOffset, OS_EINVALPARAMS);

    CHECK(FdtFindResources(host->Blob, host->BlobLength, usb0->Firmware.Controller, &rp1) == OS_EOK);
    CHECK(rp1.DmaRangesLength == 84);
    property = (uint8_t*)rp1.DmaRanges;
    memcpy(saved, property - 12, sizeof(saved));

    // Relocate both RAM aliases in PCI space; the third window's PCI memory
    // encoding is 32-bit even though its numeric target is above 4 GiB.
    changed = *host;
    changed.DmaWindows[1].BusBase = 0x1200000000ULL;
    __Word(property + 12, 0x12);
    __Word(property + 56 + 12, 0x12);
    CHECK(FdtResolveRp1Dma(&changed, usb0->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(other.Count == 2 && other.Ranges[0].DeviceBase == 0);
    CHECK(other.Ranges[1].DeviceBase == 0x1000000000ULL);
    memcpy(property - 12, saved, sizeof(saved));

    // Clip a RAM window to two pages. The peer aperture remains excluded even
    // though its RP1 child address has a valid translation through PCI.
    changed = *host;
    changed.DmaWindows[1].BusBase += 0x200000;
    changed.DmaWindows[1].PhysicalBase = 0x700000;
    changed.DmaWindows[1].Length = 0x2000;
    CHECK(FdtResolveRp1Dma(&changed, usb0->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(other.Count == 2 && other.Ranges[0].DeviceBase == 0x200000);
    CHECK(other.Ranges[0].PhysicalBase == 0x700000 && other.Ranges[0].Length == 0x2000);

    __Word(property, 0);
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_EINVALPARAMS);
    memcpy(property - 12, saved, sizeof(saved));
    __Word(property + 56 + 8, 0x01000000);
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    memcpy(property - 12, saved, sizeof(saved));
    // Map the low alias to the MIP doorbell; only the high RAM alias survives.
    __Word(property + 56 + 8, 0x03000000);
    __Word(property + 56 + 12, 0xff);
    __Word(property + 56 + 16, 0xfffff000);
    __Word(property + 56 + 20, 0);
    __Word(property + 56 + 24, 0x1000);
    CHECK(FdtResolveRp1Dma(host, usb0->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(other.Count == 1 && other.Ranges[0].DeviceBase == 0x1000000000ULL);
    memcpy(property - 12, saved, sizeof(saved));

    // Preserve tree structure while removing the property or making it empty.
    for (i = 0; i < sizeof(saved); i += 4) {
        __Word(property - 12 + i, FDT_NOP);
    }
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    g_rp1Children = 0;
    CHECK(FdtEnumerateRp1Children(host, bus->Bars, __Rp1Count, NULL) == OS_EOK);
    CHECK(g_rp1Children == bus->ChildCount);
    memcpy(property - 12, saved, 12);
    __Word(property - 8, 0);
    CHECK(FdtResolveRp1Dma(host, usb0->Firmware.NodeOffset, &other) == OS_EOK);
    CHECK(other.Count == 1 && other.Ranges[0].DeviceBase == 0x1000000000ULL);
    memcpy(property - 12, saved, sizeof(saved));

    property = __Rp1Property(host, usb0->Firmware.NodeOffset, "status");
    CHECK(property != NULL);
    memcpy(property, "fail", 5);
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOENT);
    g_rp1Children = 0;
    CHECK(FdtEnumerateRp1Children(host, bus->Bars, __Rp1Count, NULL) == OS_EOK);
    CHECK(g_rp1Children == bus->ChildCount - 1);
    memcpy(property, "okay", 5);
    property = __Rp1Property(host, usb0->Firmware.NodeOffset, "interrupts");
    CHECK(property != NULL);
    nameOffset = FdtReadBe32(property - 4);
    __Word(property - 4, __Rp1DmaNameOffset(host, "iommus"));
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    __Word(property - 4, __Rp1DmaNameOffset(host, "dma-ranges"));
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    __Word(property - 4, nameOffset);

    property = __Rp1Property(host, host->NodeOffset, "brcm,vdm-qos-map");
    CHECK(property != NULL);
    nameOffset = FdtReadBe32(property - 4);
    __Word(property - 4, __Rp1DmaNameOffset(host, "iommus"));
    __Rp1DmaFailure(host, usb0->Firmware.NodeOffset, OS_ENOTSUPPORTED);
    __Word(property - 4, nameOffset);
    CHECK(FdtResolveRp1Dma(host, usb0->Firmware.NodeOffset, &result) == OS_EOK);
    CHECK(g_bcmWrites == writes && g_publicationCount == publications && g_bindingCount == bindings);
    puts("RP1 shared traversal: host identity, disabled children, validation before callbacks and independent resource checks passed");
    puts("RP1 DMA: both USB paths, aliases, relocation, exclusion, unsupported paths and unchanged failure passed");
}

static void
__Rp1DmaTopology(void)
{
    struct Fixture fixture;
    struct FdtPciHost hosts[2];
    struct FdtDmaMap result;
    uint32_t children[2];
    uint32_t two = 2;
    uint32_t three = 3;
    uint32_t dma[] = { 0, 0, 0x43000000, 0x10, 0, 0, 0x10000 };
    uint32_t reg[] = { 0, 0, 0, 0x1000 };
    uint32_t structureLength;
    uint32_t totalLength;
    uint32_t hostIndex;
    uint32_t busIndex;
    uint32_t duplicate;

    for (duplicate = 0; duplicate <= 1; duplicate++) {
        memset(&fixture, 0, sizeof(fixture));
        memset(hosts, 0, sizeof(hosts));
        fixture.Used = 56;
        __Node(&fixture, "");
        __Cells(&fixture, "#address-cells", &two, 1);
        __Cells(&fixture, "#size-cells", &two, 1);
        for (hostIndex = 0; hostIndex < 2; hostIndex++) {
            hosts[hostIndex].NodeOffset = fixture.Used - 56;
            hosts[hostIndex].Type = FdtPciHostEcam;
            hosts[hostIndex].DmaWindowCount = 1;
            hosts[hostIndex].DmaWindows[0] = (struct FdtPciWindow) {
                .Space = 3, .Attributes = 0x43000000, .Kind = FdtDmaWindowRam,
                .BusBase = 0x1000000000ULL, .PhysicalBase = hostIndex * 0x100000, .Length = 0x10000
            };
            __Node(&fixture, hostIndex ? "pci1" : "pci0");
            __Property(&fixture, "compatible", "pci-host-ecam-generic", sizeof("pci-host-ecam-generic"));
            __Cells(&fixture, "#address-cells", &three, 1);
            __Cells(&fixture, "#size-cells", &two, 1);
            for (busIndex = 0; busIndex <= (hostIndex == 0 ? duplicate : 0); busIndex++) {
                __Node(&fixture, busIndex ? "rp1@1" : "rp1");
                __Property(&fixture, "compatible", "raspberrypi,rp1", sizeof("raspberrypi,rp1"));
                __Cells(&fixture, "#address-cells", &two, 1);
                __Cells(&fixture, "#size-cells", &two, 1);
                __Cells(&fixture, "dma-ranges", dma, 7);
                if (busIndex == 0) {
                    children[hostIndex] = fixture.Used - 56;
                }
                __Node(&fixture, "usb@0");
                __Property(&fixture, "compatible", "snps,dwc3", sizeof("snps,dwc3"));
                __Cells(&fixture, "reg", reg, 4);
                __Token(&fixture, FDT_END_NODE);
                __Token(&fixture, FDT_END_NODE);
            }
            __Token(&fixture, FDT_END_NODE);
        }
        __Token(&fixture, FDT_END_NODE);
        __Token(&fixture, FDT_END);
        structureLength = fixture.Used - 56;
        memcpy(fixture.Bytes + fixture.Used, fixture.Strings, fixture.StringsUsed);
        totalLength = fixture.Used + fixture.StringsUsed;
        __Word(fixture.Bytes, FDT_MAGIC);
        __Word(fixture.Bytes + 4, totalLength);
        __Word(fixture.Bytes + 8, 56);
        __Word(fixture.Bytes + 12, fixture.Used);
        __Word(fixture.Bytes + 16, 40);
        __Word(fixture.Bytes + 20, 17);
        __Word(fixture.Bytes + 24, 16);
        __Word(fixture.Bytes + 32, fixture.StringsUsed);
        __Word(fixture.Bytes + 36, structureLength);
        for (hostIndex = 0; hostIndex < 2; hostIndex++) {
            hosts[hostIndex].Blob = fixture.Bytes;
            hosts[hostIndex].BlobLength = totalLength;
        }
        CHECK(FdtResolveRp1Dma(&hosts[1], children[1], &result) == OS_EOK);
        CHECK(result.Count == 1 && result.Ranges[0].PhysicalBase == 0x100000);
        if (duplicate) {
            __Rp1DmaFailure(&hosts[0], children[0], OS_EINVALPARAMS);
        } else {
            CHECK(FdtResolveRp1Dma(&hosts[0], children[0], &result) == OS_EOK);
            CHECK(result.Count == 1 && result.Ranges[0].PhysicalBase == 0);
            __Rp1DmaFailure(&hosts[0], children[1], OS_ENOENT);
        }
    }
    puts("RP1 DMA topology: independent hosts, exact ancestry and duplicate buses passed");
}

static void
__Rp1DmaRealTree(
    _In_ const char* path)
{
    FILE* file;
    long length;
    void* bytes;
    struct Rp1Bus* bus;
    struct PciBar bars[6] = { 0 };

    file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    CHECK(bytes != NULL && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    g_count = 0;
    CHECK(FdtEnumeratePciHosts(bytes, (size_t)length, __Host, NULL) == OS_EOK);
    CHECK(g_count == 1 && g_host.Type == FdtPciHostBcm2712);
    bars[1] = (struct PciBar) {
        .CpuAddress = 0x1f00000000ULL, .Size = 0x400000, .Space = 2, .State = PciBarAssigned
    };
    bars[2] = (struct PciBar) {
        .BusAddress = 0x400000, .CpuAddress = 0x1f00400000ULL,
        .Size = 0x10000, .Space = 2, .State = PciBarAssigned
    };
    CHECK(Rp1BusCreate(&g_host, bars, &bus) == OS_EOK);
    __Rp1DmaAcceptance(&g_host, bus);
    __Rp1DmaTopology();
    __Rp1ConfiguredDma(&g_host);
    __Rp1HostLifetime(&g_host, bars);
    Rp1BusDestroy(bus);
    free(bytes);
}
