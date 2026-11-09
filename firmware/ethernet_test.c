#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <threads.h>
static int __TestSleep(const struct timespec*, struct timespec*);
#define thrd_sleep __TestSleep
#include "../../services/deviced/firmware/fdt.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../services/deviced/firmware/mapping.c"
#include "../../services/deviced/firmware/ethernet.c"
#include "../../services/deviced/firmware/platform-runtime.c"
#include "../../services/deviced/core/io.c"
#include "../../services/deviced/bus/rp1/ethernet.c"
#include "../../services/deviced/bus/rp1/resources.c"
#include "../../librt/libddk/resources.c"

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t g_registers[3][0x10038 / 4];
static uint32_t g_original[3][0x10038 / 4];
static unsigned int g_ios;
static unsigned int g_writes;
static unsigned int g_failWrite;
static unsigned int g_failRelease;
static uint64_t g_delayUs;
static struct Rp1Child* g_clientChild;
static int g_badVersion;
oserr_t
OSDeviceIOCtl(
    _In_ uuid_t              device,
    _In_ enum OSIOCtlRequest request,
    _In_ void*               buffer,
    _In_ size_t              length)
{
    oserr_t status;
    CHECK(device == 42 && g_clientChild != NULL);
    status = Rp1ChildResourceControl(g_clientChild, 1, request, buffer, length);
    if (status == OS_EOK && request == OSIOCTLREQUEST_ETHERNET_CONFIG && g_badVersion) {
        ((struct DeviceEthernetConfiguration*)buffer)->Version = 99;
    }
    return status;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}
oserr_t
CreateDeviceMemoryIo(
    _Out_ DeviceIo_t* io,
    _In_  uintptr_t   base,
    _In_  size_t      length)
{
    CHECK(base >= 1 && base <= 3);
    *io = (DeviceIo_t) { .Id = base, .Type = DeviceIoMemoryBased };
    io->Access.Memory.Length = length;
    g_ios++;
    return OS_EOK;
}
oserr_t
AcquireDeviceIo(
    _InOut_ DeviceIo_t* io)
{
    io->Access.Memory.VirtualBase = 1;
    return OS_EOK;
}
oserr_t
ReleaseDeviceIo(
    _InOut_ DeviceIo_t* io)
{
    if (g_failRelease) {
        g_failRelease--;
        return OS_EBUSY;
    }
    io->Access.Memory.VirtualBase = 0;
    return OS_EOK;
}
oserr_t
DestroyDeviceIo(
    _InOut_ DeviceIo_t* io)
{
    (void)io;
    g_ios--;
    return OS_EOK;
}
size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    CHECK(width == 4 && offset < io->Access.Memory.Length);
    return g_registers[io->Id - 1][offset / 4];
}
oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      width)
{
    CHECK(width == 4 && offset < io->Access.Memory.Length);
    g_registers[io->Id - 1][offset / 4] = (uint32_t)value;
    return ++g_writes == g_failWrite ? OS_EUNKNOWN : OS_EOK;
}
static int
__TestSleep(
    _In_  const struct timespec* interval,
    _Out_ struct timespec*       remaining)
{
    (void)remaining;
    g_delayUs += (uint64_t)interval->tv_sec * 1000000 + interval->tv_nsec / 1000;
    return 0;
}

/** @brief Inventory nodes used to exercise the bundled Ethernet descriptions. */
struct __EthernetNodes {
    const struct FdtResources* Mac;
    const struct FdtResources* Clock;
    const struct FdtResources* Gpio;
    struct FdtTree* Tree;
};
static void
__Find(
    _In_    const struct FdtResources* nodes,
    _In_    int                        depth,
    _InOut_ void*                      context)
{
    struct __EthernetNodes*    found = context;
    const struct FdtResources* node = FdtTreeNode(found->Tree, nodes[depth].NodeOffset);
    if (FdtCompatible(&node->View, "brcm,bcm2711-genet-v5") ||
        FdtCompatible(&node->View, "raspberrypi,rp1-gem")) {
        found->Mac = node;
    }
    if (FdtCompatible(&node->View, "raspberrypi,rp1-clocks")) {
        found->Clock = node;
    }
    if (FdtCompatible(&node->View, "raspberrypi,rp1-gpio")) {
        found->Gpio = node;
    }
}

