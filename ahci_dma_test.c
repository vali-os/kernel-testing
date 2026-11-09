// AHCI regression scenarios use the production driver with a controlled device model.
#include "ahci_dma_model.inc"

static AhciController_t*
__CreateController(
    _In_ bool bits64,
    _Out_ oserr_t* status)
{
    BusDevice_t* device = __AhciAllocate(sizeof(*device));
    AhciController_t* controller = NULL;

    memset(g_mmio, 0, sizeof(g_mmio));
    device->Base.Id = 7;
    device->IoSpaces[0].Type = DeviceIoMemoryBased;
    ((AHCIGenericRegisters_t*)g_mmio)->Capabilities = (bits64 ? AHCI_CAPABILITIES_S64A : 0) | (3 << 8);
    ((AHCIGenericRegisters_t*)g_mmio)->PortsImplemented = 1;
    *status = AhciControllerCreate(device, &controller);
    if (controller != NULL) {
        list_append(&controllers, &controller->header);
    }
    else {
        __AhciFree(device);
    }
    g_controller = controller;
    return controller;
}

static void
__ExpectEmpty(void)
{
    if (g_worker != NULL) {
        CHECK(g_cleanupControllers.count == 0);
        g_worker(NULL, NULL);
        g_worker = NULL;
    }
    CHECK(g_allocations == 0 && g_contexts == 0 && g_buffers == 0 && g_mappings == 0 && g_owned == 0);
    CHECK(g_irqs == 0 && g_io == 0 && g_events == 0 && g_iosets == 0);
    CHECK(controllers.count == 0 && g_cleanupControllers.count == 0 && !g_cleanupWorkerRunning);
}

static AhciDevice_t
__Device(
    _In_ AhciController_t* controller)
{
    AhciDevice_t device = {0};

    device.Controller = controller;
    device.Port = controller->Ports[0];
    device.SectorSize = 512;
    device.SectorCount = UINT64_C(0x100000000);
    device.AddressingMode = AHCI_DEVICE_MODE_LBA48;
    device.HasDMAEngine = 1;
    return device;
}

static AhciTransaction_t*
__Request(
    _In_ AhciDevice_t* device,
    _In_ size_t sectors)
{
    struct gracht_message message = {0};

    CHECK(AhciTransactionStorageCreate(device, &message, __STORAGE_OPERATION_READ, 8, 42, 128, sectors) ==
          OS_EOK);
    return device->Port->Transactions.tail->value;
}

static void
__Complete(
    _In_ AhciDevice_t* device,
    _In_ AhciTransaction_t* transaction,
    _In_ size_t bytes)
{
    AHCICommandList_t* list = DMABufferData(device->Port->Dma.CommandList);
    int slot = __GetTransferKey(transaction);

    __Registers()->CI &= ~(1U << slot);
    __Registers()->SACT &= ~(1U << slot);
    list->Headers[slot].PRDByteCount = (uint32_t)bytes;
    atomic_store(&device->Controller->InterruptResource.PortInterruptStatus[0], AHCI_PORT_IE_DHRE);
    atomic_store(&device->Controller->InterruptResource.ControllerInterruptStatus, 1);
    OnInterrupt(device->Controller);
}

static void
__TestSetupFailures(void)
{
    AhciController_t* controller;
    oserr_t status;
    int i;

    for (i = 1; i <= 4; i++) {
        g_failAllocate = g_allocateCalls + i;
        controller = __CreateController(true, &status);
        CHECK(status == OS_EOOM && controller != NULL && g_buffers == i - 1);
        g_failAllocate = 0;
        CHECK(AhciControllerDestroy(controller) == OS_EOK);
        __ExpectEmpty();
    }
    g_openFailure = 1;
    controller = __CreateController(false, &status);
    CHECK(status == OS_EUNKNOWN && controller->DmaContext != NULL);
    CHECK(controller->DmaContext->Limit == UINT32_MAX);
    g_closeFailure = 1;
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && controllers.count == 1 && g_io == 1);
    g_closeFailure = g_openFailure = 0;
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();

    g_failMalloc = g_mallocCalls + 3;
    controller = __CreateController(true, &status);
    CHECK(status == OS_EOOM && controller != NULL && controller->Ports[0] == NULL);
    g_failMalloc = 0;
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

