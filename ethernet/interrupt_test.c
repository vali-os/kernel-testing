#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <event.h>
#include <ioctl.h>
#include <ioset.h>
#include "../../modules/eth/genet/genet.h"

// Calls the module makes into the system, in the order they are made.
enum Call {
    CallEventCreate,
    CallNonBlocking,
    CallLoopAdd,
    CallInitialize,
    CallSelectSource,
    CallAllocate,
    CallSetDescriptor,
    CallSetHandler,
    CallAddRegisters,
    CallAddShared,
    CallEnable,
    CallMask,
    CallDestroy,
    CallLoopRemove,
    CallClose,
    CallPoll,
    CallUnmask,
    CallFault
};

#define EVENT_DESCRIPTOR 7
#define LOOP_HANDLE      99
#define MAX_CALLS        32

extern unsigned g_diagnosticLogs;

static enum Call g_calls[MAX_CALLS];
static unsigned g_callCount;
static int g_failAt = -1;
static int g_allocated;
static int g_descriptorsOpen;
static int g_loopEntries;
static int g_readFails;
static int g_eventRaised;
static int g_queryFails;
static unsigned g_unmaskedCauses;
static oserr_t g_faultStatus;
static unsigned g_polls;
static unsigned g_unmasks;
static unsigned g_faults;
#ifdef VALI_ETHERNET_DIAGNOSTICS
static unsigned g_diagnosticInterrupts;
#endif
static EthernetDevice_t* g_polledAdapter;

static Device_t g_device = { .Id = 21 };
static DeviceIo_t g_registersIo;
static uint32_t g_status;
static uint32_t g_mask;
static unsigned g_writeOffsets[8];
static unsigned g_writeCount;
static int g_writeFails;
static unsigned g_signals;
static uuid_t g_signalHandle;
static int g_signalFails;
static oserr_t g_signalResult;
static unsigned g_reads;
static GenetInterrupt_t* g_interruptDuringPoll;
static uint32_t g_causesDuringPoll;

static void
__Reset(void)
{
    g_callCount = 0;
    g_failAt = -1;
    g_allocated = 0;
    g_descriptorsOpen = 0;
    g_loopEntries = 0;
    g_readFails = 0;
    g_eventRaised = 1;
    g_queryFails = 0;
    g_reads = 0;
    g_interruptDuringPoll = NULL;
    g_polls = 0;
    g_unmasks = 0;
    g_faults = 0;
    g_unmaskedCauses = 0;
    g_polledAdapter = NULL;
}

static void
__Record(
    _In_ enum Call call)
{
    assert(g_callCount < MAX_CALLS);
    g_calls[g_callCount++] = call;
}

// Records the call and tells whether the test wants it to fail.
static int
__Fails(
    _In_ enum Call call)
{
    __Record(call);
    return g_failAt == (int)call;
}

static void
__ExpectCalls(
    _In_ const enum Call* expected,
    _In_ unsigned         count)
{
    assert(g_callCount == count);
    for (unsigned i = 0; i < count; i++) {
        assert(g_calls[i] == expected[i]);
    }
}

int
eventd(
    unsigned int initialValue,
    unsigned int flags)
{
    if (__Fails(CallEventCreate)) {
        return -1;
    }
    assert(initialValue == 0 && flags == EVT_RESET_EVENT);
    g_descriptorsOpen++;
    return EVENT_DESCRIPTOR;
}

int
__wrap_ioctl(
    int           descriptor,
    unsigned long request,
    ...)
{
    va_list arguments;
    int*    value;

    va_start(arguments, request);
    value = va_arg(arguments, int*);
    va_end(arguments);

    // The service asks for the counter before it reads it. This query is not
    // recorded, so the expected call lists stay the same.
    if (request == FIONREAD) {
        assert(descriptor == EVENT_DESCRIPTOR && value != NULL);
        if (g_queryFails) {
            return -1;
        }
        *value = g_eventRaised;
        return 0;
    }

    if (__Fails(CallNonBlocking)) {
        return -1;
    }
    assert(descriptor == EVENT_DESCRIPTOR && request == FIONBIO && *value == 1);
    return 0;
}