static void
__Decode(
    _In_ struct __EthernetNodes* nodes,
    _In_ int                     rp1)
{
    struct DeviceEthernetConfiguration config;
    struct DeviceEthernetConfiguration before;
    struct FdtPlatformDependencies dependencies;
    struct FdtResources* phy;
    struct FdtResources saved;
    struct FdtProperty properties[32];
    uint8_t badReg[4] = { 0, 0, 0, 32 };
    size_t i;

    CHECK(FdtEthernetConfiguration(nodes->Tree, nodes->Mac->NodeOffset, &config) == OS_EOK);
    CHECK(config.Version == 1 && config.PhyAddress == 1 && config.MacPresent == 0);
    CHECK(config.Mode == (rp1 ? DeviceEthernetRgmiiId : DeviceEthernetRgmiiRxId));
    CHECK(config.MdioOffset == (rp1 ? 0 : 0xe14));
    CHECK(config.MdioLength == (rp1 ? 0 : 8));
    CHECK(config.Reset.GpioPresent == (uint32_t)rp1);
    if (rp1) {
        CHECK(config.MdioNode == nodes->Mac->NodeOffset);
        CHECK(config.Reset.Controller == nodes->Gpio->NodeOffset);
        CHECK(config.Reset.Pin == 32 && config.Reset.Flags == 1 && config.Reset.AssertUs == 5000);
    }
    CHECK(FdtPlatformLocalDependencies(nodes->Tree, nodes->Mac->NodeOffset, &dependencies) == OS_EOK);
    CHECK(dependencies.Count == (rp1 ? 4U : 0U));
    phy = (void*)FdtTreeNode(nodes->Tree, config.PhyNode);
    saved = *phy;
    before = config;
    phy->Disabled = 1;
    CHECK(FdtEthernetConfiguration(nodes->Tree, nodes->Mac->NodeOffset, &config) == OS_ENOENT);
    CHECK(!memcmp(&config, &before, sizeof(config)));
    *phy = saved;
    memcpy(properties, phy->View.Properties, phy->View.PropertyCount * sizeof(*properties));
    for (i = 0; i < phy->View.PropertyCount; i++) {
        if (!strcmp(properties[i].Name, "reg")) {
            properties[i].Value = badReg;
        }
    }
    phy->Reg = badReg;
    phy->View.Properties = properties;
    CHECK(FdtEthernetConfiguration(nodes->Tree, nodes->Mac->NodeOffset, &config) == OS_EINVALPARAMS);
    CHECK(!memcmp(&config, &before, sizeof(config)));
    *phy = saved;
}

static void
__SetProperty(
    _InOut_ struct FdtResources* node,
    _InOut_ struct FdtProperty*  properties,
    _In_    const char*          name,
    _In_    const void*          value,
    _In_    uint32_t             length)
{
    size_t i;

    for (i = 0; i < node->View.PropertyCount; i++) {
        if (!strcmp(properties[i].Name, name)) {
            break;
        }
    }
    CHECK(i < 32);
    properties[i] = (struct FdtProperty) { name, value, length };
    if (i == node->View.PropertyCount) {
        node->View.PropertyCount++;
    }
    node->View.Properties = properties;
}

