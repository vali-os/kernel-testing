#define __need_minmax
// Diagnostics must evaluate their register reads in this combined translation unit.
#define __TRACE
#ifndef XHCI_VERBOSE_DIAGNOSTICS
#define XHCI_VERBOSE_DIAGNOSTICS 1
#endif
#include <stdlib.h>
#include "../modules/serial/usb/common/hci.h"

static oserr_t __TestElementsNeeded(UsbManagerTransfer_t*, uint32_t,
    enum USBTransferDirection, int*);
static void __TestElementFill(UsbManagerTransfer_t*, uint32_t,
    enum USBTransferDirection);

static void* __TestReallocate(void* memory, size_t length);
#define realloc __TestReallocate
// Real xHCI element/ring code and common DMA preparation with translated addresses.
#define HCITransferElementsNeeded __TestElementsNeeded
#define HCITransferElementFill __TestElementFill
#include "../modules/serial/usb/common/transfer/dma.c"
#undef HCITransferElementsNeeded
#undef HCITransferElementFill
#include "../modules/serial/usb/xhci/transfer.c"
#include "../modules/serial/usb/xhci/dma.c"
#include "../modules/serial/usb/xhci/ring.c"
#include "../modules/serial/usb/xhci/mmio.c"
#include "../modules/serial/usb/xhci/interrupt.c"
#include "../modules/serial/usb/xhci/command.c"
static int __TestHubSleep(const struct timespec*, struct timespec*);
#define thrd_sleep __TestHubSleep
#include "../modules/serial/usb/xhci/hub.c"
#undef thrd_sleep
#include "../modules/serial/usb/xhci/diagnostics.c"
#include "../modules/serial/usb/xhci/ep0-diagnostics.c"
#include "../modules/serial/usb/xhci/port.c"
#include "../modules/serial/usb/xhci/endpoint.c"
#include "../modules/serial/usb/xhci/queue.c"
static oserr_t __TestStartupOpen(uuid_t, uint64_t, struct DMAContext**);
static oserr_t __TestStartupQueue(XhciController_t*);
static oserr_t __TestStartupRegister(UsbManagerController_t*);
#define DMAOpen __TestStartupOpen
#define XhciQueueInitialize __TestStartupQueue
#define UsbManagerRegisterController __TestStartupRegister
// Keep teardown failure stubs in the host fixture; exercise real startup MMIO.
static oserr_t __FixtureControllerDestroy(UsbManagerController_t* controller);
#define XhciHalt __FixtureHalt
#define HCIControllerDestroy __FixtureControllerDestroy
#include "../modules/serial/usb/xhci/controller.c"
#undef HCIControllerDestroy
#undef XhciHalt
#undef DMAOpen
#undef XhciQueueInitialize
#undef UsbManagerRegisterController
#include "../modules/serial/usb/common/scheduler/settings.c"
#include "../librt/libds/list.c"
#undef realloc
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

oserr_t
UsbSchedulerInitialize(
    _In_  UsbSchedulerSettings_t* settings,
    _Out_ UsbScheduler_t**        schedulerOut)
{
    // Check the real xHCI setup selects cached software pools, then stop
    // initialization before this fixture would need to emulate a controller.
    CHECK(settings->Flags == USB_SCHEDULER_SOFTWARE_ELEMENTS);
    CHECK(settings->PoolCount == 1);
    CHECK(settings->Pools[0].ElementBaseSize == sizeof(XhciTransferDescriptor_t));
    CHECK(*schedulerOut == NULL);
    return OS_EOOM;
}

struct DMAMapping {
    int Owned;
    size_t Offset;
    size_t ActiveLength;
    uuid_t Buffer;
    void* Backing;
    struct DMASegment Segment;
};
struct DMABuffer {
    void* Backing;
    struct DMASegment Segment;
};
static bool g_enforceDmaReservations;
static struct DMAMapping* g_activeReservations[16];
static unsigned int g_dmaBusyAttempts;
static struct DMASegment g_deviceSegment;
static size_t g_mappedOffset, g_cpuBytes;
static int g_failSync, g_failUnmap, g_maps;
static struct DMABuffer* g_failBufferFree;
static struct DMAMapping* g_failMappingSync;
static struct DMAMapping* g_busyMapping;
static int g_failAllocateAfter = -1;
static int g_allocateCalls;
static bool g_scatteredAllocations;
static uint8_t g_source[1024 * 1024 + 4096];
static size_t g_cpuOffset;
static int g_mmioHelpers, g_signals, g_processCalls;
static int g_signalFailure;
static XhciController_t g_cancelController;
static XhciPortRegisters_t* g_ports;
static size_t g_portCount;
static unsigned int g_portEvents[4];
static int g_mapCalls, g_unmapCalls, g_elementGrowths;
static int g_elementCounts, g_elementFills;
static int g_descriptorAllocations, g_descriptorFrees;
static int g_chainWalks;
static int g_failMap, g_failSegments, g_failElementGrowth;
static int g_failDescriptorAfter = -1;
static int g_failUnmapAfter = -1;
static XhciTransferDescriptor_t g_descriptors[16];
static int g_bounceBuffers, g_bounceCreates, g_bounceMaps;
static int g_failCreate, g_failClientMap;
static int g_failShmRelease;
static size_t g_bounceBytes;
static void* g_releasedBounce;

