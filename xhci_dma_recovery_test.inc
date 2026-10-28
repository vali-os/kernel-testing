/** Faults after confirmed hardware stop must recover without another IRQ. */
static void
__TestCancelReclaimRetry(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;

    __InitializeRecovery(&fixture, &transfer);
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    g_failSync = 1;
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(fixture.Endpoint.State == XHCI_ENDPOINT_REBUILD_PENDING);
    CHECK(g_cancelController.RecoveryPending && transfer.Dma.DeviceOwned);
    CHECK(!transfer.Dma.HardwareActive && fixture.Endpoint.TransferRing.Used == 0);
    CHECK(g_notifications == 0 && g_destroyed == 0);
    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_cancelController.RecoveryPending && g_destroyed == 0);

    // Shutdown suppresses both command recovery and ready-transfer rearming.
    g_cancelController.Stopping = true;
    g_failSync = 0;
    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_destroyed == 0 && transfer.Dma.DeviceOwned);
    CHECK(HCITransferQueue(&transfer) == OS_EBUSY);
    CHECK(XhciTransferSubmit(&g_cancelController, &fixture.Endpoint, &transfer) == OS_EBUSY);
    g_cancelController.Stopping = false;

    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_destroyed == 1 && g_notifications == 1);
    CHECK(g_lastResult == USBTRANSFERCODE_CANCELLED && !g_cancelController.RecoveryPending);
    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_destroyed == 1 && g_notifications == 1);
    __DestroyRecovery(&fixture);
}

static void
__TestReclaimBeforeResubmit(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t first;
    UsbManagerTransfer_t second;
    XhciTransferDescriptor_t* descriptor;

    __InitializeRecovery(&fixture, &first);
    second = (UsbManagerTransfer_t) {
        .DeviceID = 77,
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8,
        .Address = first.Address,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = (struct DMAContext*)42, .BufferLength = 64 }
    };
    ELEMENT_INIT(&second.ListHeader, 0, &second);
    list_append(&g_cancelController.Base.TransactionList, &second.ListHeader);
    CHECK(USBTransferDmaPrepare(&second) == OS_EOK);
    CHECK(HCITransferQueue(&second) == OS_EOK);
    CHECK(HCITransferDequeue(&second) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    g_failMappingSync = second.Dma.Mapping;
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(!first.Dma.DeviceOwned && second.Dma.DeviceOwned);
    CHECK(!first.Dma.HardwareActive && !second.Dma.HardwareActive);
    CHECK(fixture.Endpoint.TransferRing.Used == 0);
    CHECK(g_destroyed == 0 && g_notifications == 0);
    HciTimerCallback(&g_cancelController.Base);
    CHECK(!first.Dma.DeviceOwned && fixture.Endpoint.TransferRing.Used == 0);

    g_failMappingSync = NULL;
    HciTimerCallback(&g_cancelController.Base);
    CHECK(g_destroyed == 1 && g_notifications == 1);
    CHECK(first.Dma.DeviceOwned && first.Dma.HardwareActive);
    descriptor = first.RootElement;
    CHECK(fixture.Endpoint.TRBOwners[descriptor->FirstTrbIndex] == descriptor);
    CHECK(fixture.Endpoint.TransferRing.Used == 1);
    // Only the surviving transfer is visible; cancelled trailing TRBs are invalid.
    CHECK(!(fixture.Endpoint.TransferRing.Trbs[1].Control & XHCI_TRB_CONTROL_CYCLE));
    CHECK(HCITransferDequeue(&first) == OS_EOK);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
    CHECK(g_destroyed == 2 && g_maps == 2);
    CHECK(XhciRingDestroy(&fixture.Endpoint.TransferRing) == OS_EOK);
    CHECK(XhciRingDestroy(&g_cancelController.CommandRing) == OS_EOK);
    CHECK(g_maps == 0);
}

