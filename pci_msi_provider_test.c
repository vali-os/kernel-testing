/**
 * Integrate the real PCI provider, configuration access and MSI engine.
 * Bus-control requests must survive Release, including cleanup retries.
 */
#include <bus/pci/device.h>
#if defined(_MSC_VER) && defined(__x86_64__)
#include <emmintrin.h>
#endif

// Publication is outside this test. Local linkage lets the host compiler omit
// these unused entry points without mocking the device registry and BAR setup.
#define PciPublishDevice __UnusedPublishDevice
#define PciUnpublishDevice __UnusedUnpublishDevice
static oserr_t __UnusedPublishDevice(struct PciDevice* device);
static oserr_t __UnusedUnpublishDevice(struct PciDevice* device);
#include "../services/deviced/bus/pci/device.c"
#undef PciPublishDevice
#undef PciUnpublishDevice
#include "../services/deviced/bus/pci/io.c"
#include "../services/deviced/bus/pci/msi.c"
#include "../services/deviced/bus/pci/msix.c"
#include "../services/deviced/bus/pci/msix-device.c"
#include <stdio.h>

// The engine logs through the kernel log, which is not available on the host.
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static unsigned char g_config[256];
static int           g_locked;
static int           g_dropPayload;
static int           g_msiAvailable = 1;
static _Alignas(4096) uint32_t g_mmio[1024];
static int g_msixMapped;
static int g_msixUnmapFailure;
static unsigned g_unpublications;
static unsigned g_ioDestructions;
static const struct PciFunctionHandler g_blockedHandler = { .BlockActivation = 1 };
oserr_t
DmPublicationRemove(struct DmPublicationGroup* group)
{
    (void)group;
    CHECK(!g_locked && !g_msixMapped);
    g_unpublications++;
    return OS_EOK;
}

oserr_t
DestroyDeviceIo(DeviceIo_t* io)
{
    CHECK(io->Id == 77 && !g_msixMapped);
    g_ioDestructions++;
    return OS_EOK;
}

oserr_t
MapDeviceIo(uuid_t id, size_t offset, size_t length, void** mapping)
{
    CHECK(g_locked && id == 77 && offset == 0x100 && length == 64 && !g_msixMapped);
    g_msixMapped = 1;
    *mapping = (unsigned char*)g_mmio + offset;
    return OS_EOK;
}

size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
MemoryFree(void* mapping, size_t length)
{
    CHECK(g_locked && g_msixMapped && mapping == g_mmio && length == 0x140);
    if (g_msixUnmapFailure) {
        return OS_EUNKNOWN;
    }
    g_msixMapped = 0;
    return OS_EOK;
}

void
PciCriticalSectionEnter(void)
{
    CHECK(!g_locked);
    g_locked = 1;
}

void
PciCriticalSectionLeave(void)
{
    CHECK(g_locked);
    g_locked = 0;
}

int
PciIsMsiAvailable(void)
{
    return g_msiAvailable;
}

static size_t
__ReadConfig(
    _In_ PciHost_t*  host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t       offset,
    _In_ size_t       width)
{
    size_t value = 0;
    size_t i;

    (void)host;
    (void)bus;
    (void)slot;
    (void)function;
    CHECK(g_locked && offset + width <= sizeof(g_config));
    for (i = 0; i < width; i++) {
        value |= (size_t)g_config[offset + i] << (8 * i);
    }
    return value;
}

static void
__WriteConfig(
    _In_ PciHost_t*  host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t       offset,
    _In_ size_t       value,
    _In_ size_t       width)
{
    size_t i;

    (void)host;
    (void)bus;
    (void)slot;
    (void)function;
    CHECK(g_locked && offset + width <= sizeof(g_config));
    if (g_dropPayload && offset == 0x44) {
        return;
    }
    for (i = 0; i < width; i++) {
        g_config[offset + i] = (unsigned char)(value >> (8 * i));
    }
}