int
ioset_ctrl(
    int                 setDescriptor,
    int                 operation,
    int                 descriptor,
    struct ioset_event* event)
{
    assert(setDescriptor == LOOP_HANDLE && descriptor == EVENT_DESCRIPTOR);
    if (operation == IOSET_ADD) {
        if (__Fails(CallLoopAdd)) {
            return -1;
        }
        assert(event->events == IOSETSYN && event->data.iod == EVENT_DESCRIPTOR);
        g_loopEntries++;
        return 0;
    }
    // Removal passes the same event that was used when adding.
    assert(operation == IOSET_DEL && event != NULL);
    if (__Fails(CallLoopRemove)) {
        return -1;
    }
    assert(event->events == IOSETSYN && event->data.iod == EVENT_DESCRIPTOR);
    g_loopEntries--;
    return 0;
}

int
__wrap_close(
    int descriptor)
{
    assert(descriptor == EVENT_DESCRIPTOR);
    __Record(CallClose);
    g_descriptorsOpen--;
    return 0;
}

int
__wrap_read(
    int          descriptor,
    void*        buffer,
    unsigned int length)
{
    assert(descriptor == EVENT_DESCRIPTOR && length == sizeof(unsigned int));
    g_reads++;
    if (g_readFails) {
        return -1;
    }
    memset(buffer, 0, length);
    return (int)length;
}

gracht_server_t*
EthernetServer(void)
{
    return (gracht_server_t*)1;
}

gracht_handle_t
gracht_server_get_aio_handle(
    gracht_server_t* server)
{
    assert(server == (gracht_server_t*)1);
    return LOOP_HANDLE;
}

void
DeviceInterruptInitialize(
    _Out_ DeviceInterrupt_t* interrupt,
    _In_  Device_t*          device)
{
    __Record(CallInitialize);
    assert(device == &g_device);
    memset(interrupt, 0, sizeof(*interrupt));
}

oserr_t
DeviceInterruptSelectSource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index)
{
    if (__Fails(CallSelectSource)) {
        return OS_EINVALPARAMS;
    }
    assert(index == GENET_PACKET_INTERRUPT_INDEX);
    return OS_EOK;
}

oserr_t
DeviceInterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           min,
    _In_    uint32_t           optimal,
    _Out_   uint32_t*          countOut)
{
    if (__Fails(CallAllocate)) {
        *countOut = 0;
        return OS_ENOTSUPPORTED;
    }
    assert(min == 1 && optimal == 1);
    g_allocated = 1;
    *countOut = 1;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetDescriptor(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    int                descriptor)
{
    if (__Fails(CallSetDescriptor)) {
        return OS_EBUSY;
    }
    assert(index == 0 && descriptor == EVENT_DESCRIPTOR);
    return OS_EOK;
}

oserr_t
DeviceInterruptSetHandler(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    InterruptHandler_t handler)
{
    if (__Fails(CallSetHandler)) {
        return OS_EBUSY;
    }
    assert(index == 0 && handler == GenetFastInterrupt);
    return OS_EOK;
}

oserr_t
DeviceInterruptAddIoResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    DeviceIo_t*        ioSpace)
{
    GenetController_t* controller = (GenetController_t*)((char*)interrupt - offsetof(GenetController_t, Interrupt));

    if (__Fails(CallAddRegisters)) {
        return OS_EOOM;
    }
    assert(index == 0 && ioSpace == &controller->Registers);
    return OS_EOK;
}

