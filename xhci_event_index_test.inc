/** Select by hardware identity even when another ring contains the same address. */
static void
__TestIndexedTransferEvents(void)
{
    XhciController_t          controller = { .SlotCount = 255 };
    XhciDevice_t              devices[2] = { 0 };
    XhciEndpoint_t            endpoints[3] = { 0 };
    XhciTransferDescriptor_t  descriptors[3] = { 0 };
    UsbManagerTransfer_t      transfers[3] = { 0 };
    XhciTrb_t                event = {
        .Parameter = 0x100001000ULL,
        .Status = XHCI_TRB_COMPLETION_SUCCESS << 24
    };
    unsigned int             i;

    controller.Slots[1] = &devices[0];
    controller.Slots[255] = &devices[1];
    devices[0].Endpoints[1] = &endpoints[0];
    devices[1].Endpoints[1] = &endpoints[1];
    devices[1].Endpoints[31] = &endpoints[2];
    for (i = 0; i < 3; i++) {
        endpoints[i].TransferRing.DeviceBase = event.Parameter;
        endpoints[i].TransferRing.TrbCount = 8;
        endpoints[i].TransferRing.Used = 1;
        endpoints[i].TRBOwners[0] = &descriptors[i];
        descriptors[i].Transfer = &transfers[i];
        descriptors[i].TransferLength = 32;
        descriptors[i].TrbCount = 1;
        descriptors[i].Object.DepthIndex = USB_ELEMENT_NO_INDEX;
        transfers[i].RootElement = &descriptors[i];
        transfers[i].Type = USBTRANSFER_TYPE_BULK;
        transfers[i].Dma.HardwareActive = true;
    }

    event.Control = XHCI_TRB_CONTROL_EP_ID(1);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Control = XHCI_TRB_CONTROL_SLOT_ID(2) | XHCI_TRB_CONTROL_EP_ID(1);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Control = XHCI_TRB_CONTROL_SLOT_ID(255);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Control |= XHCI_TRB_CONTROL_EP_ID(30);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Control = XHCI_TRB_CONTROL_SLOT_ID(255) | XHCI_TRB_CONTROL_EP_ID(31);
    controller.SlotCount = 254;
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    controller.SlotCount = 255;

    // Validate against the selected ring, without falling back to other rings.
    endpoints[2].TransferRing.DeviceBase += 0x1000;
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    endpoints[2].TransferRing.DeviceBase -= 0x1000;
    event.Parameter++;
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Parameter += 7 * sizeof(XhciTrb_t) - 1;
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Parameter += sizeof(XhciTrb_t);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Parameter = endpoints[2].TransferRing.DeviceBase;
    for (i = 0; i < 3; i++) {
        CHECK(transfers[i].Dma.HardwareActive && descriptors[i].Flags == 0);
        CHECK(endpoints[i].TransferRing.Used == 1);
    }

    // There is deliberately no endpoint list to scan in this fixture.
    CHECK(XhciTransferHandleEvent(&controller, &event));
    CHECK(!transfers[2].Dma.HardwareActive && descriptors[2].BytesTransferred == 32);
    CHECK(transfers[0].Dma.HardwareActive && transfers[1].Dma.HardwareActive);
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    event.Control = XHCI_TRB_CONTROL_SLOT_ID(255) | XHCI_TRB_CONTROL_EP_ID(1);
    CHECK(XhciTransferHandleEvent(&controller, &event));
    CHECK(transfers[0].Dma.HardwareActive && !transfers[1].Dma.HardwareActive);
    event.Control = XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_EP_ID(1);
    CHECK(XhciTransferHandleEvent(&controller, &event));
}