static void
__ConfigurationVariants(
    _In_ struct __EthernetNodes* nodes)
{
    static const char* const modes[] = { "rgmii", "rgmii-id", "rgmii-rxid", "rgmii-txid" };
    struct DeviceEthernetConfiguration config;
    struct DeviceEthernetConfiguration before;
    struct FdtPlatformDependencies     dependencies;
    struct FdtResources*               mac = (void*)nodes->Mac;
    struct FdtResources                savedMac = *mac;
    struct FdtResources*               phy;
    struct FdtResources                savedPhy;
    struct FdtProperty                 macProperties[32];
    struct FdtProperty                 phyProperties[32];
    const uint8_t macAddress[] = { 2, 1, 2, 3, 4, 5 };
    const uint8_t badAddress[] = { 1, 0, 0, 0, 0, 0 };
    const uint8_t zeroAddress[6] = { 0 };
    const uint8_t speed1000[] = { 0, 0, 3, 232 };
    const uint8_t speed100[] = { 0, 0, 0, 100 };
    const uint8_t invalidReference[] = { 255, 255, 255, 254 };
    const uint8_t delay[] = { 0, 0, 0, 7 };
    const uint8_t malformedMode[] = { 'r', 'g', 'm', 'i', 'i' };
    uint8_t resetLine[12];
    uint32_t length;
    unsigned int i;

    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EOK);
    phy = (void*)FdtTreeNode(nodes->Tree, config.PhyNode);
    savedPhy = *phy;
    memcpy(macProperties, mac->View.Properties, mac->View.PropertyCount * sizeof(*macProperties));
    memcpy(phyProperties, phy->View.Properties, phy->View.PropertyCount * sizeof(*phyProperties));
    for (i = 0; i < 4; i++) {
        __SetProperty(mac, macProperties, "phy-mode", modes[i], (uint32_t)strlen(modes[i]) + 1);
        CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EOK);
        CHECK(config.Mode == DeviceEthernetRgmii + i);
    }
    before = config;
    __SetProperty(mac, macProperties, "phy-mode", malformedMode, sizeof(malformedMode));
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EINVALPARAMS);
    CHECK(!memcmp(&config, &before, sizeof(config)));
    __SetProperty(mac, macProperties, "phy-mode", "sgmii", 6);
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_ENOTSUPPORTED);
    __SetProperty(mac, macProperties, "phy-mode", "rgmii-rxid", 11);
    __SetProperty(mac, macProperties, "local-mac-address", macAddress, sizeof(macAddress));
    __SetProperty(mac, macProperties, "mac-address", badAddress, sizeof(badAddress));
    __SetProperty(mac, macProperties, "max-speed", speed1000, sizeof(speed1000));
    __SetProperty(phy, phyProperties, "max-speed", speed100, sizeof(speed100));
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EOK);
    CHECK(config.MaxSpeed == 100 && config.MacPresent && !memcmp(config.Mac, macAddress, 6));
    __SetProperty(mac, macProperties, "mac-address", macAddress, sizeof(macAddress));
    __SetProperty(mac, macProperties, "local-mac-address", zeroAddress, sizeof(zeroAddress));
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EOK);
    CHECK(config.MacPresent && !memcmp(config.Mac, macAddress, 6));
    before = config;
    __SetProperty(mac, macProperties, "mac-address", macAddress, 5);
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EINVALPARAMS);
    CHECK(!memcmp(&config, &before, sizeof(config)));
    __SetProperty(mac, macProperties, "mac-address", macAddress, sizeof(macAddress));
    __SetProperty(mac, macProperties, "phy-handle", invalidReference, 4);
    CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) != OS_EOK);
    *mac = savedMac;
    *phy = savedPhy;

    // Generic requirements on a PHY cannot disappear from platform preparation.
    memcpy(phyProperties, phy->View.Properties, phy->View.PropertyCount * sizeof(*phyProperties));
    __SetProperty(phy, phyProperties, "power-domains", invalidReference, 4);
    CHECK(FdtPlatformLocalDependencies(nodes->Tree, mac->NodeOffset, &dependencies) != OS_EOK);
    *phy = savedPhy;
    if (nodes->Gpio != NULL) {
        memcpy(macProperties, mac->View.Properties, mac->View.PropertyCount * sizeof(*macProperties));
        memcpy(phyProperties, phy->View.Properties, phy->View.PropertyCount * sizeof(*phyProperties));
        memcpy(resetLine, FdtProperty(&mac->View, "phy-reset-gpios", &length), 12);
        for (i = 0; i < mac->View.PropertyCount; i++) {
            if (!strcmp(macProperties[i].Name, "phy-reset-gpios")) {
                macProperties[i].Name = "test-removed-gpio";
            }
        }
        mac->View.Properties = macProperties;
        __SetProperty(phy, phyProperties, "reset-gpios", resetLine, 12);
        __SetProperty(phy, phyProperties, "reset-assert-us", delay, 4);
        __SetProperty(phy, phyProperties, "reset-deassert-us", delay, 4);
        CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_EOK);
        CHECK(config.Reset.AssertUs == 7 && config.Reset.DeassertUs == 7);
        __SetProperty(mac, macProperties, "phy-reset-gpios", resetLine, 12);
        CHECK(FdtEthernetConfiguration(nodes->Tree, mac->NodeOffset, &config) == OS_ENOTSUPPORTED);
        *mac = savedMac;
        *phy = savedPhy;
    }
}