static void
__TestBusControl(
    _In_ int initiallyDisabled,
    _In_ int requests,
    _In_ int finallyEnabled,
    _In_ int retryRelease)
{
    static const struct PciHostOperations operations = {
        .Read = __ReadConfig,
        .Write = __WriteConfig
    };
    PciHost_t host = { .Operations = &operations, .InterruptControllerId = 91 };
    PciDevice_t device = { .Host = &host };
    struct PciInterruptRequest request = {
        .Operation = PciInterruptReserve,
        .Allocation = 1023,
        .Strategy = INTERRUPT_STRATEGY_MSI,
        .Count = 1
    };
    struct OSIOCtlBusControl control = { 0 };
    size_t command;
    int    expectedDisabled;
    int    i;

    memset(g_config, 0, sizeof(g_config));
    g_config[5] = initiallyDisabled ? 4 : 0;
    g_config[6] = 0x10;
    g_config[0x34] = 0x40;
    g_config[0x40] = 5;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK(device.Msi.Active);
    request.Operation = PciInterruptProgram;
    request.Count = 0;
    request.AddressLow = 0xfee00000;
    request.Data = 0xe0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    request.AddressLow = 0;
    request.Data = 0;

    if (retryRelease) {
        // Stop at payload restoration, leaving the reservation alive. A bus
        // request accepted between cleanup attempts must still take precedence.
        g_dropPayload = 1;
        request.Operation = PciInterruptRelease;
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
            &request, sizeof(request)) == OS_EUNKNOWN);
        CHECK(device.Msi.Active);
        g_dropPayload = 0;
    }
    for (i = 0; i < requests; i++) {
        control.Flags = __DEVICEMANAGER_IOCTL_MMIO_ENABLE;
        if ((i == requests - 1) ? finallyEnabled : !finallyEnabled) {
            control.Flags |= __DEVICEMANAGER_IOCTL_ENABLE;
        }
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_BUS_CONTROL,
            &control, sizeof(control)) == OS_EOK);
        CHECK(__PciProviderAccessRegister(&device, __DEVICEMANAGER_IOCTL_EXT_READ,
            4, &command, 2) == OS_EOK);
        CHECK(command & PCI_COMMAND_INTDISABLE);
    }

    // Rejected requests must not replace the accepted state.
    device.Handler = &g_blockedHandler;
    control.Flags ^= __DEVICEMANAGER_IOCTL_ENABLE;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    device.Handler = NULL;
    request.Operation = PciInterruptRelease;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK(!device.Msi.Active);
    CHECK(__PciProviderAccessRegister(&device, __DEVICEMANAGER_IOCTL_EXT_READ,
        4, &command, 2) == OS_EOK);
    expectedDisabled = requests ? !finallyEnabled : initiallyDisabled;
    CHECK(!!(command & PCI_COMMAND_INTDISABLE) == expectedDisabled);
    CHECK(!!(command & PCI_COMMAND_MMIO) == !!requests);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK(!g_locked);
}

static void
__TestCapacityQuery(void)
{
    static const struct PciHostOperations operations = {
        .Read = __ReadConfig,
        .Write = __WriteConfig
    };
    PciHost_t host = { .Operations = &operations, .InterruptControllerId = 91 };
    PciDevice_t device = { .Host = &host };
    struct PciInterruptSupport support;
    struct PciInterruptRequest request = {
        .Operation = PciInterruptReserve,
        .Allocation = 1023,
        .Strategy = INTERRUPT_STRATEGY_MSI,
        .Count = 1
    };
    unsigned char original[256];
    unsigned int mode;

    for (mode = 0; mode < 3; mode++) {
        memset(g_config, 0, sizeof(g_config));
        g_config[6] = 0x10;
        g_config[0x34] = 0x40;
        g_config[0x40] = mode == 1 ? 0x11 : 5;
        g_config[0x42] = mode == 1 ? 0xff : 10;
        g_config[0x43] = mode == 1 ? 7 : 0;
        if (mode == 2) {
            g_config[0x41] = 0x60;
            g_config[0x60] = 0x11;
            g_config[0x62] = 7;
        }
        memcpy(original, g_config, sizeof(original));
        memset(&support, 0xff, sizeof(support));
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
            &support, sizeof(support)) == OS_EOK);
        CHECK(support.Strategies == (mode == 1 ? 0 : INTERRUPT_STRATEGY_MSI));
        CHECK(support.MsiCapacity == (mode == 1 ? 0 : 32));
        CHECK(support.ControllerID == 91);
        CHECK(support.MsixCapacity == (mode == 0 ? 0 : (mode == 1 ? 2048 : 8)));
        CHECK(!device.Msi.Active && memcmp(original, g_config, sizeof(original)) == 0);
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
            &support, sizeof(uint32_t)) == OS_EINVALPARAMS);
        if (mode == 1) {
            CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
                &request, sizeof(request)) == OS_ENOTSUPPORTED);
            CHECK(!device.Msi.Active && memcmp(original, g_config, sizeof(original)) == 0);
        }
    }
    g_msiAvailable = 0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(!support.Strategies && !support.MsiCapacity && !support.MsixCapacity && !support.ControllerID);
    g_msiAvailable = 1;
    host.InterruptControllerId = 0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(!support.Strategies && !support.MsiCapacity && !support.MsixCapacity && !support.ControllerID);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_ENOTSUPPORTED);
    host.InterruptControllerId = 91;
    device.Handler = &g_blockedHandler;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(!support.Strategies && !support.MsiCapacity && !support.MsixCapacity && !support.ControllerID);
    CHECK(!g_locked);
    puts("PCI provider: hardware capacity remains separate from usable interrupt strategies passed");
}

