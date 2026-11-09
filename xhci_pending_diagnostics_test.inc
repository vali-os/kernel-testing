// Pending-command diagnostics must not consume hardware events or acknowledge IRQs.
static void
__TestPendingDiagnostics(void)
{
    XhciController_t controller = { 0 };
    Device_t device = { 0 };
    XhciOperationalRegisters_t operational = { 0 };
    XhciRuntimeRegisters_t runtime = { 0 };
    XhciInterrupterRegisters_t interrupter = { 0 };
    XhciEventRingSegmentTableEntry_t erst = { 0 };
    XhciTrb_t commands[XHCI_COMMAND_RING_ENTRIES] = { 0 };
    XhciTrb_t events[XHCI_EVENT_RING_ENTRIES] = { 0 };
    unsigned int i;

    __PreparePciDiagnostics();
    device.Id = 3;
    controller.Base.Device = &device;
    controller.Base.Interrupt.IsPci = 1;
    controller.OpRegisters = &operational;
    controller.RuntimeRegisters = &runtime;
    controller.InterrupterRegisters = &interrupter;
    controller.Erst = &erst;
    controller.CommandRing.Trbs = commands;
    controller.CommandRing.Used = 1;
    controller.CommandRing.EnqueueIndex = 1;
    controller.CommandRing.DeviceBase = 0x12340000;
    controller.EventRing = events;
    controller.EventRingIndex = XHCI_EVENT_RING_ENTRIES - 1;
    controller.EventRingCycle = 1;
    controller.Commands[0].Type = XHCI_COMMAND_ENABLE_SLOT;
    controller.Commands[1].Type = XHCI_COMMAND_ADDRESS_DEVICE;
    commands[0].Control = XHCI_TRB_CONTROL_TYPE(XHCI_TRB_TYPE_ENABLE_SLOT) | 1;
    events[XHCI_EVENT_RING_ENTRIES - 1].Control =
        XHCI_TRB_CONTROL_TYPE(XHCI_TRB_TYPE_COMMAND_COMPLETION) | 1;
    events[XHCI_EVENT_RING_ENTRIES - 1].Parameter = controller.CommandRing.DeviceBase;
    events[XHCI_EVENT_RING_ENTRIES - 1].Status = XHCI_TRB_COMPLETION_SUCCESS << 24;
    operational.UsbStatus = XHCI_OP_USBSTS_EVENT;
    interrupter.Management = XHCI_INTERRUPTER_MANAGEMENT_PENDING | XHCI_INTERRUPTER_MANAGEMENT_ENABLE;
    interrupter.EventRingDequeuePointer = 0x56780008;
    atomic_store(&controller.Base.InterruptStatus, XHCI_OP_USBSTS_EVENT);

    controller.State = XHCI_CONTROLLER_HALTED;
    HciTimerCallback(&controller.Base);
    CHECK(controller.Commands[0].DiagnosticSamples == 0);
    controller.State = XHCI_CONTROLLER_RUNNING;
    for (i = 0; i < 5; i++) {
        HciTimerCallback(&controller.Base);
    }
    CHECK(controller.Commands[0].DiagnosticSamples == 3);
    CHECK(g_pciReadCount == 33);
    CHECK(controller.Commands[1].DiagnosticSamples == 0);
    CHECK(controller.EventRingIndex == XHCI_EVENT_RING_ENTRIES - 1);
    CHECK(controller.EventRingCycle == 1);
    CHECK(controller.CommandRing.Used == 1 && controller.CommandRing.EnqueueIndex == 1);
    CHECK(commands[0].Control == (XHCI_TRB_CONTROL_TYPE(XHCI_TRB_TYPE_ENABLE_SLOT) | 1));
    CHECK(events[XHCI_EVENT_RING_ENTRIES - 1].Control ==
        (XHCI_TRB_CONTROL_TYPE(XHCI_TRB_TYPE_COMMAND_COMPLETION) | 1));
    CHECK(events[XHCI_EVENT_RING_ENTRIES - 1].Parameter == controller.CommandRing.DeviceBase);
    CHECK(events[XHCI_EVENT_RING_ENTRIES - 1].Status == (XHCI_TRB_COMPLETION_SUCCESS << 24));
    CHECK(operational.UsbStatus == XHCI_OP_USBSTS_EVENT);
    CHECK(interrupter.Management ==
        (XHCI_INTERRUPTER_MANAGEMENT_PENDING | XHCI_INTERRUPTER_MANAGEMENT_ENABLE));
    CHECK(interrupter.EventRingDequeuePointer == 0x56780008);
    CHECK(atomic_load(&controller.Base.InterruptStatus) == XHCI_OP_USBSTS_EVENT);

    // A completed command must not sample unmapped hardware on later ticks.
    controller.Commands[0].Type = XHCI_COMMAND_NONE;
    controller.Commands[0].DiagnosticSamples = 0;
    controller.OpRegisters = NULL;
    HciTimerCallback(&controller.Base);
    CHECK(controller.Commands[0].DiagnosticSamples == 0);
}
