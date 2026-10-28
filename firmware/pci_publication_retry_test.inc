static int g_busyPublicationPreparation;

static oserr_t
__PreparePublicationRetry(
    _In_ void* attachment)
{
    CHECK(attachment != NULL);
    return g_busyPublicationPreparation ? OS_EBUSY : OS_EOK;
}

static const struct PciFunctionHandler g_retryHandler = {
    .PrepareRemoval = __PreparePublicationRetry
};

static void
__PublicationHardwareRetry(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 46, .BusEnd = 2 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t*       host;
    PciDevice_t*     endpoint;
    PciDevice_t*     sibling;
    struct Rp1Bus*   bus;
    struct Rp1Child* child;
    uuid_t          childId;
    uuid_t          siblingId;
    unsigned int    publications = g_publicationCount;
    unsigned int    bindings = g_bindingCount;
    unsigned int    drivers = g_driverRegistrations;
    unsigned int    preparations = g_rp1RuntimeStarts;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    endpoint = __PublicationFunction(host->RootDevice, 0);
    sibling = __PublicationFunction(host->RootDevice, 0);
    bus = calloc(1, sizeof(*bus));
    child = calloc(1, sizeof(*child));
    CHECK(bus != NULL && child != NULL);
    bus->Parent = endpoint;
    bus->Children = child;
    bus->ChildCount = 1;
    atomic_init(&bus->ProviderReferences, 0);
    child->Bus = bus;
    child->Firmware.Name = "peripheral";
    child->Firmware.Compatible = "test,peripheral";
    child->Firmware.CompatibleLength = sizeof("test,peripheral");
    endpoint->Handler = &g_rp1PciHandler;
    endpoint->Attachment = bus;

    // A failed RP1 runtime must leave both its children and unrelated PCI
    // descriptions visible, without enabling any drivers in the host group.
    g_failRp1Runtime = OS_EBUSY;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(host->Publication.Group.State == DmPublicationReady);
    CHECK(g_publicationCount == publications + 4 && g_bindingCount == bindings);
    CHECK(g_rp1RuntimeStarts == preparations + 1);
    CHECK(!bus->Ready && bus->Runtime == NULL);
    childId = child->DeviceId;
    siblingId = sibling->DeviceId;
    CHECK(childId != UUID_INVALID && siblingId != UUID_INVALID);
    CHECK(g_published[childId]->ParentId == endpoint->DeviceId);
    CHECK(g_published[siblingId]->ParentId == host->RootDevice->DeviceId);

    // Poison new registration to prove retries use the retained descriptions.
    g_failPublication = 1;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(child->DeviceId == childId && sibling->DeviceId == siblingId);
    CHECK(g_publicationCount == publications + 4 && g_bindingCount == bindings);
    CHECK(g_rp1RuntimeStarts == preparations + 2);
    g_failRp1Runtime = 0;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(bus->Ready && g_rp1RuntimeStarts == preparations + 3);
    CHECK(child->DeviceId == childId && sibling->DeviceId == siblingId);
    CHECK(g_publicationCount == publications + 4 && g_bindingCount == bindings + 2);
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(g_publicationCount == publications + 4 && g_bindingCount == bindings + 2);
    CHECK(g_failPublication == 1);
    g_failPublication = 0;
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_publicationCount == publications);
    g_driverRegistrations = drivers;
    puts("PCI preparation: RP1 failure retains unrelated descriptions, blocks binding and retries with stable IDs");
}

