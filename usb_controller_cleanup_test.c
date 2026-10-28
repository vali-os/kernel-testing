// Exercise the real DDK owners, controller teardown and manager retry queue.
#define main __DdkFixtureMain
#include "ddk_dma_test.c"
#undef main
#include "../modules/serial/usb/xhci/dwc3.c"
#include "../modules/serial/usb/xhci/controller.c"
#include "../modules/serial/usb/common/manager.c"
#include "../librt/libds/list.c"
#include "../librt/libds/hashtable.c"

static XhciController_t* g_controller;
static XhciOperationalRegisters_t g_registers;
static Device_t g_device = { .Id = 7 };
static int g_interruptLive;
static int g_interruptFailure;
static int g_stopFailure;
static int g_detachFailure;
static int g_stops;
static int g_removed;
static int g_reenter;
static int g_timerCalls;
static uuid_t g_destroyOther = UUID_INVALID;
static uint32_t g_dwcRegisters[0x10000 / 4];
static unsigned int g_dwcWrites;
static unsigned int g_dwcFailWrite;
static int g_dwcPreparing;

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    assert(g_controller->Prepared);
    assert(io == &g_controller->RegisterIo);
    assert(width == 4 && offset + width <= sizeof(g_dwcRegisters));
    return g_dwcRegisters[offset / 4];
}

oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      width)
{
    (void)ReadDeviceIo(io, offset, width);
    if (!g_dwcPreparing) {
        assert(g_controller->Halted && !g_interruptLive);
        assert(g_controller->Base.DmaContext == NULL);
    }
    g_dwcRegisters[offset / 4] = (uint32_t)value;
    g_dwcWrites++;
    return g_dwcWrites == g_dwcFailWrite ? OS_EUNKNOWN : OS_EOK;
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    XhciController_t* controller = (void*)((char*)interrupt -
        offsetof(UsbManagerController_t, Interrupt));

    assert(controller->Halted);
    if (g_interruptFailure) {
        return OS_EBUSY;
    }
    g_interruptLive = 0;
    return OS_EOK;
}

oserr_t
XhciQueueDestroy(
    _In_ XhciController_t* controller)
{
    assert(controller->Halted && !g_interruptLive);
    // Keep real partial buffer cleanup: unmap may succeed before detach fails.
    return DMAFree(&controller->DCBaaDMA);
}

oserr_t
DeviceActivate(
    _In_ uuid_t                device,
    _In_ enum DeviceActivation stage)
{
    XhciController_t* controller = (XhciController_t*)UsbManagerGetController(device);
    UsbManagerController_t* other;

    assert((device == 7 || device == 8) && stage == DeviceStop);
    // This models the native provider's real stop preconditions. PCI follows
    // the same sequence after XhciHalt has already stopped its DMA engine.
    assert(g_contexts == 0 && g_mappings == 0 && g_shmOwners == 0);
    assert(!g_interruptLive && controller->Base.DmaContext == NULL);
    assert(controller->Dwc3.Registers == NULL && controller->Dwc3.Changed == 0);
    g_stops++;
    if (g_reenter) {
        assert(HCIControllerDestroy(&g_controller->Base) == OS_EBUSY);
        g_reenter = 0;
    }
    if (g_destroyOther != UUID_INVALID) {
        other = UsbManagerGetController(g_destroyOther);
        g_destroyOther = UUID_INVALID;
        assert(HCIControllerDestroy(other) == OS_EOK);
    }
    if (g_stopFailure && device == 7) {
        return OS_EUNKNOWN;
    }
    // Any subsequent MMIO access is a bug: the provider can power off now.
    controller->OpRegisters = (XhciOperationalRegisters_t*)(uintptr_t)1;
    return OS_EOK;
}

oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* io)
{
    XhciController_t* controller = (void*)((char*)io - offsetof(XhciController_t, RegisterIo));

    assert(!controller->Prepared);
    return g_detachFailure ? OS_EUNKNOWN : OS_EOK;
}

