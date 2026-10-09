// Exercise the real registry with controlled callback failures and removal.
static oserr_t g_dmaStatus;
static unsigned int g_dmaCalls;

static oserr_t
__TestPrepareDma(
    _In_  void*                    context,
    _Out_ struct DmDmaDescription* description)
{
    struct __TestProvider* provider = context;

    // Removal inside the callback reproduces the point where another request
    // can mark the entry removing, without depending on thread scheduling.
    CHECK(g_registryLocks == 0 && provider->References == 1);
    g_dmaCalls++;
    description->DeviceId = UUID_INVALID;
    description->HostId = 123;
    description->CachePolicy = DmDmaCacheNonCoherent;
    description->Map.Count = 1;
    description->Map.Ranges[0] = (struct FdtDmaRange) {
        .PhysicalBase = 0x700000, .DeviceBase = 0x1000, .Length = 0x2000
    };
    if (provider->RemoveDuringRequest) {
        CHECK(DmDeviceDestroy(provider->DeviceId) == OS_EBUSY);
    }
    return g_dmaStatus;
}

static void
__TestDmaLeases(void)
{
    const struct DmDeviceProviderOperations operations = {
        .Retain = __ProviderRetain,
        .Release = __ProviderRelease,
        .PrepareDma = __TestPrepareDma
    };
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Provider = { .Operations = &operations, .Context = &provider }
    };
    struct DmDmaLease* first = NULL;
    struct DmDmaLease* second = NULL;
    const struct DmDmaDescription* description;
    struct DMDevice* entry;
    unsigned int calls;
    unsigned int mode;

    // Invalid inputs and unsupported providers must not acquire lasting guards.
    CHECK(DmDevicePrepareDma(UUID_INVALID, NULL) == OS_EINVALPARAMS);
    CHECK(DmDevicePrepareDma(UUID_INVALID, &first) == OS_ENOENT);
    CHECK(DmDmaLeaseGetDescription(NULL) == NULL);
    DmDmaLeaseRelease(NULL);
    DmDmaLeaseRelease(&first);
    registration.Provider.Operations = &g_firstOperations;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &provider.DeviceId) == OS_EOK);
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_ENOTSUPPORTED);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK);

    registration.Description = __NewDescription();
    registration.Provider.Operations = NULL;
    registration.Provider.Context = NULL;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &provider.DeviceId) == OS_EOK);
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_ENOTSUPPORTED && first == NULL);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK);

    memset(&provider, 0, sizeof(provider));
    registration.Provider.Context = &provider;
    registration.Description = __NewDescription();
    registration.Provider.Operations = &operations;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &provider.DeviceId) == OS_EOK);
    entry = __GetDeviceUnsafe(provider.DeviceId);
    calls = g_dmaCalls;
    g_failAllocation = 1;
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EOOM);
    g_failAllocation = 0;
    CHECK(first == NULL && g_dmaCalls == calls && entry->ActiveRequests == 0);
    g_dmaStatus = OS_EUNKNOWN;
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EUNKNOWN);
    CHECK(first == NULL && entry->ActiveRequests == 0 && entry->DmaLeases == 0);
    g_dmaStatus = OS_EOK;
    entry->ActiveRequests = UINT_MAX;
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EOVERFLOW);
    entry->ActiveRequests = 0;
    entry->DmaLeases = UINT_MAX;
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EOVERFLOW);
    CHECK(first == NULL && entry->ActiveRequests == 0 && entry->DmaLeases == UINT_MAX);
    entry->DmaLeases = 0;

    // Separate leases share the existing registry reference. Reusing an owning
    // slot cannot overwrite its lease, and one release cannot remove the other.
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EOK);
    description = DmDmaLeaseGetDescription(first);
    CHECK(description->DeviceId == provider.DeviceId && description->HostId == 123);
    CHECK(description->Map.Ranges[0].PhysicalBase == 0x700000);
    CHECK(description->CachePolicy == DmDmaCacheNonCoherent);
    CHECK(DmDevicePrepareDma(provider.DeviceId, &first) == OS_EBUSY);
    CHECK(DmDmaLeaseGetDescription(first) == description);
    CHECK(DmDevicePrepareDma(provider.DeviceId, &second) == OS_EOK);
    CHECK(provider.Retains == 1 && provider.References == 1);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EBUSY);
    DmDmaLeaseRelease(&second);
    DmDmaLeaseRelease(&second);
    CHECK(DmDevicePrepareDma(provider.DeviceId, &second) == OS_EBUSY);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EBUSY);
    CHECK(description->Map.Ranges[0].Length == 0x2000 && provider.Releases == 0);
    DmDmaLeaseRelease(&first);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK && provider.Releases == 1);

    // Even successful callback data must be discarded when removal has begun.
    // Callback failures in that same situation must also leave removal retryable.
    for (mode = 0; mode < 2; mode++) {
        memset(&provider, 0, sizeof(provider));
        provider.RemoveDuringRequest = 1;
        registration.Description = __NewDescription();
        CHECK(DmDeviceCreateWithProvider(&registration, 0, &provider.DeviceId) == OS_EOK);
        g_dmaStatus = mode == 0 ? OS_EOK : OS_EUNKNOWN;
        CHECK(DmDevicePrepareDma(provider.DeviceId, &first) ==
            (mode == 0 ? OS_EBUSY : OS_EUNKNOWN));
        CHECK(first == NULL && provider.Releases == 0);
        CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK && provider.Releases == 1);
    }
    puts("DMA leases: failure rollback, removal races, multiple owners and count overflow passed");
}