static oserr_t
__TestElementsNeeded(
    _In_ UsbManagerTransfer_t* transfer,
    _In_ uint32_t length,
    _In_ enum USBTransferDirection direction,
    _Out_ int* count)
{
    g_elementCounts++;
    return HCITransferElementsNeeded(transfer, length, direction, count);
}

static void
__TestElementFill(
    _In_ UsbManagerTransfer_t* transfer,
    _In_ uint32_t length,
    _In_ enum USBTransferDirection direction)
{
    g_elementFills++;
    HCITransferElementFill(transfer, length, direction);
}

static void*
__TestReallocate(
    _In_ void*  memory,
    _In_ size_t length)
{
    g_elementGrowths++;
    return g_failElementGrowth ? NULL : realloc(memory, length);
}

oserr_t
DMAMapSHM(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                shm,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct DMAMapping**              out)
{
    CHECK(context == (struct DMAContext*)42 && (shm->ID == 17 || shm->ID == 92) && *out == NULL);
    CHECK(requirements == NULL);
    CHECK(direction >= DMAToDevice && direction <= DMABidirectional);
    g_mapCalls++;
    if (g_failMap) return OS_EOOM;
    if (g_failSegments) return OS_EUNKNOWN;
    *out = calloc(1, sizeof(**out));
    g_maps++;
    g_mappedOffset = offset;
    g_deviceSegment = (struct DMASegment) { 0x10000ff00ULL + offset, length };
    if (shm->ID == 92) {
        CHECK(offset == 0 && length == 4096);
        g_deviceSegment.Address = 0x300010000ULL;
        g_bounceMaps++;
    }
    (*out)->Offset = offset;
    (*out)->Buffer = shm->ID;
    (*out)->Backing = shm->Payload;
    (*out)->Segment = g_deviceSegment;
    return OS_EOK;
}
oserr_t
DMAAllocate(struct DMAContext* context, const struct DMARequirements* requirements,
    struct DMABuffer** out)
{
    CHECK(context == (struct DMAContext*)42 && requirements->Alignment == 4096 && requirements->Boundary == 65536);
    CHECK(requirements->Contiguous && *out == NULL);
    if (g_failAllocateAfter == 0) return OS_EOOM;
    if (g_failSegments) return OS_EUNKNOWN;
    if (g_failAllocateAfter > 0) g_failAllocateAfter--;
    g_allocateCalls++;
    if (g_scatteredAllocations) CHECK(requirements->Length == XHCI_PAGE_SIZE);
    *out = calloc(1, sizeof(**out));
    (*out)->Backing = calloc(1, 4096);
    g_maps++;
    g_deviceSegment = (struct DMASegment) { 0x200001000ULL, 4096 };
    if (g_scatteredAllocations) g_deviceSegment.Address += (uint64_t)g_allocateCalls * 0x20000;
    (*out)->Segment = g_deviceSegment;
    return OS_EOK;
}
void* DMABufferData(const struct DMABuffer* buffer) { return buffer->Backing; }
const struct DMASegment* DMABufferSegments(const struct DMABuffer* buffer, uint32_t* count)
{ *count = 1; return &buffer->Segment; }
const struct DMASegment* DMAMappingSegments(const struct DMAMapping* mapping, uint32_t* count)
{ *count = 1; return &mapping->Segment; }
oserr_t DMAFree(struct DMABuffer** buffer)
{
    if (*buffer == NULL) return OS_EOK;
    if (g_failUnmap || *buffer == g_failBufferFree) return OS_EBUSY;
    free((*buffer)->Backing);
    free(*buffer);
    *buffer = NULL;
    g_maps--;
    return OS_EOK;
}
oserr_t DMAUnmap(struct DMAMapping** m)
{
    if (*m == NULL) return OS_EOK;
    if (g_failUnmap || (*m)->Owned || g_failUnmapAfter == 0) return OS_EBUSY;
    if (g_failUnmapAfter > 0) g_failUnmapAfter--;
    g_unmapCalls++;
    if ((*m)->Buffer == 92) {
        g_bounceMaps--;
        g_releasedBounce = (*m)->Backing;
    }
    free(*m); *m = NULL; g_maps--; return OS_EOK;
}
oserr_t DMABeginTransfer(struct DMAMapping* m, size_t offset, size_t length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    CHECK(mode == SHMWaitNonBlocking && deadline == NULL);
    CHECK(m != NULL && !m->Owned && offset == 0 && length > 0);
    if (g_failSync || m == g_failMappingSync) return OS_EUNKNOWN;
    if (m == g_busyMapping) return OS_EBUSY;
    if (g_enforceDmaReservations) {
        size_t first = m->Offset & ~(size_t)63;
        size_t last = (m->Offset + length - 1) | 63;
        unsigned int i;
        for (i = 0; i < 16; i++) {
            struct DMAMapping* other = g_activeReservations[i];
            if (other != NULL && other->Buffer == m->Buffer && other->Backing == m->Backing &&
                first <= ((other->Offset + other->ActiveLength - 1) | 63) &&
                (other->Offset & ~(size_t)63) <= last) {
                g_dmaBusyAttempts++;
                return OS_EBUSY;
            }
        }
        for (i = 0; i < 16 && g_activeReservations[i] != NULL; i++);
        CHECK(i < 16);
        g_activeReservations[i] = m;
        m->ActiveLength = length;
    }
    m->Owned = 1; return OS_EOK;
}
oserr_t DMAEndTransfer(struct DMAMapping* m, size_t length)
{
    CHECK(m != NULL && m->Owned);
    if (g_failSync || m == g_failMappingSync) return OS_EUNKNOWN;
    if (g_enforceDmaReservations) {
        unsigned int i;
        for (i = 0; i < 16 && g_activeReservations[i] != m; i++);
        CHECK(i < 16);
        g_activeReservations[i] = NULL;
    }
    g_cpuBytes = length; m->Owned = 0; return OS_EOK;
}
oserr_t SHMMap(OSHandle_t* handle, size_t offset, size_t length, unsigned int flags)
{
    CHECK(handle->ID == 17 && offset == 0 && length <= sizeof(g_source));
    CHECK(flags == SHM_ACCESS_READ || flags == SHM_ACCESS_WRITE);
    if (g_failClientMap) return OS_EPERMISSIONS;
    g_cpuOffset = offset;
    return OS_EOK;
}
size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
SHMCreate(
    _In_  SHM_t*      options,
    _Out_ OSHandle_t* handle)
{
    CHECK(handle->ID == UUID_INVALID && options->Size == 4096);
    CHECK(options->Flags == (SHM_COMMIT | SHM_CLEAN | SHM_PRIVATE));
    CHECK(options->Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    if (g_failCreate) {
        return OS_EOOM;
    }
    handle->ID = 92;
    handle->Type = OSHANDLE_SHM;
    handle->Payload = calloc(1, options->Size);
    CHECK(handle->Payload != NULL);
    g_bounceCreates++;
    g_bounceBuffers++;
    g_bounceBytes += options->Size;
    return OS_EOK;
}
size_t SHMBufferCapacity(OSHandle_t* handle)
{ CHECK(handle->ID == 17); return sizeof(g_source); }
void* SHMBuffer(OSHandle_t* handle)
{ return handle->ID == 17 ? g_source + g_cpuOffset : handle->Payload; }
void OSHandleDestroy(OSHandle_t* handle)
{
    if (handle->ID == UUID_INVALID) return;
    CHECK(handle->ID == 91 || handle->ID == 92);
    if (handle->ID == 92) {
        CHECK(handle->Payload == g_releasedBounce || g_bounceMaps == 0);
        g_releasedBounce = NULL;
        g_bounceBuffers--;
        g_bounceBytes -= 4096;
    }
    free(handle->Payload);
}

oserr_t
SHMRelease(
    _InOut_ OSHandle_t* handle)
{
    if (g_failShmRelease) {
        return OS_EUNKNOWN;
    }
    OSHandleDestroy(handle);
    memset(handle, 0, sizeof(*handle));
    return OS_EOK;
}

oserr_t
UsbSchedulerAllocateElement(
    _In_  UsbScheduler_t* scheduler,
    _In_  int             pool,
    _Out_ uint8_t**       elementOut)
{
    XhciTransferDescriptor_t* descriptor;
    size_t                   i;

    CHECK(pool == XHCI_TD_POOL);
    CHECK(scheduler->Settings.Pools[pool].ElementPool == (uint8_t*)g_descriptors);
    if (g_failDescriptorAfter == 0) {
        return OS_EOOM;
    }
    if (g_failDescriptorAfter > 0) {
        g_failDescriptorAfter--;
    }
    for (i = 0; i < 16; i++) {
        descriptor = &g_descriptors[i];
        if (!(descriptor->Object.Flags & USB_ELEMENT_ALLOCATED)) {
            memset(descriptor, 0, sizeof(*descriptor));
            descriptor->Object.Index = USB_ELEMENT_CREATE_INDEX(pool, i);
            descriptor->Object.DepthIndex = USB_ELEMENT_NO_INDEX;
            descriptor->Object.Flags = USB_ELEMENT_ALLOCATED;
            *elementOut = (uint8_t*)descriptor;
            g_descriptorAllocations++;
            return OS_EOK;
        }
    }
    return OS_EOOM;
}

oserr_t
UsbSchedulerChainElement(
    _In_ UsbScheduler_t* scheduler,
    _In_ int             rootPool,
    _In_ uint8_t*        root,
    _In_ int             pool,
    _In_ uint8_t*        element,
    _In_ uint16_t        marker,
    _In_ int             direction)
{
    XhciTransferDescriptor_t* tail = (XhciTransferDescriptor_t*)root;

    (void)scheduler;
    CHECK(rootPool == XHCI_TD_POOL && pool == XHCI_TD_POOL);
    CHECK(marker == USB_ELEMENT_NO_INDEX && direction == USB_CHAIN_DEPTH);
    while (tail->Object.DepthIndex != USB_ELEMENT_NO_INDEX) {
        g_chainWalks++;
        tail = &g_descriptors[tail->Object.DepthIndex & USB_ELEMENT_INDEX_MASK];
    }
    tail->Object.DepthIndex = ((XhciTransferDescriptor_t*)element)->Object.Index;
    return OS_EOK;
}

void
UsbSchedulerFreeElement(
    _In_ UsbScheduler_t* scheduler,
    _In_ uint8_t*        element)
{
    XhciTransferDescriptor_t* descriptor = (XhciTransferDescriptor_t*)element;

    (void)scheduler;
    CHECK(descriptor->Object.Flags & USB_ELEMENT_ALLOCATED);
    descriptor->Object.Flags = 0;
    g_descriptorFrees++;
}

#ifndef XHCI_MANAGER_INTEGRATION
void
UsbManagerChainEnumerate(
    _In_ UsbManagerController_t* controller,
    _In_ uint8_t*               root,
    _In_ int                    direction,
    _In_ int                    reason,
    _In_ bool (*callback)(UsbManagerController_t*, uint8_t*, enum HCIProcessReason, void*),
    _In_ void*                  context)
{
    XhciTransferDescriptor_t* descriptor = (XhciTransferDescriptor_t*)root;
    XhciTransferDescriptor_t* next;

    CHECK(direction == USB_CHAIN_DEPTH);
    while (descriptor != NULL) {
        next = __DescriptorNext((XhciController_t*)controller, descriptor);
        if (!callback(controller, (uint8_t*)descriptor, reason, context)) {
            break;
        }
        descriptor = next;
    }
}
#endif

/** Records accesses to selected MMIO registers without treating DMA RAM as MMIO. */
struct __MmioAccess {
    uintptr_t Address;
    uint32_t  Value;
    bool      Write;
};
static const volatile void* g_mmioRegisters[4];
static unsigned int g_mmioRegisterCount;
static struct __MmioAccess g_mmioAccesses[64];
static unsigned int g_mmioAccessCount;
static XhciOperationalRegisters_t* g_startupRegisters;
static unsigned int g_startupStage;
static unsigned int g_startupFailStage;
static XhciController_t* g_startupController;
static unsigned int g_startupInterruption;

static void
__RecordMmio(
    _In_ const volatile void* address,
    _In_ size_t               length,
    _In_ uint32_t             value,
    _In_ bool                 write)
{
    uintptr_t    base;
    uintptr_t    offset;
    unsigned int i;

    for (i = 0; i < g_mmioRegisterCount; i++) {
        base = (uintptr_t)g_mmioRegisters[i];
        if ((uintptr_t)address < base || (uintptr_t)address >= base + 8) {
            continue;
        }
        offset = (uintptr_t)address - base;
        CHECK(length == 4 && (offset == 0 || offset == 4));
        CHECK(g_mmioAccessCount < 64);
        g_mmioAccesses[g_mmioAccessCount++] = (struct __MmioAccess) {
            .Address = (uintptr_t)address,
            .Value = value,
            .Write = write
        };
        return;
    }
}

void WriteVolatileMemory(volatile void* destination, void* source, size_t length)
{
    size_t i;
    uint32_t old;
    uint32_t value = *(const uint32_t*)source;

    CHECK(length == 4);
    if (g_startupRegisters != NULL && destination == &g_startupRegisters->UsbCommand) {
        if (value & XHCI_OP_USBCMD_HCRESET) {
            CHECK(g_startupStage == 0);
            g_startupStage = 1;
            value &= ~XHCI_OP_USBCMD_HCRESET;
        }
        if (value & XHCI_OP_USBCMD_RUN) {
            CHECK(g_startupStage == 5);
            g_startupStage = 6;
            g_startupRegisters->UsbStatus = 0;
        } else {
            g_startupRegisters->UsbStatus = XHCI_OP_USBSTS_HALTED;
        }
        g_startupRegisters->UsbCommand = value;
        return;
    }
    __RecordMmio(destination, length, value, true);
    g_mmioHelpers++;
    for (i = 0; i < g_portCount; i++) {
        if (destination == &g_ports[i].StatusControl) {
            old = g_ports[i].StatusControl;
            g_ports[i].StatusControl = (old & ~XHCI_PORT_STATUS_RW_MASK &
                ~(value & XHCI_PORT_STATUS_CHANGE_BITS)) | (value & XHCI_PORT_STATUS_RW_MASK);
            return;
        }
    }
    *(volatile uint32_t*)destination = *(const uint32_t*)source;
}

void ReadVolatileMemory(const volatile void* source, volatile void* destination, size_t length)
{
    CHECK(length == 4 || length == 8);
    __RecordMmio(source, length, *(const volatile uint32_t*)source, false);
    g_mmioHelpers++;
    if (length == 8) {
        *(volatile uint64_t*)destination = *(const volatile uint64_t*)source;
    } else {
        *(volatile uint32_t*)destination = *(const volatile uint32_t*)source;
    }
}
#ifndef XHCI_MANAGER_INTEGRATION
UsbManagerController_t* UsbManagerGetController(uuid_t device)
{ CHECK(device == 77); return &g_cancelController.Base; }
void UsbManagerProcessTransfers(UsbManagerController_t* controller)
{ CHECK(controller == &g_cancelController.Base); g_processCalls++; }
#endif
static oserr_t __Signal(uuid_t handle)
{ CHECK(handle == 31); g_signals++; return g_signalFailure ? OS_EUNKNOWN : OS_EOK; }
static void __TestFastHandler(void)
{
    uint8_t registers[8192] = { 0 };
    XhciController_t controller = { 0 };
    DeviceIo_t io = { .Type = DeviceIoMemoryBased,
        .Access.Memory.VirtualBase = (uintptr_t)registers };
    InterruptResourceTable_t resources = { .HandleResource = 31, .IoResources = { &io } };
    InterruptFunctionTable_t functions = { .EventSignal = __Signal };
    XhciCapabilityRegisters_t* cap = (void*)registers;
    XhciOperationalRegisters_t* op = (void*)(registers + 64);
    XhciInterrupterRegisters_t* intr = (void*)(registers + 4096 + 32);
    resources.MemoryResources[0].Address = (uintptr_t)&controller;
    cap->CapLength = 64;
    cap->RuntimeOffset = 4096;
    op->UsbStatus = XHCI_OP_USBSTS_EVENT;
    intr->Management = XHCI_INTERRUPTER_MANAGEMENT_PENDING;
    g_mmioHelpers = 0;
    CHECK(OnFastInterrupt(&functions, &resources) == IRQSTATUS_HANDLED);
    CHECK(g_signals == 1 && g_mmioHelpers == 0);
    CHECK(atomic_load(&controller.Base.InterruptStatus) == XHCI_OP_USBSTS_EVENT);

    // A shared interrupt can observe USBSTS before an event arrives, then IP
    // afterward. Accepting that snapshot must still schedule an event drain.
    atomic_store(&controller.Base.InterruptStatus, 0);
    op->UsbStatus = 0;
    intr->Management = XHCI_INTERRUPTER_MANAGEMENT_PENDING;
    CHECK(OnFastInterrupt(&functions, &resources) == IRQSTATUS_HANDLED);
    CHECK(g_signals == 2 && g_mmioHelpers == 0);
    CHECK(atomic_load(&controller.Base.InterruptStatus) == XHCI_OP_USBSTS_EVENT);

    atomic_store(&controller.Base.InterruptStatus, 0);
    intr->Management = 0;
    op->UsbStatus = 0;
    CHECK(OnFastInterrupt(&functions, &resources) == IRQSTATUS_NOT_HANDLED);
    CHECK(g_signals == 2 && atomic_load(&controller.Base.InterruptStatus) == 0);
    CHECK(atomic_load(&controller.InterruptDiagnostics.Entries) == 3);
    CHECK(atomic_load(&controller.InterruptDiagnostics.Handled) == 2);
    CHECK(atomic_load(&controller.InterruptDiagnostics.Signals) == 2);
    CHECK(atomic_load(&controller.InterruptDiagnostics.SignalFailures) == 0);
    g_signalFailure = 1;
    intr->Management = XHCI_INTERRUPTER_MANAGEMENT_PENDING;
    CHECK(OnFastInterrupt(&functions, &resources) == IRQSTATUS_HANDLED);
    CHECK(atomic_load(&controller.InterruptDiagnostics.SignalFailures) == 1);
    CHECK(atomic_load(&controller.InterruptDiagnostics.Signals) == 3);
    CHECK(g_mmioHelpers == 0);
    g_signalFailure = 0;
}
static void __TestCompletedCancel(void)
{
    UsbManagerTransfer_t transfer = { .DeviceID = 77, .State = USBTRANSFER_STATE_COMPLETED };
    CHECK(HCITransferDequeue(&transfer) == OS_EOK);
    CHECK(transfer.State == USBTRANSFER_STATE_CLEANUP && g_processCalls == 1);
    CHECK(transfer.ResultCode == USBTRANSFERCODE_CANCELLED);
    UsbManagerForgetTransferWork(&transfer);
}

static void __TestRing(void)
{
    XhciRing_t ring = { 0 };
    XhciTrb_t trb = { .Parameter = 0x1234 };

    CHECK(XhciRingInitialize((struct DMAContext*)42, &ring, 8) == OS_EOK);
    CHECK(ring.DeviceBase == 0x200001000ULL);
    CHECK(ring.Trbs[7].Parameter == ring.DeviceBase);
    CHECK(XhciRingEnqueue(&ring, &trb, NULL) == OS_EOK && ring.Used == 1);
    g_failUnmap = 1;
    CHECK(XhciRingDestroy(&ring) == OS_EBUSY && ring.Trbs != NULL);
    g_failUnmap = 0;
    CHECK(XhciRingDestroy(&ring) == OS_EOK && ring.Trbs == NULL && g_maps == 0);
}

static void __TestPeriodic(void)
{
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN, .MaxPacketSize = 64,
        .SHMHandle = { .ID = 17 }, .Dma = { .Context = (struct DMAContext*)42, .BufferOffset = 31, .BufferLength = 1024 } };
    XhciController_t controller = { 0 };
    XhciEndpoint_t endpoint = { .SlotId = 1, .DeviceContextIndex = 3 };
    XhciTransferDescriptor_t descriptor = { 0 };
    XhciTrb_t ring[8] = { 0 };
    reg32_t doorbells[2] = { 0 };

    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EOK && transfer.Dma.Length == 64);
    CHECK(g_mappedOffset == 0 && transfer.ElementCount == 1);
    CHECK(transfer.Dma.Bounce.ClientOffset == 31);
    CHECK(transfer.Elements[0].Data.Address == g_deviceSegment.Address);
    descriptor.Object.DepthIndex = USB_ELEMENT_NO_INDEX;
    transfer.RootElement = &descriptor;
    transfer.ChainLength = 1;
    controller.Doorbells = doorbells;
    endpoint.TransferRing.Trbs = ring;
    endpoint.TransferRing.TrbCount = 8;
    endpoint.TransferRing.CycleState = 1;
    g_failSync = 1;
    CHECK(XhciTransferSubmit(&controller, &endpoint, &transfer) == OS_EUNKNOWN);
    CHECK(endpoint.TransferRing.Used == 0 && !transfer.Dma.DeviceOwned && doorbells[1] == 0);
    g_failSync = 0;
    CHECK(XhciTransferSubmit(&controller, &endpoint, &transfer) == OS_EOK);
    CHECK(transfer.Dma.DeviceOwned && ring[0].Parameter == g_deviceSegment.Address);
    CHECK(doorbells[1] == 3);
    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EBUSY);
    g_failSync = 1;
    CHECK(UsbTransferDmaComplete(&transfer, 7) == OS_EUNKNOWN && transfer.Dma.DeviceOwned);
    g_failSync = 0;
    CHECK(UsbTransferDmaComplete(&transfer, 7) == OS_EOK && g_cpuBytes == 7);
    transfer.TData.Periodic.CurrentDataIndex = 64;
    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EOK && g_mappedOffset == 0);
    CHECK(transfer.Dma.Bounce.ClientOffset == 95);
    CHECK(UsbTransferDmaDestroy(&transfer) == OS_EOK);
    free(transfer.Elements);
    CHECK(g_maps == 0);
}

