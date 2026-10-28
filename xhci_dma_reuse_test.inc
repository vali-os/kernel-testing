/** Exercise periodic reuse through production preparation, submission and completion. */
static void
__TestPeriodicReuse(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 256,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferOffset = 128, .BufferLength = 600 }
    };
    XhciController_t          controller = { .SlotCount = 1 };
    XhciDevice_t              device = { .SlotId = 1 };
    XhciEndpoint_t            endpoint = { .SlotId = 1, .DeviceContextIndex = 3 };
    UsbScheduler_t            scheduler = { 0 };
    XhciTrb_t                 ring[8] = { 0 };
    XhciTrb_t                 event;
    reg32_t                   doorbells[2] = { 0 };
    XhciTransferDescriptor_t* descriptor;
    struct TransferElement*  elements = NULL;
    struct DMAMapping*        mappings[3] = { 0 };
    struct HCIProcessReasonScanContext scan;
    int                      maps = g_mapCalls;
    int                      unmaps = g_unmapCalls;
    int                      growths = g_elementGrowths;
    int                      allocations = g_descriptorAllocations;
    int                      frees = g_descriptorFrees;
    int                      cycle;
    int                      slot;

    controller.Base.Scheduler = &scheduler;
    controller.Slots[1] = &device;
    device.Endpoints[3] = &endpoint;
    controller.Doorbells = doorbells;
    list_append(&controller.XhciEndpoints, &endpoint.Header);
    endpoint.Header.value = &endpoint;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementPool = (uint8_t*)g_descriptors;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementAlignedSize = sizeof(g_descriptors[0]);
    endpoint.TransferRing.Trbs = ring;
    endpoint.TransferRing.TrbCount = 8;
    endpoint.TransferRing.CycleState = 1;
    endpoint.TransferRing.DeviceBase = 0x200001000ULL;

    for (cycle = 0; cycle < 4; cycle++) {
        for (slot = 0; slot < 3; slot++) {
            transfer.TData.Periodic.CurrentDataIndex = slot * 256;
            CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
            CHECK(transfer.Dma.Length == (slot == 2 ? 88 : 256));
            CHECK(transfer.ElementCount == 1);
            CHECK(transfer.Elements[0].Data.Address == 0x300010000ULL);
            if (cycle == 0) {
                mappings[slot] = transfer.Dma.Mapping;
            } else {
                CHECK(transfer.Dma.Mapping == mappings[slot]);
            }
            if (elements == NULL) {
                elements = transfer.Elements;
            }
            CHECK(transfer.Elements == elements);
            CHECK(XhciTransferPrepare(&controller, &transfer, &endpoint) == OS_EOK);
            descriptor = transfer.RootElement;
            while (descriptor != NULL) {
                CHECK(descriptor->Flags == 0 && descriptor->BytesTransferred == 0);
                CHECK(descriptor->CompletionCode == 0 && descriptor->Endpoint == &endpoint);
                descriptor = __DescriptorNext(&controller, descriptor);
            }
            CHECK(XhciEndpointEnqueueTransfer(&endpoint, transfer.RootElement) == OS_EOK);
            CHECK(XhciTransferSubmit(&controller, &endpoint, &transfer) == OS_EOK);
            transfer.State = USBTRANSFER_STATE_QUEUED;
            CHECK(USBTransferDmaPrepare(&transfer) == OS_EBUSY);
            CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);

            descriptor = transfer.RootElement;
            while (__DescriptorNext(&controller, descriptor) != NULL) {
                descriptor = __DescriptorNext(&controller, descriptor);
            }
            event = (XhciTrb_t) {
                .Parameter = endpoint.TransferRing.DeviceBase + descriptor->FirstTrbIndex * sizeof(XhciTrb_t),
                .Status = XHCI_TRB_COMPLETION_SUCCESS << 24,
                .Control = XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_EP_ID(3)
            };
            if (cycle == 2 && slot == 0) {
                descriptor = transfer.RootElement;
                event.Parameter = endpoint.TransferRing.DeviceBase + descriptor->FirstTrbIndex * sizeof(XhciTrb_t);
                event.Status = (XHCI_TRB_COMPLETION_SHORT_PACKET << 24) | (descriptor->TransferLength - 7);
            }
            CHECK(XhciTransferHandleEvent(&controller, &event));
            CHECK(endpoint.TransferRing.Used == 0 && !transfer.Dma.HardwareActive);
            scan = (struct HCIProcessReasonScanContext) { .Transfer = &transfer };
            UsbManagerChainEnumerate(&controller.Base, transfer.RootElement,
                USB_CHAIN_DEPTH, HCIPROCESS_REASON_SCAN, HCIProcessElement, &scan);
            CHECK(scan.BytesTransferred == (cycle == 2 && slot == 0 ? 7 : transfer.Dma.Length));
            CHECK(USBTransferDmaComplete(&transfer, scan.BytesTransferred) == OS_EOK);
            transfer.ResultCode = USBTRANSFERCODE_SUCCESS;
            CHECK(HCITransferFinalize(&controller.Base, &transfer, false) == OS_EOK);
            CHECK(transfer.RootElement == NULL && transfer.ReusableElements != NULL);
            CHECK(endpoint.Pending.head == NULL && endpoint.CurrentTd == NULL);
        }
        CHECK(g_mapCalls == maps + 1 && g_unmapCalls == unmaps);
        CHECK(g_elementGrowths == growths + 1);
        CHECK(g_descriptorAllocations == allocations + 1 && g_descriptorFrees == frees);
    }

    // A stalled transfer remains on its endpoint until reset or cancellation.
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(XhciTransferPrepare(&controller, &transfer, &endpoint) == OS_EOK);
    CHECK(XhciEndpointEnqueueTransfer(&endpoint, transfer.RootElement) == OS_EOK);
    descriptor = transfer.RootElement;
    descriptor->Flags = XHCI_TD_FLAG_FAILED | XHCI_TD_FLAG_COMPLETED;
    descriptor->BytesTransferred = 7;
    descriptor->CompletionCode = 6;
    transfer.ResultCode = USBTRANSFERCODE_STALL;
    CHECK(HCITransferFinalize(&controller.Base, &transfer, false) == OS_EOK);
    CHECK(transfer.ReusableElements == NULL && transfer.RootElement == descriptor);
    CHECK(descriptor->Endpoint == &endpoint && endpoint.Pending.head == &descriptor->QueueHeader);
    transfer.State = USBTRANSFER_STATE_STALLED;

    // Terminal cleanup frees both an active chain and excess cached capacity.
    transfer.State = USBTRANSFER_STATE_CLEANUP;
    transfer.ResultCode = USBTRANSFERCODE_CANCELLED;
    CHECK(HCITransferFinalize(&controller.Base, &transfer, false) == OS_EOK);
    CHECK(transfer.RootElement == NULL && transfer.ReusableElements == NULL);
    CHECK(g_descriptorFrees == frees + 1);

    // A failed release must retain the backing page along with its mapping.
    g_failUnmapAfter = 0;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);
    CHECK(g_maps == 1 && g_stagingBuffers == 1 && g_stagingBytes == 4096);
    g_failUnmapAfter = -1;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(g_maps == 0 && transfer.Dma.Mapping == NULL && transfer.Dma.Mappings == NULL);
    CHECK(g_stagingBuffers == 0 && g_stagingBytes == 0);
    free(transfer.Elements);
}

