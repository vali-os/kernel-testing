// Provider-only MMIO model. Drivers never receive the private DWC globals.
static uint32_t g_usbGlobals[1024];
static unsigned int g_ioCount;
static unsigned int g_ioNext;
static unsigned int g_sharedMappings;
static int g_failUsbWrite;

oserr_t CreateDeviceMemoryIo(DeviceIo_t* io, uintptr_t base, size_t length)
{
    memset(io, 0, sizeof(*io));
    io->Id = ++g_ioNext;
    io->Type = DeviceIoMemoryBased;
    io->Access.Memory.PhysicalBase = base;
    io->Access.Memory.Length = length;
    g_ioCount++;
    return OS_EOK;
}
oserr_t DestroyDeviceIo(DeviceIo_t* io)
{
    CHECK(io->Id != UUID_INVALID && g_ioCount != 0);
    g_ioCount--;
    return OS_EOK;
}
oserr_t MapDeviceIo(uuid_t id, size_t offset, size_t length, void** output)
{
    CHECK(id != UUID_INVALID && offset == 0 && length == sizeof(g_usbGlobals));
    g_sharedMappings++;
    *output = g_usbGlobals;
    return OS_EOK;
}
oserr_t MemoryFree(void* buffer, size_t length)
{
    CHECK(buffer == g_usbGlobals && length == sizeof(g_usbGlobals));
    CHECK(g_sharedMappings == 1);
    g_sharedMappings--;
    return OS_EOK;
}
size_t ReadDeviceIo(DeviceIo_t* io, size_t offset, size_t width)
{
    CHECK(io->Access.Memory.VirtualBase == (uintptr_t)g_usbGlobals);
    CHECK(width == 4 && offset < sizeof(g_usbGlobals));
    return g_usbGlobals[offset / 4];
}
oserr_t WriteDeviceIo(DeviceIo_t* io, size_t offset, size_t value, size_t width)
{
    (void)ReadDeviceIo(io, offset, width);
    if (g_failUsbWrite) return OS_EUNKNOWN;
    g_usbGlobals[offset / 4] = value;
    return OS_EOK;
}

static void __TestRp1ResourcePath(void)
{
    struct Rp1Bus bus = { .Ready = 1 };
    struct Rp1Child child = { .Bus = &bus };
    struct DeviceResources resources;
    enum DeviceActivation stage = DeviceStart;
    uuid_t published;

    child.Firmware.Compatible = "snps,dwc3";
    child.Firmware.CompatibleLength = 9;
    CHECK(!Rp1ChildIsUsb(&child));
    child.Firmware.CompatibleLength = 10;
    CHECK(Rp1ChildIsUsb(&child));
    child.Firmware.RegisterCount = 1;
    child.Firmware.Registers[0].Base = 0x100000;
    child.Firmware.Registers[0].Length = 0x100000;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
    stage = DevicePrepare;
    g_usbGlobals[0x120 / 4] = 0x55330000;
    g_usbGlobals[0x140 / 4] = 1;
    g_failUsbWrite = 1;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EUNKNOWN);
    CHECK(!child.Resources.Prepared && g_sharedMappings == 1);
    g_failUsbWrite = 0;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
    CHECK(g_sharedMappings == 1 && child.Resources.Prepared);
    CHECK(g_usbGlobals[0x104 / 4] == 7U << 8);
    CHECK(g_usbGlobals[0x110 / 4] == 1U << 12);
    CHECK(g_usbGlobals[0x11c / 4] == ((1U << 19) | (7U << 15)));
    CHECK(g_usbGlobals[0x108 / 4] == ((8U << 16) | (2U << 24) | (1U << 29)));
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_RESOURCES, &resources, sizeof(resources)) == OS_EOK);
    CHECK(resources.Count == 1 && resources.Registers[0].Access.Memory.Length == 0x8000);
    CHECK(resources.Registers[0].Access.Memory.VirtualBase == 0);
    published = resources.Registers[0].Id;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_RESOURCES, &resources, sizeof(resources)) == OS_EOK);
    CHECK(resources.Registers[0].Id == published && g_ioCount == 2);
    stage = DeviceStart;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
    stage = DeviceStop;
    CHECK(Rp1ChildResourceControl(&child, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
    CHECK(Rp1ChildResourcesDestroy(&child) == OS_EOK);
    CHECK(g_ioCount == 0 && g_sharedMappings == 0);
}

static void __TestDmaSessions(void)
{
    const struct DmDeviceProviderOperations operations = {
        .Retain = __ProviderRetain, .Release = __ProviderRelease, .PrepareDma = __TestPrepareDma
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
