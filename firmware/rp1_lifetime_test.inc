// Uses a real RP1 attachment and PCI host tree, with the existing registry model.
static int g_deferRp1Destroy;

static oserr_t
__RetryRp1Destroy(
    _In_ void* attachment)
{
    // Model an attachment that needs another cleanup attempt. PCI must preserve
    // the enclosing objects even when no provider reference explains the delay.
    if (g_deferRp1Destroy) {
        return OS_EBUSY;
    }
    return g_rp1PciHandler.Destroy(attachment);
}

static void
__Rp1HostLifetime(
    _In_ const struct FdtPciHost* firmware,
    _In_ const struct PciBar* bars)
{
    PciHost_t* host = __RegistrationHost(0, 0, 1);
    PciDevice_t* endpoint = calloc(1, sizeof(*endpoint));
    struct PciFirmwareMapping* mapping = calloc(1, sizeof(*mapping));
    struct PciFunctionHandler retryHandler = g_rp1PciHandler;
    struct DmDeviceProvider retained;
    struct Rp1Bus* inventory;
    struct Rp1Child* usb;
    unsigned int failure;
    unsigned int unmaps = g_firmwareUnmaps;
    unsigned int publications = g_publicationCount;

    // The PCI host owns both the endpoint and its firmware mapping, just as in
    // discovery. This lets teardown assertions cover the complete ownership path.
    CHECK(endpoint != NULL && mapping != NULL);
    mapping->Blob = firmware->Blob;
    mapping->Length = firmware->BlobLength;
    mapping->References = 1;
    host->Firmware = firmware;
    CHECK(PciHostAttach(host, mapping) == OS_EOK);
    PciFirmwareRelease(mapping);
    endpoint->Host = host;
    endpoint->Parent = host->RootDevice;
    endpoint->Bus = 1;
    endpoint->Header = calloc(1, sizeof(*endpoint->Header));
    CHECK(endpoint->Header != NULL);
    endpoint->Header->VendorId = RP1_VENDOR_ID;
    endpoint->Header->DeviceId = RP1_DEVICE_ID;
    endpoint->Header->Revision = RP1_REVISION_C0;
    endpoint->Handler = &g_rp1PciHandler;
    endpoint->Resources.Firmware = firmware;
    memcpy(endpoint->Resources.Bars, bars, sizeof(endpoint->Resources.Bars));
    list_construct(&endpoint->children);
    ELEMENT_INIT(&endpoint->child_header, 0, endpoint);
    ELEMENT_INIT(&endpoint->list_header, 0, endpoint);
    list_append(&host->RootDevice->children, &endpoint->child_header);
    list_append(&g_pciDevices, &endpoint->list_header);
    CHECK(endpoint->Handler->Attach(endpoint, &endpoint->Resources, &endpoint->Attachment) == OS_EOK);
    inventory = endpoint->Attachment;
    usb = __Rp1Find(inventory, "usb@200000");
    CHECK(usb != NULL && usb->Bus == inventory && inventory->Parent == endpoint);

    // Fail every publication position. Registry rollback must release all
    // acquired references, while leaving the same inventory available for retry.
    for (failure = 1; failure <= inventory->ChildCount + 2; failure++) {
        g_failPublication = failure;
        CHECK(PciPublishDevice(host->RootDevice) == OS_EOOM);
        CHECK(g_publicationCount == publications && endpoint->ProviderReferences == 0);
        CHECK(atomic_load(&inventory->ProviderReferences) == 0);
    }
    g_failPublication = 0;
    CHECK(PciPublishDevice(host->RootDevice) == OS_EOK);
    CHECK(endpoint->ProviderReferences == inventory->ChildCount + 1);
    retained = g_providers[usb->DeviceId];
    CHECK(retained.Context == usb);
    CHECK(retained.Operations->Retain(retained.Context) == OS_EOK);

    // An owner beyond the registry keeps the host alive even after all IDs have
    // been removed. Firmware must not be unmapped while that context survives.
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(g_publicationCount == publications && usb->DeviceId == UUID_INVALID);
    CHECK(endpoint->ProviderReferences == 1 && atomic_load(&inventory->ProviderReferences) == 1);
    CHECK(host->RootDevice->children.count == 1 && endpoint->Attachment == inventory);
    CHECK(g_firmwareUnmaps == unmaps && mapping->References == 1);
    CHECK(Rp1BusDestroy(inventory) == OS_EBUSY);
    retained.Operations->Release(retained.Context);
    CHECK(endpoint->ProviderReferences == 0 && atomic_load(&inventory->ProviderReferences) == 0);

    // Exercise propagation from the attachment callback itself. Returning busy
    // must occur before PCI unlinks or frees this endpoint or releases the host.
    retryHandler.Destroy = __RetryRp1Destroy;
    endpoint->Handler = &retryHandler;
    g_deferRp1Destroy = 1;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(host->RootDevice->children.count == 1 && endpoint->Attachment == inventory);
    CHECK(g_firmwareUnmaps == unmaps);
    g_deferRp1Destroy = 0;
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_firmwareUnmaps == unmaps + 1 && g_pciRoots.count == 0);
    puts("RP1 host lifetime: publication rollback, retained child, firmware ownership and attachment cleanup retry passed");
}