static void
__TestReuseFailures(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 64,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferLength = 128 }
    };
    XhciController_t controller = { 0 };
    XhciEndpoint_t   endpoint = { 0 };
    UsbScheduler_t   scheduler = { 0 };
    int              maps;
    int              allocations = g_descriptorAllocations;
    int              frees = g_descriptorFrees;

    controller.Base.Scheduler = &scheduler;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementPool = (uint8_t*)g_descriptors;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementAlignedSize = sizeof(g_descriptors[0]);
    g_failMap = 1;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOOM && g_maps == 0);
    g_failMap = 0;
    g_failSegments = 1;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EUNKNOWN && g_maps == 0);
    g_failSegments = 0;
    g_failElementGrowth = 1;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOOM && transfer.Elements == NULL);
    maps = g_mapCalls;
    g_failElementGrowth = 0;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK && g_mapCalls == maps);

    // Failure after some descriptors were acquired releases the partial chain.
    transfer.ElementCount = 2;
    g_failDescriptorAfter = 1;
    CHECK(XhciTransferPrepare(&controller, &transfer, &endpoint) == OS_EOOM);
    CHECK(transfer.RootElement == NULL && transfer.ReusableElements == NULL);
    CHECK(g_descriptorAllocations == allocations + 1 && g_descriptorFrees == frees + 1);
    g_failDescriptorAfter = -1;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK && g_maps == 0);
    free(transfer.Elements);
}