oserr_t
DeviceInterruptAddMemoryResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    uintptr_t          address,
    _In_    size_t             length,
    _In_    unsigned int       flags)
{
    GenetController_t* controller = (GenetController_t*)((char*)interrupt - offsetof(GenetController_t, Interrupt));

    if (__Fails(CallAddShared)) {
        return OS_EOOM;
    }
    assert(index == 0 && flags == 0);
    assert(address == (uintptr_t)&controller->Interrupt.Shared);
    assert(length == sizeof(controller->Interrupt.Shared));
    return OS_EOK;
}

oserr_t
DeviceInterruptEnable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    if (__Fails(CallEnable)) {
        return OS_EBUSY;
    }
    return OS_EOK;
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    if (__Fails(CallDestroy)) {
        return OS_EBUSY;
    }
    g_allocated = 0;
    return OS_EOK;
}

oserr_t
GenetMacMaskInterrupts(
    _In_ GenetController_t* controller)
{
    return __Fails(CallMask) ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
GenetMacUnmaskDmaInterrupts(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes)
{
    g_unmasks++;
    g_unmaskedCauses = causes;
    return __Fails(CallUnmask) ? OS_EDEVFAULT : OS_EOK;
}

void
EthernetPoll(
    _In_ EthernetDevice_t* device)
{
    __Record(CallPoll);
    g_polls++;
    g_polledAdapter = device;

    // Model the handler firing while the rings drain.
    if (g_interruptDuringPoll != NULL) {
        atomic_fetch_or(&g_interruptDuringPoll->Shared.Causes, g_causesDuringPoll);
    }
}

void
EthernetFault(
    _In_ EthernetDevice_t* device,
    _In_ oserr_t           status)
{
    __Record(CallFault);
    g_faults++;
    g_faultStatus = status;
}

#ifdef VALI_ETHERNET_DIAGNOSTICS
void
GenetLogInterrupt(
    _In_ GenetController_t* controller,
    _In_ uint32_t           causes)
{
    g_diagnosticInterrupts++;
}
#endif

// A model of INTRL2 bank 0: raw status, mask, and write-to-set/clear registers.
static size_t
__FakeRead(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    assert(io == &g_registersIo && length == GENET_REGISTER_SIZE);
    if (offset == GENET_INTR0_STATUS) {
        return g_status;
    }
    assert(offset == GENET_INTR0_MASK_STATUS);
    return g_mask;
}

static oserr_t
__FakeWrite(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(io == &g_registersIo && length == GENET_REGISTER_SIZE);
    assert(g_writeCount < sizeof(g_writeOffsets) / sizeof(g_writeOffsets[0]));
    g_writeOffsets[g_writeCount++] = (unsigned)offset;
    if (offset == GENET_INTR0_CLEAR) {
        g_status &= ~(uint32_t)value;
    } else {
        assert(offset == GENET_INTR0_MASK_SET);
        g_mask |= (uint32_t)value;
    }
    return g_writeFails ? OS_EDEVFAULT : OS_EOK;
}

static oserr_t
__FakeSignal(
    _In_ uuid_t handle)
{
    g_signals++;
    g_signalHandle = handle;
    return g_signalFails ? OS_EDEVFAULT : g_signalResult;
}

// The handler may not call Trace: its message text is outside the mapped memory.
static void
__FakeTrace(
    _In_ const char* format,
    ...)
{
    assert(0);
}

static irqstatus_t
__RunHandler(
    _InOut_ GenetInterruptShared_t* shared)
{
    InterruptFunctionTable_t table = {
        .ReadIoSpace = __FakeRead,
        .WriteIoSpace = __FakeWrite,
        .EventSignal = __FakeSignal,
        .Trace = __FakeTrace
    };
    InterruptResourceTable_t resources = { .HandleResource = 55 };

    resources.IoResources[0] = &g_registersIo;
    resources.MemoryResources[0].Address = (uintptr_t)shared;
    return GenetFastInterrupt(&table, &resources);
}

static void
__ResetHandlerState(
    _In_ uint32_t status,
    _In_ uint32_t mask)
{
    g_status = status;
    g_mask = mask;
    g_writeCount = 0;
    g_writeFails = 0;
    g_signals = 0;
    g_signalFails = 0;

    // The kernel returns OS_EINCOMPLETE when a signal raises the event
    // counter. This is the normal result of a successful signal.
    g_signalResult = OS_EINCOMPLETE;
}

static void
__FastHandler(void)
{
    GenetInterruptShared_t shared;

    atomic_init(&shared.Causes, 0);
    atomic_init(&shared.Failures, 0);

    // Nothing pending: the interrupt is not ours and nothing is written.
    __ResetHandlerState(0, GENET_INTR_ALL);
    assert(__RunHandler(&shared) == IRQSTATUS_NOT_HANDLED);
    assert(g_writeCount == 0 && g_signals == 0 && atomic_load(&shared.Causes) == 0);

    // A cause that is set but masked cannot be what raised the line.
    __ResetHandlerState(GENET_INTR0_RX_DONE, GENET_INTR_ALL);
    assert(__RunHandler(&shared) == IRQSTATUS_NOT_HANDLED);
    assert(g_writeCount == 0 && g_signals == 0);

    // A packet cause is masked, then cleared, and handed to the module.
    __ResetHandlerState(GENET_INTR0_RX_DONE, ~GENET_INTR0_DMA_DONE);
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(g_writeCount == 2);
    assert(g_writeOffsets[0] == GENET_INTR0_MASK_SET && g_writeOffsets[1] == GENET_INTR0_CLEAR);
    assert(g_mask & GENET_INTR0_RX_DONE);
    assert(!(g_status & GENET_INTR0_RX_DONE));
    assert(atomic_load(&shared.Causes) == GENET_INTR0_RX_DONE);
    assert(g_signals == 1 && g_signalHandle == 55);

    // Causes that arrive before the module runs accumulate.
    g_status = GENET_INTR0_TX_DONE;
    g_signals = 0;
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(atomic_load(&shared.Causes) == GENET_INTR0_DMA_DONE);
    assert(g_signals == 1);

    // Both packet causes at once are reported with one wake-up.
    atomic_store(&shared.Causes, 0);
    __ResetHandlerState(GENET_INTR0_DMA_DONE, ~GENET_INTR0_DMA_DONE);
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(atomic_load(&shared.Causes) == GENET_INTR0_DMA_DONE && g_signals == 1);

    // Any other unmasked cause must be silenced too, or the level triggered
    // line would stay asserted. It is not reported as a packet cause.
    atomic_store(&shared.Causes, 0);
    __ResetHandlerState(1U << 4, 0);
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(g_mask & (1U << 4));
    assert(!(g_status & (1U << 4)));
    assert(atomic_load(&shared.Causes) == 0 && g_signals == 0);

    // Failures cannot be handled in interrupt context. They are counted for
    // the module to report, and the handler still finishes.
    __ResetHandlerState(GENET_INTR0_RX_DONE, 0);
    g_writeFails = 1;
    g_signalFails = 1;
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(atomic_load(&shared.Failures) == 3 && g_signals == 1);
    assert(atomic_load(&shared.Causes) == GENET_INTR0_RX_DONE);

    // A clean run leaves the count alone.
    atomic_store(&shared.Failures, 0);
    __ResetHandlerState(GENET_INTR0_TX_DONE, 0);
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(atomic_load(&shared.Failures) == 0 && g_signals == 1);

    // OS_EOK means the event counter was already raised and the signal merged
    // with the earlier one. That is a success too.
    __ResetHandlerState(GENET_INTR0_RX_DONE, 0);
    g_signalResult = OS_EOK;
    assert(__RunHandler(&shared) == IRQSTATUS_HANDLED);
    assert(atomic_load(&shared.Failures) == 0 && g_signals == 1);
}

static void
__ResetController(
    _Out_ GenetController_t* controller)
{
    memset(controller, 0, sizeof(*controller));
    controller->Device = &g_device;
    controller->State = GENET_STATE_MAPPED;
    __Reset();
}

static void
__Register(void)
{
    static GenetController_t controller;
    const enum Call expected[] = {
        CallEventCreate, CallNonBlocking, CallLoopAdd, CallInitialize, CallSelectSource,
        CallAllocate, CallSetDescriptor, CallSetHandler, CallAddRegisters, CallAddShared,
        CallEnable
    };

    __ResetController(&controller);
    atomic_store(&controller.Interrupt.Shared.Causes, 0xdead);
    atomic_store(&controller.Interrupt.Shared.Failures, 0xdead);
    assert(GenetInterruptRegister(&controller) == OS_EOK);
    __ExpectCalls(expected, 11);
    assert(controller.Interrupt.State == GENET_INTERRUPT_ENABLED);
    assert(controller.Interrupt.EventDescriptor == EVENT_DESCRIPTOR);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == 0);
    assert(atomic_load(&controller.Interrupt.Shared.Failures) == 0);
    assert(g_descriptorsOpen == 1 && g_loopEntries == 1 && g_allocated);
    assert(GenetServiceInterval(&controller) == GENET_MAINTENANCE_INTERVAL_MS);
    assert(GenetInterruptIsEvent(&controller, EVENT_DESCRIPTOR));
    assert(!GenetInterruptIsEvent(&controller, EVENT_DESCRIPTOR + 1));

    // Release masks the device first, then stops delivery, then drops the descriptor.
    const enum Call released[] = { CallMask, CallDestroy, CallLoopRemove, CallClose };
    g_callCount = 0;
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    __ExpectCalls(released, 4);
    assert(controller.Interrupt.State == GENET_INTERRUPT_NONE);
    assert(g_descriptorsOpen == 0 && g_loopEntries == 0 && !g_allocated);
    assert(GenetServiceInterval(&controller) == GENET_POLL_INTERVAL_MS);
    assert(!GenetInterruptIsEvent(&controller, EVENT_DESCRIPTOR));
}

