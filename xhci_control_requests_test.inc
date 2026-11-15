static void
__TestControlRequest(
    bool initialDescriptor)
{
    XhciController_t* controller = &g_cancelController;
    XhciOperationalRegisters_t operational = { .UsbStatus = XHCI_OP_USBSTS_EVENT };
    XhciInterrupterRegisters_t interrupter = { .Management = 3, .EventRingDequeuePointer = 0x12340008 };
    XhciEventRingSegmentTableEntry_t erst = { 0 };
    XhciTrb_t events[XHCI_EVENT_RING_ENTRIES] = { 0 };
    uint32_t context[32] = { 0 };
    uint32_t original[32];
    uint8_t inputContext[XHCI_INPUT_CONTEXT_ENTRIES * 32] = { 0 };
    uint32_t parentContext[8] = { 0 };
    XhciDevice_t parent = { .SlotId = 2, .Speed = USBSPEED_HIGH,
        .DeviceContext = (uint8_t*)parentContext };
    reg32_t doorbells[2] = { 0 };
    UsbScheduler_t scheduler = { 0 };
    XhciEndpoint_t endpoint = { .SlotId = 1, .DeviceContextIndex = 1, .State = XHCI_ENDPOINT_RUNNING };
    XhciDevice_t device = { .SlotId = 1, .UsbAddress = 5, .State = XHCI_DEVICE_ADDRESSED,
        .DeviceContext = (uint8_t*)context, .DefaultEndpoint = &endpoint };
    UsbManagerTransfer_t transfer = { .ID = 123, .DeviceID = 77, .Type = USBTRANSFER_TYPE_CONTROL,
        .Direction = USBTRANSFER_DIRECTION_OUT, .MaxPacketSize = 64,
        .Address = { .DeviceAddress = 5 }, .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferLength = 8 } };
    usb_packet_t setup = { .Direction = 0x23, .Type = 3, .ValueLo = 8, .Index = 1 };
    XhciTrb_t event;
    uint64_t setupBytes;
    unsigned int i;

    if (initialDescriptor) {
        setup = (usb_packet_t) { .Direction = USBPACKET_DIRECTION_IN,
            .Type = USBPACKET_TYPE_GET_DESC, .ValueHi = USB_DESCRIPTOR_DEVICE, .Length = 8 };
        transfer.Address.DeviceAddress = 0;
        transfer.Address.HubAddress = 5;
        transfer.Address.PortAddress = 1;
        transfer.Speed = USBSPEED_FULL;
        transfer.Direction = USBTRANSFER_DIRECTION_IN;
        transfer.Dma.BufferLength = 16;
        device.UsbAddress = 0;
        device.HubAddress = 5;
        device.PortAddress = 1;
        device.RootPort = 1;
        device.RouteString = 1;
        device.Depth = 1;
        device.Speed = USBSPEED_FULL;
        device.Parent = &parent;
        device.State = XHCI_DEVICE_DEFAULT;
        device.InputContext = inputContext;
        device.InitTransfer = &transfer;
        parentContext[0] = XHCI_SLOT_CONTEXT_HUB | XHCI_SLOT_CONTEXT_SPEED(3);
        parentContext[1] = XHCI_SLOT_CONTEXT_NUM_PORTS(4) | XHCI_SLOT_CONTEXT_ROOT_PORT(1);
        parentContext[2] = XHCI_SLOT_CONTEXT_TT_THINK(2);
    }

    memset(controller, 0, sizeof(*controller));
    controller->OpRegisters = &operational;
    controller->InterrupterRegisters = &interrupter;
    controller->Erst = &erst;
    controller->EventRing = events;
    controller->EventRingCycle = 1;
    controller->SlotCount = 1;
    controller->Slots[1] = &device;
    controller->ContextSize = 32;
    controller->Doorbells = doorbells;
    controller->Base.Scheduler = &scheduler;
    scheduler.Settings.Pools[0].ElementPool = (uint8_t*)g_descriptors;
    scheduler.Settings.Pools[0].ElementAlignedSize = sizeof(g_descriptors[0]);
    device.Endpoints[1] = &endpoint;
    endpoint.Device = &device;
    endpoint.Address = transfer.Address;
    context[8] = 1;
    context[10] = 0x7fc45001;
    context[11] = 4;

    ELEMENT_INIT(&device.Header, 0, &device);
    ELEMENT_INIT(&endpoint.Header, 0, &endpoint);
    list_append(&controller->Devices, &device.Header);
    list_append(&controller->XhciEndpoints, &endpoint.Header);
    CHECK(XhciRingInitialize((struct DMAContext*)42, &endpoint.TransferRing, 8) == OS_EOK);
    memcpy(g_source, &setup, sizeof(setup));
    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EOK);
    if (initialDescriptor) {
        __BuildAddressContext(controller, &device, &transfer);
        memcpy(context, inputContext + controller->ContextSize, 2 * controller->ContextSize);
        CHECK((context[0] & 0xfffff) == 1 && ((context[0] >> 20) & 15) == 1);
        CHECK(context[2] == (XHCI_SLOT_CONTEXT_TT_HUB(2) | XHCI_SLOT_CONTEXT_TT_PORT(1)));
    }
    memcpy(original, context, sizeof(context));
    CHECK(HCITransferQueue(&transfer) == OS_EOK);
    CHECK(transfer.ElementCount == (initialDescriptor ? 3 : 2));
    CHECK(endpoint.TransferRing.Used == transfer.ElementCount && doorbells[1] == 1);
    memcpy(&setupBytes, &setup, sizeof(setup));
    CHECK(endpoint.TransferRing.Trbs[0].Parameter == setupBytes);
    CHECK(!(endpoint.TransferRing.Trbs[0].Control & 0x1e));
    CHECK(XHCI_TRB_CONTROL_TYPE_GET(endpoint.TransferRing.Trbs[0].Control) == XHCI_TRB_TYPE_SETUP_STAGE);
    i = initialDescriptor ? 2 : 1;
    CHECK(XHCI_TRB_CONTROL_TYPE_GET(endpoint.TransferRing.Trbs[i].Control) == XHCI_TRB_TYPE_STATUS_STAGE);
    CHECK(!!(endpoint.TransferRing.Trbs[i].Control & XHCI_TRB_CONTROL_DIR_IN) == !initialDescriptor);
    CHECK(endpoint.TransferRing.Trbs[i].Control & XHCI_TRB_CONTROL_IOC);
    CHECK(!(endpoint.TransferRing.Trbs[i].Control & XHCI_TRB_CONTROL_CHAIN));
    CHECK(endpoint.TransferRing.Trbs[i].Status == 0);
    if (initialDescriptor) {
        CHECK(XHCI_TRB_CONTROL_TYPE_GET(endpoint.TransferRing.Trbs[1].Control) == XHCI_TRB_TYPE_DATA_STAGE);
        CHECK(endpoint.TransferRing.Trbs[1].Control & XHCI_TRB_CONTROL_DIR_IN);
        CHECK(!(endpoint.TransferRing.Trbs[1].Control & XHCI_TRB_CONTROL_CHAIN));
        CHECK((endpoint.TransferRing.Trbs[1].Status & 0x1ffff) == 8);
    }

    g_mmioRegisters[0] = &interrupter.EventRingDequeuePointer;
    g_mmioRegisterCount = 1;
    g_mmioAccessCount = 0;
    for (i = 0; i < 5; i++) {
        HciTimerCallback(&controller->Base);
    }
    CHECK(g_mmioAccessCount == 0);
    CHECK(memcmp(original, context, sizeof(context)) == 0);
    CHECK(controller->EventRingIndex == 0 && endpoint.TransferRing.Used == transfer.ElementCount);
    CHECK(operational.UsbStatus == XHCI_OP_USBSTS_EVENT && interrupter.Management == 3);
    event = (XhciTrb_t) {
        .Parameter = endpoint.TransferRing.DeviceBase + (initialDescriptor ? 2 : 1) * sizeof(XhciTrb_t),
        .Status = (initialDescriptor ? 6 /* Stall Error */ : XHCI_TRB_COMPLETION_SUCCESS) << 24,
        .Control = XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_EP_ID(1)
    };
    CHECK(XhciTransferHandleEvent(controller, &event));
    CHECK(g_mmioAccessCount == 0);
    CHECK(memcmp(original, context, sizeof(context)) == 0);
    CHECK(endpoint.State == (initialDescriptor ? XHCI_ENDPOINT_HALTED : XHCI_ENDPOINT_RUNNING));
    CHECK(operational.UsbStatus == XHCI_OP_USBSTS_EVENT && interrupter.Management == 3);
    g_mmioRegisterCount = 0;
    CHECK(UsbTransferDmaComplete(&transfer, transfer.Dma.Length) == OS_EOK);
    XhciTransferCleanup(controller, &transfer);
    UsbManagerForgetTransferWork(&transfer);
    CHECK(UsbTransferDmaDestroy(&transfer) == OS_EOK);
    free(transfer.Elements);
    CHECK(XhciRingDestroy(&endpoint.TransferRing) == OS_EOK);
    CHECK(g_maps == 0);
    puts(initialDescriptor ?
        "xHCI initial descriptor: TT routing, Setup/Data/Status, stall handling and DMA completion passed" :
        "xHCI port-power: Setup/Status submission, idle monitor and completion passed");
}

static void
__TestControlRequests(void)
{
    __TestControlRequest(false);
    __TestControlRequest(true);
}
