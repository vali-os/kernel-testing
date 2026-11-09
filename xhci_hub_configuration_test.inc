static XhciController_t* g_hubController;
static XhciDevice_t* g_hubDevice;
static unsigned int g_hubSleeps;
static unsigned int g_hubCompletion;
static bool g_hubTimeout;
static bool g_hubRemoval;

static void
__TestHubCommandComplete(void)
{
    XhciTrb_t event = {
        .Parameter = g_hubController->CommandRing.DeviceBase,
        .Status = g_hubCompletion << 24
    };

    if (g_hubCompletion == XHCI_TRB_COMPLETION_SUCCESS) {
        memcpy(g_hubDevice->DeviceContext,
            g_hubDevice->InputContext + g_hubController->ContextSize,
            g_hubController->ContextSize);
        ((reg32_t*)g_hubDevice->DeviceContext)[3] = (3U << 27) | 5;
    }
    XhciCommandHandleCompletion(g_hubController, &event);
}

static int
__TestHubSleep(
    _In_ const struct timespec* duration,
    _Out_ struct timespec* remaining)
{
    XhciEndpoint_t endpoint = { 0 };
    UsbManagerTransfer_t transfer = { 0 };
    uint8_t original[33 * 64];

    CHECK(duration->tv_nsec == 10 * NSEC_PER_MSEC);
    CHECK(g_hubDevice->HubConfiguration.Pending);
    CHECK(g_hubDevice->HubConfiguration.Waiting);
    CHECK(g_hubController->Base.State.OperationsInFlight == 1);
    CHECK(__DestroyDevice(g_hubController, g_hubDevice) == OS_EBUSY);
    CHECK(HCIConfigureHub(&g_hubController->Base, 5, 4, 0) == OS_EBUSY);
    memcpy(original, g_hubDevice->InputContext, sizeof(original));
    CHECK(XhciDeviceConfigureEndpoint(g_hubController, g_hubDevice, &endpoint, &transfer) == OS_EINCOMPLETE);
    CHECK(!memcmp(original, g_hubDevice->InputContext, sizeof(original)));
    g_hubSleeps++;
    if (g_hubRemoval) {
        g_hubController->Base.State.Phase = USB_CONTROLLER_DESTROY_PENDING;
    } else if (!g_hubTimeout && g_hubSleeps == 2) {
        __TestHubCommandComplete();
    }
    return 0;
}

