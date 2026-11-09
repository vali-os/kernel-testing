// Execute production reset/setup/run ordering, with external operations observed.
static oserr_t
__StartupStep(
    _In_ unsigned int stage)
{
    CHECK(g_startupStage + 1 == stage);
    g_startupStage = stage;
    return g_startupFailStage == stage ? OS_EUNKNOWN : OS_EOK;
}

static oserr_t
__TestStartupOpen(
    _In_    uuid_t              device,
    _In_    uint64_t            maximum,
    _InOut_ struct DMAContext** context)
{
    CHECK(device == 3);
    CHECK(maximum == ((g_startupController->HccParams1 & XHCI_HCCPARAMS1_AC64)
        ? UINT64_MAX : UINT32_MAX));
    CHECK(*context == NULL);
    return __StartupStep(2);
}

static oserr_t
__TestStartupQueue(
    _In_ XhciController_t* controller)
{
    CHECK(controller->OpRegisters == g_startupRegisters);
    return __StartupStep(3);
}

oserr_t
DeviceActivate(
    _In_ uuid_t device,
    _In_ enum DeviceActivation action)
{
    CHECK(device == 3 && action == DeviceStart);
    return __StartupStep(4);
}

oserr_t
DeviceInterruptEnable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    CHECK(interrupt != NULL);
    CHECK(!(g_startupRegisters->UsbCommand & XHCI_OP_USBCMD_RUN));
    return __StartupStep(5);
}

static oserr_t
__TestStartupRegister(
    _In_ UsbManagerController_t* controller)
{
    CHECK(controller->Device->Id == 3);
    CHECK(g_startupRegisters->UsbCommand & XHCI_OP_USBCMD_RUN);
    CHECK(__StartupStep(7) == OS_EOK);
    // Stop before publishing ports. This fixture tests startup, not registration.
    return OS_EUNKNOWN;
}

static void
__TestInterruptStartup(void)
{
    XhciController_t controller = { 0 };
    Device_t device = { .Id = 3 };
    XhciCapabilityRegisters_t capability = { 0 };
    XhciOperationalRegisters_t operational = { 0 };
    XhciInterrupterRegisters_t interrupter = { 0 };
    unsigned int failure;

    capability.HcsParams1 = (1u << 24) | 32;
    capability.HccParams1 = XHCI_HCCPARAMS1_AC64;
    controller.CapRegisters = &capability;
    controller.OpRegisters = &operational;
    controller.InterrupterRegisters = &interrupter;
    controller.Base.Device = &device;
    controller.State = XHCI_CONTROLLER_RUNNING;
    g_startupRegisters = &operational;
    g_startupController = &controller;
    for (failure = 2; failure <= 6; failure++) {
        g_startupStage = 0;
        g_startupFailStage = failure == 6 ? 0 : failure;
        CHECK(__SetupController(&controller) == OS_EUNKNOWN);
        CHECK(g_startupStage == (failure == 6 ? 7 : failure));
        if (failure < 6) {
            CHECK(!(operational.UsbCommand & XHCI_OP_USBCMD_RUN));
        }
    }
    // A 32-bit controller still constrains device addresses at DMAOpen while
    // accepting ordinary client payload memory.
    capability.HccParams1 = 0;
    g_startupStage = 0;
    g_startupFailStage = 2;
    CHECK(__SetupController(&controller) == OS_EUNKNOWN && g_startupStage == 2);
    capability.HccParams1 = XHCI_HCCPARAMS1_AC64;

    // Model removal and fatal interrupt processing while a post-enable PCI
    // read yields. Neither may be overwritten by the later run operation.
    controller.Base.Interrupt.IsPci = 1;
    g_startupController = &controller;
    for (g_startupInterruption = 1; g_startupInterruption <= 2; g_startupInterruption++) {
        __PreparePciDiagnostics();
        controller.Base.State.Phase = USB_CONTROLLER_INITIALIZING;
        controller.State = XHCI_CONTROLLER_RUNNING;
        g_startupStage = 0;
        g_startupFailStage = 0;
        CHECK(__SetupController(&controller) == OS_EUNKNOWN);
        CHECK(g_startupStage == 5);
        CHECK(!(operational.UsbCommand & XHCI_OP_USBCMD_RUN));
    }
    g_startupController = NULL;
    g_startupInterruption = 0;
    g_startupRegisters = NULL;
    puts("xHCI startup: reset, queues, DeviceStart, IRQ enable, run ordering and failure stops passed");
}
