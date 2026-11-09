// Run each legacy descriptor builder against translated, merged and fragmented
// DMA segments. Cleanup uses controlled hardware acknowledgements.
#define __need_minmax
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#if defined(TEST_UHCI)
#include "../modules/serial/usb/uhci/transfer.c"
#include "../modules/serial/usb/uhci/structures/td.c"
typedef UhciController_t TestController;
typedef UhciQueueHead_t TestQH;
typedef UhciTransferDescriptor_t TestTD;
#elif defined(TEST_OHCI)
#include "../modules/serial/usb/ohci/transfer.c"
#include "../modules/serial/usb/ohci/structures/qh.c"
#include "../modules/serial/usb/ohci/interrupts.c"
#include "../modules/serial/usb/ohci/structures/td.c"
#include "../modules/serial/usb/ohci/structures/itd.c"
typedef OhciController_t TestController;
typedef OhciQueueHead_t TestQH;
typedef OhciTransferDescriptor_t TestTD;
#else
#include "../modules/serial/usb/ehci/ehci.h"
#define HCIProcessElement __UnusedProcessElement
#define HCIProcessEvent(...) __UnusedProcessEvent(__VA_ARGS__)
#define EhciRingDoorbell __UnusedRingDoorbell
#include "../modules/serial/usb/ehci/queue.c"
#undef HCIProcessElement
#undef HCIProcessEvent
#undef EhciRingDoorbell
#include "../modules/serial/usb/ehci/transfer.c"
#include "../modules/serial/usb/ehci/structures/td.c"
#include "../modules/serial/usb/ehci/structures/itd.c"
#include "../modules/serial/usb/ehci/structures/sitd.c"
typedef EhciController_t TestController;
typedef EhciQueueHead_t TestQH;
typedef EhciTransferDescriptor_t TestTD;
#endif

static TestController g_controller;
static UsbScheduler_t g_scheduler;
static TestQH g_qh;
static TestTD g_td;
static int g_unlinks, g_cleanups, g_stops, g_failStop, g_doorbells;
static int g_failBegin, g_cancelBegin, g_begins, g_restarts;
static int g_available = 1000, g_allocated, g_lastPool;

#if defined(TEST_OHCI)
oserr_t OHCIQueueReset(OhciController_t* controller)
{ (void)controller; return OS_EBUSY; }
void UsbManagerDeferControllerDestroy(UsbManagerController_t* controller)
{ controller->State.Phase = USB_CONTROLLER_DESTROY_PENDING; }
#endif

oserr_t UsbSchedulerAllocateElement(UsbScheduler_t* scheduler, int pool, uint8_t** out)
{
    (void)scheduler;
    CHECK(pool != 0);
    if (!g_available) return OS_ENOENT;
    g_available--; g_allocated++; g_lastPool = pool;
    *out = (uint8_t*)&g_td;
    return OS_EOK;
}
oserr_t UsbSchedulerChainElement(UsbScheduler_t* scheduler, int rootPool, uint8_t* root,
    int pool, uint8_t* element, uint16_t marker, int direction)
{
    (void)scheduler; (void)pool; (void)element;
#if defined(TEST_OHCI)
    if (direction == USB_CHAIN_BREATH) CHECK(marker == USB_ELEMENT_NO_INDEX);
#endif
    (void)marker; (void)direction;
    CHECK(rootPool == 0 && root == (uint8_t*)&g_qh);
    return OS_EOK;
}

void ReadVolatileMemory(const volatile void* source, volatile void* destination, size_t length)
{ memcpy((void*)destination, (const void*)source, length); }
void WriteVolatileMemory(volatile void* destination, void* source, size_t length)
{ memcpy((void*)destination, source, length); }

oserr_t UsbTransferDmaBegin(UsbManagerTransfer_t* transfer)
{
    g_begins++;
    if (g_cancelBegin) HCITransferDequeue(transfer);
    if (g_failBegin) return OS_EUNKNOWN;
    transfer->Dma.DeviceOwned = true;
    return OS_EOK;
}
void HCIProcessEvent(UsbManagerController_t* c, enum HCIProcessEvent event, void* context)
{ (void)c; (void)event; (void)context; g_restarts++; }
oserr_t UsbSchedulerGetPoolElement(UsbScheduler_t* scheduler, int pool, int index, uint8_t** out, uintptr_t* address)
{
    (void)scheduler; (void)pool; (void)index;
    if (out) *out = pool == 0 ? (uint8_t*)&g_qh : (uint8_t*)&g_td;
    if (address) *address = 0x400000;
    return OS_EOK;
}