static void
__RegisterFailures(void)
{
    static GenetController_t controller;
    const enum Call failing[] = {
        CallEventCreate, CallNonBlocking, CallLoopAdd, CallSelectSource, CallAllocate,
        CallSetDescriptor, CallSetHandler, CallAddRegisters, CallAddShared, CallEnable
    };

    unsigned failed;

    for (unsigned i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        __ResetController(&controller);
        g_failAt = failing[i];
        assert(GenetInterruptRegister(&controller) != OS_EOK);

        // Nothing past the failed step runs, except for closing a descriptor
        // that the module opened itself. The driver keeps polling.
        for (failed = 0; g_calls[failed] != failing[i]; failed++) {
        }
        for (unsigned next = failed + 1; next < g_callCount; next++) {
            assert(g_calls[next] == CallClose);
        }
        assert(controller.Interrupt.State != GENET_INTERRUPT_ENABLED);
        assert(GenetServiceInterval(&controller) == GENET_POLL_INTERVAL_MS);

        // Cleaning up after any failure releases everything that was set up.
        g_failAt = -1;
        assert(GenetInterruptUnregister(&controller) == OS_EOK);
        assert(g_descriptorsOpen == 0 && g_loopEntries == 0 && !g_allocated);
        assert(controller.Interrupt.State == GENET_INTERRUPT_NONE);
    }
}

