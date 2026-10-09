// Included by pci_host_test.c to share its firmware and PCI register fixtures.
static unsigned int g_rp1Children;

static void
__Rp1Count(const struct FdtRp1Device* device, void* context)
{
    (void)device;
    (void)context;
    g_rp1Children++;
}

static struct Rp1Child*
__Rp1Find(struct Rp1Bus* bus, const char* name)
{
    struct Rp1Child* child;

    for (child = bus->Children; child != NULL; child = child->Next) {
        if (!strcmp(child->Firmware.Name, name)) {
            return child;
        }
    }
    return NULL;
}

static uint8_t*
__Rp1Property(const struct FdtPciHost* host, uint32_t node, const char* name)
{
    uint8_t* base = (uint8_t*)host->Blob;
    uint8_t* strings = base + FdtReadBe32(base + 12);
    uint8_t* cursor = base + FdtReadBe32(base + 8) + node + 4;

    cursor += FDT_ALIGN(strlen((char*)cursor) + 1);
    while (FdtReadBe32(cursor) == FDT_PROP) {
        if (!strcmp((char*)strings + FdtReadBe32(cursor + 8), name)) {
            return cursor + 12;
        }
        cursor += 12 + FDT_ALIGN(FdtReadBe32(cursor + 4));
    }
    return NULL;
}

#include "rp1_lifetime_test.inc"
#include "rp1_dma_test.inc"