void
UsbControllerUnregister(
    _In_ uuid_t device)
{
    assert(UsbManagerGetController(device) == NULL);
    g_removed++;
}

void
HciTimerCallback(
    _In_ UsbManagerController_t* controller)
{
    assert(!controller->DestroyPending);
    g_timerCalls++;
}

static void
__CreateController(
    _In_ int       resources,
    _In_ Device_t* device)
{
    struct DMARequirements requirements = {
        .Length = 256, .Alignment = 64, .Boundary = 0x10000, .Contiguous = true
    };

    g_controller = calloc(1, sizeof(*g_controller));
    assert(g_controller != NULL);
    g_controller->Base.Device = device;
    g_controller->Base.event_descriptor = -1;
    g_controller->Prepared = true;
    memset((void*)&g_registers, 0, sizeof(g_registers));
    g_registers.UsbStatus = XHCI_OP_USBSTS_HALTED;
    g_registers.UsbCommand = XHCI_OP_USBCMD_RUN | XHCI_OP_USBCMD_INTERRUPTER;
    g_controller->OpRegisters = &g_registers;
    g_controller->Base.IoBase = &g_controller->RegisterIo;
    list_construct(&g_controller->Base.TransactionList);
    list_construct(&g_controller->Base.ReadyTransfers);
    list_construct(&g_controller->Base.WaitingTransfers);
    hashtable_construct(&g_controller->Base.Endpoints, 0,
        sizeof(struct USBManagerEndpointToggle), endpoint_hash, endpoint_cmp);
    hashtable_set(&g_controllers, &(struct usb_controller_device_index) {
        .deviceId = device->Id, .pointer = &g_controller->Base
    });
    g_interruptLive = resources;
    if (resources) {
        assert(DMAOpen(7, UINT64_MAX, &g_controller->Base.DmaContext) == OS_EOK);
        assert(DMAAllocate(g_controller->Base.DmaContext, &requirements,
            &g_controller->DCBaaDMA) == OS_EOK);
    }
}

static void
__TestTeardownRetries(void)
{
    int removed = g_removed;
    int timerCalls = g_timerCalls;
    int stops = g_stops;
    int unmaps;

    __CreateController(1, &g_device);
    // First prove halt failure retains everything; no interrupt or DMA release.
    g_registers.UsbStatus = 0;
    assert(HCIControllerDestroy(&g_controller->Base) == OS_ETIMEOUT);
    assert(g_interruptLive && g_mappings == 1 && g_contexts == 1);
    assert(g_destroyPending.count == 1 && !g_controller->Destroying);
    g_registers.UsbStatus = XHCI_OP_USBSTS_HALTED;
    g_interruptFailure = 1;
    __RetryControllerDestruction();
    assert(g_interruptLive && g_mappings == 1 && g_stops == stops);
    assert(g_destroyPending.count == 1);
    assert(!(g_registers.UsbCommand & XHCI_OP_USBCMD_RUN));
    hashtable_enumerate(&g_controllers, __CheckController, NULL);
    assert(g_timerCalls == timerCalls);

    g_interruptFailure = 0;
    g_failStatus = OS_EUNKNOWN;
    g_failSyscall = 84;
    __RetryControllerDestruction();
    assert(!g_interruptLive && g_mappings == 1 && g_destroyPending.count == 1);
    g_failSyscall = 61;
    __RetryControllerDestruction();
    assert(g_mappings == 0 && g_shmOwners == 1 && g_contexts == 1);
    unmaps = g_unmaps;
    g_failSyscall = 85;
    __RetryControllerDestruction();
    assert(g_shmOwners == 0 && g_contexts == 1 && g_unmaps == unmaps);
    g_failSyscall = -1;
    g_releaseFailure = 1;
    __RetryControllerDestruction();
    assert(g_contexts == 0 && g_controller->Base.DmaContext != NULL);
    assert(g_stops == stops);

    g_releaseFailure = 0;
    g_stopFailure = 1;
    g_reenter = 1;
    __RetryControllerDestruction();
    assert(g_controller->Prepared && g_removed == removed);
    g_stopFailure = 0;
    g_detachFailure = 1;
    __RetryControllerDestruction();
    assert(!g_controller->Prepared && g_destroyPending.count == 1);
    stops = g_stops;
    g_detachFailure = 0;
    __RetryControllerDestruction();
    assert(g_removed == removed + 1 && g_destroyPending.count == 0);
    assert(g_stops == stops && g_allocations == 0 && g_dmaContexts == NULL);
    assert(UsbManagerGetController(7) == NULL);
    __RetryControllerDestruction();
    assert(g_removed == removed + 1);
}