static void __TestControl(void)
{
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_CONTROL,
        .Direction = USBTRANSFER_DIRECTION_IN, .MaxPacketSize = 64,
        .SHMHandle = { .ID = 17 }, .Dma = { .Context = (struct DMAContext*)42, .BufferOffset = 23, .BufferLength = 4096 } };
    XhciTrb_t trb;
    uint64_t setup = 0x8877665544332211ULL;
    int i;

    memcpy(g_source + 23, &setup, sizeof(setup));
    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EOK);
    memset(g_source + 23, 0, sizeof(setup));
    __BuildTrb(&transfer, 0, &trb);
    CHECK(trb.Parameter == setup);
    CHECK(transfer.ElementCount == 4);
    for (i = 1; i < transfer.ElementCount - 1; i++) {
        __BuildTrb(&transfer, i, &trb);
        CHECK(trb.Parameter > UINT32_MAX);
        CHECK((trb.Parameter & 0xffff) + trb.Status <= 0x10000);
    }
    CHECK(DMAUnmap(&transfer.Dma.Mapping) == OS_EOK);
    free(transfer.Elements);
    CHECK(g_maps == 0);
}

static void __TestShortCompletion(void)
{
    XhciController_t controller = { .SlotCount = 1 };
    XhciDevice_t device = { .SlotId = 1 };
    XhciEndpoint_t endpoint = { 0 };
    UsbScheduler_t scheduler = { 0 };
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_CONTROL };
    XhciTransferDescriptor_t descriptors[4] = { 0 };
    XhciTrb_t trbs[8] = { 0 };
    XhciTrb_t event = { .Parameter = 0x100000010ULL,
        .Control = XHCI_TRB_CONTROL_SLOT_ID(1) | XHCI_TRB_CONTROL_EP_ID(1),
        .Status = (XHCI_TRB_COMPLETION_SHORT_PACKET << 24) | 200 };
    int i;

    controller.Base.Scheduler = &scheduler;
    controller.Slots[1] = &device;
    device.Endpoints[1] = &endpoint;
    controller.XhciEndpoints.head = &endpoint.Header;
    endpoint.Header.value = &endpoint;
    endpoint.TransferRing.Trbs = trbs;
    endpoint.TransferRing.DeviceBase = 0x100000000ULL;
    endpoint.TransferRing.TrbCount = 8;
    endpoint.TransferRing.Used = 4;
    scheduler.Settings.Pools[0].ElementPool = (uint8_t*)descriptors;
    scheduler.Settings.Pools[0].ElementAlignedSize = sizeof(descriptors[0]);
    transfer.RootElement = descriptors;
    transfer.Dma.HardwareActive = true;
    transfer.Dma.Diagnostics.Enabled = true;
    device.Ep0Diagnostics.Active = true;
    for (i = 0; i < 4; i++) {
        descriptors[i].Transfer = &transfer;
        descriptors[i].FirstTrbIndex = i;
        descriptors[i].TrbCount = 1;
        descriptors[i].Object.DepthIndex = i == 3 ? USB_ELEMENT_NO_INDEX : i + 1;
        endpoint.TRBOwners[i] = &descriptors[i];
    }
    descriptors[0].TransferLength = 8;
    descriptors[1].TransferLength = 248;
    descriptors[2].TransferLength = 3840;
    CHECK(!XhciTransferHandleEvent(&controller, &event));
    CHECK(device.Ep0Diagnostics.Active && transfer.Dma.Diagnostics.Events == 1);
    CHECK(transfer.Dma.HardwareActive && endpoint.TransferRing.Used == 4);
    CHECK(descriptors[1].BytesTransferred == 48 && descriptors[2].BytesTransferred == 0);
    CHECK(!(descriptors[3].Flags & XHCI_TD_FLAG_COMPLETED));
    event.Parameter += 32;
    event.Status = XHCI_TRB_COMPLETION_SUCCESS << 24;
    CHECK(XhciTransferHandleEvent(&controller, &event));
    CHECK(!device.Ep0Diagnostics.Active && transfer.Dma.Diagnostics.Events == 2);
    CHECK(!transfer.Dma.HardwareActive && endpoint.TransferRing.Used == 0);
    CHECK(descriptors[2].BytesTransferred == 0);
    for (i = 0; i < 4; i++) CHECK(endpoint.TRBOwners[i] == NULL);

    // Bulk short packets finish the TD immediately, including skipped entries.
    transfer.Type = USBTRANSFER_TYPE_BULK;
    transfer.Dma.HardwareActive = true;
    endpoint.TransferRing.Used = 4;
    for (i = 0; i < 4; i++) {
        descriptors[i].Flags = 0;
        endpoint.TRBOwners[i] = &descriptors[i];
    }
    event.Parameter = 0x100000000ULL;
    event.Status = (XHCI_TRB_COMPLETION_SHORT_PACKET << 24) | 1;
    CHECK(XhciTransferHandleEvent(&controller, &event));
    CHECK(!transfer.Dma.HardwareActive && endpoint.TransferRing.Used == 0);
    for (i = 1; i < 4; i++) CHECK(descriptors[i].BytesTransferred == 0 && endpoint.TRBOwners[i] == NULL);
}

