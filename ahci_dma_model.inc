// Real AHCI lifecycle, PRDT and completion code with translated DMA addresses.
#define __need_minmax
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            printf("line %d: %s\n", __LINE__, #x);                                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void* __AhciAllocate(size_t size);
static void __AhciFree(void* pointer);
#define close __TestClose
#define read __TestRead
#define ioctl __TestIoctl
#define malloc __AhciAllocate
#define free __AhciFree
#include "../modules/storage/ahci/ahci.c"
#include "../modules/storage/ahci/port.c"
#include "../modules/storage/ahci/dispatch.c"
#include "../modules/storage/ahci/transactions.c"
#include "../modules/storage/ahci/main.c"
#undef malloc
#undef close
#undef read
#undef ioctl
#undef free
#include "../librt/libds/list.c"

// Device address limit used to verify controller capability propagation.
struct DMAContext {
    uint64_t Limit;
};
// Persistent CPU allocation and the distinct address visible to hardware.
struct DMABuffer {
    void* Data;
    size_t Length;
    struct DMASegment Segment;
};
// Request segments and device ownership, checked on every transfer transition.
struct DMAMapping {
    bool Owned;
    size_t Length;
    uint32_t Count;
    struct DMASegment Segments[300];
};
static uint64_t g_mmio[1024];
static AhciController_t* g_controller;
static int g_allocations, g_contexts, g_buffers, g_mappings, g_owned;
static int g_events, g_iosets, g_io, g_irqs, g_responses, g_controls;
static int g_allocateCalls, g_freeCalls, g_mapCalls, g_beginCalls, g_endCalls, g_unmapCalls;
static int g_irqFailure, g_openFailure, g_closeFailure, g_mapFailure, g_beginFailure, g_endFailure,
    g_unmapFailure;
static int g_failAllocate, g_failMalloc, g_mallocCalls;
static unsigned int g_stopBlock;
static int g_recoverAfterSleep;
static uint64_t g_nextAddress;
static size_t g_lastMapOffset, g_lastMapLength, g_lastEndLength;
static enum DMADirection g_lastDirection;
static struct DMABuffer* g_failFree;
static struct DMASegment g_segments[300];
static uint32_t g_segmentCount;
static oserr_t g_responseStatus;
static size_t g_responseSectors;
static void (*g_responseHook)(void);
static usched_task_fn g_worker;
static int g_warnings;
static int g_hostErrno;
static AhciDevice_t* g_logicalDevice;
static oserr_t g_ioctlStatus;
static struct OSIOCtlRequestRequirements g_ioRequirements;

static void*
__AhciAllocate(
    _In_ size_t size)
{
    void* memory;

    if (++g_mallocCalls == g_failMalloc) {
        return NULL;
    }
    memory = calloc(1, size);
    CHECK(memory != NULL);
    g_allocations++;
    return memory;
}

static void
__AhciFree(
    _In_ void* pointer)
{
    if (pointer != NULL) {
        g_allocations--;
        free(pointer);
    }
}

static AHCIPortRegisters_t*
__Registers(void)
{
    return (void*)((uint8_t*)g_mmio + AHCI_REGISTER_PORTBASE(0));
}

void
ReadVolatileMemory(
    _In_ const volatile void* source,
    _Out_ volatile void* destination,
    _In_ size_t length)
{
    CHECK(length == 4);
    *(volatile uint32_t*)destination = *(const volatile uint32_t*)source;
}

void
WriteVolatileMemory(
    _In_ volatile void* destination,
    _In_ void* source,
    _In_ size_t length)
{
    uint32_t value = *(uint32_t*)source;

    CHECK(length == 4);
    if (destination == &__Registers()->CI || destination == &__Registers()->SACT) {
        *(volatile uint32_t*)destination |= value;
    }
    else if (destination == &__Registers()->InterruptStatus || destination == &__Registers()->SERR) {
        *(volatile uint32_t*)destination &= ~value;
    }
    else {
        *(volatile uint32_t*)destination = value;
    }
}

int*
__errno(void)
{
    return &g_hostErrno;
}
void
spinlock_init(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
}

oserr_t
DMAOpen(
    _In_ uuid_t device,
    _In_ uint64_t limit,
    _In_ struct DMAContext** output)
{
    CHECK(device == 7 && *output == NULL);
    *output = calloc(1, sizeof(**output));
    (*output)->Limit = limit;
    g_nextAddress = limit == UINT32_MAX ? 0x3456000 : UINT64_C(0x123456000);
    g_contexts++;
    return g_openFailure ? OS_EUNKNOWN : OS_EOK;
}

oserr_t
DMAClose(
    _In_ struct DMAContext** context)
{
    if (*context == NULL) {
        return OS_EOK;
    }
    CHECK(g_buffers == 0 && g_mappings == 0 && g_owned == 0);
    if (g_closeFailure) {
        return OS_EBUSY;
    }
    free(*context);
    *context = NULL;
    g_contexts--;
    return OS_EOK;
}

oserr_t
DMAAllocate(
    _In_ struct DMAContext* context,
    _In_ const struct DMARequirements* requirements,
    _In_ struct DMABuffer** output)
{
    struct DMABuffer* buffer;

    CHECK(context != NULL && *output == NULL && requirements->Contiguous);
    if (++g_allocateCalls == g_failAllocate) {
        return OS_EOOM;
    }
    buffer = calloc(1, sizeof(*buffer));
    buffer->Length = (requirements->Length + 4095) & ~(size_t)4095;
    buffer->Data = calloc(1, buffer->Length);
    CHECK(buffer->Data != NULL);
    buffer->Segment = (struct DMASegment){g_nextAddress, buffer->Length};
    CHECK(buffer->Segment.Address % requirements->Alignment == 0);
    CHECK(buffer->Segment.Address + buffer->Length - 1 <= context->Limit);
    g_nextAddress += buffer->Length;
    *output = buffer;
    g_buffers++;
    return OS_EOK;
}
void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    CHECK(buffer != NULL);
    return buffer->Data;
}
size_t
DMABufferSize(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Length;
}
const struct DMASegment*
DMABufferSegments(
    _In_ const struct DMABuffer* buffer,
    _In_ uint32_t* count)
{
    *count = 1;
    return &buffer->Segment;
}