UsbManagerController_t* UsbManagerGetController(uuid_t id)
{ (void)id; return &g_controller.Base; }
int UsbManagerGetToggle(UsbManagerController_t* c, USBAddress_t* a)
{ (void)c; (void)a; return 0; }
void UsbManagerSetToggle(UsbManagerController_t* c, USBAddress_t* a, int toggle)
{ (void)c; (void)a; (void)toggle; }
void SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{ (void)level; (void)format; }

void UsbManagerChainEnumerate(UsbManagerController_t* c, uint8_t* root, int direction,
    int reason, UsbSchedulerElementCallback callback, void* context)
{
    (void)c; (void)root; (void)direction; (void)callback; (void)context;
    if (reason == HCIPROCESS_REASON_UNLINK) g_unlinks++;
    else if (reason == HCIPROCESS_REASON_CLEANUP) g_cleanups++;
    else if (reason == HCIPROCESS_REASON_NONE) {
        CHECK(((UsbManagerTransfer_t*)((struct __PrepareContext*)context)->Transfer)->Dma.DeviceOwned);
        callback(c, (uint8_t*)&g_qh, reason, context);
        callback(c, (uint8_t*)&g_td, reason, context);
    } else abort();
}

#if defined(TEST_UHCI)
oserr_t UhciStop(UhciController_t* c)
{ g_stops++; if (g_failStop) return OS_ETIMEOUT; c->Base.State.DmaStopped = true; return OS_EOK; }
oserr_t UhciStart(UhciController_t* c, int wait)
{ (void)wait; c->Base.State.DmaStopped = false; return OS_EOK; }
#elif defined(TEST_OHCI)
static OhciRegisters_t g_registers;
oserr_t OHCIStop(OhciController_t* c)
{ g_stops++; if (g_failStop) return OS_ETIMEOUT; c->Base.State.DmaStopped = true; return OS_EOK; }
void OHCISetMode(OhciController_t* c, reg32_t mode)
{ (void)mode; c->Base.State.DmaStopped = false; }
uintptr_t UsbSchedulerGetDma(UsbSchedulerPool_t* pool, const uint8_t* element)
{ return pool->ElementPoolDeviceAddress + (element - pool->ElementPool); }
#else
static EchiOperationalRegisters_t g_registers;
oserr_t EhciHalt(EhciController_t* c)
{
    g_stops++;
    c->OpRegisters->UsbCommand = 0;
    c->OpRegisters->UsbIntr = 0;
    if (g_failStop) return OS_ETIMEOUT;
    c->Base.State.DmaStopped = true;
    return OS_EOK;
}
void EhciRingDoorbell(EhciController_t* c)
{
    if (!c->AdvancePending && c->AdvanceIssued != c->AdvanceQueued) {
        c->AdvancePending = true;
        c->AdvanceIssued = c->AdvanceQueued;
        g_doorbells++;
    }
}
#endif