static void
__PublicationRetry(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 45, .BusEnd = 2 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t*   host;
    PciDevice_t* first;
    PciDevice_t* second;
    uuid_t       firstId;
    unsigned int publications = g_publicationCount;
    unsigned int bindings = g_bindingCount;
    unsigned int drivers = g_driverRegistrations;
    unsigned int writes;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    first = __PublicationFunction(host->RootDevice, 0);
    second = __PublicationFunction(host->RootDevice, 0);
    host->RootDevice->Handler = &g_retryHandler;
    host->RootDevice->Attachment = host;
    writes = g_bcmWrites + g_bridgeWrites;

    // Preparation fails before the group removal helper is entered. Preserve
    // the rollback intent, IDs and providers across repeated reconciliation.
    g_busyPublicationPreparation = 1;
    g_failPublication = 3;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(host->Publication.CleanupPending);
    CHECK(host->Publication.Group.State == DmPublicationCollecting);
    CHECK(g_publicationCount == publications + 2 && g_bindingCount == bindings);
    firstId = first->DeviceId;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(host->Publication.CleanupPending);
    CHECK(first->DeviceId == firstId && g_publicationCount == publications + 2);

    // Registry removal can also remain busy after preparation succeeds.
    g_busyPublicationPreparation = 0;
    g_failRemoval = 1;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(host->Publication.CleanupPending);
    CHECK(host->Publication.Group.State == DmPublicationRemoving);
    CHECK(first->DeviceId == firstId && g_bindingCount == bindings);
    g_failRemoval = 0;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(!host->Publication.CleanupPending);
    CHECK(first->DeviceId != firstId && second->DeviceId != UUID_INVALID);
    CHECK(g_publicationCount == publications + 3);
    CHECK(g_bindingCount == bindings + 2);

    // A direct removal refused before rollback starts keeps the live group usable.
    firstId = first->DeviceId;
    g_busyPublicationPreparation = 1;
    CHECK(PciUnpublishDevice(host->RootDevice) == OS_EBUSY);
    CHECK(!host->Publication.CleanupPending);
    CHECK(host->Publication.Group.State == DmPublicationReady);
    CHECK(PciPublishDevice(host->RootDevice).Status == OS_EOK);
    CHECK(first->DeviceId == firstId && g_bindingCount == bindings + 2);
    g_busyPublicationPreparation = 0;

    // A later binding failure keeps successful drivers and their IDs intact.
    CHECK(PciUnpublishDevice(host->RootDevice) == OS_EOK);
    g_failBinding = 2;
    CHECK(BusRecoverAndRepublish().Status == OS_EOOM);
    firstId = first->DeviceId;
    CHECK(g_bindingCount == bindings + 3);
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(first->DeviceId == firstId && g_bindingCount == bindings + 4);
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(g_publicationCount == publications + 3 && g_bindingCount == bindings + 4);
    CHECK(g_bcmWrites + g_bridgeWrites == writes);

    host->RootDevice->Attachment = NULL;
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_publicationCount == publications);
    g_driverRegistrations = drivers;
    puts("PCI reconciliation: unfinished rollback, partial binding and idempotent retries passed");
}

static void
__PublicationIoRetry(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 49, .BusEnd = 1 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    PciHost_t*   host;
    PciDevice_t* endpoint;
    unsigned int resources;
    unsigned int publications = g_publicationCount;
    unsigned int drivers = g_driverRegistrations;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    endpoint = __PublicationFunction(host->RootDevice, 0);
    endpoint->Resources.Bars[0] = (struct PciBar) {
        .State = PciBarAssigned, .Space = 2, .CpuAddress = 0xd0000000, .Size = 0x1000
    };
    endpoint->Resources.Bars[1] = endpoint->Resources.Bars[0];
    endpoint->Resources.Bars[1].CpuAddress += 0x1000;
    CHECK(PciPublishDevice(host->RootDevice).Status == OS_EOK);

    // A later cleanup failure preserves only the unfinished slots and host.
    g_failIoDestroyBase = 0xd0001000;
    CHECK(PciHostDestroy(host) == OS_EBUSY);
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 1);
    CHECK(endpoint->DeviceId == UUID_INVALID && host->RootDevice->DeviceId == UUID_INVALID);
    CHECK(endpoint->PublishedIo[0].Type == DeviceIoInvalid);
    CHECK(endpoint->PublishedIo[1].Type == DeviceIoMemoryBased);
    CHECK(host->Publication.CleanupPending);
    CHECK(host->Publication.Group.State == DmPublicationCollecting);
    CHECK(host->Publication.Group.First == NULL && host->Publication.Group.Last == NULL);
    resources = g_resources;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(g_resources == resources && host->Publication.CleanupPending);
    g_failIoDestroyBase = 0;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(!host->Publication.CleanupPending);
    CHECK(endpoint->DeviceId != UUID_INVALID && g_resources == resources + 2);
    CHECK(PciUnpublishDevice(host->RootDevice) == OS_EOK);

    // Registration can fail after BAR creation. Those handles have the same
    // persistent owner and must be cleaned before another publication attempt.
    g_failIoDestroyBase = 0xd0000000;
    g_failPublication = 2;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(host->Publication.CleanupPending);
    CHECK(host->Publication.Group.State == DmPublicationCollecting);
    CHECK(g_publicationCount == publications && endpoint->DeviceId == UUID_INVALID);
    CHECK(endpoint->PublishedIo[0].Type == DeviceIoMemoryBased);
    CHECK(endpoint->PublishedIo[1].Type == DeviceIoMemoryBased);
    resources = g_resources;
    CHECK(BusRecoverAndRepublish().Status == OS_EBUSY);
    CHECK(g_resources == resources);
    g_failIoDestroyBase = 0;
    CHECK(BusRecoverAndRepublish().Status == OS_EOK);
    CHECK(!host->Publication.CleanupPending);
    CHECK(g_resources == resources + 2);
    CHECK(PciHostDestroy(host) == OS_EOK);
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(g_publicationCount == publications);
    g_driverRegistrations = drivers;
    puts("PCI I/O ownership: partial destruction, failed registration and publication retry passed");
}