static void
__TestFailedInitialization(void)
{
    int removed = g_removed;
    int calls;

    __CreateController(0, &g_device);
    g_controller->OpRegisters = NULL;
    g_failSyscall = 78;
    g_failStatus = OS_EOOM;
    g_releaseFailure = 1;
    assert(DMAOpen(7, UINT64_MAX, &g_controller->Base.DmaContext) == OS_EOOM);
    assert(g_controller->Base.DmaContext != NULL);
    calls = g_calls;
    assert(HCIControllerDestroy(&g_controller->Base) == OS_EUNKNOWN);
    assert(g_destroyPending.count == 1 && g_removed == removed);
    g_failSyscall = -1;
    g_releaseFailure = 0;
    __RetryControllerDestruction();
    assert(g_calls == calls && g_removed == removed + 1);
    assert(g_destroyPending.count == 0 && g_allocations == 0);
}

static void
__TestQueueRemovalDuringRetry(void)
{
    Device_t otherDevice = { .Id = 8 };
    XhciController_t* first;
    int removed = g_removed;

    __CreateController(0, &g_device);
    first = g_controller;
    UsbManagerDeferControllerDestroy(&first->Base);
    UsbManagerDeferControllerDestroy(&first->Base);
    __CreateController(0, &otherDevice);
    UsbManagerDeferControllerDestroy(&g_controller->Base);
    assert(g_destroyPending.count == 2);

    // A provider call may yield and unregister a different queued controller.
    // The monitor must not dereference a saved next node after that owner dies,
    // and a failure on the first controller must not prevent the other release.
    g_destroyOther = 8;
    g_stopFailure = 1;
    __RetryControllerDestruction();
    assert(g_removed == removed + 1 && g_destroyPending.count == 1);
    assert(UsbManagerGetController(7) == &first->Base);
    assert(UsbManagerGetController(8) == NULL);
    g_stopFailure = 0;
    __RetryControllerDestruction();
    assert(g_removed == removed + 2 && g_destroyPending.count == 0);
}

static void
__TestDwc3Cleanup(void)
{
    struct DeviceResources resources = {
        .Count = 1, .ParentPci = { 0x1de4, 1, 2 }
    };
    size_t length;
    unsigned int partial;
    unsigned int writes;
    int stops;

    for (partial = 0; partial < 2; partial++) {
        __CreateController(1, &g_device);
        memset(g_dwcRegisters, 0, sizeof(g_dwcRegisters));
        g_dwcRegisters[0xc120 / 4] = 0x55330000;
        g_dwcRegisters[0xc140 / 4] = 1;
        g_controller->RegisterIo.Type = DeviceIoMemoryBased;
        g_controller->RegisterIo.Access.Memory.Length = sizeof(g_dwcRegisters);
        g_controller->RegisterIo.Access.Memory.VirtualBase = (uintptr_t)g_dwcRegisters;
        g_dwcWrites = 0;
        g_dwcFailWrite = partial ? 3 : 0;
        g_dwcPreparing = 1;
        assert(Dwc3Prepare(&g_controller->Dwc3, &resources, &g_controller->RegisterIo, &length) ==
            (partial ? OS_EUNKNOWN : OS_EOK));
        g_dwcPreparing = 0;
        if (partial) {
            // Failed wrapper setup never reached the xHCI register parser.
            g_controller->OpRegisters = NULL;
        }
        stops = g_stops;
        g_dwcFailWrite = g_dwcWrites + 1;
        assert(HCIControllerDestroy(&g_controller->Base) == OS_EUNKNOWN);
        assert(g_controller->Dwc3.Registers != NULL && g_controller->Prepared);
        assert(g_stops == stops && g_destroyPending.count == 1);
        assert(!g_interruptLive && g_contexts == 0 && g_mappings == 0);
        g_dwcFailWrite = 0;
        g_stopFailure = 1;
        __RetryControllerDestruction();
        assert(g_controller->Dwc3.Registers == NULL && g_controller->Prepared);
        writes = g_dwcWrites;
        g_stopFailure = 0;
        g_detachFailure = 1;
        __RetryControllerDestruction();
        assert(!g_controller->Prepared && g_dwcWrites == writes);
        g_detachFailure = 0;
        __RetryControllerDestruction();
        assert(g_dwcWrites == writes && g_destroyPending.count == 0);
        assert(UsbManagerGetController(7) == NULL);
    }
}

