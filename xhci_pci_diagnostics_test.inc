// PCI diagnostic reads must preserve configuration and select the MSI layout.
static uint8_t g_pciConfig[4096];
static unsigned int g_pciReadCount;
static unsigned int g_pciReadOffsets[2048];
static size_t g_pciReadWidths[2048];
static unsigned int g_pciFailOffset;

oserr_t
IoctlDeviceEx(
    _In_    uuid_t       deviceId,
    _In_    int          direction,
    _In_    unsigned int offset,
    _InOut_ size_t*      value,
    _In_    size_t       width)
{
    CHECK(deviceId == 3);
    CHECK(direction == __DEVICEMANAGER_IOCTL_EXT_READ);
    CHECK(width == 1 || width == 2 || width == 4);
    CHECK(offset + width <= sizeof(g_pciConfig));
    CHECK((offset % width) == 0);
    CHECK(g_pciReadCount < 2048);
    g_pciReadOffsets[g_pciReadCount] = offset;
    g_pciReadWidths[g_pciReadCount++] = width;
    if (offset == g_pciFailOffset) {
        return OS_EUNKNOWN;
    }
    if (g_startupController != NULL && g_startupStage == 5) {
        if (g_startupInterruption == 1) {
            g_startupController->Base.State.Phase = USB_CONTROLLER_DESTROY_PENDING;
        } else if (g_startupInterruption == 2) {
            g_startupController->State = XHCI_CONTROLLER_HALTED;
        }
    }
    *value = 0;
    memcpy(value, &g_pciConfig[offset], width);
    return OS_EOK;
}

static void
__PreparePciDiagnostics(void)
{
    memset(g_pciConfig, 0, sizeof(g_pciConfig));
    g_pciReadCount = 0;
    g_pciFailOffset = UINT32_MAX;
    g_pciConfig[4] = 6;
    g_pciConfig[5] = 4;
    g_pciConfig[6] = 0x10;
    g_pciConfig[0x34] = 0x40;
    g_pciConfig[0x40] = 1;
    g_pciConfig[0x41] = 0x90;
    g_pciConfig[0x90] = 5;
    g_pciConfig[0x92] = 0x85;
    memset(&g_pciConfig[0x94], 0xff, 4);
    g_pciConfig[0x94] = 0xfc;
    g_pciConfig[0x9c] = 0x40;
    g_pciConfig[0x9d] = 0x65;
}

static void
__TestPciDiagnostics(void)
{
    XhciController_t controller = { 0 };
    Device_t device = { 0 };
    uint8_t original[4096];
    const unsigned int readOffsets[] = { 4, 0x34, 0x40, 0x90, 0x94, 0x98, 0x9c };
    unsigned int i;

    device.Id = 3;
    controller.Base.Device = &device;
    __PreparePciDiagnostics();
    __ReportPciInterrupt(&controller, "platform");
    CHECK(g_pciReadCount == 0);
    controller.Base.Interrupt.IsPci = 1;
    memcpy(original, g_pciConfig, sizeof(original));
    __ReportPciInterrupt(&controller, "after-reset");
    CHECK(g_pciReadCount == 7);
    CHECK(g_pciReadOffsets[4] == 0x94 && g_pciReadWidths[4] == 4);
    CHECK(g_pciReadOffsets[5] == 0x98 && g_pciReadWidths[5] == 4);
    CHECK(g_pciReadOffsets[6] == 0x9c && g_pciReadWidths[6] == 2);
    CHECK(memcmp(original, g_pciConfig, sizeof(original)) == 0);

    // Disabled, 32-bit MSI still reports its current message, with no high word.
    g_pciConfig[0x92] = 0;
    g_pciReadCount = 0;
    __ReportPciInterrupt(&controller, "after-start");
    CHECK(g_pciReadCount == 6);
    CHECK(g_pciReadOffsets[5] == 0x98 && g_pciReadWidths[5] == 2);

    // Stop at every failed read without reporting a fabricated message.
    for (i = 0; i < 7; i++) {
        __PreparePciDiagnostics();
        g_pciFailOffset = readOffsets[i];
        __ReportPciInterrupt(&controller, "read-failure");
        CHECK(g_pciReadCount == i + 1);
    }

    __PreparePciDiagnostics();
    g_pciConfig[6] = 0;
    __ReportPciInterrupt(&controller, "no-list");
    CHECK(g_pciReadCount == 1);
    __PreparePciDiagnostics();
    g_pciConfig[0x41] = 0;
    __ReportPciInterrupt(&controller, "no-msi");
    CHECK(g_pciReadCount == 3);
    __PreparePciDiagnostics();
    g_pciConfig[0x41] = 0x40;
    __ReportPciInterrupt(&controller, "cycle");
    CHECK(g_pciReadCount == 50);
    __PreparePciDiagnostics();
    g_pciConfig[0x34] = 0x41;
    __ReportPciInterrupt(&controller, "unaligned");
    CHECK(g_pciReadCount == 2);
    __PreparePciDiagnostics();
    g_pciConfig[0x34] = 0xfc;
    g_pciConfig[0xfc] = 5;
    __ReportPciInterrupt(&controller, "truncated");
    CHECK(g_pciReadCount == 3);
    puts("xHCI PCI diagnostics: read-only MSI layouts, failures and bounded capabilities passed");
}

