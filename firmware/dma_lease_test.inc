// Exercise the real registry with controlled callback failures and removal.
static oserr_t g_dmaStatus;
static unsigned int g_dmaCalls;
static unsigned int g_dmaGuards;

static oserr_t
__TestPrepareDma(
    _In_  void*                    context,
    _In_  uuid_t                   principal,
    _Out_ struct DmDmaDescription* description)
{
    struct __TestProvider* provider = context;

    // Removal inside the callback reproduces the point where another request
    // can mark the entry removing, without depending on thread scheduling.
    CHECK(g_registryLocks == 0 && provider->References == 1);
    g_dmaCalls++;
    description->DeviceId = UUID_INVALID;
    description->HostId = 123;
    description->AddressLimit = 0x2fff;
    description->CachePolicy = DmDmaCacheNonCoherent;
    description->Map.Count = 1;
    description->Map.Ranges[0] = (DmDmaRange_t) {
        .PhysicalBase = 0x700000, .DeviceBase = 0x1000, .Length = 0x2000
    };
    if (provider->RemoveDuringRequest) {
        CHECK(DmDeviceDestroy(provider->DeviceId) == OS_EBUSY);
    }
    if (g_dmaStatus == OS_EOK) {
        g_dmaGuards++;
    }
    return g_dmaStatus;
}

static void __TestReleaseDma(void* context)
{
    struct __TestProvider* provider = context;
    DmDevice_t* device = DmDeviceFindLocked(provider->DeviceId);
    CHECK(g_registryLocks == 0 && provider->References == 1 && g_dmaGuards != 0);
    CHECK(device != NULL && (device->ActiveRequests || device->DmaLeases));
    g_dmaGuards--;
}

static unsigned int g_dmaDescriptionCalls;
static oserr_t      g_dmaDescriptionStatus;

static oserr_t
__TestDescribeDma(
    _In_  void*               context,
    _Out_ DmDmaDescription_t* description)
{
    struct __TestProvider* busState = context;
    DmDevice_t*            device;

    CHECK(g_registryLocks == 0 && busState->References == 1);
    usched_mtx_lock(&g_dmDevicesLock);
    device = DmDeviceFindLocked(busState->DeviceId);
    CHECK(device != NULL && device->ActiveRequests == 1 && device->DmaLeases == 0);
    usched_mtx_unlock(&g_dmDevicesLock);
    g_dmaDescriptionCalls++;
    *description = (DmDmaDescription_t) {
        .DeviceId = UUID_INVALID,
        .HostId = 123,
        .AddressLimit = 0x2fff,
        .CachePolicy = DmDmaCacheNonCoherent,
        .Map = { .Count = 1, .Ranges = {
            { .PhysicalBase = 0x700000, .DeviceBase = 0x1000, .Length = 0x2000 }
        } }
    };
    if (busState->RemoveDuringRequest) {
        CHECK(DmDeviceDestroy(busState->DeviceId) == OS_EBUSY);
    }
    return g_dmaDescriptionStatus;
}

static void
__TestDmaDescriptions(void)
{
    struct DmDeviceBusOperations operations = {
        .AddReference = __ProviderRetain,
        .Release = __ProviderRelease,
        .PrepareDma = __TestPrepareDma,
        .ReleaseDma = __TestReleaseDma
    };
    struct __TestProvider busState = { 0 };
    DmDeviceRegistration_t registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Bus = { .Operations = &operations, .Context = &busState }
    };
    DmDmaDescription_t description;
    DmDmaDescription_t unchanged;
    DmDevice_t*        device;
    unsigned int       prepareCalls = g_dmaCalls;
    unsigned int       describeCalls;

    memset(&description, 0xa5, sizeof(description));
    unchanged = description;
    CHECK(DmDeviceDescribeDma(UUID_INVALID, NULL) == OS_EINVALPARAMS);
    CHECK(DmDeviceDescribeDma(UUID_INVALID, &description) == OS_ENOENT);
    CHECK(memcmp(&description, &unchanged, sizeof(description)) == 0);
    CHECK(DmDeviceCreateWithBus(&registration, 0, &busState.DeviceId) == OS_EOK);
    // Never substitute active preparation when static description is absent.
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_ENOTSUPPORTED);
    CHECK(g_dmaCalls == prepareCalls && g_dmaGuards == 0);
    CHECK(memcmp(&description, &unchanged, sizeof(description)) == 0);

    operations.DescribeDma = __TestDescribeDma;
    g_dmaDescriptionStatus = OS_EOK;
    g_failAllocation = 1;
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_EOK);
    g_failAllocation = 0;
    CHECK(description.DeviceId == busState.DeviceId && description.HostId == 123);
    CHECK(description.AddressLimit == 0x2fff && description.Map.Count == 1);
    CHECK(description.Map.Ranges[0].PhysicalBase == 0x700000);
    CHECK(description.Map.Ranges[0].DeviceBase == 0x1000);
    CHECK(description.Map.Ranges[0].Length == 0x2000);
    CHECK(description.CachePolicy == DmDmaCacheNonCoherent);
    CHECK(g_dmaCalls == prepareCalls && g_dmaGuards == 0);

    // Inspect counts with the registry lock, then make the callback fail after
    // it writes data. The public output and active guard count must not change.
    usched_mtx_lock(&g_dmDevicesLock);
    device = DmDeviceFindLocked(busState.DeviceId);
    CHECK(device->ActiveRequests == 0 && device->DmaLeases == 0);
    usched_mtx_unlock(&g_dmDevicesLock);
    description = unchanged;
    g_dmaDescriptionStatus = OS_EUNKNOWN;
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_EUNKNOWN);
    CHECK(memcmp(&description, &unchanged, sizeof(description)) == 0);
    CHECK(device->ActiveRequests == 0 && device->DmaLeases == 0);
    g_dmaDescriptionStatus = OS_EOK;
    describeCalls = g_dmaDescriptionCalls;
    device->ActiveRequests = UINT_MAX;
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_EOVERFLOW);
    CHECK(g_dmaDescriptionCalls == describeCalls);
    device->ActiveRequests = 0;

    busState.RemoveDuringRequest = 1;
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_EBUSY);
    CHECK(memcmp(&description, &unchanged, sizeof(description)) == 0);
    CHECK(device->ActiveRequests == 0 && device->DmaLeases == 0 && busState.Releases == 0);
    describeCalls = g_dmaDescriptionCalls;
    CHECK(DmDeviceDescribeDma(busState.DeviceId, &description) == OS_EBUSY);
    CHECK(g_dmaDescriptionCalls == describeCalls);
    CHECK(DmDeviceDestroy(busState.DeviceId) == OS_EOK && busState.Releases == 1);
    CHECK(g_dmaCalls == prepareCalls && g_dmaGuards == 0);
    puts("DMA descriptions: pre-start snapshots, no active guards, output rollback and removal races passed");
}