static void
__Rp1Acceptance(const struct FdtPciHost* host)
{
    struct PciBar bars[6] = { 0 };
    struct FdtPciHost wrongHost = *host;
    struct Rp1Bus* bus = NULL;
    struct Rp1Bus* second = NULL;
    struct Rp1Child* usb0;
    struct Rp1Child* usb1;
    struct Rp1Child* gpio;
    struct Rp1Child* ethernet;
    struct FdtResources rp1;
    uint8_t* property;
    uint32_t saved;
    unsigned int count;
    PciHost_t pciHost = { 0 };
    PciNativeHeader_t header = {
        .VendorId = RP1_VENDOR_ID,
        .DeviceId = RP1_DEVICE_ID,
        .Revision = RP1_REVISION_C0
    };
    PciDevice_t endpoint = { .Host = &pciHost, .Header = &header, .Bus = 1 };
    struct PciFunctionResources resources = { .Firmware = host };
    const struct PciFunctionHandler* handler;
    void* attachment = NULL;
    struct Rp1Bus* published;
    struct Rp1Child* publishedUsb;
    PlatformDevice_t* description;
    unsigned int failure;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;

    bars[1] = (struct PciBar) { .BusAddress = 0, .CpuAddress = 0x1f00000000ULL, .Size = 0x400000, .Space = 2, .State = PciBarAssigned };
    bars[2] = (struct PciBar) { .BusAddress = 0x400000, .CpuAddress = 0x1f00400000ULL, .Size = 0x10000, .Space = 2, .State = PciBarAssigned };
    CHECK(Rp1BusCreate(host, bars, &bus) == OS_EOK);
    CHECK(bus != NULL && bus->ChildCount > 6);
    count = bus->ChildCount;
    handler = PciFunctionHandlerFind(&endpoint);
    CHECK(handler == &g_rp1PciHandler && handler->BlockActivation);
    header.VendorId++;
    CHECK(PciFunctionHandlerFind(&endpoint) == NULL);
    header.VendorId--;
    header.DeviceId++;
    CHECK(PciFunctionHandlerFind(&endpoint) == NULL);
    header.DeviceId--;
    endpoint.IsBridge = 1;
    CHECK(PciFunctionHandlerFind(&endpoint) == NULL);
    endpoint.IsBridge = 0;
    header.Revision++;
    CHECK(PciFunctionHandlerFind(&endpoint) == handler);
    CHECK(handler->Attach(&endpoint, &resources, &attachment) == OS_ENOTSUPPORTED);
    CHECK(attachment == NULL);
    header.Revision--;
    resources.Firmware = NULL;
    CHECK(handler->Attach(&endpoint, &resources, &attachment) == OS_ENOENT);
    CHECK(attachment == NULL);
    resources.Firmware = host;
    endpoint.Slot = 1;
    CHECK(handler->Attach(&endpoint, &resources, &attachment) == OS_ENOENT);
    CHECK(attachment == NULL);
    endpoint.Slot = 0;
    CHECK(handler->Attach(&endpoint, &resources, &attachment) != OS_EOK);
    CHECK(attachment == NULL);
    resources.Bars[1] = bars[1];
    resources.Bars[2] = bars[2];
    list_construct(&endpoint.children);
    endpoint.Handler = handler;
    endpoint.Resources = resources;
    CHECK(handler->Attach(&endpoint, &resources, &attachment) == OS_EOK);
    CHECK(attachment != NULL && g_publicationCount == publications);
    endpoint.Attachment = attachment;
    // Use a real registered parent so the normal PCI publication path owns
    // the endpoint and can roll back every partial RP1 child publication.
    PciDevice_t root = { .Host = &pciHost, .IsBridge = 1 };
    list_construct(&root.children);
    endpoint.Parent = &root;
    ELEMENT_INIT(&endpoint.child_header, 0, &endpoint);
    list_append(&root.children, &endpoint.child_header);
    for (failure = 1; failure <= count + 2; failure++) {
        g_failPublication = failure;
        CHECK(PciPublishDevice(&root) == OS_EOOM);
        CHECK(g_publicationCount == publications);
        CHECK(root.DeviceId == UUID_INVALID && endpoint.DeviceId == UUID_INVALID);
        CHECK(g_bindingCount == bindings);
    }
    g_failPublication = 0;
    g_expectedPublicationCount = publications + count + 2;
    g_failBinding = 2;
    CHECK(PciPublishDevice(&root) == OS_EOOM);
    CHECK(g_publicationCount == publications + count + 2);
    CHECK(g_bindingCount == bindings + 1);
    CHECK(PciPublishDevice(&root) == OS_EOK);
    CHECK(g_publicationCount == publications + count + 2);
    CHECK(g_bindingCount == bindings + count);
    published = attachment;
    publishedUsb = __Rp1Find(published, "usb@200000");
    CHECK(publishedUsb != NULL && publishedUsb->DeviceId != UUID_INVALID);
    description = (PlatformDevice_t*)g_published[publishedUsb->DeviceId];
    CHECK(description->Base.ParentId == endpoint.DeviceId);
    CHECK(g_published[endpoint.DeviceId]->ParentId == root.DeviceId);
    CHECK(description->Base.Length == sizeof(*description) && description->Pending == 3);
    CHECK(description->CompatibleLength == publishedUsb->Firmware.CompatibleLength);
    CHECK(!memcmp(description->Compatibles, publishedUsb->Firmware.Compatible, description->CompatibleLength));
    CHECK(description->Compatibles != publishedUsb->Firmware.Compatible);
    CHECK(description->Registers[0].Base == 0x1f00200000ULL);
    CHECK(description->Interrupts[0].Number == 31 && description->Interrupts[0].Type == 1);
    CHECK(PciPublishDevice(&root) == OS_EOK);
    CHECK(g_publicationCount == publications + count + 2 && g_bindingCount == bindings + count);
    g_expectedPublicationCount = 0;
    CHECK(PciUnpublishDevice(&root) == OS_EOK);
    handler->Destroy(attachment);
    CHECK(g_publicationCount == publications);
    usb0 = __Rp1Find(bus, "usb@200000");
    usb1 = __Rp1Find(bus, "usb@300000");
    gpio = __Rp1Find(bus, "gpio@d0000");
    ethernet = __Rp1Find(bus, "ethernet@100000");
    CHECK(usb0 && usb1 && gpio && ethernet);
    __Rp1DmaAcceptance(host, bus);
    __Rp1DmaTopology();
    CHECK(usb0->Firmware.Interrupts[0].Number == 31 && usb0->Firmware.Interrupts[0].Type == 1);
    CHECK(usb1->Firmware.Interrupts[0].Number == 36 && usb1->Firmware.Interrupts[0].Type == 1);
    CHECK(usb0->Firmware.Registers[0].Base == 0x1f00200000ULL);
    CHECK(usb1->Firmware.Registers[0].Base == 0x1f00300000ULL);
    CHECK(gpio->Firmware.RegisterCount == 3 && gpio->Firmware.InterruptCount == 3);
    CHECK(gpio->Firmware.Interrupts[0].Number == 0 && gpio->Firmware.Interrupts[2].Number == 2);
    CHECK(ethernet->Firmware.Interrupts[0].Number == 6 && ethernet->Firmware.Interrupts[0].Type == 4);
    CHECK(__Rp1Find(bus, "serial@34000") == NULL);
    CHECK(__Rp1Find(bus, "ethernet-phy@1") == NULL);
    CHECK(__Rp1Find(bus, "shmem@ff00") == NULL);
    CHECK(__Rp1Find(bus, "sram@400000") != NULL);
    CHECK(Rp1BusCreate(host, bars, &second) == OS_EOK);
    CHECK(second->Children != bus->Children);
    Rp1BusDestroy(second);
    second = NULL;

    g_rp1Children = 0;
    bars[1].Size--;
    CHECK(FdtEnumerateRp1Children(host, bars, __Rp1Count, NULL) != OS_EOK);
    CHECK(g_rp1Children == 0);
    bars[1].Size++;
    bars[2].Size = 0;
    CHECK(Rp1BusCreate(host, bars, &second) != OS_EOK && second == NULL);
    bars[2].Size = 0x10000;
    wrongHost.NodeOffset++;
    CHECK(FdtEnumerateRp1Children(&wrongHost, bars, __Rp1Count, NULL) == OS_ENOENT);
    CHECK(g_rp1Children == 0);

    property = __Rp1Property(host, usb1->Firmware.NodeOffset, "interrupts");
    CHECK(property != NULL);
    saved = FdtReadBe32(property);
    __Word(property, 61);
    CHECK(FdtEnumerateRp1Children(host, bars, __Rp1Count, NULL) == OS_ENOTSUPPORTED);
    CHECK(g_rp1Children == 0);
    __Word(property, saved);
    __Word(property + 4, 8);
    CHECK(FdtEnumerateRp1Children(host, bars, __Rp1Count, NULL) == OS_ENOTSUPPORTED);
    CHECK(g_rp1Children == 0);
    __Word(property + 4, 1);

    property = __Rp1Property(host, usb1->Firmware.NodeOffset, "status");
    CHECK(property != NULL);
    memcpy(property, "fail", 5);
    CHECK(Rp1BusCreate(host, bars, &second) == OS_EOK);
    CHECK(second->ChildCount == count - 1 && __Rp1Find(second, "usb@300000") == NULL);
    Rp1BusDestroy(second);
    second = NULL;
    memcpy(property, "okay", 5);

    property = __Rp1Property(host, gpio->Firmware.NodeOffset, "phandle");
    saved = FdtReadBe32(property);
    __Word(property, gpio->Firmware.Controller);
    CHECK(FdtEnumerateRp1Children(host, bars, __Rp1Count, NULL) == OS_EINVALPARAMS);
    CHECK(g_rp1Children == 0);
    __Word(property, saved);

    property = __Rp1Property(host, usb1->Firmware.NodeOffset, "reg");
    saved = FdtReadBe32(property + 12);
    __Word(property + 12, 0);
    CHECK(FdtEnumerateRp1Children(host, bars, __Rp1Count, NULL) != OS_EOK);
    CHECK(g_rp1Children == 0);
    __Word(property + 12, saved);

    wrongHost = *host;
    wrongHost.BlobLength--;
    CHECK(FdtEnumerateRp1Children(&wrongHost, bars, __Rp1Count, NULL) != OS_EOK);
    CHECK(g_rp1Children == 0);

    // Model firmware relocating its PCI target and the endpoint's assigned BARs.
    CHECK(FdtFindResources(host->Blob, host->BlobLength,
        gpio->Firmware.Controller, &rp1) == OS_EOK);
    __Word((uint8_t*)rp1.Ranges + 16, 0x800000);
    bars[1].CpuAddress += 0x800000;
    bars[2].CpuAddress += 0x800000;
    CHECK(Rp1BusCreate(host, bars, &second) == OS_EOK);
    CHECK(__Rp1Find(second, "usb@200000")->Firmware.Registers[0].Base == 0x1f00a00000ULL);
    Rp1BusDestroy(second);
    __Word((uint8_t*)rp1.Ranges + 16, 0);
    Rp1BusDestroy(bus);
    puts("RP1 firmware: resources, handler matching, attachment failures and publication rollback passed");
}

