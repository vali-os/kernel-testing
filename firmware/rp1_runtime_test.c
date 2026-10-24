// Production RP1 callback/runtime and MSI-X engine. Kernel binding semantics
// are independently tested by interrupt_registration_test.c; these traps model
// only their contract so hardware fault injection can cover the provider.
#include <ddk/barrier.h>
#undef mb
#define mb() atomic_thread_fence(memory_order_seq_cst)
#include "../../services/deviced/bus/rp1/completion.c"
#include "../../services/deviced/bus/pci/msi.c"
#include "../../services/deviced/bus/pci/msix.c"
#include "../../services/deviced/bus/rp1/interrupt.c"
#include "../../services/deviced/bus/rp1/runtime.c"
#include "../../services/deviced/bus/rp1/runtime-pci.c"
#include "../../services/deviced/bus/rp1/runtime-source.c"
#include "../../librt/libddk/interrupt-provider.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static struct {
    unsigned char Config[256];
    uint32_t Table[61][4];
    uint32_t Registers[1024];
    struct {
        int Allocated;
        uint32_t Count;
        uint32_t Strategy;
        int Updating;
        int Claimed;
        int Closed;
        uuid_t Completions[8];
        int Enabled;
        int Pending;
        int AutoMasked;
        InterruptResourceTable_t Resources;
    } Groups[61];
    struct {
        int Allocated;
        InterruptResourceTable_t Resources;
    } Completions[61];
    unsigned int CompletionCount;
    unsigned int Allocated;
    unsigned int IoCount;
    unsigned int Mapped;
    unsigned int Events;
    unsigned int Acks;
    unsigned int Calls;
    unsigned int Fail;
    int FailUnbind;
    int FailBind;
    int LevelCleared;
    int DmaReady;
    int FailAttachCopy;
    int FailClose;
    unsigned int AttachCalls;
} g_model;
static struct Rp1Bus g_bus;
static struct Rp1Child g_child;
static struct PciDevice g_device;
static PciHost_t g_host;

static int
__Fail(void)
{
    return ++g_model.Calls == g_model.Fail;
}

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

uint32_t
PciDeviceRead(struct PciDevice* device, size_t offset, size_t width)
{
    uint32_t value = 0;
    CHECK(device == &g_device && offset + width <= 256);
    memcpy(&value, g_model.Config + offset, width);
    return value;
}

void
PciDeviceWrite(struct PciDevice* device, size_t offset, uint32_t value, size_t width)
{
    CHECK(device == &g_device && offset + width <= 256);
    if (!__Fail()) {
        memcpy(g_model.Config + offset, &value, width);
    }
}

oserr_t
Rp1GetDmaDescription(const struct PciDevice* device, uint32_t node, struct PciDmaDescription* dma)
{
    CHECK(device == &g_device && node == 123);
    memset(dma, 0, sizeof(*dma));
    dma->Host.HostId = 77;
    dma->Map.Count = 1;
    dma->CachePolicy = g_model.DmaReady ? DmDmaCacheNonCoherent : DmDmaCacheUnknown;
    return OS_EOK;
}

oserr_t
CreateDeviceMemoryIo(DeviceIo_t* io, uintptr_t address, size_t length)
{
    if (__Fail()) {
        return OS_EOOM;
    }
    io->Type = DeviceIoMemoryBased;
    io->Id = address == 0x801000 ? 1 : 2;
    CHECK(io->Id == 1 || address == 0x1000000 + RP1_PCIE_APBS_OFFSET);
    io->Access.Memory.Length = length;
    g_model.IoCount++;
    return OS_EOK;
}

oserr_t
AcquireDeviceIo(DeviceIo_t* io)
{
    if (__Fail()) {
        return OS_EOOM;
    }
    io->Access.Memory.VirtualBase = (uintptr_t)(io->Id == 1 ? (void*)g_model.Table : g_model.Registers);
    g_model.Mapped++;
    return OS_EOK;
}

oserr_t
ReleaseDeviceIo(DeviceIo_t* io)
{
    unsigned int i;
    if (__Fail()) {
        return OS_EUNKNOWN;
    }
    for (i = 0; i < 61; i++) {
        CHECK(!g_model.Groups[i].Enabled && g_model.Groups[i].Resources.Handler == NULL);
    }
    CHECK(io->Access.Memory.VirtualBase && g_model.Mapped);
    g_model.Mapped--;
    return OS_EOK;
}