static void
__TestDmaLeases(void)
{
    const struct DmDeviceBusOperations operations = {
        .AddReference = __ProviderRetain,
        .Release = __ProviderRelease,
        .PrepareDma = __TestPrepareDma,
        .ReleaseDma = __TestReleaseDma
    };
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Bus = { .Operations = &operations, .Context = &provider }
    };
    struct DmDmaLease* first = NULL;
    struct DmDmaLease* second = NULL;
    const struct DmDmaDescription* description;
    DmDevice_t* entry;
    unsigned int calls;
    unsigned int mode;

    // Invalid inputs and unsupported providers must not acquire lasting guards.
    CHECK(DmDevicePrepareDmaForPrincipal(UUID_INVALID, 1, NULL) == OS_EINVALPARAMS);
    CHECK(DmDevicePrepareDmaForPrincipal(UUID_INVALID, 1, &first) == OS_ENOENT);
    CHECK(DmDmaLeaseGetDescription(NULL) == NULL);
    DmDmaLeaseRelease(NULL);
    DmDmaLeaseRelease(&first);
    registration.Bus.Operations = &g_firstOperations;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &provider.DeviceId) == OS_EOK);
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_ENOTSUPPORTED);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK);

    registration.Description = __NewDescription();
    registration.Bus.Operations = NULL;
    registration.Bus.Context = NULL;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &provider.DeviceId) == OS_EOK);
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_ENOTSUPPORTED && first == NULL);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK);

    memset(&provider, 0, sizeof(provider));
    registration.Bus.Context = &provider;
    registration.Description = __NewDescription();
    registration.Bus.Operations = &operations;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &provider.DeviceId) == OS_EOK);
    entry = DmDeviceFindLocked(provider.DeviceId);
    calls = g_dmaCalls;
    g_failAllocation = 1;
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EOOM);
    g_failAllocation = 0;
    CHECK(first == NULL && g_dmaCalls == calls && entry->ActiveRequests == 0);
    g_dmaStatus = OS_EUNKNOWN;
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EUNKNOWN);
    CHECK(first == NULL && entry->ActiveRequests == 0 && entry->DmaLeases == 0);
    g_dmaStatus = OS_EOK;
    entry->ActiveRequests = UINT_MAX;
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EOVERFLOW);
    entry->ActiveRequests = 0;
    entry->DmaLeases = UINT_MAX;
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EOVERFLOW);
    CHECK(first == NULL && entry->ActiveRequests == 0 && entry->DmaLeases == UINT_MAX);
    entry->DmaLeases = 0;

    // Separate leases share the existing registry reference. Reusing an owning
    // slot cannot overwrite its lease, and one release cannot remove the other.
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EOK);
    description = DmDmaLeaseGetDescription(first);
    CHECK(description->DeviceId == provider.DeviceId && description->HostId == 123);
    CHECK(description->Map.Ranges[0].PhysicalBase == 0x700000);
    CHECK(description->CachePolicy == DmDmaCacheNonCoherent);
    CHECK(description->AddressLimit == 0x2fff);
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) == OS_EBUSY);
    CHECK(DmDmaLeaseGetDescription(first) == description);
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &second) == OS_EOK);
    CHECK(provider.Retains == 1 && provider.References == 1);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EBUSY);
    DmDmaLeaseRelease(&second);
    DmDmaLeaseRelease(&second);
    CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &second) == OS_EBUSY);
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
        CHECK(DmDeviceCreateWithBus(&registration, 0, &provider.DeviceId) == OS_EOK);
        g_dmaStatus = mode == 0 ? OS_EOK : OS_EUNKNOWN;
        CHECK(DmDevicePrepareDmaForPrincipal(provider.DeviceId, 1, &first) ==
            (mode == 0 ? OS_EBUSY : OS_EUNKNOWN));
        CHECK(first == NULL && provider.Releases == 0);
        CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK && provider.Releases == 1);
    }
    CHECK(g_dmaGuards == 0);
    puts("DMA leases: paired provider guards, rollback, removal races, owners and overflow passed");
}