static void
__PrepareRp1(
    _In_ struct __EthernetNodes* nodes)
{
    struct FdtPciHost host = { .Tree = nodes->Tree };
    PciNativeHeader_t header = { .Revision = RP1_REVISION_C0 };
    struct PciDevice parent = { .Header = &header };
    struct Rp1Bus bus = { .Host = &host, .Parent = &parent, .Ready = 1 };
    struct Rp1Child mac = { .Bus = &bus };
    struct Rp1Child clock = { .Bus = &bus };
    struct Rp1Child gpio = { .Bus = &bus };
    enum DeviceActivation stage;
    struct DeviceEthernetConfiguration config;
    struct Rp1Runtime runtime = { .Bus = &bus };
    uint32_t reserved = 0;
    unsigned int failure;

    bus.Children = &mac;
    mac.Next = &clock;
    clock.Next = &gpio;
    mac.Firmware.NodeOffset = nodes->Mac->NodeOffset;
    mac.Firmware.Compatible = (const char*)nodes->Mac->Compatible;
    mac.Firmware.CompatibleLength = nodes->Mac->CompatibleLength;
    clock.Firmware.NodeOffset = nodes->Clock->NodeOffset;
    gpio.Firmware.NodeOffset = nodes->Gpio->NodeOffset;
    clock.Firmware.RegisterCount = 1;
    clock.Firmware.Registers[0] = (struct FdtRp1Range) { .Base = 1, .Length = sizeof(g_registers[0]) };
    gpio.Firmware.RegisterCount = 3;
    gpio.Firmware.Registers[0] = (struct FdtRp1Range) { .Base = 2, .Length = sizeof(g_registers[1]) };
    gpio.Firmware.Registers[2] = (struct FdtRp1Range) { .Base = 3, .Length = sizeof(g_registers[2]) };
    g_registers[0][0x8000 / 4] = 0x80000001;
    g_registers[0][0x8008 / 4] = 20;
    g_registers[0][0x8010 / 4] = 0x51000;
    g_registers[0][0x8014 / 4] = 0x800;
    g_registers[0][0x14 / 4] = 2;
    g_registers[0][0x18 / 4] = 1;
    memcpy(g_original, g_registers, sizeof(g_original));

    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_ETHERNET_CONFIG, &config, sizeof(config)) == OS_EOK);
    CHECK(config.Mode == DeviceEthernetRgmiiId);
    g_clientChild = &mac;
    CHECK(DeviceGetEthernetConfiguration(42, &config) == OS_EOK);
    CHECK(config.Mode == DeviceEthernetRgmiiId && config.Reset.Pin == 32);
    g_badVersion = 1;
    CHECK(DeviceGetEthernetConfiguration(42, &config) == OS_EPROTOCOL);
    CHECK(config.Version == 1);
    g_badVersion = 0;
    stage = DevicePrepare;
    CHECK(Rp1ChildResourceControl(&mac, 0, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EPERMISSIONS);
    for (failure = 1; failure <= 4; failure++) {
        g_failWrite = failure;
        g_writes = 0;
        CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EUNKNOWN);
        CHECK(mac.Ethernet == NULL && !mac.Resources.Prepared && !g_ios);
        CHECK(!memcmp(g_original, g_registers, sizeof(g_original)));
    }
    g_failWrite = 0;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
    CHECK(mac.Resources.Prepared && g_ios == 3);
    CHECK(g_registers[0][0x68 / 4] == 1 && (g_registers[0][0x64 / 4] & RP1_CLOCK_ENABLE));
    CHECK(g_registers[0][0x138 / 4] == 1 && (g_registers[0][0x134 / 4] & RP1_CLOCK_ENABLE));
    CHECK(Rp1ChildResourceControl(&mac, 2, OSIOCTLREQUEST_ETHERNET_PHY_RESET, &reserved, 4) == OS_EPERMISSIONS);
    CHECK(DeviceResetEthernetPhy(42) == OS_EOK);
    CHECK(g_delayUs == 5000 && (g_registers[1][0x4024 / 4] & 0xf000) == 0xf000);
    CHECK(Rp1EthernetStatus(&mac, 2) == OS_EPERMISSIONS);
    CHECK(Rp1EthernetStatus(&mac, 1) == OS_EOK);
    stage = DeviceStop;
    mac.Resources.DmaReferences = 1;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
    CHECK(mac.Resources.Prepared && mac.Ethernet->Ready);
    mac.Resources.DmaReferences = 0;
    bus.Runtime = &runtime;
    runtime.Sources[6].Owner = &mac;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
    CHECK(mac.Resources.Prepared && mac.Ethernet->Ready);
    runtime.Sources[6].Owner = NULL;
    runtime.Quiescing = 1;
    CHECK(Rp1EthernetStatus(&mac, 1) == OS_EBUSY);
    stage = DevicePrepare;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
    stage = DeviceStop;
    runtime.Quiescing = 0;
    bus.Runtime = NULL;
    g_failRelease = 1;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
    CHECK(mac.Ethernet != NULL);
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
    CHECK(!g_ios && !mac.Ethernet && !mac.Resources.Prepared);
    CHECK(!memcmp(g_original, g_registers, sizeof(g_original)));

    for (failure = 1; failure <= 3; failure++) {
        stage = DevicePrepare;
        CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        g_writes = 0;
        g_failWrite = failure;
        CHECK(DeviceResetEthernetPhy(42) == OS_EUNKNOWN);
        CHECK(!mac.Resources.Prepared && !mac.Ethernet->Ready);
        stage = DeviceStart;
        CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EBUSY);
        stage = DeviceStop;
        CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_EOK);
        CHECK(!g_ios && !mac.Ethernet);
        CHECK(!memcmp(g_original, g_registers, sizeof(g_original)));
        g_failWrite = 0;
    }
    g_registers[0][0x8010 / 4] = 0x21000;
    stage = DevicePrepare;
    CHECK(Rp1ChildResourceControl(&mac, 1, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage)) == OS_ENOTSUPPORTED);
    CHECK(!g_ios && !mac.Ethernet);
    memcpy(g_registers, g_original, sizeof(g_original));
}

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    FILE* file;
    long length;
    void* blob;
    struct __EthernetNodes nodes = { 0 };
    CHECK(argc == 3);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    blob = malloc((size_t)length);
    CHECK(blob != NULL && fread(blob, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    CHECK(FdtTreeCreate(blob, (size_t)length, &nodes.Tree) == OS_EOK);
    CHECK(FdtWalkResources(nodes.Tree, __Find, &nodes) == OS_EOK && nodes.Mac != NULL);
    __Decode(&nodes, atoi(argv[2]));
    __ConfigurationVariants(&nodes);
    if (atoi(argv[2])) {
        __PrepareRp1(&nodes);
    }
    FdtTreeRelease(nodes.Tree);
    free(blob);
    printf("Ethernet firmware and preparation: %s passed\n", argv[1]);
    return 0;
}
