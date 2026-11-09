// Verify that publication supplies an assigned MMIO resource to the driver.
static uint32_t g_assignmentConfig[64];

static size_t
__AssignmentRead(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t width)
{
    return g_assignmentConfig[reg / 4];
}

static void
__AssignmentWrite(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t value,
    _In_ size_t width)
{
    g_assignmentConfig[reg / 4] = value;
}

static void
__PublicationBarAssignment(void)
{
    struct PciEcamHostConfiguration configuration = {
        .Identification = { .Segment = 50, .BusEnd = 2 },
        .Base = 0xb0000000,
        .IoResourcePolicy = PciIoResourceMemory
    };
    struct FdtPciHost firmware = {
        .WindowCount = 1,
        .Windows = {{ .Space = FdtPciSpaceMemory32, .BusBase = 0xc0000000,
            .PhysicalBase = 0x600000000ULL, .Length = 0x4000000 }}
    };
    const struct PciHostOperations operations = {
        .Read = __AssignmentRead, .Write = __AssignmentWrite
    };
    const struct PciHostOperations* original;
    struct DmStartupResult result;
    PciHost_t* host;
    PciDevice_t* endpoint;

    CHECK(PciEcamHostCreate(&configuration, &host) == OS_EOK);
    CHECK(PciHostRegister(host) == OS_EOK);
    endpoint = __PublicationFunction(host->RootDevice, 0);
    endpoint->Resources.Bars[0] = (struct PciBar) {
        .State = PciBarUnassigned, .Space = 2, .Size = 0x1000
    };
    original = host->Operations;
    host->Operations = &operations;
    host->Firmware = &firmware;
    CHECK(PciHostLoadDtWindows(host) == OS_EOK);
    memset(g_assignmentConfig, 0, sizeof(g_assignmentConfig));
    PciPublishDevice(host->RootDevice, &result);
    CHECK(result.Status == OS_EOK);
    CHECK(g_assignmentConfig[4] == 0xc0000000);
    CHECK(endpoint->Resources.Bars[0].State == PciBarAssigned);
    CHECK(endpoint->PublishedIo[0].Type == DeviceIoMemoryBased);
    CHECK(endpoint->PublishedIo[0].Access.Memory.PhysicalBase == 0x600000000ULL);
    CHECK(endpoint->PublishedIo[0].Access.Memory.Length == 0x1000);
    host->Firmware = NULL;
    host->Operations = original;
    CHECK(PciHostDestroy(host) == OS_EOK);
}