static void
__TestRootDeviceContext(void)
{
    XhciController_t     controller = { 0 };
    XhciEndpoint_t       endpoint = { 0 };
    UsbManagerTransfer_t transfer = { 0 };
    XhciDevice_t*        device = NULL;
    reg32_t             doorbell = 0;
    reg32_t*            slotContext;

    controller.Base.DmaContext = (struct DMAContext*)42;
    controller.Base.PortCount = 8;
    controller.ContextSize = 32;
    controller.Doorbells = &doorbell;
    transfer.Address.PortAddress = 5;
    transfer.Speed = USBSPEED_FULL;
    transfer.MaxPacketSize = 8;
    CHECK(XhciRingInitialize((struct DMAContext*)42, &controller.CommandRing, 8) == OS_EOK);
    CHECK(XhciDeviceEnsure(&controller, &transfer, &endpoint, &device) == OS_EINCOMPLETE);
    CHECK(device != NULL && device->RootPort == 6 && device->RouteString == 0);
    __BuildAddressContext(&controller, device, &transfer);
    slotContext = (reg32_t*)(device->InputContext + controller.ContextSize);
    CHECK((slotContext[0] & 0xfffff) == 0);
    CHECK(((slotContext[1] >> 16) & 0xff) == 6);
    CHECK(DMAFree(&device->InputContextDMA) == OS_EOK);
    CHECK(DMAFree(&device->DeviceContextDMA) == OS_EOK);
    CHECK(XhciRingDestroy(&controller.CommandRing) == OS_EOK);
    free(device);
    CHECK(g_maps == 0);
}