static void
__SetPciDword(
    _In_ unsigned int offset,
    _In_ uint32_t     value)
{
    memcpy(&g_pciConfig[offset], &value, sizeof(value));
}

static void
__PreparePcieErrors(void)
{
    unsigned int i;

    __PreparePciDiagnostics();
    g_pciConfig[0x91] = 0xa0;
    __SetPciDword(0xa0, 0x00020010);
    __SetPciDword(0xa8, 0x000f000e);
    __SetPciDword(0xb0, 0x20410040);
    __SetPciDword(0x100, 0x18010003);
    __SetPciDword(0x180, 0x00010001);
    for (i = 0; i < 13; i++) {
        __SetPciDword(0x184 + i * 4, 0x12340000 + i);
    }
}

static void
__TestPcieErrors(void)
{
    XhciController_t controller = { 0 };
    Device_t device = { .Id = 3 };
    struct PciErrorSnapshot snapshot;
    uint8_t original[4096];
    unsigned int offsets[32];
    unsigned int count;
    unsigned int i;

    controller.Base.Device = &device;
    controller.Base.Interrupt.IsPci = 1;
    __PreparePcieErrors();
    memcpy(original, g_pciConfig, sizeof(original));
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EOK);
    CHECK(snapshot.ExpressOffset == 0xa0 && snapshot.AerOffset == 0x180);
    CHECK(snapshot.DeviceControlStatus == 0x000f000e && snapshot.LinkControlStatus == 0x20410040);
    for (i = 0; i < 10; i++) {
        CHECK(snapshot.Aer[i] == 0x12340000 + i);
    }
    CHECK(snapshot.RootValid == 0);
    CHECK(memcmp(original, g_pciConfig, sizeof(original)) == 0);
    count = g_pciReadCount;
    CHECK(count < 32);
    memcpy(offsets, g_pciReadOffsets, count * sizeof(offsets[0]));
    for (i = 0; i < count; i++) {
        __PreparePcieErrors();
        g_pciFailOffset = offsets[i];
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EUNKNOWN);
        CHECK(g_pciReadCount == i + 1);
        CHECK(snapshot.AerOffset == 0);
    }

    __PreparePcieErrors();
    __SetPciDword(0xa0, 0x00420010);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EOK);
    CHECK(snapshot.RootValid == 1 && snapshot.RootCommand == 0x1234000a);
    CHECK(snapshot.RootStatus == 0x1234000b && snapshot.RootSource == 0x1234000c);

    // No AER and inaccessible extended configuration still preserve PCIe status.
    for (i = 0; i < 3; i++) {
        __PreparePcieErrors();
        __SetPciDword(0x100, i == 0 ? 0 : UINT32_MAX);
        if (i == 2) {
            g_pciFailOffset = 0x100;
        }
        memset(&snapshot, 0, sizeof(snapshot));
        CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) ==
            (i == 2 ? OS_EUNKNOWN : OS_EOK));
        CHECK(snapshot.ExpressOffset == 0xa0 && snapshot.AerOffset == 0);
    }

    // A cycle and malformed offsets must terminate without an out-of-range read.
    __PreparePcieErrors();
    __SetPciDword(0x100, 0x10010003);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EINVALPARAMS);
    CHECK(g_pciReadCount == 967);
    __PreparePcieErrors();
    __SetPciDword(0x100, 0x10210003);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EINVALPARAMS);
    __PreparePcieErrors();
    __SetPciDword(0x100, 0xffc10003);
    __SetPciDword(0xffc, 0x00010001);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EINVALPARAMS);
    CHECK(g_pciReadCount == 9 && snapshot.AerOffset == 0);
    __PreparePcieErrors();
    g_pciConfig[0x91] = 0xfc;
    __SetPciDword(0xfc, 0x00020010);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(__PciReadErrorSnapshot(&controller, __ReadPciErrorRegister, &snapshot) == OS_EINVALPARAMS);
    CHECK(snapshot.ExpressOffset == 0);

    __PreparePcieErrors();
    memcpy(original, g_pciConfig, sizeof(original));
    XhciDiagnosePciInterrupt(&controller, "pending-enable-slot");
    CHECK(memcmp(original, g_pciConfig, sizeof(original)) == 0);
    CHECK(g_pciReadCount == 26);
    puts("PCIe diagnostics: endpoint/root AER, read failures, absent/truncated/cyclic capabilities passed");
}