static void
__TestLinearDescriptorChain(void)
{
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_BULK };
    XhciController_t controller = { 0 };
    XhciEndpoint_t endpoint = { 0 };
    UsbScheduler_t scheduler = { 0 };
    XhciTransferDescriptor_t* descriptor;
    int count;
    int i;
    int walks;
    int allocations;
    int frees;

    controller.Base.Scheduler = &scheduler;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementPool = (uint8_t*)g_descriptors;
    scheduler.Settings.Pools[XHCI_TD_POOL].ElementAlignedSize = sizeof(g_descriptors[0]);
    // Exercise increasing chain sizes and verify every descriptor remains
    // reachable in order, with no searches through previously appended nodes.
    for (count = 1; count <= 16; count *= 2) {
        transfer.ElementCount = count;
        walks = g_chainWalks;
        allocations = g_descriptorAllocations;
        frees = g_descriptorFrees;
        CHECK(XhciTransferPrepare(&controller, &transfer, &endpoint) == OS_EOK);
        CHECK(g_chainWalks == walks);
        descriptor = transfer.RootElement;
        for (i = 0; i < count; i++) {
            CHECK(descriptor == &g_descriptors[i]);
            CHECK(descriptor->Transfer == &transfer && descriptor->Endpoint == &endpoint);
            descriptor = __DescriptorNext(&controller, descriptor);
        }
        CHECK(descriptor == NULL);
        CHECK(XhciTransferPrepare(&controller, &transfer, &endpoint) == OS_EOK);
        CHECK(g_descriptorAllocations == allocations + count);
        XhciTransferCleanup(&controller, &transfer);
        CHECK(transfer.RootElement == NULL && g_descriptorFrees == frees + count);
    }
    puts("xHCI descriptor chains: linear append, ordering and cleanup passed");
}

static void
__TestDeviceSegmentBuilder(void)
{
    struct DMASegment segments[] = {
        { 0x10000fffcULL, 4 },
        { 0x20000fffcULL, 4 },
        { 0x30000fffcULL, 12 }
    };
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_CONTROL,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8,
        .Dma = { .Segments = segments, .SegmentCount = 3 }
    };
    struct TransferElement elements[8] = { 0 };
    int                    count;

    // Setup spans two discontiguous segments; data crosses a 64 KiB boundary.
    CHECK(__BuildTransferElements(&transfer, 20, NULL, &count) == OS_EOK && count == 4);
    CHECK(__BuildTransferElements(&transfer, 20, elements, &count) == OS_EOK);
    CHECK(elements[0].Type == TRANSFERELEMENT_TYPE_SETUP && elements[0].Length == 8);
    CHECK(elements[1].Data.Address == 0x30000fffcULL && elements[1].Length == 4);
    CHECK(elements[2].Data.Address == 0x300010000ULL && elements[2].Length == 8);
    CHECK(elements[3].Type == TRANSFERELEMENT_TYPE_OUT && elements[3].Length == 0);
    CHECK(__BuildTransferElements(&transfer, 21, NULL, &count) == OS_EINVALPARAMS);
    CHECK(__BuildTransferElements(&transfer, 7, NULL, &count) == OS_EINVALPARAMS);

    transfer.Type = USBTRANSFER_TYPE_BULK;
    transfer.Direction = USBTRANSFER_DIRECTION_OUT;
    transfer.Dma.Segments = &segments[2];
    transfer.Dma.SegmentCount = 1;
    memset(elements, 0, sizeof(elements));
    CHECK(__BuildTransferElements(&transfer, 12, elements, &count) == OS_EOK && count == 3);
    CHECK(elements[2].Type == TRANSFERELEMENT_TYPE_OUT && elements[2].Length == 0);

    transfer.Type = USBTRANSFER_TYPE_ISOC;
    transfer.Direction = USBTRANSFER_DIRECTION_IN;
    memset(elements, 0, sizeof(elements));
    CHECK(__BuildTransferElements(&transfer, 12, elements, &count) == OS_EOK && count == 3);
    CHECK(elements[2].Type == TRANSFERELEMENT_TYPE_IN && elements[2].Length == 0);
}