static void
__TestMsixProvider(void)
{
    static const struct PciHostOperations operations = {
        .Read = __ReadConfig, .Write = __WriteConfig
    };
    PciHost_t host = { .Operations = &operations, .InterruptControllerId = 91 };
    PciDevice_t device = { .Host = &host };
    struct PciInterruptSupport support;
    struct PciInterruptRequest request = { .Strategy = INTERRUPT_STRATEGY_MSIX,
        .Operation = PciInterruptReserve, .Allocation = 2047, .Count = 2 };
    element_t child = { .value = &device };
    PciDevice_t root = { .Host = &host, .children.head = &child };
    struct PciInterruptRequest msi = { .Operation = PciInterruptReserve, .Allocation = 1023,
        .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 1 };
    struct OSIOCtlBusControl bus = {0};
    uint32_t original[1024];
    size_t value = 0;
    unsigned i;

    memset(g_config, 0, sizeof(g_config));
    memset(g_mmio, 0, sizeof(g_mmio));
    memcpy(original, g_mmio, sizeof(original));
    g_config[6] = 0x10;
    g_config[0x34] = 0x40;
    g_config[0x40] = 5;
    g_config[0x41] = 0x60;
    g_config[0x60] = 0x11;
    g_config[0x62] = 3;
    g_config[0x65] = 1;
    g_config[0x69] = 2;
    g_config[0x13] = 0x40;
    device.Resources.Bars[0] = (struct PciBar) {
        .BusAddress = 0x40000000, .CpuAddress = 0x80000000,
        .Size = 4096, .Space = 2, .State = PciBarAssigned
    };
    device.PublishedIo[0] = (DeviceIo_t) {
        .Id = 77, .Type = DeviceIoMemoryBased,
        .Access.Memory = { .PhysicalBase = 0x80000000, .Length = 4096 }
    };
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(support.Strategies == (INTERRUPT_STRATEGY_MSI | INTERRUPT_STRATEGY_MSIX));
    CHECK(!g_msixMapped && device.Msix == NULL);
    // A bad MSI-X extent must not hide a working single-message MSI capability.
    g_config[0x65] = 0xff;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY,
        &support, sizeof(support)) == OS_EOK);
    CHECK(support.Strategies == INTERRUPT_STRATEGY_MSI);
    g_config[0x65] = 1;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request) - 4) == OS_EINVALPARAMS);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &msi, sizeof(msi)) == OS_EOK);
    CHECK(__UnusedUnpublishDevice(&root) == OS_EBUSY);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EBUSY);
    device.Handler = &g_blockedHandler;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EBUSY);
    device.Handler = NULL;
    msi.Operation = PciInterruptRelease;
    msi.Count = 0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &msi, sizeof(msi)) == OS_EOK);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK(device.Msix != NULL && g_msixMapped);
    CHECK(__UnusedUnpublishDevice(&root) == OS_EBUSY);
    CHECK((g_config[0x63] & 0xc0) == 0xc0);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &msi, sizeof(msi)) == OS_EBUSY);
    CHECK(__PciProviderAccessRegister(&device, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        0x10, &value, 4) == OS_EBUSY);
    request.Count = 0;
    request.Operation = PciInterruptProgram;
    request.AddressLow = 0xfee00000;
    for (i = 0; i < 2; i++) {
        request.Index = i;
        request.Data = 0xe0 + i;
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
            &request, sizeof(request)) == OS_EOK);
    }
    request = (struct PciInterruptRequest) { .Strategy = INTERRUPT_STRATEGY_MSIX,
        .Operation = PciInterruptEnable, .Allocation = 2047 };
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK((g_config[0x63] & 0xc0) == 0x80);
    CHECK(g_mmio[0x10c / 4] == 0 && g_mmio[0x11c / 4] == 0);
    CHECK(g_mmio[0x12c / 4] == 1 && g_mmio[0x13c / 4] == 1);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_BUS_CONTROL, &bus, sizeof(bus)) == OS_EOK);
    CHECK(g_config[4] == 2 && g_config[5] == 4);
    g_msixUnmapFailure = 1;
    request.Operation = PciInterruptRelease;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EUNKNOWN);
    CHECK(device.Msix != NULL && g_msixMapped);
    CHECK(__UnusedUnpublishDevice(&root) == OS_EBUSY);
    CHECK(!g_unpublications && !g_ioDestructions && device.PublishedIo[0].Id == 77);
    // Accept a new desired INTx/MMIO state while cleanup is pending.
    bus.Flags = __DEVICEMANAGER_IOCTL_ENABLE | __DEVICEMANAGER_IOCTL_BUSMASTER_ENABLE;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_BUS_CONTROL, &bus, sizeof(bus)) == OS_EOK);
    CHECK(g_config[4] == 6 && g_config[5] == 4);
    device.Handler = &g_blockedHandler;
    bus.Flags = 0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_BUS_CONTROL, &bus, sizeof(bus)) == OS_ENOTSUPPORTED);
    g_msiAvailable = 0;
    g_msixUnmapFailure = 0;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    CHECK(device.Msix == NULL && !g_msixMapped && !g_locked);
    CHECK(g_config[4] == 4 && g_config[5] == 0 && g_config[0x63] == 0);
    CHECK(memcmp(original, g_mmio, sizeof(original)) == 0);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EOK);
    g_msiAvailable = 1;
    CHECK(__UnusedUnpublishDevice(&root) == OS_EOK);
    CHECK(host.InterruptControllerId == 91);
    CHECK(g_unpublications == 1 && g_ioDestructions == 1);
    puts("MSI-X provider: mapped table, mode exclusion, bus-control and policy-independent cleanup passed");
}

