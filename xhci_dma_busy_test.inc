static void
__QueueHubControl(
    _Out_ UsbManagerTransfer_t* transfer,
    _In_ size_t offset,
    _In_ bool reset)
{
    usb_packet_t packet = {
        .Direction = reset ? 0x23 : 0xA3,
        .Type = reset ? USBPACKET_TYPE_SET_FEATURE : USBPACKET_TYPE_GET_STATUS,
        .ValueLo = reset ? 4 : 0,
        .Index = 1,
        .Length = reset ? 0 : 4
    };

    *transfer = (UsbManagerTransfer_t) {
        .DeviceID = 77, .Type = USBTRANSFER_TYPE_CONTROL,
        .Direction = reset ? USBTRANSFER_DIRECTION_OUT : USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8, .Address = { .DeviceAddress = 5 },
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferOffset = offset,
            .BufferLength = sizeof(packet) + packet.Length }
    };
    memcpy(g_source + offset, &packet, sizeof(packet));
    ELEMENT_INIT(&transfer->ListHeader, 0, transfer);
    list_append(&g_cancelController.Base.TransactionList, &transfer->ListHeader);
    CHECK(UsbTransferDmaPrepare(transfer) == OS_EOK);
    CHECK(HCITransferQueue(transfer) == OS_EOK);
}

static void
__DeliverHubCompletion(
    _In_ UsbManagerTransfer_t* transfer)
{
    XhciController_t* controller = &g_cancelController;
    XhciTransferDescriptor_t* descriptor = transfer->RootElement;
    XhciTransferDescriptor_t* next;
    XhciTrb_t events[2] = { 0 };
    XhciInterrupterRegisters_t interrupter = { 0 };

    while ((next = __DescriptorNext(controller, descriptor)) != NULL) {
        descriptor = next;
    }
    events[0] = (XhciTrb_t) {
        .Parameter = descriptor->Endpoint->TransferRing.DeviceBase +
            descriptor->FirstTrbIndex * sizeof(XhciTrb_t),
        .Status = XHCI_TRB_COMPLETION_SUCCESS << 24,
        .Control = XHCI_TRB_CONTROL_TYPE(XHCI_TRB_TYPE_TRANSFER_EVENT) |
            XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_CYCLE |
            XHCI_TRB_CONTROL_EP_ID(descriptor->Endpoint->DeviceContextIndex)
    };
    controller->EventRing = events;
    controller->EventRingIndex = 0;
    controller->EventRingCycle = 1;
    controller->EventRingDeviceAddress = 0x400004000ULL;
    controller->InterrupterRegisters = &interrupter;
    atomic_store(&controller->Base.InterruptStatus, XHCI_OP_USBSTS_EVENT);
    HciInterruptCallback(&controller->Base);
    CHECK(controller->EventRingIndex == 1);
    controller->EventRing = NULL;
    controller->InterrupterRegisters = NULL;
}