static void
__Unregister(void)
{
    static GenetController_t controller;
    const enum Call          retry[] = { CallMask, CallDestroy };
    const enum Call          removeFailed[] = { CallMask, CallDestroy, CallLoopRemove, CallClose };
    unsigned                 logs;

    // Nothing registered: release is harmless and touches neither the loop nor a descriptor.
    __ResetController(&controller);
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    assert(g_loopEntries == 0 && g_descriptorsOpen == 0);

    // If delivery cannot be stopped, the handler may still run. The descriptor
    // and shared state stay valid until a later attempt succeeds.
    __ResetController(&controller);
    assert(GenetInterruptRegister(&controller) == OS_EOK);
    g_callCount = 0;
    g_failAt = CallDestroy;
    assert(GenetInterruptUnregister(&controller) == OS_EBUSY);
    __ExpectCalls(retry, 2);
    assert(controller.Interrupt.State == GENET_INTERRUPT_ENABLED);
    assert(g_descriptorsOpen == 1 && g_loopEntries == 1);
    g_failAt = -1;
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    assert(g_descriptorsOpen == 0 && g_loopEntries == 0 && !g_allocated);

    // The device mask is best effort. Stopping delivery still goes ahead.
    __ResetController(&controller);
    assert(GenetInterruptRegister(&controller) == OS_EOK);
    g_failAt = CallMask;
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    assert(g_descriptorsOpen == 0 && !g_allocated);

    // A failed removal from the I/O set is logged. The descriptor is still
    // closed, since it cannot be used after the interrupt is destroyed.
    __ResetController(&controller);
    assert(GenetInterruptRegister(&controller) == OS_EOK);
    g_callCount = 0;
    g_failAt = CallLoopRemove;
    logs = g_diagnosticLogs;
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    __ExpectCalls(removeFailed, 4);
    assert(g_diagnosticLogs == logs + 1);
    assert(controller.Interrupt.State == GENET_INTERRUPT_NONE);
    assert(g_descriptorsOpen == 0 && !g_allocated);

    // Without mapped registers there is no device to mask.
    __ResetController(&controller);
    assert(GenetInterruptRegister(&controller) == OS_EOK);
    controller.State = GENET_STATE_DESTROYED;
    g_callCount = 0;
    assert(GenetInterruptUnregister(&controller) == OS_EOK);
    assert(g_calls[0] == CallDestroy);
}