oserr_t
DestroyDeviceIo(DeviceIo_t* io)
{
    if (__Fail()) {
        return OS_EUNKNOWN;
    }
    CHECK(io->Id && !io->Access.Memory.VirtualBase && g_model.IoCount);
    g_model.IoCount--;
    return OS_EOK;
}

static void __Deliver(unsigned int source);

size_t
ReadDeviceIo(DeviceIo_t* io, size_t offset, size_t width)
{
    CHECK(width == 4 && offset + width <= io->Access.Memory.Length);
    return ((uint32_t*)io->Access.Memory.VirtualBase)[offset / 4];
}

oserr_t
WriteDeviceIo(DeviceIo_t* io, size_t offset, size_t value, size_t width)
{
    unsigned int source = offset / 16;
    CHECK(width == 4 && offset + width <= io->Access.Memory.Length);
    if (__Fail()) {
        return OS_EUNKNOWN;
    }
    if (io->Id == 1) {
        ((uint32_t*)g_model.Table)[offset / 4] = (uint32_t)value;
        if (offset % 16 == 12 && !(value & 1) && g_model.Groups[source].Pending &&
            !(PciDeviceRead(&g_device, 0x42, 2) & MSIX_FUNCTION_MASK)) {
            __Deliver(source);
        }
    } else {
        CHECK(offset >= 0x800);
        if ((offset & 0xc00) == 0xc00) {
            g_model.Registers[(offset & 0x3ff) / 4] &= ~value;
        } else {
            if (value & RP1_IRQ_IACK) {
                if (g_model.Registers[(offset & 0x3ff) / 4] & RP1_IRQ_ENABLE) {
                    CHECK(g_model.LevelCleared);
                    g_model.Acks++;
                }
                g_model.Groups[((offset & 0x3ff) - 8) / 4].AutoMasked = 0;
            }
            g_model.Registers[(offset & 0x3ff) / 4] |= value & ~RP1_IRQ_IACK;
        }
    }
    return OS_EOK;
}

static oserr_t
__Signal(uuid_t event)
{
    CHECK(event >= 900 && event < 908);
    g_model.Events++;
    return OS_EOK;
}

static void
__Deliver(unsigned int source)
{
    InterruptFunctionTable_t functions = { ReadDeviceIo, WriteDeviceIo, __Signal, NULL };
    CHECK(g_model.Groups[source].Enabled && g_model.Groups[source].Resources.Handler != NULL);
    g_model.Groups[source].Pending = 0;
    CHECK(g_model.Groups[source].Resources.Handler(&functions,
        &g_model.Groups[source].Resources) == IRQSTATUS_HANDLED);
}

static void
__Raise(unsigned int source)
{
    CHECK(g_model.Registers[RP1_IRQ_CFG(source) / 4] & RP1_IRQ_ENABLE);
    CHECK(!g_model.Groups[source].AutoMasked);
    if (g_model.Registers[RP1_IRQ_CFG(source) / 4] & RP1_IRQ_IACK_ENABLE) {
        g_model.Groups[source].AutoMasked = 1;
    }
    g_model.Groups[source].Pending = 1;
    if (!(g_model.Table[source][3] & 1)) {
        __Deliver(source);
    }
}