static void
__TestAddressesAndSlots(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    AhciTransaction_t* first;
    AhciTransaction_t* second;
    AHCICommandList_t* list;
    AHCICommandTable_t* table;
    oserr_t status;
    int calls;
    int controls;
    int i;

    for (i = 0; i < 2; i++) {
        controller = __CreateController(i != 0, &status);
        CHECK(status == OS_EOK);
        device = __Device(controller);
        list = DMABufferData(device.Port->Dma.CommandList);
        CHECK(__Registers()->FISBaseAddress == (uint32_t)device.Port->Dma.ReceivedFis->Segment.Address);
        CHECK(__Registers()->FISBaseAdressUpper ==
              (uint32_t)(device.Port->Dma.ReceivedFis->Segment.Address >> 32));
        CHECK(list->Headers[3].CmdTableBaseAddress ==
              (uint32_t)(device.Port->Dma.CommandTables->Segment.Address + 3 * 4096));
        CHECK(list->Headers[3].CmdTableBaseAddressUpper == (i != 0 ? 1U : 0U));
        calls = g_mapCalls + g_beginCalls + g_endCalls + g_unmapCalls;
        controls = g_controls;
        CHECK(AhciTransactionControlCreate(&device, AtaPIOIdentifyDevice, 512, __STORAGE_OPERATION_READ) ==
              OS_EOK);
        first = device.Port->Transactions.head->value;
        CHECK(g_mapCalls + g_beginCalls + g_endCalls + g_unmapCalls == calls);
        __Complete(&device, first, 512);
        CHECK(g_controls == controls + 1 && g_mapCalls + g_beginCalls + g_endCalls + g_unmapCalls == calls);
        first = __Request(&device, 1);
        second = __Request(&device, 1);
        CHECK(__GetTransferKey(first) == 0 && __GetTransferKey(second) == 1);
        table = (void*)((uint8_t*)DMABufferData(device.Port->Dma.CommandTables) + 4096);
        CHECK(table->FISCommand[0] == FISRegisterH2D);
        CHECK(table->PrdtEntry[0].DataBaseAddressUpper == (i != 0 ? 1U : 0U));
        CHECK(table->PrdtEntry[0].Descriptor == (511U | AHCI_PRDT_IOC));
        CHECK(table->PrdtEntry[1].Descriptor == 0);
        controls = g_responses;
        AhciPortInterruptHandler(controller, device.Port);
        CHECK(g_responses == controls && g_owned == 2);
        __Complete(&device, first, 512);
        CHECK(g_owned == 1 && second->State == TransactionInProgress);
        __Complete(&device, second, 512);
        CHECK(g_owned == 0 && g_responseSectors == 1);
        CHECK(AhciControllerDestroy(controller) == OS_EOK);
        __ExpectEmpty();
    }
}