oserr_t
DMAFree(
    _In_ struct DMABuffer** slot)
{
    struct DMABuffer* buffer = *slot;

    if (buffer == NULL) {
        return OS_EOK;
    }
    CHECK((__Registers()->CommandAndStatus & (AHCI_PORT_ST | AHCI_PORT_CR | AHCI_PORT_FRE | AHCI_PORT_FR)) ==
          0);
    CHECK(g_irqs == 0 && g_owned == 0 && g_mappings == 0);
    g_freeCalls++;
    if (buffer == g_failFree) {
        return OS_EBUSY;
    }
    free(buffer->Data);
    free(buffer);
    *slot = NULL;
    g_buffers--;
    return OS_EOK;
}

oserr_t
DMAMapSHM(
    _In_ struct DMAContext* context,
    _In_ const OSHandle_t* buffer,
    _In_ size_t offset,
    _In_ size_t length,
    _In_ enum DMADirection direction,
    _In_ struct DMAMapping** output)
{
    struct DMAMapping* mapping;

    CHECK(context != NULL && *output == NULL && buffer->ID == 42 && buffer->Type == OSHANDLE_SHM);
    g_mapCalls++;
    g_lastMapOffset = offset;
    g_lastMapLength = length;
    g_lastDirection = direction;
    if (g_mapFailure) {
        return OS_EOOM;
    }
    mapping = calloc(1, sizeof(*mapping));
    mapping->Length = length;
    mapping->Count = g_segmentCount == 0 ? 1 : g_segmentCount;
    if (g_segmentCount == 0) {
        mapping->Segments[0] = (struct DMASegment){g_nextAddress + 0x100000, length};
    }
    else {
        memcpy(mapping->Segments, g_segments, g_segmentCount * sizeof(g_segments[0]));
    }
    *output = mapping;
    g_mappings++;
    return OS_EOK;
}
const struct DMASegment*
DMAMappingSegments(
    _In_ const struct DMAMapping* mapping,
    _In_ uint32_t* count)
{
    *count = mapping->Count;
    return mapping->Segments;
}