SCTYPE
syscall2(SCTYPE function, SCTYPE arg0, SCTYPE arg1)
{
    InterruptGroupRequest_t* request = (void*)arg0;
    InterruptGroupResult_t* result = (void*)arg1;
    InterruptBindRequest_t* bind = (void*)arg1;
    unsigned int i;

    if (function == 27) {
        CHECK(arg0 >= 1 && arg0 <= 61 && g_model.Completions[arg0 - 1].Allocated);
        if (__Fail()) {
            return OS_EUNKNOWN;
        }
        g_model.Completions[arg0 - 1].Resources = *(InterruptResourceTable_t*)arg1;
        return OS_EOK;
    }
    if (function == 18) {
        if (__Fail()) {
            return OS_EOOM;
        }
        CHECK(request->ControllerID == 1234 && request->Minimum == 1 && (request->Optimal == 1 || request->Optimal == 2));
        CHECK(request->Strategies == INTERRUPT_STRATEGY_MSIX);
        for (i = 0; i < 61 && g_model.Groups[i].Allocated; i++) { }
        CHECK(i < 61);
        memset(&g_model.Groups[i], 0, sizeof(g_model.Groups[i]));
        g_model.Groups[i].Allocated = 1;
        g_model.Groups[i].Count = request->Optimal;
        g_model.Groups[i].Strategy = INTERRUPT_STRATEGY_MSIX;
        g_model.Allocated++;
        *result = (InterruptGroupResult_t){ .Handle = i + 1, .Count = request->Optimal, .Strategy = INTERRUPT_STRATEGY_MSIX };
        return OS_EOK;
    }
    CHECK(function == 19 && arg0 >= 1 && arg0 <= 61 && !g_model.Groups[arg0 - 1].Enabled);
    CHECK(bind->Index < g_model.Groups[arg0 - 1].Count);
    g_model.Groups[arg0 - 1].Resources = bind->Resources;
    return __Fail() || g_model.FailBind ? OS_EUNKNOWN : OS_EOK;
}

SCTYPE
syscall3(SCTYPE function, SCTYPE arg0, SCTYPE arg1, SCTYPE arg2)
{
    DeviceInterruptMessage_t* message = (void*)arg2;
    unsigned int index = (unsigned int)arg0 - 1;

    if (function == 30 || function == 31) {
        if (index >= 61 || !g_model.Groups[index].Allocated) {
            return OS_ENOENT;
        }
        // The kernel reports an in-progress operation before checking ownership.
        if (g_model.Groups[index].Updating) {
            return OS_EBUSY;
        }
        if (arg1 != 700 || arg2 != 42) {
            return OS_EPERMISSIONS;
        }
        if (g_model.Groups[index].Strategy != INTERRUPT_STRATEGY_MSIX) {
            return OS_ENOTSUPPORTED;
        }
        if (__Fail()) {
            return OS_EUNKNOWN;
        }
        if (function == 30) {
            return g_model.Groups[index].Claimed && g_model.Groups[index].Enabled &&
                !g_model.Groups[index].Closed ? OS_EOK : OS_EBUSY;
        }
        if (g_model.FailClose) {
            return OS_EBUSY;
        }
        if (g_model.Groups[index].Enabled || g_model.Groups[index].Resources.Handler != NULL) {
            return OS_EBUSY;
        }
        g_model.Groups[index].Closed = 1;
        memset(g_model.Groups[index].Completions, 0, sizeof(g_model.Groups[index].Completions));
        return OS_EOK;
    }
    CHECK(function == 24 && arg0 >= 1 && arg0 <= 61 && arg1 < g_model.Groups[arg0 - 1].Count);
    if (__Fail()) {
        return OS_EUNKNOWN;
    }
    *message = (DeviceInterruptMessage_t){ .AddressLow = 0xfff00000, .Data = (uint32_t)arg0 - 1 + (uint32_t)arg1 };
    return OS_EOK;
}

static oserr_t
__ModelProviderAttach(
    struct InterruptProviderAttachRequest* request)
{
    unsigned int index = request->Allocation - 1;
    unsigned int i;

    g_model.AttachCalls++;

    if (index >= 61 || !g_model.Groups[index].Allocated) {
        return OS_ENOENT;
    }
    if (g_model.Groups[index].Updating) {
        return OS_EBUSY;
    }
    if (request->Principal != 700 || request->Device != 42) {
        return OS_EPERMISSIONS;
    }
    if (g_model.Groups[index].Strategy != INTERRUPT_STRATEGY_MSIX) {
        return OS_ENOTSUPPORTED;
    }
    if (__Fail()) {
        return OS_EUNKNOWN;
    }
    if (g_model.Groups[index].Closed) {
        return OS_ENOENT;
    }
    if (request->ControllerID != 1234 || request->Count != g_model.Groups[index].Count) {
        return OS_EINVALPARAMS;
    }
    CHECK(request->Count <= 8 && request->Vectors != NULL);
    for (i = 0; i < request->Count; i++) {
        if (g_model.Groups[index].Claimed) {
            CHECK(g_model.Groups[index].Completions[i] == request->Vectors[i].Completion);
        }
        g_model.Groups[index].Completions[i] = request->Vectors[i].Completion;
    }
    g_model.Groups[index].Claimed = 1;
    if (g_model.FailAttachCopy) {
        return OS_EUNKNOWN;
    }
    for (i = 0; i < request->Count; i++) {
        request->Vectors[i].Message = (DeviceInterruptMessage_t) {
            .AddressLow = 0xfff00000, .Data = index + i
        };
    }
    return OS_EOK;
}