static void
__ServiceSetup(
    _Out_ GenetController_t* controller,
    _In_  uint32_t           causes)
{
    __ResetController(controller);
    controller->State = GENET_STATE_READY;
    controller->Queues.DmaState = GENET_DMA_STATE_MAY_BE_RUNNING;
    controller->Interrupt.EventDescriptor = EVENT_DESCRIPTOR;
    controller->Interrupt.State = GENET_INTERRUPT_ENABLED;
    atomic_store(&controller->Interrupt.Shared.Causes, causes);
}

static void
__Service(void)
{
    static GenetController_t controller;
    const enum Call          served[] = { CallPoll, CallUnmask };
    unsigned                 logs;

    // The rings are drained before the causes are unmasked, and only the
    // causes the handler masked are unmasked again.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    GenetServiceInterrupt(&controller);
    __ExpectCalls(served, 2);
    assert(g_polledAdapter == &controller.Adapter);
    assert(g_unmaskedCauses == GENET_INTR0_RX_DONE);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == 0);
#ifdef VALI_ETHERNET_DIAGNOSTICS
    assert(g_diagnosticInterrupts == 1);
#endif

    // A wake-up whose causes were already taken only drains the rings.
    g_callCount = 0;
    g_polls = 0;
    g_unmasks = 0;
    GenetServiceInterrupt(&controller);
    assert(g_polls == 1 && g_unmasks == 0 && g_reads == 2);

    // Both causes are unmasked together.
    __ServiceSetup(&controller, GENET_INTR0_DMA_DONE);
    GenetServiceInterrupt(&controller);
    assert(g_unmaskedCauses == GENET_INTR0_DMA_DONE);

    // A failed read of the event does not strand the causes the handler masked.
    __ServiceSetup(&controller, GENET_INTR0_TX_DONE);
    g_readFails = 1;
    logs = g_diagnosticLogs;
    GenetServiceInterrupt(&controller);
    assert(g_polls == 1 && g_unmaskedCauses == GENET_INTR0_TX_DONE);
    assert(g_diagnosticLogs == logs + 1);

    // A second interrupt that lands after the loop took the first event shares
    // its raised counter, and its event then finds the counter at 0. That is
    // normal: no read, no log, and the rings are still drained.
    __ServiceSetup(&controller, 0);
    g_eventRaised = 0;
    logs = g_diagnosticLogs;
    GenetServiceInterrupt(&controller);
    assert(g_reads == 0 && g_polls == 1 && g_unmasks == 0);
    assert(g_diagnosticLogs == logs);

    // A failed query is logged, and the causes are still served.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    g_queryFails = 1;
    logs = g_diagnosticLogs;
    GenetServiceInterrupt(&controller);
    assert(g_reads == 0 && g_polls == 1 && g_unmaskedCauses == GENET_INTR0_RX_DONE);
    assert(g_diagnosticLogs == logs + 1);

    // After a stop the causes stay masked until the next start unmasks them.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    controller.Queues.DmaState = GENET_DMA_STATE_STOPPED;
    GenetServiceInterrupt(&controller);
    assert(g_polls == 1 && g_unmasks == 0 && g_faults == 0);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == 0);

    // A late event during a failed DMA stop must not poll or unmask causes.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    controller.State = GENET_STATE_STOPPING;
    GenetServiceInterrupt(&controller);
    assert(g_polls == 0 && g_unmasks == 0);

    // Causes that cannot be unmasked would stay silent, so the adapter faults.
    // A stop and start recovers it.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    g_failAt = CallUnmask;
    GenetServiceInterrupt(&controller);
    assert(g_faults == 1 && g_faultStatus == OS_EDEVFAULT);
    assert(g_calls[g_callCount - 1] == CallFault);
}