static void __TestGeometry(void)
{
    struct DMASegment segments[] = { { .Address = 0x200000, .Length = 65536 } };
    struct TransferElement elements[1025];
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_BULK, .MaxPacketSize = 64,
        .Dma = { .Segments = segments, .SegmentCount = 1 },
        .Elements = elements
    };
    uint32_t total;
    int count;
    int i;

    CHECK(HCITransferElementsNeeded(&transfer, 32769, USBTRANSFER_DIRECTION_IN, &count) == OS_EOK);
    memset(elements, 0, sizeof(elements));
    HCITransferElementFill(&transfer, 32769, USBTRANSFER_DIRECTION_IN);
    total = 0;
    for (i = 0; i < count; i++) {
        CHECK(elements[i].Length != 0);
#if defined(TEST_UHCI)
        CHECK(elements[i].Data.Address == 0x200000 + total);
#elif defined(TEST_OHCI)
        CHECK(elements[i].Data.OHCI.Page0 + elements[i].Data.OHCI.Offsets[0] == 0x200000 + total);
        CHECK(elements[i].Data.OHCI.Page1 == 0x200000 + total + elements[i].Length - 1);
#else
        CHECK(elements[i].Data.EHCI.Addresses[0] == 0x200000 + total);
        if (elements[i].Length > 4096) CHECK(elements[i].Data.EHCI.Addresses[1] == elements[i].Data.EHCI.Addresses[0] + 4096);
#endif
        total += elements[i].Length;
    }
    CHECK(total == 32769);

    // A periodic OUT packet must neither count nor write a trailing bulk ZLP.
    transfer.Type = USBTRANSFER_TYPE_INTERRUPT;
    CHECK(HCITransferElementsNeeded(&transfer, 64, USBTRANSFER_DIRECTION_OUT, &count) == OS_EOK && count == 1);
    memset(elements, 0, sizeof(elements));
    elements[1].Length = 0xDEADBEEF;
    elements[1].Type = TRANSFERELEMENT_TYPE_SETUP;
    HCITransferElementFill(&transfer, 64, USBTRANSFER_DIRECTION_OUT);
    CHECK(elements[1].Length == 0xDEADBEEF && elements[1].Type == TRANSFERELEMENT_TYPE_SETUP);

    // The CPU pointer is unrelated to these device addresses. Setup crosses
    // from the last two bytes of one page into a nonadjacent mapped page.
    {
        struct DMASegment split[] = { { .Address = 0x300FFE, .Length = 2 }, { .Address = 0x900000, .Length = 64 } };
        transfer.Type = USBTRANSFER_TYPE_CONTROL;
        transfer.Dma.Segments = split;
        transfer.Dma.SegmentCount = 2;
#if defined(TEST_UHCI)
        CHECK(HCITransferElementsNeeded(&transfer, 8, USBTRANSFER_DIRECTION_IN, &count) == OS_EOK && count == 2);
        memset(elements, 0, sizeof(elements));
        HCITransferElementFill(&transfer, 8, USBTRANSFER_DIRECTION_IN);
        CHECK(elements[0].NeedsBounce && elements[0].Length == 8);
        CHECK(!elements[1].NeedsBounce && elements[1].Length == 0);
#else
        CHECK(HCITransferElementsNeeded(&transfer, 8, USBTRANSFER_DIRECTION_IN, &count) == OS_EOK && count == 2);
        memset(elements, 0, sizeof(elements));
        HCITransferElementFill(&transfer, 8, USBTRANSFER_DIRECTION_IN);
#if defined(TEST_OHCI)
        OHCITDSetup(&g_td, &elements[0]);
        CHECK(g_td.Cbp == 0x300FFE && g_td.BufferEnd == 0x900005);
#else
        EHCITDSetup(&g_controller, &g_td, &elements[0]);
        CHECK(g_td.Buffers[0] == 0x300FFE && g_td.Buffers[1] == 0x900000);
#endif
#endif
    }
}

static void __TestPreparation(void)
{
    struct TransferElement element = { .Type = TRANSFERELEMENT_TYPE_IN, .Length = 17 };
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT, .MaxPacketSize = 64,
        .Elements = &element, .ElementCount = 1, .RootElement = &g_qh
    };
    struct __PrepareContext context = { .Transfer = &transfer, .LastTDIndex = 0 };
    TestQH saved;

    memset(&g_qh, 0x51, sizeof(g_qh));
    memcpy(&saved, &g_qh, sizeof(saved));
    CHECK(__PrepareDescriptor(&g_controller.Base, (uint8_t*)&g_qh, HCIPROCESS_REASON_NONE, &context));
    CHECK(context.TDIndex == 0 && memcmp(&g_qh, &saved, sizeof(saved)) == 0);
#if !defined(TEST_UHCI) && !defined(TEST_OHCI)
    // An interrupt pipe has a QH root even though it is periodic.
    CHECK(__AllocateDescriptors(&g_controller, &transfer, EHCI_TD_POOL) == 1);
#endif
}

static void __TestHandoff(void)
{
    struct TransferElement element = { .Type = TRANSFERELEMENT_TYPE_IN, .Length = 17 };
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT, .MaxPacketSize = 64,
        .Elements = &element, .ElementCount = 1, .RootElement = &g_qh,
        .ChainLength = 1, .State = USBTRANSFER_STATE_RESTART
    };

    memset(&g_qh, 0, sizeof(g_qh));
    memset(&g_td, 0, sizeof(g_td));
    g_failBegin = 1;
    CHECK(HCITransferQueue(&transfer) == OS_EUNKNOWN);
    CHECK(!transfer.Dma.DeviceOwned && g_restarts == 0 && transfer.RootElement == &g_qh);
    g_failBegin = 0;
    CHECK(HCITransferQueue(&transfer) == OS_EOK);
    CHECK(transfer.Dma.DeviceOwned && g_restarts == 1 && transfer.RootElement == &g_qh);
    transfer.Dma.DeviceOwned = false;
    transfer.State = USBTRANSFER_STATE_RESTART;
    g_cancelBegin = 1;
    CHECK(HCITransferQueue(&transfer) == OS_EBUSY);
    CHECK(transfer.Dma.DeviceOwned && g_restarts == 1 && transfer.State == USBTRANSFER_STATE_CLEANUP);
    transfer.Dma.DeviceOwned = false;
    transfer.State = USBTRANSFER_STATE_RESTART;
    g_failBegin = 1;
    CHECK(HCITransferQueue(&transfer) == OS_EUNKNOWN);
    CHECK(!transfer.Dma.DeviceOwned && g_restarts == 1 && transfer.State == USBTRANSFER_STATE_CLEANUP);
    g_failBegin = g_cancelBegin = 0;
}