static void
__TestRequestContract(void)
{
    static const struct PciInterruptRequest invalid[] = {
        { .Strategy = 0, .Count = 1 },
        { .Strategy = INTERRUPT_STRATEGY_INTx, .Count = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI | INTERRUPT_STRATEGY_MSIX, .Count = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 0 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 2 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Count = 2049 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Count = 1, .Reserved = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 1, .Index = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Count = 1, .AddressLow = 0xfee00000 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Count = 1, .AddressHigh = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 1, .Data = 0x90 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Operation = PciInterruptProgram, .Index = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptProgram, .Index = 2048 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptProgram, .AddressLow = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Operation = PciInterruptProgram, .Count = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptEnable, .Data = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Operation = PciInterruptDisable, .Count = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptRelease, .Index = 1 },
        { .Strategy = INTERRUPT_STRATEGY_MSI, .Operation = PciInterruptRelease + 1 }
    };
    PciHost_t host = {0};
    PciDevice_t device = { .Host = &host };
    struct PciInterruptRequest request;
    size_t i;

    // No host accessors are installed: malformed envelopes must be rejected
    // before any configuration access, mapping, or ownership change.
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        request = invalid[i];
        request.Allocation = 1023;
        CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
            &request, sizeof(request)) == OS_EINVALPARAMS);
        CHECK(!device.Msi.Active && device.Msix == NULL && !g_locked);
    }
    request = (struct PciInterruptRequest) {
        .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 1, .Allocation = UUID_INVALID
    };
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, sizeof(request)) == OS_EINVALPARAMS);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        NULL, sizeof(request)) == OS_EINVALPARAMS);
    request.Allocation = 1023;
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, 24) == OS_EINVALPARAMS);
    CHECK(__PciProviderControl(&device, OSIOCTLREQUEST_PCI_INTERRUPT,
        &request, 32) == OS_EINVALPARAMS);
    CHECK(__PciProviderControl(&device, (enum OSIOCtlRequest)4,
        &request, sizeof(request)) == OS_ENOTSUPPORTED);
    puts("PCI interrupt contract: strategy, fields, counts, indices and legacy envelopes rejected passed");
}

int
main(void)
{
    int initial;
    int requests;
    int enabled;
    int retry;

    __TestRequestContract();
    __TestCapacityQuery();
    __TestMsixProvider();
    for (initial = 0; initial < 2; initial++) {
        for (requests = 0; requests < 3; requests++) {
            for (enabled = 0; enabled < 2; enabled++) {
                for (retry = 0; retry < 2; retry++) {
                    __TestBusControl(initial, requests, enabled, retry);
                }
            }
        }
    }
    puts("PCI MSI provider: 24 bus-control/Release cases passed");
    return 0;
}