SCTYPE
syscall1(SCTYPE function, SCTYPE handle)
{
    unsigned int i = (unsigned int)handle - 1;
    unsigned int j;
    unsigned int k;

    if (function == 26) {
        if (__Fail()) {
            return OS_EOOM;
        }
        for (i = 0; i < 61 && g_model.Completions[i].Allocated; i++) { }
        CHECK(i < 61);
        g_model.Completions[i].Allocated = 1;
        g_model.CompletionCount++;
        *(uuid_t*)handle = i + 1;
        return OS_EOK;
    }
    if (function == 28) {
        CHECK(i < 61 && g_model.Completions[i].Allocated);
        for (j = 0; j < 61; j++) {
            for (k = 0; k < 8; k++) {
                CHECK(!g_model.Groups[j].Allocated || g_model.Groups[j].Completions[k] != handle);
            }
        }
        if (__Fail()) {
            return OS_EUNKNOWN;
        }
        memset(&g_model.Completions[i], 0, sizeof(g_model.Completions[i]));
        g_model.CompletionCount--;
        return OS_EOK;
    }
    if (function == 29) {
        return __ModelProviderAttach((struct InterruptProviderAttachRequest*)handle);
    }
    CHECK(i < 61 && g_model.Groups[i].Allocated);
    if (__Fail() || (function == 22 && g_model.FailUnbind)) {
        return OS_EUNKNOWN;
    }
    switch (function) {
        case 20:
            CHECK(g_model.Groups[i].Resources.Handler != NULL);
            g_model.Groups[i].Enabled = 1;
            break;
        case 21:
            g_model.Groups[i].Enabled = 0;
            break;
        case 22:
            CHECK(!g_model.Groups[i].Enabled);
            memset(&g_model.Groups[i].Resources, 0, sizeof(InterruptResourceTable_t));
            break;
        case 23:
            CHECK(!g_model.Groups[i].Enabled && !g_model.Groups[i].Resources.Handler);
            CHECK(handle != g_bus.Runtime->Pci.Allocation || g_model.IoCount == 0);
            g_model.Groups[i].Allocated = 0;
            g_model.Allocated--;
            break;
        default:
            CHECK(0);
    }
    return OS_EOK;
}

static void
__Setup(void)
{
    CHECK(!g_model.CompletionCount && !g_model.Allocated && !g_model.Mapped && !g_model.IoCount && g_bus.Runtime == NULL);
    memset(&g_model, 0, sizeof(g_model));
    memset(&g_bus, 0, sizeof(g_bus));
    memset(&g_child, 0, sizeof(g_child));
    g_bus.Parent = &g_device;
    g_bus.Children = &g_child;
    g_child.Bus = &g_bus;
    g_child.DeviceId = 42;
    g_child.Firmware.NodeOffset = 123;
    g_child.Firmware.Controller = 22;
    g_child.Firmware.InterruptCount = 2;
    g_child.Firmware.Interrupts[0] = (struct FdtRp1Interrupt){ 22, 6, 4 };
    g_child.Firmware.Interrupts[1] = (struct FdtRp1Interrupt){ 22, 31, 1 };
    g_device.Host = &g_host;
    g_host.InterruptControllerId = 1234;
    g_bus.Bars[0] = (struct PciBar){ .CpuAddress = 0x800000, .BusAddress = 0x800000,
        .Size = 0x20000, .Space = 2, .State = PciBarAssigned };
    g_bus.Bars[1] = (struct PciBar){ .CpuAddress = 0x1000000, .Size = 0x400000,
        .Space = 2, .State = PciBarAssigned };
    PciDeviceWrite(&g_device, 6, 0x10, 2);
    PciDeviceWrite(&g_device, 0x34, 0x40, 1);
    PciDeviceWrite(&g_device, 0x40, 0x11, 1);
    PciDeviceWrite(&g_device, 0x42, 60, 2);
    PciDeviceWrite(&g_device, 0x44, 0x1000, 4);
    PciDeviceWrite(&g_device, 0x48, 0x10000, 4);
    PciDeviceWrite(&g_device, 0x10, 0x800000, 4);
    g_model.DmaReady = 1;
    g_model.Calls = 0;
}