static void __TestCleanup(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT, .ResultCode = USBTRANSFERCODE_CANCELLED,
        .RootElement = &g_qh, .ChainLength = 1,
        .Dma = { .Linked = true, .DeviceOwned = true }
    };
    memset(&g_qh, 0, sizeof(g_qh));
    g_failStop = 1;
#if !defined(TEST_UHCI) && !defined(TEST_OHCI)
    g_registers.UsbCommand = EHCI_COMMAND_RUN | EHCI_COMMAND_ASYNC_ENABLE;
    g_registers.UsbIntr = 0x3F;
#endif
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_ETIMEOUT);
    CHECK(g_unlinks == 0 && g_cleanups == 0 && transfer.RootElement == &g_qh);
    CHECK(transfer.Dma.DeviceOwned);
    g_failStop = 0;
#if !defined(TEST_UHCI) && !defined(TEST_OHCI)
    // The first periodic request arrives during a failed cancellation stop.
    // It must be enabled when the saved running state is restored.
    CHECK(g_controller.ResumePending);
    EhciEnableScheduler(&g_controller, USBTRANSFER_TYPE_INTERRUPT);
    CHECK(g_controller.ResumeCommand & EHCI_COMMAND_PERIODIC_ENABLE);
    CHECK(!(g_registers.UsbCommand & EHCI_COMMAND_PERIODIC_ENABLE));
#endif
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EOK);
    CHECK(g_unlinks == 1 && g_cleanups == 1 && transfer.RootElement == NULL);
    CHECK(transfer.Dma.DeviceOwned); // The manager still owes DMAEndTransfer.
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EOK);
    CHECK(g_cleanups == 1);
#if !defined(TEST_UHCI) && !defined(TEST_OHCI)
    CHECK(g_registers.UsbCommand == (EHCI_COMMAND_RUN | EHCI_COMMAND_ASYNC_ENABLE | EHCI_COMMAND_PERIODIC_ENABLE));
    CHECK(g_registers.UsbIntr == 0x3F);
    transfer.Type = USBTRANSFER_TYPE_BULK;
    transfer.ResultCode = USBTRANSFERCODE_SUCCESS;
    transfer.RootElement = &g_qh;
    transfer.Dma.Linked = true;
    transfer.Dma.Unlinked = false;
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EBUSY);
    CHECK(g_doorbells == 1 && g_cleanups == 1);
    // An ordinary processing interrupt does not acknowledge async advancement.
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EBUSY && g_cleanups == 1);
    g_controller.AdvanceCompleted = g_controller.AdvanceIssued;
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EOK && g_cleanups == 2);
#endif
}