#ifndef __OSCONFIG_HAS_LEGACY_PCI
static void
__Rp1ScannerAcceptance(const struct FdtPciHost* host)
{
    struct FdtResources rp1;
    struct Rp1Bus* inventory;
    struct PciBar bars[6] = { 0 };
    struct PciFirmwareMapping* mapping;
    PciHost_t* bus;
    PciDevice_t* endpoint;
    unsigned int registrations = g_driverRegistrations;
    unsigned int unmaps = g_firmwareUnmaps;
    uint64_t bridgeResetBase = g_bridgeResetBase;
    uint64_t rescalBase = g_rescalBase;

    bars[1] = (struct PciBar) { .BusAddress = 0, .CpuAddress = 0x1f00000000ULL, .Size = 0x400000, .Space = 2, .State = PciBarAssigned };
    bars[2] = (struct PciBar) { .BusAddress = 0x400000, .CpuAddress = 0x1f00400000ULL, .Size = 0x10000, .Space = 2, .State = PciBarAssigned };
    CHECK(Rp1BusCreate(host, bars, &inventory) == OS_EOK);
    CHECK(FdtFindResources(host->Blob, host->BlobLength,
        inventory->Children->Firmware.Controller, &rp1) == OS_EOK);
    Rp1BusDestroy(inventory);
    __Word((uint8_t*)rp1.Ranges + 16, 0x800000);
    __Bcm2712Model();
    g_bcmRegisters[0x8008 / 4] = 0x02000002;
    memset(&g_bcmRegisters[0x8010 / 4], 0, 6 * 4);
    memset(g_barMasks, 0, sizeof(g_barMasks));
    g_bcmRegisters[0x8014 / 4] = 0x800000;
    g_bcmRegisters[0x8018 / 4] = 0xc00000;
    g_barMasks[1] = 0xffc00000;
    g_barMasks[2] = 0xffff0000;
    list_construct(&g_pciRoots);
    list_construct(&g_pciDevices);
    mapping = calloc(1, sizeof(*mapping));
    CHECK(mapping != NULL);
    mapping->Blob = host->Blob;
    mapping->Length = host->BlobLength;
    mapping->References = 1;
    __OnFdtPciHost(host, mapping);
    PciFirmwareRelease(mapping);
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 2);
    bus = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    endpoint = ((PciDevice_t*)bus->RootDevice->children.head->value)->children.head->value;
    CHECK(endpoint->Handler == &g_rp1PciHandler && endpoint->Attachment != NULL);
    CHECK(__Rp1Find(endpoint->Attachment, "usb@200000")->Firmware.Registers[0].Base == 0x1f00a00000ULL);
    CHECK(bus->DriversBlocked && mapping->References == 1);
    CHECK(PciPublishDevice(bus->RootDevice) == OS_EOK);
    CHECK(g_driverRegistrations == registrations);
    // RP1 must still stay blocked even if generic host policy later changes.
    bus->DriversBlocked = 0;
    CHECK(PciPublishDevice(bus->RootDevice) == OS_EOK);
    CHECK(g_driverRegistrations == registrations);
    CHECK(!(g_bcmRegisters[0x8004 / 4] & PCI_COMMAND_BUSMASTER));
    CHECK(g_bcmRegisters[0x8014 / 4] == 0x800000);
    CHECK(g_bcmRegisters[0x8018 / 4] == 0xc00000);
    PciHostDestroy(bus);
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(g_firmwareUnmaps == unmaps + 1);
    __Word((uint8_t*)rp1.Ranges + 16, 0);
    g_bcmIo = NULL;
    g_model2712 = 0;
    g_bridgeResetBase = bridgeResetBase;
    g_rescalBase = rescalBase;
    puts("RP1 PCI integration: assigned BAR probe, child inventory, activation gate and teardown passed");
}
#endif
