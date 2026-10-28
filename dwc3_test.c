// Exercise the real integration against fault-injected MMIO.
#include "../modules/serial/usb/xhci/dwc3.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t g_registers[0x10000 / 4];
static unsigned int g_reads;
static unsigned int g_writes;
static unsigned int g_failWrite;
static int g_badReadback;

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    CHECK(io->Access.Memory.VirtualBase == (uintptr_t)g_registers);
    CHECK(width == 4 && offset + width <= io->Access.Memory.Length);
    g_reads++;
    return g_registers[offset / 4];
}

oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      width)
{
    (void)ReadDeviceIo(io, offset, width);
    g_writes++;
    if (g_writes == g_failWrite) {
        // A reported write failure need not mean hardware was left untouched.
        g_registers[offset / 4] = (uint32_t)value ^ (g_badReadback ? UINT32_MAX : 0);
        return g_badReadback ? OS_EOK : OS_EUNKNOWN;
    }
    g_registers[offset / 4] = (uint32_t)value;
    return OS_EOK;
}

static void
__Initialize(
    _Out_ struct DeviceResources* resources)
{
    memset(resources, 0, sizeof(*resources));
    resources->Count = 1;
    resources->ParentPci = (struct DeviceParentPciIdentity) { 0x1de4, 1, 2 };
    resources->Registers[0].Type = DeviceIoMemoryBased;
    resources->Registers[0].Access.Memory.VirtualBase = (uintptr_t)g_registers;
    resources->Registers[0].Access.Memory.Length = sizeof(g_registers);
    memset(g_registers, 0, sizeof(g_registers));
    g_registers[0xc120 / 4] = 0x55330000;
    g_registers[0xc140 / 4] = 1;
    g_reads = g_writes = g_failWrite = 0;
    g_badReadback = 0;
}

static void
__TestIdentity(void)
{
    PlatformDevice_t device = { 0 };
    PlatformDevice_t original;
    Device_t pci = { .Length = sizeof(Device_t) };
    const char compatibles[] = "test,specific-usb\0snps,dwc3";

    device.Base.Length = sizeof(device);
    device.Version = PLATFORM_DEVICE_VERSION;
    device.CompatibleLength = sizeof(compatibles);
    memcpy(device.Compatibles, compatibles, sizeof(compatibles));
    original = device;
    CHECK(Dwc3IsDevice(&device.Base));
    CHECK(!memcmp(&device, &original, sizeof(device)));
    device.CompatibleLength--;
    CHECK(!Dwc3IsDevice(&device.Base));
    memcpy(device.Compatibles, "generic-xhci", sizeof("generic-xhci"));
    device.CompatibleLength = sizeof("generic-xhci");
    CHECK(!Dwc3IsDevice(&device.Base));
    CHECK(!Dwc3IsDevice(&pci));
}

static void
__TestUnsupported(void)
{
    struct DeviceResources resources;
    struct Dwc3Integration integration = { 0 };
    size_t length;
    unsigned int variant;

    for (variant = 0; variant < 8; variant++) {
        __Initialize(&resources);
        length = 123;
        switch (variant) {
            case 0:
                resources.ParentPci.VendorId = 0;
                break;
            case 1:
                resources.ParentPci.ProductId = 2;
                break;
            case 2:
                resources.ParentPci.Revision = 3;
                break;
            case 3:
                resources.Count = 2;
                break;
            case 4:
                resources.Registers[0].Access.Memory.Length = 0xcfff;
                break;
            case 5:
                resources.Registers[0].Type = DeviceIoPortBased;
                break;
            case 6:
                g_registers[0xc120 / 4] = 0x12340000;
                break;
            case 7:
                g_registers[0xc140 / 4] = 2;
                break;
        }
        CHECK(Dwc3Prepare(&integration, &resources, &resources.Registers[0], &length) == OS_ENOTSUPPORTED);
        CHECK(length == 123 && g_writes == 0 && integration.Registers == NULL);
        CHECK(variant >= 6 || g_reads == 0);
        CHECK(Dwc3Release(&integration) == OS_EOK);
    }
}