static void
__TestSplitAndShort(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    AhciTransaction_t* transaction;
    AHCICommandTable_t* table;
    AHCICommandList_t* list;
    oserr_t status;
    int maps, begins, ends, unmaps;
    unsigned int i;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    maps = g_mapCalls;
    begins = g_beginCalls;
    ends = g_endCalls;
    unmaps = g_unmapCalls;
    g_segments[0] = (struct DMASegment){UINT64_C(0x200000000), 600};
    for (i = 1; i < 248; i++) {
        g_segments[i] = (struct DMASegment){UINT64_C(0x200010000) + i * 4096, 2};
    }
    g_segments[248] = (struct DMASegment){UINT64_C(0x300000000), 442};
    g_segmentCount = 249;
    transaction = __Request(&device, 3);
    CHECK(transaction->CommandBytes == 1024 && transaction->BytesLeft == 512);
    CHECK(g_lastMapOffset == 128 && g_lastMapLength == 1536 && g_lastDirection == DMAFromDevice);
    table = DMABufferData(device.Port->Dma.CommandTables);
    list = DMABufferData(device.Port->Dma.CommandList);
    CHECK(list->Headers[0].TableLength == 213);
    CHECK(table->PrdtEntry[212].Descriptor & AHCI_PRDT_IOC);
    __Complete(&device, transaction, 1024);
    CHECK(transaction->CommandBytes == 512 && transaction->Dma.Completed == 1024);
    CHECK(g_beginCalls == begins + 1 && g_endCalls == ends);
    __Complete(&device, transaction, 512);
    CHECK(g_mapCalls == maps + 1 && g_beginCalls == begins + 1 && g_endCalls == ends + 1 &&
          g_unmapCalls == unmaps + 1);
    CHECK(g_lastEndLength == 1536 && g_responseSectors == 3);
    g_segmentCount = 0;
    transaction = __Request(&device, 16384);
    CHECK(list->Headers[0].TableLength == 2);
    CHECK((table->PrdtEntry[0].Descriptor & ~AHCI_PRDT_IOC) == AHCI_PRDT_MAX_LENGTH - 1);
    __Complete(&device, transaction, 512);
    CHECK(g_lastEndLength == 512 && g_responseStatus == OS_EDEVFAULT && g_responseSectors == 1);
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

static void
__TestRequestFailures(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    AhciTransaction_t* transaction;
    struct gracht_message message = {0};
    oserr_t status;
    int responses;
    int ends;
    int calls;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    g_mapFailure = 1;
    CHECK(AhciTransactionStorageCreate(&device, &message, __STORAGE_OPERATION_READ, 0, 42, 0, 1) == OS_EOOM);
    CHECK(device.Port->Transactions.count == 0);
    g_mapFailure = 0;
    g_beginFailure = g_unmapFailure = 1;
    responses = g_responses;
    CHECK(AhciTransactionStorageCreate(&device, &message, __STORAGE_OPERATION_WRITE, 0, 42, 0, 1) ==
          OS_EUNKNOWN);
    CHECK(g_lastDirection == DMAToDevice && g_owned == 0 && g_mappings == 1);
    CHECK(device.Port->CleanupTransactions.count == 1 && g_responses == responses);
    g_beginFailure = g_unmapFailure = 0;
    g_worker(NULL, NULL);
    CHECK(device.Port->Transactions.count == 0 && g_responses == responses);

    transaction = __Request(&device, 2);
    g_endFailure = 1;
    __Complete(&device, transaction, 1024);
    CHECK(g_owned == 1 && g_responses == responses && transaction->Dma.Completed == 1024);
    ends = g_endCalls;
    AhciTransactionHandleResponse(controller, device.Port, transaction, 1024);
    CHECK(g_endCalls == ends && transaction->Dma.Completed == 1024);
    g_endFailure = 0;
    g_unmapFailure = 1;
    AhciTransactionsRetry(controller, device.Port);
    CHECK(g_owned == 0 && g_responses == responses + 1 && g_mappings == 1);
    g_unmapFailure = 0;
    g_worker(NULL, NULL);
    CHECK(g_responses == responses + 1 && device.Port->Transactions.count == 0);

    atomic_store(&device.Port->Slots, (1U << device.Port->SlotCount) - 1);
    calls = g_beginCalls;
    CHECK(AhciTransactionStorageCreate(&device, &message, __STORAGE_OPERATION_READ, 0, 42, 0, 1) == OS_EBUSY);
    CHECK(g_beginCalls == calls && g_mappings == 0);
    atomic_store(&device.Port->Slots, 0);
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

static void
__TestTeardown(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    oserr_t status;
    int frees;
    int responses;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    __Request(&device, 1);
    __Registers()->CommandAndStatus = AHCI_PORT_ST | AHCI_PORT_CR | AHCI_PORT_FRE | AHCI_PORT_FR;
    g_irqFailure = 1;
    frees = g_freeCalls;
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && g_owned == 1 && g_freeCalls == frees);
    g_irqFailure = 0;
    g_stopBlock = AHCI_PORT_CR;
    CHECK(AhciControllerDestroy(controller) == OS_ETIMEOUT && g_owned == 1 && g_freeCalls == frees);
    g_stopBlock = AHCI_PORT_FR;
    CHECK(AhciControllerDestroy(controller) == OS_ETIMEOUT && g_owned == 1 && g_freeCalls == frees);
    g_stopBlock = 0;
    g_endFailure = 1;
    CHECK(AhciControllerDestroy(controller) == OS_EUNKNOWN && g_owned == 1 && g_freeCalls == frees);
    g_endFailure = 0;
    g_unmapFailure = 1;
    responses = g_responses;
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && g_owned == 0 && g_freeCalls == frees);
    CHECK(g_responses == responses + 1 && g_responseStatus == OS_ECANCELLED && g_lastEndLength == 0);
    g_unmapFailure = 0;
    g_failFree = device.Port->Dma.CommandTables;
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && device.Port->Dma.ReceivedFis == NULL);
    CHECK(g_buffers == 3 && controller->Ports[0] == device.Port && g_responses == responses + 1);
    g_failFree = NULL;
    g_closeFailure = 1;
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && controller->Ports[0] == NULL && g_buffers == 0 &&
          g_io == 1);
    g_closeFailure = 0;
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    __Registers()->CommandAndStatus = AHCI_PORT_ST | AHCI_PORT_CR | AHCI_PORT_FRE | AHCI_PORT_FR;
    g_stopBlock = AHCI_PORT_CR;
    g_recoverAfterSleep = 9;
    OnUnload();
    CHECK(controllers.count == 0);
    __ExpectEmpty();
}