static void
__TestDetachReleaseRetries(void)
{
    struct __RecoveryFixture fixture;
    UsbManagerTransfer_t transfer;
    XhciController_t* controller = &g_cancelController;
    XhciDevice_t* device;
    XhciEndpoint_t* endpoint;
    XhciTransferDescriptor_t* descriptor;
    XhciDevice_t replacement = { 0 };
    reg64_t dcbaa[2] = { 0 };
    unsigned int failure;

    for (failure = 0; failure < 4; failure++) {
        __InitializeRecovery(&fixture, &transfer);
        // Detach frees these objects; replace the fixture's stack owners.
        device = malloc(sizeof(*device));
        endpoint = malloc(sizeof(*endpoint));
        CHECK(device != NULL && endpoint != NULL);
        *device = fixture.Device;
        *endpoint = fixture.Endpoint;
        endpoint->Device = device;
        device->Endpoints[3] = endpoint;
        descriptor = transfer.RootElement;
        descriptor->Endpoint = endpoint;
        list_construct(&controller->Devices);
        list_construct(&controller->XhciEndpoints);
        ELEMENT_INIT(&device->Header, 0, device);
        ELEMENT_INIT(&endpoint->Header, 0, endpoint);
        list_append(&controller->Devices, &device->Header);
        list_append(&controller->XhciEndpoints, &endpoint->Header);
        controller->Slots[1] = device;
        controller->DCBaa = dcbaa;
        CHECK(XhciDmaAllocate((struct DMAContext*)42, 4096, &device->DeviceContextDMA,
            &device->DeviceContextDeviceAddress, NULL) == OS_EOK);
        CHECK(XhciDmaAllocate((struct DMAContext*)42, 4096, &device->InputContextDMA,
            &device->InputContextDeviceAddress, NULL) == OS_EOK);

        CHECK(HCIDeviceDetach(&controller->Base, &transfer.Address) == OS_EOK);
        if (failure == 0) g_failSync = 1;
        if (failure == 1) g_failBufferFree = endpoint->TransferRing.BufferHandle;
        if (failure == 2) g_failBufferFree = device->DeviceContextDMA;
        if (failure == 3) g_failBufferFree = device->InputContextDMA;
        __CompleteRecoveryCommand(XHCI_TRB_COMPLETION_SUCCESS);
        CHECK(device->State == XHCI_DEVICE_RELEASE_PENDING);
        CHECK(controller->RecoveryPending && controller->Devices.count == 1);
        CHECK(controller->Slots[1] == NULL);
        CHECK(g_destroyed == (failure == 0 ? 0 : 1));
        if (failure < 2) {
            CHECK(HCIEndpointReset(&controller->Base, &transfer.Address) == OS_EBUSY);
            CHECK(controller->CommandRing.Used == 0);
        }
        CHECK(HCIDeviceDetach(&controller->Base, &transfer.Address) == OS_EOK);
        CHECK(controller->CommandRing.Used == 0);
        HciTimerCallback(&controller->Base);
        CHECK(controller->Devices.count == 1 && controller->RecoveryPending);

        // A later cleanup retry must not clear a slot assigned to another device.
        controller->Slots[1] = &replacement;
        dcbaa[1] = 0x12345000;
        g_failSync = 0;
        g_failBufferFree = NULL;
        HciTimerCallback(&controller->Base);
        CHECK(controller->Devices.count == 0 && controller->XhciEndpoints.count == 0);
        CHECK(controller->Slots[1] == &replacement && dcbaa[1] == 0x12345000);
        CHECK(__DeviceGet(controller, &transfer.Address) == NULL);
        CHECK(g_destroyed == 1 && g_notifications == 1);
        CHECK(g_lastResult == USBTRANSFERCODE_CANCELLED);
        CHECK(g_maps == 1 && !controller->RecoveryPending);
        HciTimerCallback(&controller->Base);
        CHECK(g_destroyed == 1 && g_notifications == 1);
        CHECK(XhciRingDestroy(&controller->CommandRing) == OS_EOK);
        CHECK(g_maps == 0);
    }
}

static void
__TestStoppedRingInvalidation(void)
{
    XhciEndpoint_t endpoint = { 0 };
    unsigned int cycle;
    unsigned int i;
    unsigned int expected;

    CHECK(XhciRingInitialize((struct DMAContext*)42, &endpoint.TransferRing, 8) == OS_EOK);
    for (cycle = 0; cycle < 2; cycle++) {
        memset(endpoint.TransferRing.Trbs, 0xff, 8 * sizeof(XhciTrb_t));
        endpoint.TransferRing.DequeueIndex = 3;
        endpoint.TransferRing.DequeueCycleState = cycle;
        __ResetRingOwnership(&endpoint);
        for (i = 0; i < 8; i++) {
            expected = i < 3 ? !cycle : cycle;
            CHECK(!!(endpoint.TransferRing.Trbs[i].Control & XHCI_TRB_CONTROL_CYCLE) != expected);
        }
        CHECK(endpoint.TransferRing.Trbs[7].Parameter == endpoint.TransferRing.DeviceBase);
        CHECK(XHCI_TRB_CONTROL_TYPE_GET(endpoint.TransferRing.Trbs[7].Control) == XHCI_TRB_TYPE_LINK);
    }
    CHECK(XhciRingDestroy(&endpoint.TransferRing) == OS_EOK);
}