static void
__TestHubDmaContention(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t scan, reset, status, independent, interrupt, timed, cancelled;
    XhciEndpoint_t interruptEndpoint = { 0 };
    usb_packet_t scanPacket = { .Direction = 0xA3, .Type = USBPACKET_TYPE_GET_STATUS,
        .Index = 1, .Length = 4 };
    OSHandle_t shared = { .ID = 17 };
    struct DMAMapping* cpuBlocker = NULL;
    unsigned int attempts;
    unsigned int commands;
    unsigned int i;

    g_enforceDmaReservations = true;
    g_dmaBusyAttempts = 0;
    __InitializeRecoveryWithPacket(&fixture, &scan, true, &scanPacket);
    __QueueHubControl(&reset, 32, true);
    __QueueHubControl(&status, 48, false);
    CHECK(reset.State == USBTRANSFER_STATE_WAITING && status.State == USBTRANSFER_STATE_WAITING);
    CHECK(reset.RootElement == NULL && status.RootElement == NULL);
    CHECK(!reset.Dma.DeviceOwned && !reset.Dma.HardwareActive);
    CHECK(fixture.Endpoint.TransferRing.Used == 3 && fixture.Endpoint.Pending.count == 1);
    CHECK(g_notifications == 0 && g_cancelController.Base.WaitingTransfers.count == 2);

    // An independent control request and the interrupt pipe must keep progressing.
    __QueueHubControl(&independent, 128, false);
    CHECK(independent.Dma.HardwareActive);
    attempts = g_dmaBusyAttempts;
    __DeliverHubCompletion(&independent);
    CHECK(g_dmaBusyAttempts == attempts + 2 && g_lastResult == USBTRANSFERCODE_SUCCESS);

    interrupt = (UsbManagerTransfer_t) {
        .DeviceID = 77, .Type = USBTRANSFER_TYPE_INTERRUPT, .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 1, .Address = { .DeviceAddress = 5, .EndpointAddress = 1 },
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferOffset = 512, .BufferLength = 64 }
    };
    interruptEndpoint.Device = &fixture.Device;
    interruptEndpoint.SlotId = 1;
    interruptEndpoint.DeviceContextIndex = 3;
    interruptEndpoint.Address = interrupt.Address;
    interruptEndpoint.State = XHCI_ENDPOINT_RUNNING;
    interruptEndpoint.ConfiguredMaxPacketSize = 1;
    fixture.Device.Endpoints[3] = &interruptEndpoint;
    ELEMENT_INIT(&interruptEndpoint.Header, 0, &interruptEndpoint);
    list_append(&g_cancelController.XhciEndpoints, &interruptEndpoint.Header);
    ELEMENT_INIT(&interrupt.ListHeader, 0, &interrupt);
    list_append(&g_cancelController.Base.TransactionList, &interrupt.ListHeader);
    CHECK(XhciRingInitialize((struct DMAContext*)42, &interruptEndpoint.TransferRing, XHCI_RING_TRB_COUNT) == OS_EOK);
    CHECK(UsbTransferDmaPrepare(&interrupt) == OS_EOK);
    CHECK(HCITransferQueue(&interrupt) == OS_EOK && interrupt.Dma.HardwareActive);
    attempts = g_dmaBusyAttempts;
    __DeliverHubCompletion(&interrupt);
    CHECK(interrupt.Dma.HardwareActive && g_dmaBusyAttempts == attempts + 2);
    CHECK(g_notifications == 2 && reset.State == USBTRANSFER_STATE_WAITING);

    __DeliverHubCompletion(&scan);
    CHECK(reset.Dma.HardwareActive && status.State == USBTRANSFER_STATE_WAITING);
    CHECK(fixture.Endpoint.Pending.count == 1 && fixture.Endpoint.TransferRing.Used == 2);
    __DeliverHubCompletion(&reset);
    CHECK(status.Dma.HardwareActive && g_cancelController.Base.WaitingTransfers.count == 0);
    __DeliverHubCompletion(&status);
    CHECK(g_notifications == 5 && g_destroyed == 4);
    CHECK(fixture.Endpoint.Pending.count == 0 && fixture.Endpoint.TransferRing.Used == 0);

    // An external reservation produces no USB interrupt. The timer retries once,
    // and cancellation removes a waiter without stopping the working endpoint.
    CHECK(DMAMapSHM((struct DMAContext*)42, &shared, 256, 64, DMABidirectional, NULL, &cpuBlocker) == OS_EOK);
    CHECK(DMABeginTransfer(cpuBlocker, 0, 64, SHMWaitNonBlocking, NULL) == OS_EOK);
    __QueueHubControl(&timed, 280, true);
    __QueueHubControl(&cancelled, 296, false);
    CHECK(timed.State == USBTRANSFER_STATE_WAITING && cancelled.State == USBTRANSFER_STATE_WAITING);
    commands = g_cancelController.CommandRing.Used;
    CHECK(HCITransferDequeue(&cancelled) == OS_EOK);
    CHECK(g_destroyed == 5 && g_cancelController.CommandRing.Used == commands);
    CHECK(g_cancelController.Base.WaitingTransfers.count == 1);
    attempts = g_dmaBusyAttempts;
    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_dmaBusyAttempts == attempts + 1 && timed.State == USBTRANSFER_STATE_WAITING);
    CHECK(DMAEndTransfer(cpuBlocker, 0) == OS_EOK);
    CHECK(DMAUnmap(&cpuBlocker) == OS_EOK);
    HciTimerCallback(&g_cancelController.Base);
    CHECK(timed.Dma.HardwareActive && g_cancelController.Base.WaitingTransfers.count == 0);
    __DeliverHubCompletion(&timed);

    CHECK(HCITransferDequeue(&interrupt) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(g_destroyed == 7 && g_cancelController.Base.TransactionList.count == 0);
    CHECK(g_cancelController.Base.WaitingTransfers.count == 0);
    CHECK(XhciRingDestroy(&interruptEndpoint.TransferRing) == OS_EOK);
    CHECK(XhciRingDestroy(&fixture.Endpoint.TransferRing) == OS_EOK);
    CHECK(XhciRingDestroy(&g_cancelController.CommandRing) == OS_EOK);
    for (i = 0; i < 16; i++) {
        CHECK(g_activeReservations[i] == NULL);
    }
    CHECK(g_maps == 0 && g_bounceBuffers == 0);
    g_enforceDmaReservations = false;
    puts("xHCI DMA busy: overlapping hub scan/reset/interrupt activity, completion/timer retry and cancellation passed");
}

static void
__TestDmaBusyDuringRecovery(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;
    unsigned int notifications;

    __InitializeRecovery(&fixture, &transfer, false);
    __CompleteRecoveryTransfer(&transfer, 6); // STALL completion.
    notifications = g_notifications;
    g_busyMapping = transfer.Dma.Mapping;
    CHECK(HCIEndpointReset(&g_cancelController.Base, &transfer.Address) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_RUNNING);
    CHECK(transfer.State == USBTRANSFER_STATE_WAITING && transfer.RootElement == NULL);
    CHECK(!transfer.Dma.HardwareActive && !transfer.Dma.DeviceOwned);
    CHECK(fixture.Endpoint.Pending.count == 0 && fixture.Endpoint.TransferRing.Used == 0);
    CHECK(g_notifications == notifications && g_destroyed == 0);
    g_busyMapping = NULL;
    HciTimerCallback(&g_cancelController.Base);
    CHECK(transfer.State == USBTRANSFER_STATE_QUEUED && transfer.Dma.HardwareActive);
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __DestroyRecovery(&fixture);
    puts("xHCI DMA busy: endpoint recovery preserves and retries the pending request");
}
