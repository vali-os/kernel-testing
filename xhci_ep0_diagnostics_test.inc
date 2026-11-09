static void
__TestEp0Diagnostics(void)
{
    XhciController_t controller = { .State = XHCI_CONTROLLER_RUNNING };
    XhciOperationalRegisters_t operational = { .UsbStatus = XHCI_OP_USBSTS_EVENT };
    XhciInterrupterRegisters_t interrupter = { .Management = 3, .EventRingDequeuePointer = 0x47fc22008ULL };
    XhciEventRingSegmentTableEntry_t erst = { 0 };
    XhciTrb_t events[XHCI_EVENT_RING_ENTRIES] = { 0 };
    XhciTrb_t trbs[8] = { 0 };
    uint32_t context[32] = { 0 };
    uint32_t original[32];
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_CONTROL };
    XhciEndpoint_t endpoint = { .DeviceContextIndex = 1, .State = XHCI_ENDPOINT_RUNNING };
    XhciDevice_t device = { .State = XHCI_DEVICE_DEFAULT, .DefaultEndpoint = &endpoint,
        .DeviceContext = (uint8_t*)context, .InitTransfer = &transfer };
    struct DMAMapping mapping = { .Owned = 1 };
    unsigned int size;
    unsigned int i;

    controller.OpRegisters = &operational;
    controller.InterrupterRegisters = &interrupter;
    controller.EventRing = events;
    controller.EventRingIndex = XHCI_EVENT_RING_ENTRIES - 1;
    controller.EventRingCycle = 1;
    controller.Erst = &erst;
    endpoint.TransferRing.Trbs = trbs;
    endpoint.TransferRing.TrbCount = 8;
    endpoint.TransferRing.DequeueIndex = 7;
    endpoint.TransferRing.Used = 3;
    trbs[7].Parameter = 0x12345678000ULL;
    events[XHCI_EVENT_RING_ENTRIES - 1].Control = 1;
    ELEMENT_INIT(&device.Header, 0, &device);
    list_append(&controller.Devices, &device.Header);
    transfer.Dma.Setup.Direction = USBPACKET_DIRECTION_IN;
    transfer.Dma.Setup.Type = USBPACKET_TYPE_GET_DESC;
    transfer.Dma.Setup.ValueHi = USB_DESCRIPTOR_DEVICE;
    transfer.Dma.Setup.Length = 18;
    XhciDiagnoseEp0Address(&controller, &device);
    CHECK(!transfer.Dma.Diagnostics.Enabled && !device.Ep0Diagnostics.Active);
    transfer.Dma.Setup.Length = 8;
    transfer.Address.DeviceAddress = 1;
    XhciDiagnoseEp0Address(&controller, &device);
    CHECK(!transfer.Dma.Diagnostics.Enabled);
    transfer.Address.DeviceAddress = 0;

    // Only the exact initial descriptor request may select address-zero tracing.
    for (i = 0; i < 6; i++) {
        switch (i) {
            case 0: transfer.Dma.Setup.ValueLo = 1; break;
            case 1: transfer.Dma.Setup.Index = 1; break;
            case 2: transfer.Address.EndpointAddress = 1; break;
            case 3: transfer.Dma.Setup.Direction = 0; break;
            case 4: transfer.Dma.Setup.Type = USBPACKET_TYPE_SET_FEATURE; break;
            case 5: transfer.Dma.Setup.ValueHi = USB_DESCRIPTOR_CONFIG; break;
        }
        XhciDiagnoseControlRequest(&transfer);
        XhciDiagnoseEp0Address(&controller, &device);
        CHECK(!transfer.Dma.Diagnostics.Enabled);
        transfer.Dma.Setup = (usb_packet_t) { .Direction = USBPACKET_DIRECTION_IN,
            .Type = USBPACKET_TYPE_GET_DESC, .ValueHi = USB_DESCRIPTOR_DEVICE, .Length = 8 };
        transfer.Address.EndpointAddress = 0;
    }
    XhciDiagnoseControlRequest(&transfer);
    CHECK(transfer.Dma.Diagnostics.Enabled);

    // Both context sizes must keep DMA storage intact, including ring wrap.
    g_mmioRegisters[0] = &interrupter.EventRingDequeuePointer;
    g_mmioRegisterCount = 1;
    for (size = 32; size <= 64; size += 32) {
        controller.ContextSize = size;
        context[size / 4] = 1;
        context[size / 4 + 2] = 0x7fc26001;
        context[size / 4 + 3] = 4;
        memcpy(original, context, sizeof(context));
        memset(&device.Ep0Diagnostics, 0, sizeof(device.Ep0Diagnostics));
        g_mmioAccessCount = 0;
        XhciDiagnoseEp0Address(&controller, &device);
        CHECK(transfer.Dma.Diagnostics.Enabled && device.Ep0Diagnostics.Active);
        CHECK(g_mmioAccessCount == 2);
        for (i = 0; i < 5; i++) {
            XhciDiagnosePendingEp0(&controller);
        }
        CHECK(device.Ep0Diagnostics.Samples == 3 && g_mmioAccessCount == 8);
        for (i = 0; i < g_mmioAccessCount; i++) {
            CHECK(!g_mmioAccesses[i].Write);
        }
        CHECK(memcmp(original, context, sizeof(context)) == 0);
        CHECK(controller.EventRingIndex == XHCI_EVENT_RING_ENTRIES - 1);
        CHECK(controller.EventRingCycle == 1 && events[XHCI_EVENT_RING_ENTRIES - 1].Control == 1);
        CHECK(endpoint.TransferRing.DequeueIndex == 7 && endpoint.TransferRing.Used == 3);
        CHECK(trbs[7].Parameter == 0x12345678000ULL);
        CHECK(operational.UsbStatus == XHCI_OP_USBSTS_EVENT && interrupter.Management == 3);
        CHECK(interrupter.EventRingDequeuePointer == 0x47fc22008ULL);
    }
    // Inactive, detaching and halted paths must not touch released mappings.
    device.DeviceContext = NULL;
    device.Ep0Diagnostics.Samples = 0;
    device.State = XHCI_DEVICE_DETACHING;
    XhciDiagnosePendingEp0(&controller);
    device.State = XHCI_DEVICE_DEFAULT;
    device.Ep0Diagnostics.Active = false;
    XhciDiagnosePendingEp0(&controller);
    device.Ep0Diagnostics.Active = true;
    controller.State = XHCI_CONTROLLER_HALTED;
    HciTimerCallback(&controller.Base);
    CHECK(device.Ep0Diagnostics.Samples == 0 && g_mmioAccessCount == 8);
    g_mmioRegisterCount = 0;

    // Diagnostic completion retries must not change DMA handoff semantics.
    transfer.Dma.Mapping = &mapping;
    transfer.Dma.Length = 16;
    transfer.Dma.DeviceOwned = true;
    transfer.Dma.HardwareActive = true;
    transfer.Dma.Bounce.Buffer.ID = UUID_INVALID;
    g_failSync = 1;
    for (i = 0; i < 5; i++) {
        CHECK(UsbTransferDmaComplete(&transfer, 16) == OS_EUNKNOWN);
    }
    CHECK(transfer.Dma.Diagnostics.Completions == 3);
    CHECK(mapping.Owned && transfer.Dma.DeviceOwned && transfer.Dma.HardwareActive);
    g_failSync = 0;
    CHECK(UsbTransferDmaComplete(&transfer, 16) == OS_EOK);
    CHECK(!mapping.Owned && !transfer.Dma.DeviceOwned && !transfer.Dma.HardwareActive);
    puts("xHCI EP0 diagnostics: selection, bounded snapshots, context sizes and read-only MMIO passed");
}