static void
__TestHostAndRelease(void)
{
    struct DeviceResources resources;
    struct Dwc3Integration integration = { 0 };
    size_t length = 0;
    unsigned int writes;

    __Initialize(&resources);
    // An unrelated bit must survive both setup and restoration.
    g_registers[0xc104 / 4] = 1U << 31;
    CHECK(Dwc3Prepare(&integration, &resources, &resources.Registers[0], &length) == OS_EOK);
    CHECK(length == 0x8000 && resources.Registers[0].Access.Memory.Length == sizeof(g_registers));
    CHECK(g_registers[0xc110 / 4] == 1U << 12);
    CHECK(g_registers[0xc200 / 4] == 0);
    CHECK(g_registers[0xc2c0 / 4] == 1U << 28);
    CHECK(g_registers[0xc104 / 4] == ((1U << 31) | (7U << 8)));
    CHECK(g_registers[0xc11c / 4] == ((1U << 19) | (7U << 15)));
    CHECK(g_registers[0xc108 / 4] == ((8U << 16) | (2U << 24) | (1U << 29)));
    CHECK(Dwc3Prepare(&integration, &resources, &resources.Registers[0], &length) == OS_EBUSY);
    CHECK(Dwc3Release(&integration) == OS_EOK);
    CHECK(g_registers[0xc110 / 4] == 0 && g_registers[0xc2c0 / 4] == 0);
    CHECK(g_registers[0xc104 / 4] == 1U << 31);
    CHECK(g_registers[0xc11c / 4] == 0 && g_registers[0xc108 / 4] == 0);
    writes = g_writes;
    CHECK(Dwc3Release(&integration) == OS_EOK && g_writes == writes);
}

static void
__TestPartialPreparation(void)
{
    struct DeviceResources resources;
    struct Dwc3Integration integration;
    uint32_t original[sizeof(g_registers) / sizeof(g_registers[0])];
    size_t length;
    unsigned int failure;
    unsigned int readback;

    for (readback = 0; readback < 2; readback++) {
        for (failure = 1; failure <= 6; failure++) {
            __Initialize(&resources);
            memset(&integration, 0, sizeof(integration));
            // Include nonzero original settings to exercise restoration.
            g_registers[0xc200 / 4] = 1U << 6;
            g_registers[0xc2c0 / 4] = 1U << 17;
            memcpy(original, g_registers, sizeof(original));
            length = 123;
            g_failWrite = failure;
            g_badReadback = readback;
            CHECK(Dwc3Prepare(&integration, &resources, &resources.Registers[0], &length) == OS_EUNKNOWN);
            CHECK(length == 123 && integration.Changed == failure);
            CHECK(integration.Registers == &resources.Registers[0]);
            // Restore only owned bits if the simulated hardware changed others.
            if (readback) {
                g_registers[(0xc000 + g_rp1C0Settings[failure - 1].Offset) / 4] ^=
                    ~g_rp1C0Settings[failure - 1].Mask;
            }
            g_badReadback = 0;
            g_failWrite = g_writes + 1;
            CHECK(Dwc3Release(&integration) == OS_EUNKNOWN);
            CHECK(integration.Changed == failure && integration.Registers != NULL);
            g_failWrite = 0;
            CHECK(Dwc3Release(&integration) == OS_EOK);
            CHECK(integration.Registers == NULL && integration.Changed == 0);
            CHECK(!memcmp(original, g_registers, sizeof(original)));
        }
    }
}

int
main(void)
{
    __TestIdentity();
    __TestUnsupported();
    __TestHostAndRelease();
    __TestPartialPreparation();
    puts("DWC3: identity, integration scope, host setup, partial writes and cleanup retries passed");
    return 0;
}