static void
__TestHubConfigurationCase(
    _In_ unsigned int contextSize,
    _In_ unsigned int version,
    _In_ unsigned int completion,
    _In_ bool         timeout,
    _In_ bool         removal,
    _In_ bool         fullSpeed)
{
    XhciController_t controller = { 0 };
    XhciCapabilityRegisters_t capability = { .HciVersion = version };
    XhciDevice_t device = { .SlotId = 1, .UsbAddress = 5, .State = XHCI_DEVICE_ADDRESSED,
        .Speed = fullSpeed ? USBSPEED_FULL : USBSPEED_HIGH, .ContextEntries = 3 };
    uint32_t input[33 * 16] = { 0 };
    uint32_t output[32 * 16] = { 0 };
    uint32_t before[32 * 16];
    uint32_t* slot = (uint32_t*)((uint8_t*)input + contextSize);
    reg32_t doorbell = 0xFFFF;
    oserr_t result;
    XhciEndpoint_t endpoint = { .DeviceContextIndex = 5 };
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN, .MaxPacketSize = 1, .Speed = device.Speed };

    controller.CapRegisters = &capability;
    controller.ContextSize = contextSize;
    controller.Doorbells = &doorbell;
    controller.Base.State.Phase = USB_CONTROLLER_READY;
    device.DeviceContext = (uint8_t*)output;
    device.InputContext = (uint8_t*)input;
    device.InputContextDeviceAddress = 0x480004000ULL;
    device.DeviceContextDeviceAddress = 0x480006000ULL;
    output[0] = (3U << 27) | ((fullSpeed ? 1U : 3U) << 20) | XHCI_SLOT_CONTEXT_MTT | 0x321;
    output[1] = (9U << 24) | (2U << 16) | 0x1234;
    output[2] = (7U << 22) | (3U << 16) | 0x0201;
    output[3] = (2U << 27) | 5;
    output[contextSize / 4] = 0xDEADBEEF;
    memcpy(before, output, sizeof(output));
    ELEMENT_INIT(&device.Header, 0, &device);
    list_append(&controller.Devices, &device.Header);
    CHECK(XhciRingInitialize((struct DMAContext*)42, &controller.CommandRing, 8) == OS_EOK);
    g_hubController = &controller;
    g_hubDevice = &device;
    g_hubSleeps = 0;
    g_hubCompletion = completion;
    g_hubTimeout = timeout;
    g_hubRemoval = removal;

    CHECK(HCIConfigureHub(&controller.Base, 0, 4, 0) == OS_EINVALPARAMS);
    CHECK(HCIConfigureHub(&controller.Base, 5, 0, 0) == OS_EINVALPARAMS);
    CHECK(HCIConfigureHub(&controller.Base, 6, 4, 0) == OS_ENOENT);
    controller.Commands[2].Device = &device;
    CHECK(HCIConfigureHub(&controller.Base, 5, 4, 0) == OS_EBUSY);
    CHECK(input[1] == 0);
    controller.Commands[2].Device = NULL;
    controller.CommandRing.Used = controller.CommandRing.TrbCount - 1;
    CHECK(HCIConfigureHub(&controller.Base, 5, 4, 0) == OS_EBUSY);
    CHECK(g_hubSleeps == 0 && !device.HubConfiguration.Pending);
    CHECK(!device.HubConfiguration.Waiting && controller.Base.State.OperationsInFlight == 0);
    CHECK(device.State == XHCI_DEVICE_ADDRESSED && doorbell == 0xFFFF);
    controller.CommandRing.Used = 0;

    // Compound-device bit must not become MTT. Think time applies only at high speed.
    result = HCIConfigureHub(&controller.Base, 5, 4, 0x44);
    CHECK(result == (removal ? OS_EDEVFAULT : timeout ? OS_ETIMEOUT :
        completion == XHCI_TRB_COMPLETION_SUCCESS ? OS_EOK : OS_EDEVFAULT));
    CHECK(g_hubSleeps == (removal ? 1U : timeout ? 100U : 2U));
    CHECK(!device.HubConfiguration.Waiting && controller.Base.State.OperationsInFlight == 0);
    CHECK(doorbell == 0);
    CHECK(((controller.CommandRing.Trbs[0].Control >> 10) & 0x3F) ==
        (version > 0x95 ? XHCI_TRB_TYPE_CONFIGURE_ENDPOINT : XHCI_TRB_TYPE_EVALUATE_CONTEXT));
    CHECK(controller.CommandRing.Trbs[0].Parameter == device.InputContextDeviceAddress);
    CHECK(input[0] == 0 && input[1] == 1);
    CHECK(slot[0] == ((before[0] | XHCI_SLOT_CONTEXT_HUB) & ~XHCI_SLOT_CONTEXT_MTT));
    if (version > 0x95) {
        CHECK(slot[1] == ((before[1] & 0xFFFFFFU) | (4U << 24)));
        CHECK(slot[2] == ((before[2] & ~(3U << 16)) | ((fullSpeed ? 0U : 2U) << 16)));
    } else {
        CHECK(slot[1] == before[1] && slot[2] == before[2]);
    }
    CHECK(slot[3] == 0);
    CHECK(input[2 * contextSize / 4] == 0);
    CHECK(output[contextSize / 4] == 0xDEADBEEF);
    if (timeout || removal) {
        CHECK(device.HubConfiguration.Pending);
        __TestHubCommandComplete();
        CHECK(!device.HubConfiguration.Pending);
        CHECK(device.State != XHCI_DEVICE_ADDRESSED || removal);
    } else if (completion == XHCI_TRB_COMPLETION_SUCCESS) {
        CHECK(device.State == XHCI_DEVICE_ADDRESSED);
        // Later interrupt endpoint configuration must preserve the updated hub slot.
        __BuildConfigureEndpointContext(&controller, &device, &endpoint, &transfer, false);
        CHECK((slot[0] & ~(0x1FU << 27)) == (output[0] & ~(0x1FU << 27)));
        CHECK(slot[0] >> 27 == 5 && slot[1] == output[1] && slot[2] == output[2]);
    } else {
        CHECK(!memcmp(before, output, sizeof(output)));
        CHECK(device.State == XHCI_DEVICE_FAILED);
    }
    CHECK(XhciRingDestroy(&controller.CommandRing) == OS_EOK);
}

static void
__TestHubConfiguration(void)
{
    __TestHubConfigurationCase(32, 0x100, 1, false, false, false);
    __TestHubConfigurationCase(64, 0x110, 1, false, false, false);
    __TestHubConfigurationCase(64, 0x100, 1, false, false, true);
    __TestHubConfigurationCase(32, 0x95, 1, false, false, true);
    __TestHubConfigurationCase(64, 0x100, 17, false, false, false);
    __TestHubConfigurationCase(32, 0x100, 1, true, false, false);
    __TestHubConfigurationCase(32, 0x100, 1, false, true, false);
    puts("xHCI hub: context preservation, command wait, failure, timeout and removal passed");
}