oserr_t
DMABeginTransfer(
    _In_ struct DMAMapping* mapping,
    _In_ size_t offset,
    _In_ size_t length)
{
    CHECK(!mapping->Owned && offset == 0 && length == mapping->Length);
    g_beginCalls++;
    if (g_beginFailure) {
        return OS_EUNKNOWN;
    }
    mapping->Owned = true;
    g_owned++;
    return OS_EOK;
}

oserr_t
DMAEndTransfer(
    _In_ struct DMAMapping* mapping,
    _In_ size_t completed)
{
    CHECK(mapping->Owned && completed <= mapping->Length);
    g_endCalls++;
    if (g_endFailure) {
        return OS_EUNKNOWN;
    }
    g_lastEndLength = completed;
    mapping->Owned = false;
    g_owned--;
    return OS_EOK;
}

oserr_t
DMAUnmap(
    _In_ struct DMAMapping** mapping)
{
    if (*mapping == NULL) {
        return OS_EOK;
    }
    CHECK(!(*mapping)->Owned);
    g_unmapCalls++;
    if (g_unmapFailure) {
        return OS_EBUSY;
    }
    free(*mapping);
    *mapping = NULL;
    g_mappings--;
    return OS_EOK;
}

void
DeviceInterruptInitialize(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ Device_t* device)
{
    (void)device;
    memset(interrupt, 0, sizeof(*interrupt));
}
oserr_t
DeviceInterruptAllocatePci(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ uint32_t min,
    _In_ uint32_t optimal,
    _In_ uint32_t modes,
    _In_ uint32_t* count)
{
    (void)min;
    (void)optimal;
    (void)modes;
    interrupt->State = (void*)1;
    *count = 1;
    g_irqs++;
    return OS_EOK;
}
oserr_t
DeviceInterruptSetDescriptor(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ uint32_t index,
    _In_ int descriptor)
{
    (void)interrupt;
    (void)index;
    (void)descriptor;
    return OS_EOK;
}
oserr_t
DeviceInterruptSetHandler(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ uint32_t index,
    _In_ InterruptHandler_t handler)
{
    (void)interrupt;
    (void)index;
    (void)handler;
    return OS_EOK;
}
oserr_t
DeviceInterruptAddIoResource(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ uint32_t index,
    _In_ DeviceIo_t* io)
{
    (void)interrupt;
    (void)index;
    (void)io;
    return OS_EOK;
}
oserr_t
DeviceInterruptAddMemoryResource(
    _In_ DeviceInterrupt_t* interrupt,
    _In_ uint32_t index,
    _In_ uintptr_t address,
    _In_ size_t length,
    _In_ unsigned int flags)
{
    (void)interrupt;
    (void)index;
    (void)address;
    (void)length;
    (void)flags;
    return OS_EOK;
}
oserr_t
DeviceInterruptEnable(
    _In_ DeviceInterrupt_t* interrupt)
{
    (void)interrupt;
    return OS_EOK;
}
oserr_t
DeviceInterruptDestroy(
    _In_ DeviceInterrupt_t* interrupt)
{
    if (g_irqFailure) {
        return OS_EBUSY;
    }
    if (interrupt->State != NULL) {
        g_irqs--;
        interrupt->State = NULL;
    }
    return OS_EOK;
}
oserr_t
AcquireDeviceIo(
    _In_ DeviceIo_t* io)
{
    io->Access.Memory.VirtualBase = (uintptr_t)g_mmio;
    g_io++;
    return OS_EOK;
}
oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* io)
{
    (void)io;
    CHECK(g_contexts == 0 && g_irqs == 0);
    g_io--;
    return OS_EOK;
}
oserr_t
OSDeviceIOCtl(
    _In_ uuid_t device,
    _In_ enum OSIOCtlRequest request,
    _In_ void* data,
    _In_ size_t length)
{
    (void)data;
    (void)length;
    CHECK(device == 7 && request == OSIOCTLREQUEST_BUS_CONTROL && g_contexts == 0);
    return OS_EOK;
}
size_t
AhciManagerGetFrameSize(void)
{
    return 4096;
}
oserr_t
AhciManagerRegisterDevice(
    _In_ AhciController_t* controller,
    _In_ AhciPort_t* port,
    _In_ uint32_t signature)
{
    (void)controller;
    (void)port;
    (void)signature;
    return OS_EOK;
}
void
AhciManagerUnregisterDevice(
    _In_ AhciController_t* controller,
    _In_ AhciPort_t* port)
{
    (void)controller;
    (void)port;
}
void
AhciManagerHandleControlResponse(
    _In_ AhciPort_t* port,
    _In_ AhciTransaction_t* transaction)
{
    (void)transaction;
    CHECK(port->Dma.InternalBuffer != NULL);
    g_controls++;
}
AhciDevice_t*
AhciManagerGetDevice(
    _In_ uuid_t id)
{
    return id == 99 ? g_logicalDevice : NULL;
}

