/** Model independent startup owners with outcomes that cannot be inferred from errno. */
struct __TestStartupOwner {
    struct DmStartupResult Result;
    unsigned int           Attempts;
};

static void
__TestStartupPrepare(
    _In_  void*                   context,
    _Out_ struct DmStartupResult* result)
{
    struct __TestStartupOwner* owner = context;

    owner->Attempts++;
    *result = owner->Result;
}

static const struct PciFunctionHandler g_startupOutcomeHandler = {
    .PrepareHardware = __TestStartupPrepare
};

static void
__StartupOutcomes(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 64, .BusEnd = 1 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    struct __TestStartupOwner failed = {
        .Result = { .Status = OS_EUNKNOWN, .Outcome = DmStartupFailed }
    };
    struct __TestStartupOwner pending = {
        .Result = { .Status = OS_ENOTSUPPORTED, .Outcome = DmStartupRetry }
    };
    struct DmStartupResult result;
    PciHost_t* first;
    PciHost_t* second;
    PciDevice_t* firstEndpoint;
    PciDevice_t* secondEndpoint;
    uuid_t firstId;
    uuid_t secondId;
    unsigned int bindings = g_bindingCount;
    unsigned int drivers = g_driverRegistrations;
    unsigned int publications = g_publicationCount;
    unsigned int attempts;

    CHECK(PciEcamHostCreate(&configuration, &first) == OS_EOK);
    CHECK(PciHostRegister(first) == OS_EOK);
    configuration.Identification.Segment++;
    CHECK(PciEcamHostCreate(&configuration, &second) == OS_EOK);
    CHECK(PciHostRegister(second) == OS_EOK);
    firstEndpoint = __PublicationFunction(first->RootDevice, 0);
    secondEndpoint = __PublicationFunction(second->RootDevice, 0);
    first->RootDevice->Handler = &g_startupOutcomeHandler;
    first->RootDevice->Attachment = &failed;
    second->RootDevice->Handler = &g_startupOutcomeHandler;
    second->RootDevice->Attachment = &pending;

    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupRetry && result.Status == OS_EUNKNOWN);
    CHECK(failed.Attempts == 1 && pending.Attempts == 1);
    CHECK(first->Status.State == PciHostFailed && first->Status.Error == OS_EUNKNOWN);
    CHECK(second->Status.State == PciHostReady);
    CHECK(g_bindingCount == bindings);
    firstId = firstEndpoint->DeviceId;
    secondId = secondEndpoint->DeviceId;
    CHECK(firstId != UUID_INVALID && secondId != UUID_INVALID);

    // A terminal owner stays blocked while another host completes with the same IDs.
    __DmStartupFromStatus(OS_EOK, &pending.Result);
    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupFailed && result.Status == OS_EUNKNOWN);
    CHECK(failed.Attempts == 1 && pending.Attempts == 2);
    CHECK(g_bindingCount == bindings + 1);
    CHECK(firstEndpoint->DeviceId == firstId && secondEndpoint->DeviceId == secondId);
    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupFailed);
    CHECK(failed.Attempts == 1 && g_bindingCount == bindings + 1);

    // Explicit removal completes the old lifecycle before a new attempt is allowed.
    CHECK(PciUnpublishDevice(first->RootDevice) == OS_EOK);
    CHECK(first->Status.State == PciHostReady && first->Status.Error == OS_EOK);
    __DmStartupFromStatus(OS_EOK, &failed.Result);
    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupComplete);
    CHECK(failed.Attempts == 2 && g_bindingCount == bindings + 2);

    // Unknown configuration blocks startup, and unpublishing does not clear it.
    __PciHostSetState(second, PciHostConfigurationUnknown, OS_ENOTSUPPORTED);
    attempts = pending.Attempts;
    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupFailed && result.Status == OS_ENOTSUPPORTED);
    CHECK(PciUnpublishDevice(second->RootDevice) == OS_EOK);
    CHECK(second->Status.State == PciHostConfigurationUnknown);
    BusRecoverAndRepublish(&result);
    CHECK(result.Outcome == DmStartupFailed && result.Status == OS_ENOTSUPPORTED);
    CHECK(pending.Attempts == attempts);

    first->RootDevice->Attachment = NULL;
    second->RootDevice->Attachment = NULL;
    CHECK(PciHostDestroy(first) == OS_EOK);
    CHECK(PciHostDestroy(second) == OS_EOK);
    CHECK(g_publicationCount == publications);
    g_driverRegistrations = drivers;
    puts("Startup outcomes: terminal host isolation, explicit retries, stable IDs and unchanged binding barrier passed");
}