#include "xhci_enumeration_test.inc"
#include "xhci_dma_reuse_test.inc"
#include "usb_dma_bounce_test.inc"
#include "xhci_event_index_test.inc"
#include "xhci_dma_allocation_test.inc"
#include "xhci_pci_diagnostics_test.inc"
#include "xhci_startup_test.inc"
#include "xhci_pending_diagnostics_test.inc"
#include "xhci_mmio_test.inc"
#include "xhci_ep0_diagnostics_test.inc"
#include "xhci_port_power_diagnostics_test.inc"
#include "xhci_hub_configuration_test.inc"

int main(void)
{
    XhciController_t controller = { 0 };

    CHECK(XhciQueueInitialize(&controller) == OS_EOOM);
    __TestPciDiagnostics();
    __TestPcieErrors();
    __TestInterruptStartup();
    __TestPendingDiagnostics();
    __TestMmio64();
    __TestEp0Diagnostics();
    __TestPortPowerDiagnostics();
    __TestHubConfiguration();
    __TestScratchpadPages();
    __TestStartupPorts();
    __TestHubRoutes();
    __TestRootDeviceContext();
    __TestFastHandler();
    __TestCompletedCancel();
    __TestRing();
    __TestPeriodic();
    __TestControl();
    __TestShortCompletion();
    __TestPeriodicReuse();
    __TestReuseFailures();
    __TestLinearDescriptorChain();
    __TestBounceBounded();
    __TestBounceHandoffs();
    __TestBounceSetupFailures();
    __TestLargePeriodicCleanup();
    __TestPeriodicLayoutCache();
    __TestDeviceSegmentBuilder();
    __TestIndexedTransferEvents();
    __TestEndpointIndexRemoval();
    __TestSlotIndexLifecycle();
    puts("xHCI event index: slot/endpoint dispatch, invalid pointers, duplicate events and slot reuse passed");
    puts("USB DMA bounce: bounded memory across 262144 packets, short copies, ownership and cleanup failures passed");
    puts("xHCI DMA: translated rings/TRBs, 64 KiB splits, cached setup, packet offsets and sync/release failures passed");
    return 0;
}