static void
__TestEndpointIndexRemoval(void)
{
    XhciController_t controller = { 0 };
    XhciDevice_t     device = { .SlotId = 7 };
    XhciEndpoint_t*  endpoint = calloc(1, sizeof(*endpoint));

    CHECK(endpoint != NULL);
    endpoint->Device = &device;
    endpoint->DeviceContextIndex = 31;
    device.Endpoints[31] = endpoint;
    ELEMENT_INIT(&endpoint->Header, 0, endpoint);
    list_append(&controller.XhciEndpoints, &endpoint->Header);
    CHECK(XhciRingInitialize(42, &endpoint->TransferRing, 8) == OS_EOK);
    g_failUnmap = 1;
    CHECK(XhciEndpointDestroyDevice(&controller, &device) == OS_EBUSY);
    CHECK(device.Endpoints[31] == endpoint && controller.XhciEndpoints.head != NULL);
    g_failUnmap = 0;
    CHECK(XhciEndpointDestroyDevice(&controller, &device) == OS_EOK);
    CHECK(device.Endpoints[31] == NULL && controller.XhciEndpoints.head == NULL && g_maps == 0);
}

static void
__TestSlotIndexLifecycle(void)
{
    XhciController_t controller = { .Base = { .DmaHandle = 42, .PortCount = 1 },
        .SlotCount = 1, .ContextSize = 32 };
    XhciEndpoint_t   endpoint = { .DeviceContextIndex = 1 };
    XhciEndpoint_t   dataEndpoint = { .DeviceContextIndex = 3 };
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_CONTROL,
        .Speed = USBSPEED_FULL, .MaxPacketSize = 8 };
    XhciDevice_t*    device = NULL;
    XhciDevice_t*    existing = NULL;
    XhciDevice_t     replacement = { .SlotId = 1 };
    XhciTrb_t       event = {
        .Status = XHCI_TRB_COMPLETION_SUCCESS << 24,
        .Control = XHCI_TRB_CONTROL_SLOT_ID(1)
    };
    reg64_t         dcbaa[2] = { 0 };
    reg32_t         doorbell = 0;
    uint16_t        commandIndex;

    controller.DCBaa = dcbaa;
    controller.Doorbells = &doorbell;
    CHECK(XhciRingInitialize(42, &controller.CommandRing, 8) == OS_EOK);
    CHECK(XhciDeviceEnsure(&controller, &transfer, &endpoint, &device) == OS_EINCOMPLETE);
    CHECK(device->Endpoints[1] == &endpoint && controller.Slots[1] == NULL);

    event.Parameter = controller.CommandRing.DeviceBase;
    XhciCommandHandleCompletion(&controller, &event);
    CHECK(controller.Slots[1] == device && endpoint.SlotId == 1);
    CHECK(device->State == XHCI_DEVICE_DEFAULT_PENDING);
    event.Parameter += sizeof(XhciTrb_t);
    XhciCommandHandleCompletion(&controller, &event);
    CHECK(device->State == XHCI_DEVICE_DEFAULT);
    CHECK(XhciDeviceEnsure(&controller, &transfer, &dataEndpoint, &existing) == OS_EOK);
    CHECK(existing == device && device->Endpoints[3] == &dataEndpoint && dataEndpoint.SlotId == 1);

    // Disable Slot releases the index even when DMA resource cleanup must retry.
    commandIndex = controller.CommandRing.EnqueueIndex;
    CHECK(__SubmitDisableSlot(&controller, device) == OS_EOK);
    event.Parameter = controller.CommandRing.DeviceBase + commandIndex * sizeof(XhciTrb_t);
    g_failUnmap = 1;
    XhciCommandHandleCompletion(&controller, &event);
    CHECK(controller.Slots[1] == NULL && controller.Devices.head != NULL && dcbaa[1] == 0);
    g_failUnmap = 0;

    // Retrying old cleanup cannot remove a new device assigned the same slot.
    controller.Slots[1] = &replacement;
    CHECK(__DestroyDevice(&controller, device) == OS_EOK);
    CHECK(controller.Slots[1] == &replacement);
    controller.Slots[1] = NULL;
    CHECK(XhciRingDestroy(&controller.CommandRing) == OS_EOK && g_maps == 0);
}