static void
__MaintenancePoll(void)
{
    static GenetController_t controller;
    unsigned                 logs;

    // The maintenance poll finds causes whose wake-up was lost, and unmasks
    // them without touching the event descriptor.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    GenetServiceRings(&controller);
    assert(g_reads == 0 && g_polls == 1);
    assert(g_unmaskedCauses == GENET_INTR0_RX_DONE);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == 0);

    // Without pending causes it only drains the rings.
    g_unmasks = 0;
    GenetServiceRings(&controller);
    assert(g_polls == 2 && g_unmasks == 0);

    // The same goes for polling mode, where no cause is ever recorded.
    __Reset();
    controller.Interrupt.State = GENET_INTERRUPT_NONE;
    GenetServiceRings(&controller);
    assert(g_polls == 1 && g_unmasks == 0 && g_faults == 0);

    // A cause the handler masks while the rings drain was not part of this
    // poll's unmask. It stays recorded, so the next poll unmasks it.
    __ServiceSetup(&controller, GENET_INTR0_RX_DONE);
    g_interruptDuringPoll = &controller.Interrupt;
    g_causesDuringPoll = GENET_INTR0_TX_DONE;
    GenetServiceRings(&controller);
    assert(g_unmaskedCauses == GENET_INTR0_RX_DONE);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == GENET_INTR0_TX_DONE);
    g_interruptDuringPoll = NULL;
    GenetServiceRings(&controller);
    assert(g_unmaskedCauses == GENET_INTR0_TX_DONE);
    assert(atomic_load(&controller.Interrupt.Shared.Causes) == 0);

    // Failures counted by the handler are reported once and then cleared.
    __ServiceSetup(&controller, 0);
    atomic_store(&controller.Interrupt.Shared.Failures, 3);
    logs = g_diagnosticLogs;
    GenetServiceRings(&controller);
    assert(g_diagnosticLogs == logs + 1);
    assert(atomic_load(&controller.Interrupt.Shared.Failures) == 0);
    GenetServiceRings(&controller);
    assert(g_diagnosticLogs == logs + 1);
}

int
main(void)
{
    __FastHandler();
    __Register();
    __RegisterFailures();
    __Unregister();
    __Service();
    __MaintenancePoll();
    puts("GENET interrupt tests passed");
    return 0;
}