static void
__TestCapabilityWindow(void)
{
    XhciController_t controller = { 0 };

    memset(g_dwcRegisters, 0, sizeof(g_dwcRegisters));
    controller.CapRegisters = (XhciCapabilityRegisters_t*)g_dwcRegisters;
    controller.RegisterLength = 0x8000;
    controller.HccParams1 = (0xc000U / 4) << 16;
    g_dwcRegisters[0xc000 / 4] = XHCI_EXTCAP_LEGACY_SUPPORT;
    g_dwcRegisters[0xc004 / 4] = 0x12345678;
    assert(__XhciDisableLegacySupport(&controller) == OS_ENOTSUPPORTED);
    assert(g_dwcRegisters[0xc000 / 4] == XHCI_EXTCAP_LEGACY_SUPPORT);
    assert(g_dwcRegisters[0xc004 / 4] == 0x12345678);

    controller.HccParams1 = (0x7ffcU / 4) << 16;
    g_dwcRegisters[0x7ffc / 4] = XHCI_EXTCAP_LEGACY_SUPPORT;
    assert(__XhciDisableLegacySupport(&controller) == OS_ENOTSUPPORTED);
    // A non-legacy capability needs only one word, but its next link is bounded.
    g_dwcRegisters[0x7ffc / 4] = 2 | (1U << 8);
    assert(__XhciDisableLegacySupport(&controller) == OS_ENOTSUPPORTED);
    g_dwcRegisters[0x7ffc / 4] = 2;
    assert(__XhciDisableLegacySupport(&controller) == OS_EOK);
}

int
main(void)
{
    hashtable_construct(&g_controllers, 0, sizeof(struct usb_controller_device_index),
        default_dev_hash, default_dev_cmp);
    __TestTeardownRetries();
    __TestFailedInitialization();
    __TestDwc3Cleanup();
    __TestCapabilityWindow();
    // Exercise the uninterrupted path too; provider stop still verifies that
    // both the interrupt allocation and DMA lease were released first.
    __CreateController(1, &g_device);
    assert(HCIControllerDestroy(&g_controller->Base) == OS_EOK);
    assert(UsbManagerGetController(7) == NULL && g_destroyPending.count == 0);
    __TestQueueRemovalDuringRetry();
    hashtable_destroy(&g_controllers);
    puts("USB controller cleanup: halt, IRQ, DMA, provider, MMIO, reentrancy and failed-init retries passed");
    return 0;
}
void
ReadVolatileMemory(
    _In_  const volatile void* source,
    _Out_ volatile void*       destination,
    _In_  size_t               length)
{
    assert(length == 4 && (uintptr_t)source != 1);
    *(volatile uint32_t*)destination = *(const volatile uint32_t*)source;
}
void
WriteVolatileMemory(
    _Out_ volatile void* destination,
    _In_  void*          source,
    _In_  size_t         length)
{
    assert(length == 4 && (uintptr_t)destination != 1);
    *(volatile uint32_t*)destination = *(uint32_t*)source;
}