static void
__LeaveOverlappingUser(void)
{
    CHECK(g_controller->Cleaning >= 2);
    // Model another callback that remains suspended after this response returns.
    g_controller->Cleaning++;
}

static void
__TestNestedLifetime(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    AhciTransaction_t* transaction;
    oserr_t status;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    transaction = __Request(&device, 1);
    g_endFailure = 1;
    __Complete(&device, transaction, 512);
    g_endFailure = 0;
    g_responseHook = __LeaveOverlappingUser;
    g_worker(NULL, NULL);
    CHECK(controller->Cleaning == 1);
    CHECK(AhciControllerDestroy(controller) == OS_EBUSY && g_buffers == 4);
    controller->Cleaning--;
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

static AhciDevice_t g_reuseDevice;
static AhciTransaction_t* g_reusedRequest;

static void
__ReuseCompletedSlot(void)
{
    // The interrupt took its slot mask before this callback. Finish an older
    // cleanup and submit a new command into the same slot before it resumes.
    g_unmapFailure = 0;
    AhciTransactionsRetry(g_reuseDevice.Controller, g_reuseDevice.Port);
    g_reusedRequest = __Request(&g_reuseDevice, 1);
    CHECK(__GetTransferKey(g_reusedRequest) == 1);
}

static void
__TestSlotReuseDuringResponse(void)
{
    AhciController_t* controller;
    AhciTransaction_t* first;
    AhciTransaction_t* second;
    oserr_t status;
    int responses;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    g_reuseDevice = __Device(controller);
    first = __Request(&g_reuseDevice, 1);
    second = __Request(&g_reuseDevice, 1);
    g_unmapFailure = 1;
    __Complete(&g_reuseDevice, second, 512);
    CHECK(second->State == TransactionCleanup);
    responses = g_responses;
    g_responseHook = __ReuseCompletedSlot;
    __Complete(&g_reuseDevice, first, 512);
    CHECK(g_responses == responses + 1);
    CHECK(g_reusedRequest->State == TransactionInProgress && g_reusedRequest->Dma.DeviceOwned);
    CHECK(__Registers()->CI & 2);
    __Complete(&g_reuseDevice, g_reusedRequest, 512);
    CHECK(g_responses == responses + 2);
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

static void
__TestDiskRequirements(void)
{
    AhciController_t* controller;
    AhciDevice_t device;
    oserr_t status;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    g_logicalDevice = &device;
    // The disk ID is 99; controller ID 7 must not be used by filesystem IPC.
    ctt_driver_ioctl_invocation(NULL, 99, OSIOCTLREQUEST_BUS_CONTROL, NULL, 0);
    CHECK(g_ioctlStatus == OS_ENOTSUPPORTED);
    ctt_driver_ioctl_invocation(NULL, 7, OSIOCTLREQUEST_BUS_CONTROL, NULL, 0);
    CHECK(g_ioctlStatus == OS_ENOENT);
    g_logicalDevice = NULL;
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
}

int
main(void)
{
    __TestSetupFailures();
    __TestAddressesAndSlots();
    __TestSplitAndShort();
    __TestRequestFailures();
    __TestTeardown();
    __TestNestedLifetime();
    __TestSlotReuseDuringResponse();
    __TestDiskRequirements();
    CHECK(g_warnings != 0);
    puts("AHCI DMA: translated addresses, slots, split budgets, failures, teardown and nested lifetime "
         "passed");
    return 0;
}