#if defined(TEST_OHCI)
static void __TestDescriptorBudget(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_BULK, .ElementCount = 1025, .RootElement = &g_qh,
        .Dma = { .DeviceOwned = true }
    };
    int batch, i;
    CHECK(OHCI_TD_COUNT - 1 == 256);
    g_available = OHCI_TD_BUDGET; g_allocated = 0;
    // Eight concurrent requests fill the fixed pool without growing it.
    for (i = 0; i < 8; i++) CHECK(__AllocateDescriptors(&g_controller, &transfer, OHCI_TD_POOL) == 32);
    CHECK(g_allocated == 256 && g_available == 0);
    CHECK(__AllocateDescriptors(&g_controller, &transfer, OHCI_TD_POOL) == 0);
    // Retirement makes a smaller batch possible. Keep cumulative progress and
    // device ownership until all elements of the original request have run.
    g_available = 7;
    CHECK(__AllocateDescriptors(&g_controller, &transfer, OHCI_TD_POOL) == 7);
    transfer.TData.Async.ElementsCompleted = 7;
    while (transfer.TData.Async.ElementsCompleted < transfer.ElementCount) {
        g_available = 256;
        batch = __AllocateDescriptors(&g_controller, &transfer, OHCI_TD_POOL);
        CHECK(batch > 0 && batch <= 32);
        transfer.TData.Async.ElementsCompleted += batch;
        CHECK(transfer.Dma.DeviceOwned);
    }
    CHECK(transfer.TData.Async.ElementsCompleted == 1025);
    transfer.Type = USBTRANSFER_TYPE_ISOC; transfer.ElementCount = 1;
    g_available = 1;
    CHECK(__AllocateDescriptors(&g_controller, &transfer, OHCI_iTD_POOL) == 1);
    CHECK(g_lastPool == OHCI_iTD_POOL);
    g_available = 1000;

    transfer.Type = USBTRANSFER_TYPE_INTERRUPT;
    transfer.ResultCode = USBTRANSFERCODE_SUCCESS;
    transfer.Dma.Linked = true; transfer.Dma.Unlinked = true;
    transfer.Dma.ReleaseEpoch = 12; g_registers.HcFmNumber = 10;
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EBUSY);
    CHECK(g_registers.HcInterruptEnable == OHCI_SOF_EVENT && transfer.RootElement == &g_qh);
    g_registers.HcFmNumber = 12;
    CHECK(HCITransferFinalize(&g_controller.Base, &transfer, false) == OS_EOK);
    CHECK(transfer.RootElement == NULL && transfer.Dma.DeviceOwned);
}
#endif

#if defined(TEST_UHCI)
extern oserr_t g_bounceReserveStatus;

static void
__TestBounceAdmission(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_BULK, .MaxPacketSize = 64
    };
    int available = g_available;
    int begins = g_begins;

    // Pool admission precedes QH/TD allocation and the first DMA handoff.
    g_bounceReserveStatus = OS_EBUSY;
    CHECK(HCITransferQueue(&transfer) == OS_EOK);
    CHECK(transfer.State == USBTRANSFER_STATE_WAITING);
    CHECK(!transfer.RootElement && !transfer.Dma.DeviceOwned);
    CHECK(g_available == available && g_begins == begins);
    g_bounceReserveStatus = OS_EOOM;
    CHECK(HCITransferQueue(&transfer) == OS_EOOM);
    CHECK(g_available == available && g_begins == begins);
    g_bounceReserveStatus = OS_EOK;
}
#endif

int main(void)
{
    g_controller.Base.Scheduler = &g_scheduler;
#if defined(TEST_OHCI)
    g_controller.Registers = &g_registers;
    g_controller.TransactionQueueControlIndex = USB_ELEMENT_NO_INDEX;
    g_controller.TransactionQueueBulkIndex = USB_ELEMENT_NO_INDEX;
#elif !defined(TEST_UHCI)
    g_controller.OpRegisters = &g_registers;
#endif
#if defined(TEST_OHCI)
    // Failed fatal-error cleanup defers destruction instead of resetting pools.
    atomic_store(&g_controller.Base.InterruptStatus, OHCI_FATAL_EVENT);
    HciInterruptCallback(&g_controller.Base);
    CHECK(g_controller.Base.State.Phase == USB_CONTROLLER_DESTROY_PENDING);
    g_controller.Base.State.Phase = USB_CONTROLLER_READY;
    // Two pending requests use the software chain sentinel, not physical zero.
    g_registers.HcCommandStatus = OHCI_COMMAND_CONTROL_FILLED | OHCI_COMMAND_BULK_FILLED;
    g_controller.TransactionsWaitingControl = g_controller.TransactionsWaitingBulk = 1;
    g_controller.TransactionQueueControlIndex = g_controller.TransactionQueueBulkIndex = 1;
    OHCIQHLink(&g_controller, USBTRANSFER_TYPE_CONTROL, &g_qh);
    OHCIQHLink(&g_controller, USBTRANSFER_TYPE_BULK, &g_qh);
    CHECK(g_controller.TransactionsWaitingControl == 2 && g_controller.TransactionsWaitingBulk == 2);
    g_controller.TransactionsWaitingControl = g_controller.TransactionsWaitingBulk = 0;
    g_controller.TransactionQueueControlIndex = g_controller.TransactionQueueBulkIndex = USB_ELEMENT_NO_INDEX;
#endif
#if defined(TEST_UHCI)
    __TestBounceAdmission();
#endif
    __TestGeometry();
    __TestPreparation();
    __TestHandoff();
    __TestCleanup();
#if defined(TEST_OHCI)
    __TestDescriptorBudget();
#endif
    puts("Legacy USB DMA: translated geometry, QH preservation, stop failure, cleanup retry and descriptor retirement passed");
    return 0;
}


