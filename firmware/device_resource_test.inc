// The bus registers complete ranges but never maps or programs a peripheral.
static unsigned int g_ioCount;
static unsigned int g_ioNext;
static int g_failIoDestroy;
static unsigned int g_failIoCreate;

oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* io)
{
    // These tests only register child ranges; the service never maps them.
    (void)io;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

oserr_t
CreateDeviceMemoryIo(
    _Out_ DeviceIo_t* io,
    _In_ uintptr_t   base,
    _In_ size_t      length)
{
    CHECK(g_pciLocks == 0 && g_rp1LockDepth == 1);
    memset(io, 0, sizeof(*io));
    io->Type = DeviceIoMemoryBased;
    io->Access.Memory.PhysicalBase = base;
    io->Access.Memory.Length = length;
    if (g_failIoCreate != 0 && --g_failIoCreate == 0) {
        return OS_EOOM;
    }
    io->Id = ++g_ioNext;
    g_ioCount++;
    return OS_EOK;
}

oserr_t
DestroyDeviceIo(
    _In_ DeviceIo_t* io)
{
    CHECK(io->Id != UUID_INVALID && g_ioCount != 0);
    if (g_failIoDestroy) {
        return OS_EBUSY;
    }
    g_ioCount--;
    return OS_EOK;
}

static void
__TestRp1ResourcePath(void)
{
    PciNativeHeader_t header = { .VendorId = 0x1de4, .DeviceId = 1, .Revision = 2 };
    struct PciDevice parent = { .Header = &header };
    struct Rp1Bus bus = { .Ready = 1, .Parent = &parent };
    struct Rp1Child child = { .Bus = &bus };
    struct DeviceResources resources;
    struct DeviceResources unchanged;
    enum DeviceActivation stage = DeviceStart;
    uuid_t published;
    unsigned int i;

    usched_mtx_init(&bus.Lock, USCHED_MUTEX_PLAIN);

    // Neither activation nor resource exposure depends on peripheral identity.
    for (i = 0; i < 2; i++) {
        child.Firmware.Compatible = i == 0 ? "snps,dwc3" : "test,unrelated";
        child.Firmware.CompatibleLength = (uint32_t)strlen(child.Firmware.Compatible) + 1;
        child.Firmware.RegisterCount = 1;
        child.Firmware.Registers[0].Base = 0x100000;
        child.Firmware.Registers[0].Length = 0x100000;
        stage = DeviceStart;
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
        stage = DevicePrepare;
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        CHECK(child.Resources.Prepared);
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES, &resources, sizeof(resources)) == OS_EOK);
        CHECK(resources.Count == 1 && resources.Registers[0].Access.Memory.Length == 0x100000);
        CHECK(resources.Registers[0].Access.Memory.PhysicalBase == 0x100000);
        CHECK(resources.Registers[0].Access.Memory.VirtualBase == 0);
        CHECK(resources.ParentPci.VendorId == header.VendorId);
        CHECK(resources.ParentPci.ProductId == header.DeviceId);
        CHECK(resources.ParentPci.Revision == header.Revision);
        published = resources.Registers[0].Id;
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES, &resources, sizeof(resources)) == OS_EOK);
        CHECK(resources.Registers[0].Id == published && g_ioCount == 1);
        stage = DeviceStart;
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        stage = DeviceStop;
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        CHECK(!child.Resources.Prepared && bus.Ready);
        CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        g_failIoDestroy = 1;
        CHECK(Rp1ChildResourcesDestroy(&child) == OS_EBUSY);
        CHECK(child.Resources.Published.Registers[0].Id == published && g_ioCount == 1);
        g_failIoDestroy = 0;
        CHECK(Rp1ChildResourcesDestroy(&child) == OS_EOK);
        CHECK(g_ioCount == 0);
        // Metadata comes from the parent, rather than being hard-coded to RP1.
        header.Revision++;
    }

    // A failed later registration must leave the reply untouched and keep the
    // earlier handle available for reuse. The failed DDK output is not live.
    child.Firmware.RegisterCount = 2;
    child.Firmware.Registers[1].Base = 0x200000;
    child.Firmware.Registers[1].Length = 0x1000;
    memset(&resources, 0xa5, sizeof(resources));
    unchanged = resources;
    g_failIoCreate = 2;
    CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES,
        &resources, sizeof(resources)) == OS_EOOM);
    CHECK(memcmp(&resources, &unchanged, sizeof(resources)) == 0);
    CHECK(g_ioCount == 1 && child.Resources.Published.Registers[1].Type == DeviceIoInvalid);
    published = child.Resources.Published.Registers[0].Id;
    CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES,
        &resources, sizeof(resources)) == OS_EOK);
    CHECK(resources.Registers[0].Id == published && g_ioCount == 2);

    unchanged = resources;
    child.Firmware.RegisterCount = 3;
    child.Firmware.Registers[2].Base = UINTPTR_MAX;
    child.Firmware.Registers[2].Length = 2;
    CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES,
        &resources, sizeof(resources)) == OS_ENOTSUPPORTED);
    CHECK(memcmp(&resources, &unchanged, sizeof(resources)) == 0 && g_ioCount == 2);
    child.Firmware.Registers[2].Base = 0;
    child.Firmware.Registers[2].Length = 0;
    CHECK(__Rp1ProviderControl(&child, 700, OSIOCTLREQUEST_DEVICE_RESOURCES,
        &resources, sizeof(resources)) == OS_ENOTSUPPORTED);
    CHECK(Rp1ChildResourcesDestroy(&child) == OS_EOK && g_ioCount == 0);
}
static void __TestDmaSessions(void)
{
    const struct DmDeviceProviderOperations operations = {
        .AddReference = __ProviderRetain, .Release = __ProviderRelease, .PrepareDma = __TestPrepareDma,
        .ReleaseDma = __TestReleaseDma
    };
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = { .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Provider = { .Operations = &operations, .Context = &provider } };
    struct OSDmaConfiguration first, second;
    uint64_t old;

    g_dmaStatus = OS_EOK;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &provider.DeviceId) == OS_EOK);
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_QUERY, &first, sizeof(first)) == OS_EOK);
    CHECK(first.Count == 1 && first.Ranges[0].DeviceBase == 0x1000);
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_QUERY, &second, sizeof(second)) == OS_EOK);
    CHECK(first.Session == second.Session);
    CHECK(DmDmaControl(2, provider.DeviceId, OSIOCTLREQUEST_DMA_QUERY, &second, sizeof(second)) == OS_EBUSY);
    old = first.Session;
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_RELEASE, &old, sizeof(old)) == OS_EOK);
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_QUERY, &second, sizeof(second)) == OS_EOK);
    CHECK(second.Session != old);
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_RELEASE, &old, sizeof(old)) == OS_EOK);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EBUSY);
    CHECK(DmDmaControl(1, provider.DeviceId, OSIOCTLREQUEST_DMA_RELEASE, &second.Session, sizeof(second.Session)) == OS_EOK);
    CHECK(DmDeviceDestroy(provider.DeviceId) == OS_EOK);
}