int
ctt_driver_ioctl_response(
    _In_ struct gracht_message* message,
    _In_ uint8_t* data,
    _In_ uint32_t length,
    _In_ oserr_t status)
{
    (void)message;
    g_ioctlStatus = status;
    if (status == OS_EOK) {
        CHECK(length == sizeof(g_ioRequirements));
        memcpy(&g_ioRequirements, data, length);
    }
    return 0;
}
oserr_t
AhciManagerInitialize(void)
{
    return OS_EOK;
}
void
AhciManagerDestroy(void)
{
    CHECK(controllers.count == 0);
}

int
timespec_get(
    _In_ struct timespec* time,
    _In_ int base)
{
    (void)base;
    memset(time, 0, sizeof(*time));
    return 1;
}
void
timespec_add(
    _In_ const struct timespec* a,
    _In_ const struct timespec* b,
    _In_ struct timespec* result)
{
    result->tv_sec = a->tv_sec + b->tv_sec;
    result->tv_nsec = a->tv_nsec + b->tv_nsec;
}
int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    AHCIPortRegisters_t* port = __Registers();
    reg32_t status = port->CommandAndStatus;

    (void)until;
    if (g_recoverAfterSleep > 0 && --g_recoverAfterSleep == 0) {
        g_stopBlock = 0;
    }
    if (!(status & AHCI_PORT_ST) && !(g_stopBlock & AHCI_PORT_CR)) {
        status &= ~AHCI_PORT_CR;
    }
    if (!(status & AHCI_PORT_FRE) && !(g_stopBlock & AHCI_PORT_FR)) {
        status &= ~AHCI_PORT_FR;
    }
    if (status & AHCI_PORT_FRE) {
        status |= AHCI_PORT_FR;
    }
    if (status & AHCI_PORT_ST) {
        status |= AHCI_PORT_CR;
    }
    port->CommandAndStatus = status;
    ((AHCIGenericRegisters_t*)g_mmio)->GlobalHostControl &= ~AHCI_HOSTCONTROL_HR;
    return 0;
}
uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void* argument)
{
    CHECK(argument == NULL);
    g_worker = entry;
    return 1;
}
bool
usched_is_cancelled(
    _In_ const void* token)
{
    (void)token;
    return false;
}

int
ctt_storage_transfer_response(
    _In_ struct gracht_message* message,
    _In_ const oserr_t status,
    _In_ const size_t transferred)
{
    void (*hook)(void) = g_responseHook;

    (void)message;
    g_responses++;
    g_responseStatus = status;
    g_responseSectors = transferred;
    if (hook != NULL) {
        g_responseHook = NULL;
        hook();
    }
    return 0;
}

// These small wrappers keep event ownership visible to failure assertions.
int
eventd(
    _In_ unsigned int initial,
    _In_ unsigned int flags)
{
    (void)initial;
    (void)flags;
    g_events++;
    return 101;
}
int
ioset_ctrl(
    _In_ int set,
    _In_ int operation,
    _In_ int descriptor,
    _In_ struct ioset_event* event)
{
    (void)set;
    (void)descriptor;
    (void)event;
    if (operation == IOSET_ADD) {
        g_iosets++;
    }
    if (operation == IOSET_DEL) {
        g_iosets--;
    }
    return 0;
}
int
__TestClose(
    _In_ int descriptor)
{
    CHECK(descriptor == 101);
    g_events--;
    return 0;
}