static oserr_t
__Control(struct PciInterruptRequest* request, unsigned int operation)
{
    request->Operation = operation;
    return Rp1RuntimeControl(&g_child, 700, request);
}

int
main(void)
{
    struct PciInterruptRequest request = {
        .Strategy = INTERRUPT_STRATEGY_MSIX, .Count = 2
    };
    struct PciInterruptRequest competitor;
    InterruptGroupRequest_t groupRequest = {
        .ControllerID = 1234, .Minimum = 1, .Optimal = 2, .Strategies = INTERRUPT_STRATEGY_MSIX
    };
    InterruptGroupResult_t group;
    InterruptGroupResult_t newer;
    DeviceInterruptMessage_t message;
    unsigned int calls;
    unsigned int stopCalls;
    unsigned int i;
    unsigned int controlCalls;
    unsigned int attachments;
    uuid_t principal;
    oserr_t status;

    __Setup();
    g_model.DmaReady = 0;
    CHECK(Rp1RuntimeStart(&g_bus) == OS_ENOTSUPPORTED);
    CHECK(!g_bus.Ready && !g_model.Allocated && !g_model.IoCount);
    g_model.DmaReady = 1;
    g_model.Calls = 0;
    CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
    calls = g_model.Calls;
    CHECK(g_bus.Ready && g_model.Allocated == 1);
    for (i = 0; i < 61; i++) {
        CHECK(g_model.Table[i][3] & 1);
        CHECK(!(g_model.Registers[RP1_IRQ_CFG(i) / 4] & RP1_IRQ_ENABLE));
    }
    CHECK(PciDeviceRead(&g_device, 4, 2) & PCI_COMMAND_BUSMASTER);
    CHECK(Syscall_InterruptAllocate(&groupRequest, &group) == OS_EOK);
    // Attach and its rollback can both be busy before ownership or strategy
    // validation. Once that operation finishes, Release must discard a pending
    // reservation which could never have attached, allowing a valid allocation.
    for (i = 0; i < 2; i++) {
        principal = i == 0 ? 701 : 700;
        g_model.Groups[group.Handle - 1].Strategy = i == 0 ?
            INTERRUPT_STRATEGY_MSIX : INTERRUPT_STRATEGY_MSI;
        g_model.Groups[group.Handle - 1].Updating = 1;
        request.Allocation = group.Handle;
        request.Operation = PciInterruptReserve;
        request.Count = 2;
        CHECK(Rp1RuntimeControl(&g_child, principal, &request) == OS_EUNKNOWN);
        CHECK(g_bus.Runtime->Sources[6].Owner == &g_child);
        CHECK(!g_bus.Runtime->Sources[6].MessageReady);
        CHECK(Rp1RuntimePrepareRemoval(&g_bus) == OS_EBUSY);
        g_model.Groups[group.Handle - 1].Updating = 0;
        request.Operation = PciInterruptRelease;
        request.Count = 0;
        CHECK(Rp1RuntimeControl(&g_child, principal, &request) == OS_EOK);
        CHECK(g_bus.Runtime->Sources[6].Owner == NULL);
        CHECK(g_bus.Runtime->Sources[31].Owner == NULL);
        CHECK(!g_model.Groups[group.Handle - 1].Claimed);
    }
    g_model.Groups[group.Handle - 1].Strategy = INTERRUPT_STRATEGY_MSIX;
    request.Count = 2;
    request.Allocation = 101;
    CHECK(__Control(&request, PciInterruptReserve) == OS_ENOENT);
    CHECK(g_bus.Runtime->Sources[6].Owner == NULL);
    request.Allocation = group.Handle;
    request.Operation = PciInterruptReserve;
    CHECK(Rp1RuntimeControl(&g_child, 701, &request) == OS_EPERMISSIONS);
    g_host.InterruptControllerId++;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EINVALPARAMS);
    g_host.InterruptControllerId--;
    CHECK(g_model.Groups[group.Handle - 1].Closed);
    CHECK(g_bus.Runtime->Sources[6].Owner == NULL);
    CHECK(Syscall_InterruptRelease(group.Handle) == OS_EOK);
    CHECK(Syscall_InterruptAllocate(&groupRequest, &group) == OS_EOK);
    request.Allocation = group.Handle;
    // A failed message copy can leave callbacks attached. Successful rollback
    // closes that group so neither callbacks nor local reservations remain.
    g_model.FailAttachCopy = 1;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EUNKNOWN);
    CHECK(g_model.Groups[group.Handle - 1].Closed);
    CHECK(g_model.Groups[group.Handle - 1].Completions[0] == UUID_INVALID);
    CHECK(g_bus.Runtime->Sources[6].Owner == NULL);
    CHECK(Syscall_InterruptRelease(group.Handle) == OS_EOK);
    CHECK(Syscall_InterruptAllocate(&groupRequest, &group) == OS_EOK);
    request.Allocation = group.Handle;
    // If rollback also fails, remember the group and block removal until a
    // retry recovers its messages or closes it. Never program unknown messages.
    g_model.FailClose = 1;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EUNKNOWN);
    CHECK(g_model.Groups[group.Handle - 1].Claimed);
    CHECK(g_bus.Runtime->Sources[6].Owner == &g_child);
    CHECK(!g_bus.Runtime->Sources[6].MessageReady);
    CHECK(Rp1RuntimePrepareRemoval(&g_bus) == OS_EBUSY);
    CHECK(Rp1RuntimeStop(&g_bus) == OS_EBUSY);
    CHECK(g_bus.Ready && !g_bus.Runtime->Quiescing);
    competitor = request;
    competitor.Count = 0;
    CHECK(__Control(&competitor, PciInterruptProgram) == OS_EBUSY);
    g_model.FailClose = 0;
    g_model.FailAttachCopy = 0;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EOK);
    // A failed duplicate request must leave the working attachment intact.
    g_model.FailAttachCopy = 1;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EUNKNOWN);
    CHECK(!g_model.Groups[group.Handle - 1].Closed);
    CHECK(g_bus.Runtime->Sources[6].MessageReady);
    g_model.FailAttachCopy = 0;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EOK);
    CHECK(g_model.Groups[group.Handle - 1].Completions[0] == g_bus.Runtime->Completions[6].Handle);
    CHECK(g_model.Groups[group.Handle - 1].Completions[1] == UUID_INVALID);
    attachments = g_model.AttachCalls;
    CHECK(Rp1RuntimePrepareRemoval(&g_bus) == OS_EBUSY);
    competitor = request;
    competitor.Allocation = 101;
    CHECK(Rp1RuntimeControl(&g_child, 701, &competitor) == OS_EBUSY);
    request.Count = 0;
    CHECK(__Control(&request, PciInterruptEnable) == OS_EBUSY);
    CHECK(Syscall_InterruptGetMessage(group.Handle, 0, &message) == OS_EOK);
    request.AddressLow = message.AddressLow;
    request.Data = message.Data;
    request.AddressLow ^= 4096;
    CHECK(__Control(&request, PciInterruptProgram) == OS_EINVALPARAMS);
    CHECK(!g_bus.Runtime->Sources[6].Programmed);
    request.AddressLow = message.AddressLow;
    request.Data++;
    CHECK(__Control(&request, PciInterruptProgram) == OS_EINVALPARAMS);
    CHECK(!g_bus.Runtime->Sources[6].Programmed);
    request.Data = message.Data;
    CHECK(__Control(&request, PciInterruptProgram) == OS_EOK);
    CHECK(__Control(&request, PciInterruptProgram) == OS_EOK);
    request.Data++;
    CHECK(__Control(&request, PciInterruptProgram) == OS_EINVALPARAMS);
    request.Index = 1;
    CHECK(__Control(&request, PciInterruptProgram) == OS_EOK);
    CHECK(g_model.Table[6][2] == message.Data && g_model.Table[31][2] == message.Data + 1);
    CHECK(g_model.AttachCalls == attachments);
    request.Index = request.AddressLow = request.Data = 0;
    g_model.Groups[group.Handle - 1].Enabled = 1;
    CHECK(__Control(&request, PciInterruptEnable) == OS_EOK);
    controlCalls = g_model.Calls;
    CHECK(__Control(&request, PciInterruptEnable) == OS_EOK);
    CHECK(g_model.Calls == controlCalls + 1);
    CHECK(!g_model.Table[6][3] && !g_model.Table[31][3]);
    CHECK(g_model.Registers[RP1_IRQ_CFG(6)/4] & RP1_IRQ_IACK_ENABLE);
    CHECK(!(g_model.Registers[RP1_IRQ_CFG(31)/4] & RP1_IRQ_IACK_ENABLE));
    request.Operation = PciInterruptDisable;
    CHECK(Rp1RuntimeControl(&g_child, 701, &request) == OS_EPERMISSIONS);
    CHECK(__Control(&request, PciInterruptDisable) == OS_EOK);
    CHECK(g_model.Table[6][3] && g_model.Table[31][3]);
    CHECK(__Control(&request, PciInterruptEnable) == OS_EOK);
    // Closing an enabled group must keep source ownership for a cleanup retry.
    CHECK(__Control(&request, PciInterruptRelease) == OS_EBUSY);
    CHECK(g_bus.Runtime->Sources[6].Owner == &g_child);
    CHECK(Syscall_InterruptDisable(group.Handle) == OS_EOK);
    CHECK(__Control(&request, PciInterruptRelease) == OS_EOK);
    CHECK(g_bus.Runtime->Sources[6].Owner == NULL && g_bus.Runtime->Sources[31].Owner == NULL);
    CHECK(__Control(&request, PciInterruptRelease) == OS_EOK);
    request.Count = 2;
    CHECK(__Control(&request, PciInterruptReserve) == OS_ENOENT);
    CHECK(Syscall_InterruptRelease(group.Handle) == OS_EOK);
    // Leave both allocations alive while releasing the same sources twice.
    // The older closed claim must survive the newer client's entire lifetime.
    CHECK(Syscall_InterruptAllocate(&groupRequest, &group) == OS_EOK);
    request.Allocation = group.Handle;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EOK);
    request.Count = 0;
    CHECK(__Control(&request, PciInterruptRelease) == OS_EOK);
    CHECK(Syscall_InterruptAllocate(&groupRequest, &newer) == OS_EOK);
    request.Allocation = newer.Handle;
    request.Count = 2;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EOK);
    request.Count = 0;
    CHECK(__Control(&request, PciInterruptRelease) == OS_EOK);
    request.Allocation = group.Handle;
    request.Count = 2;
    CHECK(__Control(&request, PciInterruptReserve) == OS_ENOENT);
    CHECK(Syscall_InterruptRelease(group.Handle) == OS_EOK);
    CHECK(Syscall_InterruptRelease(newer.Handle) == OS_EOK);
    CHECK(Rp1RuntimePrepareRemoval(&g_bus) == OS_EOK);
    request.Allocation = 101;
    CHECK(__Control(&request, PciInterruptReserve) == OS_EBUSY);
    g_model.Calls = 0;
    CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);
    stopCalls = g_model.Calls;

    for (i = 1; i <= calls; i++) {
        __Setup();
        g_model.Fail = i;
        status = Rp1RuntimeStart(&g_bus);
        CHECK(status != OS_EOK || g_bus.Ready);
        CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);
        CHECK(!g_bus.Ready && !(PciDeviceRead(&g_device, 4, 2) & PCI_COMMAND_BUSMASTER));
    }
    for (i = 1; i <= stopCalls; i++) {
        __Setup();
        CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
        g_model.Fail = g_model.Calls + i;
        status = Rp1RuntimeStop(&g_bus);
        CHECK(status == OS_EOK || g_bus.Runtime != NULL);
        CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);
    }
    printf("RP1 runtime: group attachment, trusted messages, cleanup retry, %u setup and %u teardown faults passed\n",
        calls, stopCalls);
    return 0;
}
