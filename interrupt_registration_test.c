#include "../kernel/interrupts.c"
#include "../kernel/interrupts_allocation.c"
#include "../kernel/api/priv.c"
#include <stdio.h>
#include <os/device.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

// This harness exercises fixed INTx and the kernel vector backend. PCI RPC
// programming is covered by pci_msi_ddk_test.c with the real service engine.
oserr_t OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest request, void* buffer, size_t length)
{
    (void)device; (void)request; (void)buffer; (void)length;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

static _Atomic(unsigned int) g_liveDescriptors;
static unsigned int g_liveMappings;
static unsigned int g_liveIo;
static unsigned int g_cloneCalls;
static unsigned int g_ioCalls;
static unsigned int g_failClone;
static unsigned int g_failIo;
static int          g_failAllocation;
static int          g_failResolve;
static int          g_failConfigure;
static int          g_holdReader;
static int          g_active;
static unsigned int g_enableCalls;
static unsigned int g_disableCalls;
static int          g_failUnmap;
static unsigned int g_yields;
static unsigned int g_handlerCalls;
static unsigned int g_ackCalls;
static int          g_failCopy;
static int          g_failCopyOut;
static int          g_failSyscall = -1;
static uuid_t       g_updateProbe = UUID_INVALID;
static const uuid_t g_testIndex = 64;
static uuid_t       g_testResolveIndex = g_testIndex;
static uint32_t     g_messageVectors;
static unsigned int g_failEnableCall;
static int          g_dispatchOnConfigure;
static _Atomic(unsigned int) g_allocationCalls;
static unsigned int g_failAllocationCall;

void
LogAppendMessage(
    enum OSSysLogLevel level,
    const char* format, ...)
{
    (void)level;
    (void)format;
}

void*
kmalloc(size_t size)
{
    void* allocation;

    if (++g_allocationCalls == g_failAllocationCall || g_failAllocation) {
        return NULL;
    }
    allocation = malloc(size);
    CHECK(allocation != NULL);
    g_liveDescriptors++;
    return allocation;
}

void
kfree(void* allocation)
{
    CHECK(g_liveDescriptors > 0);
    g_liveDescriptors--;
    free(allocation);
}

void
SpinlockAcquireIrq(Spinlock_t* lock)
{
    while (atomic_exchange(&lock->Current, 1) != 0) { }
}

void
SpinlockReleaseIrq(Spinlock_t* lock)
{
    CHECK(atomic_load(&lock->Current) == 1);
    atomic_store(&lock->Current, 0);
}

uuid_t
ThreadCurrentHandle(void)
{
    return 1;
}

void
ArchThreadYield(void)
{
    // Model another CPU finishing the reader that disable must wait for.
    CHECK(g_holdReader);
    CHECK(g_liveMappings > 0);
    g_holdReader = 0;
    g_yields++;
    InterruptReadExit();
}

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return NULL;
}

SystemCpuState_t
CpuCoreState(
    SystemCpuCore_t* core)
{
    (void)core;
    return g_active ? CpuStateInterruptActive : 0;
}

static uuid_t       g_currentSpace = 2;

uuid_t
GetCurrentMemorySpaceHandle(void)
{
    return g_currentSpace;
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return NULL;
}

size_t
GetMemorySpacePageSize(void)
{
    return 4096;
}

oserr_t
MemorySpaceCloneMapping(
    MemorySpace_t* sourceSpace,
    MemorySpace_t* destinationSpace,
    vaddr_t sourceAddress,
    vaddr_t* destinationAddress,
    size_t length,
    unsigned int memoryFlags,
    unsigned int placementFlags)
{
    (void)sourceSpace;
    (void)destinationSpace;
    (void)sourceAddress;
    (void)length;
    (void)memoryFlags;
    (void)placementFlags;
    if (g_updateProbe != UUID_INVALID) {
        CHECK(InterruptSetEnabled(g_updateProbe, 1) == OS_EBUSY);
        CHECK(InterruptUnregister(g_updateProbe) == OS_EBUSY);
    }
    if (++g_cloneCalls == g_failClone) {
        return OS_EUNKNOWN;
    }
    *destinationAddress = g_cloneCalls * 4096;
    g_liveMappings++;
    return OS_EOK;
}

oserr_t
MemorySpaceUnmap(
    MemorySpace_t* memorySpace,
    vaddr_t address,
    size_t size)
{
    (void)memorySpace;
    CHECK(address != 0 && size != 0 && g_liveMappings > 0);
    if (g_failUnmap) {
        return OS_EUNKNOWN;
    }
    g_liveMappings--;
    return OS_EOK;
}

oserr_t
CreateKernelSystemDeviceIo(
    DeviceIo_t* source,
    DeviceIo_t** destination)
{
    if (++g_ioCalls == g_failIo) {
        return OS_EUNKNOWN;
    }
    *destination = source;
    g_liveIo++;
    return OS_EOK;
}

oserr_t
ReleaseKernelSystemDeviceIo(DeviceIo_t* resource)
{
    CHECK(resource != NULL && g_liveIo > 0);
    g_liveIo--;
    return OS_EOK;
}

oserr_t
InterruptResolve(
    InterruptDescriptor_t* interrupt,
    unsigned int flags,
    uuid_t* tableIndex)
{
    (void)interrupt;
    (void)flags;
    *tableIndex = (flags & INTERRUPT_MSI) ? (uuid_t)interrupt->Line : g_testResolveIndex;
    return g_failResolve ? OS_EUNKNOWN : OS_EOK;
}

uint32_t InterruptMessageVectorCount(void) { return g_messageVectors; }
oserr_t InterruptMessageVector(uint32_t ordinal, InterruptDescriptor_t* descriptor,
    InterruptMessage_t* message)
{
    if (ordinal >= g_messageVectors) {
        return OS_EINVALPARAMS;
    }
    descriptor->Line = 224 + ordinal;
    descriptor->Pin = INTERRUPT_NONE;
    message->Address = 0xFEE02000;
    message->Data = descriptor->Line;
    return OS_EOK;
}

oserr_t
InterruptConfigure(
    SystemInterrupt_t* interrupt,
    int enable)
{
    CHECK(enable == 0 || enable == 1);
    if (enable) {
        CHECK(LOWORD(interrupt->Id) == g_testIndex || (interrupt->Flags & INTERRUPT_MSI));
        g_enableCalls++;
        if (g_dispatchOnConfigure) {
            unsigned int calls = g_handlerCalls;
            InterruptHandle(NULL, LOWORD(interrupt->Id));
            CHECK(g_handlerCalls == calls);
        }
        if (g_enableCalls == g_failEnableCall) {
            return OS_EUNKNOWN;
        }
    } else {
        g_disableCalls++;
    }
    return g_failConfigure ? OS_EUNKNOWN : OS_EOK;
}

static void
__CheckEmpty(void)
{
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == NULL);
    CHECK(g_interruptTable[g_testIndex].Penalty == 0);
    CHECK(g_interruptTable[g_testIndex].EnabledCount == 0);
    CHECK(g_interruptTable[g_testIndex].Sharable == 0);
    CHECK(g_retiredInterrupts == NULL);
    CHECK(g_liveDescriptors == 0 && g_liveMappings == 0 && g_liveIo == 0);
}

static void
__TestRegistration(void)
{
    DeviceIo_t         io = {0};
    InterruptDescriptor_t  interrupt = {0};
    SystemInterrupt_t* existing;
    uuid_t             id;

    interrupt.Line = g_testIndex;
    interrupt.ResourceTable.Handler = (InterruptHandler_t)(uintptr_t)0x1003;
    interrupt.ResourceTable.IoResources[0] = &io;
    interrupt.ResourceTable.IoResources[1] = &io;
    interrupt.ResourceTable.MemoryResources[0].Address = 0x2005;
    interrupt.ResourceTable.MemoryResources[0].Length = 128;
    interrupt.ResourceTable.MemoryResources[1].Address = 0x3007;
    interrupt.ResourceTable.MemoryResources[1].Length = 256;

    CHECK(InterruptRegister(NULL, 0) == UUID_INVALID);
    g_failAllocation = 1;
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    g_failAllocation = 0;
    g_failResolve = 1;
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    g_failResolve = 0;
    __CheckEmpty();

    for (unsigned int failure = 1; failure <= 3; failure++) {
        g_cloneCalls = g_ioCalls = 0;
        g_failClone = failure;
        CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
        __CheckEmpty();
    }
    g_failClone = 0;
    for (unsigned int failure = 1; failure <= 2; failure++) {
        g_cloneCalls = g_ioCalls = 0;
        g_failIo = failure;
        CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
        __CheckEmpty();
    }
    g_failIo = 0;

    g_failConfigure = 1;
    CHECK(InterruptRegister(&interrupt, INTERRUPT_EXCLUSIVE) == UUID_INVALID);
    __CheckEmpty();
    g_failConfigure = 0;

    id = InterruptRegister(&interrupt, INTERRUPT_KERNEL);
    CHECK(id != UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Id == id);
    CHECK(InterruptRegister(&interrupt, INTERRUPT_EXCLUSIVE) == UUID_INVALID);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    id = InterruptRegister(&interrupt, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE);
    CHECK(id != UUID_INVALID);
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    id = InterruptRegister(&interrupt, 0);
    CHECK(id != UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Owner == 2);
    g_currentSpace = 3;
    CHECK(InterruptUnregister(id) == OS_ENOENT);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == existing);
    g_currentSpace = 2;
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    interrupt.Line = 17;
    id = InterruptRegister(&interrupt, 0);
    CHECK(id != UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && LOWORD(existing->Id) == g_testIndex);
    CHECK(existing->Source == 17);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    puts("Interrupt registration: failure returns, resource cleanup, shared-line rollback and reader lifetime passed");
}
/** Exercises dispatch, not just flags, so disabled handlers cannot slip through. */
uint32_t InterruptsGetPriority(void) { return 0; }
void InterruptsSetPriority(uint32_t priority) { (void)priority; }
void CpuCoreEnterInterrupt(Context_t* context, int priority)
{
    (void)context;
    (void)priority;
    g_active = 1;
}
Context_t* CpuCoreExitInterrupt(Context_t* context, int priority)
{
    (void)priority;
    g_active = 0;
    return context;
}
void InterruptsAcknowledge(int source, uint32_t index)
{
    (void)source;
    CHECK(index == g_testIndex || (index >= 224 && index < 240));
    if (index >= 224 && index < 240) {
        CHECK(source == INTERRUPT_NONE);
    }
    g_ackCalls++;
}
InterruptFunctionTable_t* GetFastInterruptTable(void) { return NULL; }

static irqstatus_t
__CountHandler(InterruptFunctionTable_t* table, void* context)
{
    (void)table;
    (void)context;
    g_handlerCalls++;
    return IRQSTATUS_NOT_HANDLED;
}

/** Checks the actual userspace-copy boundary while keeping addresses host-local. */
oserr_t
MemorySpaceCopyUser(void* user, void* kernel, size_t length, bool toUser)
{
    if (g_failCopy || (toUser && g_failCopyOut)) {
        return OS_EINVALPARAMS;
    }
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}

/** Replaces only the machine trap: the real DDK requests enter the real gate. */
#include <internal/_syscalls.h>
SCTYPE
syscall2(SCTYPE number, SCTYPE first, SCTYPE second)
{
    switch (number) {
        case 16:
            return ScInterruptAllocate((void*)(uintptr_t)first, (void*)(uintptr_t)second);
        case 17:
            return ScInterruptBind(first, (void*)(uintptr_t)second);
        default:
            CHECK(0);
            return OS_EINVALPARAMS;
    }
}

/** Dispatch the dedicated traps so a wrong syscall number fails the DDK test. */
SCTYPE
syscall1(SCTYPE number, SCTYPE handle)
{
    if (number == g_failSyscall) {
        return OS_EUNKNOWN;
    }
    switch (number) {
        case 18:
            return ScInterruptEnable(handle);
        case 19:
            return ScInterruptDisable(handle);
        case 20:
            return ScInterruptUnbind(handle);
        case 21:
            return ScInterruptRelease(handle);
        default:
            CHECK(0);
            return OS_EINVALPARAMS;
    }
}
uuid_t GetNativeHandle(int descriptor) { return descriptor == 7 ? 77 : UUID_INVALID; }

static int g_failLocalAllocation;
static int g_localAllocationCalls;
static int g_localAllocations;

/** Track both local allocations so each malloc failure proves complete rollback. */
static void* __TestCalloc(size_t count, size_t size)
{
    void* result;
    if (++g_localAllocationCalls == g_failLocalAllocation) {
        return NULL;
    }
    result = calloc(count, size);
    CHECK(result != NULL);
    g_localAllocations++;
    return result;
}
static void __TestFree(void* value)
{
    if (value != NULL) {
        CHECK(g_localAllocations > 0);
        g_localAllocations--;
        free(value);
    }
}
#define calloc __TestCalloc
#define free __TestFree
#include "../librt/libddk/interrupt.c"
#undef calloc
#undef free

/** Verify reservation, sharing, synchronization and ownership independently of DDK. */
static void
__TestLifecycle(void)
{
    InterruptDescriptor_t descriptor = {0};
    SystemInterrupt_t* entry;
    uuid_t first;
    uuid_t second;
    uuid_t rejected;
    unsigned int enables;
    unsigned int disables;
    unsigned int calls;

    descriptor.Line = g_testIndex;
    descriptor.ResourceTable.Handler = __CountHandler;
    enables = g_enableCalls;
    disables = g_disableCalls;
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL, &first) == OS_EOK);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL, &second) == OS_EOK);
    CHECK(g_enableCalls == enables && g_disableCalls == disables);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_EXCLUSIVE, &rejected) == OS_EBUSY);
    CHECK(InterruptSetEnabled(first, 1) == OS_EINVALPARAMS);
    CHECK(InterruptBind(first, &descriptor.ResourceTable) == OS_EOK);
    CHECK(InterruptBind(first, &descriptor.ResourceTable) == OS_EBUSY);
    calls = g_handlerCalls;
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls);
    CHECK(InterruptSetEnabled(first, 1) == OS_EOK);
    CHECK(InterruptSetEnabled(first, 1) == OS_EOK);
    CHECK(g_enableCalls == enables + 1);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 1);
    CHECK(InterruptUnbind(first) == OS_EBUSY);
    CHECK(InterruptBind(second, &descriptor.ResourceTable) == OS_EOK);
    CHECK(InterruptSetEnabled(second, 1) == OS_EOK);
    CHECK(g_enableCalls == enables + 1);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 3);
    CHECK(InterruptSetEnabled(first, 0) == OS_EOK);
    CHECK(g_disableCalls == disables);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 4);
    CHECK(InterruptUnbind(first) == OS_EOK);
    CHECK(InterruptUnregister(first) == OS_EOK);
    CHECK(g_disableCalls == disables);
    g_failConfigure = 1;
    CHECK(InterruptSetEnabled(second, 0) == OS_EUNKNOWN);
    CHECK(g_interruptTable[g_testIndex].EnabledCount == 1);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 5);
    CHECK(InterruptUnregister(second) == OS_EUNKNOWN);
    g_failConfigure = 0;
    CHECK(InterruptUnregister(second) == OS_EOK);
    __CheckEmpty();

    // A disabled exclusive reservation still prevents later shared allocations.
    CHECK(InterruptReserve(&descriptor, INTERRUPT_EXCLUSIVE, &first) == OS_EOK);
    CHECK(InterruptReserve(&descriptor, 0, &rejected) == OS_EBUSY);
    g_currentSpace = 3;
    CHECK(InterruptBind(first, &descriptor.ResourceTable) == OS_ENOENT);
    CHECK(InterruptSetEnabled(first, 1) == OS_ENOENT);
    CHECK(InterruptUnbind(first) == OS_ENOENT);
    CHECK(InterruptUnregister(first) == OS_ENOENT);
    g_currentSpace = 2;
    g_updateProbe = first;
    CHECK(InterruptBind(first, &descriptor.ResourceTable) == OS_EOK);
    g_updateProbe = UUID_INVALID;
    CHECK(InterruptSetEnabled(first, 1) == OS_EOK);
    InterruptReadEnter();
    g_holdReader = 1;
    CHECK(InterruptSetEnabled(first, 0) == OS_EOK);
    CHECK(g_yields == 1 && !g_holdReader);
    CHECK(g_liveMappings == 1);
    g_failUnmap = 1;
    CHECK(InterruptUnbind(first) == OS_EUNKNOWN);
    CHECK(InterruptSetEnabled(first, 1) == OS_EINVALPARAMS);
    g_failUnmap = 0;
    CHECK(InterruptUnbind(first) == OS_EOK);
    CHECK(g_liveMappings == 0);
    CHECK(InterruptUnregister(first) == OS_EOK);
    __CheckEmpty();

    // A release with failed mapping cleanup must keep a reachable reservation.
    CHECK(InterruptReserve(&descriptor, 0, &first) == OS_EOK);
    CHECK(InterruptBind(first, &descriptor.ResourceTable) == OS_EOK);
    CHECK(InterruptSetEnabled(first, 1) == OS_EOK);
    g_failUnmap = 1;
    CHECK(InterruptUnregister(first) == OS_EUNKNOWN);
    CHECK(g_liveDescriptors == 1 && g_liveMappings == 1);
    CHECK(g_interruptTable[g_testIndex].EnabledCount == 0);
    CHECK(InterruptSetEnabled(first, 1) == OS_EINVALPARAMS);
    g_failUnmap = 0;
    CHECK(InterruptUnregister(first) == OS_EOK);
    __CheckEmpty();

    // A retained reference and a running reader each prevent early freeing.
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE, &first) == OS_EOK);
    entry = InterruptGet(first);
    CHECK(entry != NULL);
    InterruptReadEnter();
    g_active = 1;
    CHECK(InterruptUnregister(first) == OS_EOK);
    CHECK(g_liveDescriptors == 1 && g_interruptTable[g_testIndex].Penalty == 1);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE, &rejected) == OS_EBUSY);
    InterruptReadExit();
    g_active = 0;
    InterruptReclaimRetired();
    CHECK(g_liveDescriptors == 1);
    InterruptPut(entry);
    __CheckEmpty();

    g_testResolveIndex = MAX_SUPPORTED_INTERRUPTS;
    CHECK(InterruptReserve(&descriptor, 0, &first) == OS_EINVALPARAMS);
    g_testResolveIndex = g_testIndex;
    __CheckEmpty();
    puts("INTx lifecycle: dispatch, sharing, reservations, ownership, retries and reader lifetime passed");
}

/** Checks malformed requests cannot bypass the public wrapper's constraints. */
static void
__TestGate(void)
{
    InterruptAllocationRequest_t request = {0};
    InterruptAllocationResult_t result = {0};
    InterruptBindRequest_t bind = {0};
    InterruptDescriptor_t kernel = {0};
    uuid_t id;

    request.Line.Line = g_testIndex;
    request.Minimum = 1;
    request.Optimal = 8;
    request.Strategies = INTERRUPT_STRATEGY_ANY;
    CHECK(ScInterruptAllocate(NULL, &result) == OS_EINVALPARAMS);
    CHECK(ScInterruptAllocate(&request, NULL) == OS_EINVALPARAMS);
    request.Flags = INTERRUPT_KERNEL;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    request.Flags = 0;
    request.Line.Line = INTERRUPT_NONE;
    CHECK(ScInterruptAllocate(&request, &result) == OS_ENOENT);
    request.Line.Line = g_testIndex;
    request.Minimum = 0;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    request.Minimum = 2;
    CHECK(ScInterruptAllocate(&request, &result) == OS_ENOTSUPPORTED);
    request.Minimum = 1;
    g_failCopy = 1;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_failCopy = 0;
    g_failCopyOut = 1;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    __CheckEmpty();
    CHECK(ScInterruptAllocate(&request, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Strategy == INTERRUPT_STRATEGY_INTx);
    bind.Index = 1;
    bind.Resources.Handler = __CountHandler;
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_EINVALPARAMS);
    bind.Index = 0;
    bind.Resources.MemoryResources[0].Address = UINTPTR_MAX - 1;
    bind.Resources.MemoryResources[0].Length = 8;
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_EINVALPARAMS);
    bind.Resources.MemoryResources[0].Address = 0;
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_EINVALPARAMS);
    memset(&bind.Resources.MemoryResources[0], 0, sizeof(bind.Resources.MemoryResources[0]));
    CHECK(ScInterruptBind(result.Handle, NULL) == OS_EINVALPARAMS);
    g_currentSpace = 3;
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_EPERMISSIONS);
    CHECK(ScInterruptEnable(result.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptDisable(result.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptUnbind(result.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptRelease(result.Handle) == OS_EPERMISSIONS);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(result.Handle) == OS_ENOENT);
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_ENOENT);
    CHECK(ScInterruptEnable(result.Handle) == OS_ENOENT);
    CHECK(ScInterruptDisable(result.Handle) == OS_ENOENT);
    CHECK(ScInterruptUnbind(result.Handle) == OS_ENOENT);
    CHECK(InterruptReserve(&kernel, INTERRUPT_KERNEL, &id) == OS_EOK);
    CHECK(ScInterruptBind(id, &bind) == OS_ENOENT);
    CHECK(ScInterruptEnable(id) == OS_ENOENT);
    CHECK(ScInterruptDisable(id) == OS_ENOENT);
    CHECK(ScInterruptUnbind(id) == OS_ENOENT);
    CHECK(ScInterruptRelease(id) == OS_ENOENT);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();
    puts("Interrupt syscall gate: copied requests, copy-out rollback, index and owner checks passed");
}

/** Runs the DDK through the actual gate/core, including every allocation stage. */
static void
__TestDdk(void)
{
    BusDevice_t bus = {0};
    DeviceIo_t io = {0};
    DeviceInterrupt_t interrupt = {0};
    uint32_t count;
    unsigned int i;
    unsigned int mappings;

    bus.Base.Id = 123;
    bus.InterruptLine = g_testIndex;
    bus.InterruptPin = 1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptDestroy(NULL) == OS_EINVALPARAMS);
    DeviceInterruptInitialize(&interrupt, &bus);
    CHECK(interrupt.DeviceId == 123 && interrupt.Line.Line == g_testIndex);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAllocate(&interrupt, 0, 1, INTERRUPT_STRATEGY_ANY, &count) == OS_EINVALPARAMS);
    CHECK(count == 0);
    CHECK(DeviceInterruptAllocate(&interrupt, 2, 1, INTERRUPT_STRATEGY_ANY, &count) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAllocate(&interrupt, 2, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_ENOTSUPPORTED);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_ENOTSUPPORTED);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, 0x80, &count) == OS_EINVALPARAMS);
    for (i = 1; i <= 2; i++) {
        g_localAllocationCalls = 0;
        g_failLocalAllocation = i;
        CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_ANY, &count) == OS_EOOM);
        CHECK(g_localAllocations == 0 && interrupt.State == NULL);
        __CheckEmpty();
    }
    g_failLocalAllocation = 0;
    g_failAllocation = 1;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_ANY, &count) == OS_EOOM);
    CHECK(g_localAllocations == 0);
    g_failAllocation = 0;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Count == 1 && interrupt.Strategy == INTERRUPT_STRATEGY_INTx);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_ANY, &count) == OS_EBUSY);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetHandler(&interrupt, 1, __CountHandler) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, NULL) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetAffinity(&interrupt, 0, 0) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetAffinity(&interrupt, 0, 1) == OS_ENOTSUPPORTED);
    CHECK(DeviceInterruptSetAffinity(&interrupt, 0, UINT32_MAX) == OS_EOK);
    CHECK(DeviceInterruptSetDescriptor(&interrupt, 0, -1) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetDescriptor(&interrupt, 0, 8) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetDescriptor(&interrupt, 0, 7) == OS_EOK);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EOK);
    CHECK(DeviceInterruptAddIoResource(&interrupt, 0, NULL) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAddMemoryResource(&interrupt, 0, UINTPTR_MAX - 2, 4, 0) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAddMemoryResource(&interrupt, 0, 0x1000, 1, 2) == OS_EINVALPARAMS);
    for (i = 0; i < INTERRUPT_MAX_IO_RESOURCES; i++) {
        CHECK(DeviceInterruptAddIoResource(&interrupt, 0, &io) == OS_EOK);
    }
    CHECK(DeviceInterruptAddIoResource(&interrupt, 0, &io) == OS_EOOM);
    for (i = 0; i < INTERRUPT_MAX_MEMORY_RESOURCES; i++) {
        CHECK(DeviceInterruptAddMemoryResource(&interrupt, 0, 0x1000 + i * 0x1000, 64, 0) == OS_EOK);
    }
    CHECK(DeviceInterruptAddMemoryResource(&interrupt, 0, 0x1000, 64, 0) == OS_EOOM);
    g_cloneCalls = 0;
    g_failClone = 2;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(g_liveMappings == 0 && !interrupt.State->Bound);
    g_cloneCalls = 0;
    g_failUnmap = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State->Bound && g_liveMappings != 0);
    g_failUnmap = 0;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(!interrupt.State->Bound && g_liveMappings == 0);
    g_failClone = 0;
    g_failConfigure = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(g_liveMappings == 0 && !interrupt.State->Bound);
    g_failUnmap = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State->Bound && !interrupt.State->Enabled);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EBUSY);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    g_failUnmap = g_failConfigure = 0;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    mappings = g_liveMappings;
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor)->KernelResources.HandleResource == 77);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor)->KernelResources.IoResources[0] == &io);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK && g_liveMappings == mappings);
    CHECK(DeviceInterruptSetDescriptor(&interrupt, 0, 7) == OS_EBUSY);
    CHECK(DeviceInterruptSetAffinity(&interrupt, 0, UINT32_MAX) == OS_EBUSY);
    CHECK(DeviceInterruptAddIoResource(&interrupt, 0, &io) == OS_EBUSY);
    CHECK(DeviceInterruptAddMemoryResource(&interrupt, 0, 0x1000, 64, 0) == OS_EBUSY);
    g_failConfigure = 1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State->Enabled && g_liveMappings == mappings);
    g_failConfigure = 0;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(g_liveMappings == 0 && g_liveDescriptors == 2);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    g_failSyscall = 21;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State != NULL && g_localAllocations == 2);
    g_failSyscall = -1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(interrupt.State == NULL && interrupt.Vectors == NULL && interrupt.Count == 0);
    CHECK(interrupt.DeviceId == 123 && g_localAllocations == 0);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_INTx, &count) == OS_EOK);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    __CheckEmpty();
    puts("DDK interrupt lifecycle: allocation failures, setters, retries and reallocation passed");
}

static void
__TestAllocations(void)
{
    InterruptAllocationRequest_t request = {
        .DeviceId = 123, .Line = { .Line = 17 },
        .Minimum = 2, .Optimal = 4, .Strategies = INTERRUPT_STRATEGY_MSIX
    };
    InterruptAllocationResult_t result = {0}, second = {0};
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    InterruptMessage_t messages[4], sentinel = { .Address = 123, .Data = 456 };
    unsigned int calls, enables;

    g_messageVectors = 4;
    for (unsigned int stage = 1; stage <= 5; stage++) {
        g_allocationCalls = 0;
        g_failAllocationCall = stage;
        CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EOOM);
        CHECK(result.Handle == 0 && g_allocations == NULL);
        __CheckEmpty();
    }
    g_failAllocationCall = 0;
    CHECK(InterruptAllocationCreate(NULL, 1, &result) == OS_EINVALPARAMS);
    request.Minimum = 0;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EINVALPARAMS);
    request.Minimum = 2;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Count == 4 && result.Strategy == INTERRUPT_STRATEGY_MSIX);
    CHECK(ScInterruptEnable(result.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptRelease(result.Handle) == OS_EPERMISSIONS);
    CHECK(InterruptAllocationGetMessage(result.Handle, 4, &sentinel) == OS_EINVALPARAMS);
    CHECK(sentinel.Address == 123 && sentinel.Data == 456);
    CHECK(InterruptAllocationBind(result.Handle, 4, &resources) == OS_EINVALPARAMS);
    CHECK(InterruptAllocationCreate(&request, 1, &second) == OS_EBUSY);
    CHECK(second.Handle == 0);
    for (uint32_t i = 0; i < result.Count; i++) {
        CHECK(InterruptAllocationGetMessage(result.Handle, i, &messages[i]) == OS_EOK);
        CHECK(messages[i].Address == 0xFEE02000 && messages[i].Data == 224 + i);
        CHECK(InterruptAllocationBind(result.Handle, i, &resources) == OS_EOK);
        if (i + 1 < result.Count) {
            enables = g_enableCalls;
            CHECK(InterruptAllocationSetEnabled(result.Handle, 1) == OS_EINVALPARAMS);
            CHECK(g_enableCalls == enables);
        }
    }
    calls = g_handlerCalls;
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls);
    // Force delivery during each hardware-enable operation: the group gate
    // must suppress it, including when a later enable fails and rolls back.
    g_dispatchOnConfigure = 1;
    g_failEnableCall = g_enableCalls + 2;
    CHECK(InterruptAllocationSetEnabled(result.Handle, 1) == OS_EUNKNOWN);
    CHECK(g_handlerCalls == calls);
    for (uint32_t i = 0; i < result.Count; i++) {
        CHECK(g_interruptTable[messages[i].Data].EnabledCount == 0);
    }
    g_failEnableCall = 0;
    CHECK(InterruptAllocationSetEnabled(result.Handle, 1) == OS_EOK);
    g_dispatchOnConfigure = 0;
    CHECK(InterruptAllocationUnbind(result.Handle) == OS_EBUSY);
    CHECK(InterruptAllocationBind(result.Handle, 0, &resources) == OS_EBUSY);
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls + 4);
    CHECK(InterruptAllocationSetEnabled(result.Handle, 0) == OS_EOK);
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls + 4);
    CHECK(InterruptAllocationUnbind(result.Handle) == OS_EOK);
    CHECK(InterruptAllocationRelease(result.Handle) == OS_EOK);
    CHECK(InterruptAllocationRelease(result.Handle) == OS_ENOENT);
    __CheckEmpty();

    // MSI always returns one, regardless of optimal. A minimum of two fails.
    request.Strategies = INTERRUPT_STRATEGY_MSI;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_ENOTSUPPORTED);
    request.Minimum = 1;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Strategy == INTERRUPT_STRATEGY_MSI);
    // An occupied message vector produces a smaller, independent MSI-X group.
    request.Minimum = 2;
    request.Strategies = INTERRUPT_STRATEGY_MSIX;
    CHECK(InterruptAllocationCreate(&request, 1, &second) == OS_EOK);
    CHECK(second.Count == 3 && second.Handle != result.Handle);
    CHECK(InterruptAllocationGetMessage(second.Handle, 0, &sentinel) == OS_EOK);
    CHECK(sentinel.Data == 225);
    CHECK(InterruptAllocationRelease(second.Handle) == OS_EOK);
    request.Minimum = 4;
    CHECK(InterruptAllocationCreate(&request, 1, &second) == OS_EBUSY);
    CHECK(g_liveDescriptors == 2); // Only the first group and its vector remain.
    CHECK(InterruptAllocationRelease(result.Handle) == OS_EOK);
    for (uint32_t i = 224; i < 228; i++) {
        CHECK(g_interruptTable[i].Penalty == 0);
        CHECK(atomic_load(&g_interruptTable[i].Descriptor) == NULL);
    }
    __CheckEmpty();

    // Partial user mapping cleanup retains the whole group for retry.
    request.Minimum = 2;
    CHECK(InterruptAllocationCreate(&request, 0, &result) == OS_EOK);
    g_cloneCalls = 0;
    g_failClone = 2;
    CHECK(InterruptAllocationBind(result.Handle, 0, &resources) == OS_EOK);
    CHECK(InterruptAllocationBind(result.Handle, 1, &resources) == OS_EUNKNOWN);
    CHECK(InterruptAllocationSetEnabled(result.Handle, 1) == OS_EINVALPARAMS);
    g_failClone = 0;
    g_failUnmap = 1;
    CHECK(InterruptAllocationRelease(result.Handle) == OS_EUNKNOWN);
    CHECK(g_liveDescriptors == 5 && g_liveMappings == 1);
    CHECK(InterruptAllocationSetEnabled(result.Handle, 1) == OS_EBUSY);
    g_failUnmap = 0;
    CHECK(InterruptAllocationRelease(result.Handle) == OS_EOK);
    __CheckEmpty();

    // An unsupported architecture falls back only when one satisfies minimum.
    g_messageVectors = 0;
    request.Minimum = 1;
    request.Strategies = INTERRUPT_STRATEGY_ANY;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Strategy == INTERRUPT_STRATEGY_INTx && result.Count == 1);
    CHECK(InterruptAllocationGetMessage(result.Handle, 0, &sentinel) == OS_ENOTSUPPORTED);
    CHECK(InterruptAllocationRelease(result.Handle) == OS_EOK);
    request.Minimum = 2;
    CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_ENOTSUPPORTED);
    __CheckEmpty();
    puts("Interrupt groups: independent MSI-X, single MSI, gating, rollback, exhaustion and teardown passed");
}

#ifdef INTERRUPT_WINDOWS_HOST_TEST
// Keep Windows SDK headers out of the kernel-header translation unit.
__declspec(dllimport) void* __stdcall CreateThread(void*, size_t,
    unsigned long (__stdcall*)(void*), void*, unsigned long, unsigned long*);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
__declspec(dllimport) int __stdcall CloseHandle(void*);
static _Atomic(unsigned int) g_claimedMessages[16];

static unsigned long __stdcall
__AllocationWorker(void* context)
{
    (void)context;
    for (unsigned int iteration = 0; iteration < 250; iteration++) {
        InterruptAllocationRequest_t request = {
            .Minimum = 1, .Optimal = 2,
            .Strategies = iteration % 2 ? INTERRUPT_STRATEGY_MSI : INTERRUPT_STRATEGY_MSIX
        };
        InterruptAllocationResult_t result;
        InterruptMessage_t messages[2];
        CHECK(InterruptAllocationCreate(&request, 1, &result) == OS_EOK);
        CHECK(result.Count == (iteration % 2 ? 1U : 2U));
        for (uint32_t i = 0; i < result.Count; i++) {
            CHECK(InterruptAllocationGetMessage(result.Handle, i, &messages[i]) == OS_EOK);
            CHECK(atomic_exchange(&g_claimedMessages[messages[i].Data - 224], 1) == 0);
        }
        for (uint32_t i = 0; i < result.Count; i++) {
            CHECK(atomic_exchange(&g_claimedMessages[messages[i].Data - 224], 0) == 1);
        }
        CHECK(InterruptAllocationRelease(result.Handle) == OS_EOK);
    }
    return 0;
}

static void __TestConcurrentAllocations(void)
{
    void* threads[4];
    g_messageVectors = 16;
    for (unsigned int i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, __AllocationWorker, NULL, 0, NULL);
        CHECK(threads[i] != NULL);
    }
    for (unsigned int i = 0; i < 4; i++) {
        CHECK(WaitForSingleObject(threads[i], 30000) == 0);
        CHECK(CloseHandle(threads[i]));
    }
    for (unsigned int i = 224; i < 240; i++) {
        CHECK(g_interruptTable[i].Penalty == 0);
        CHECK(atomic_load(&g_interruptTable[i].Descriptor) == NULL);
    }
    CHECK(g_allocations == NULL);
    g_messageVectors = 0;
    __CheckEmpty();
    puts("Interrupt groups: 1000 allocations/releases across four concurrent threads passed");
}
#endif

static void __TestPciRetirement(void)
{
    InterruptAllocationRequest_t request = {
        .Minimum = 1, .Optimal = 1, .Strategies = INTERRUPT_STRATEGY_MSI
    };
    InterruptAllocationResult_t first, second;
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    SystemInterrupt_t* tombstone;
    uuid_t retired;
    unsigned calls;
    g_messageVectors = 2;
    CHECK(ScInterruptAllocate(&request, &first) == OS_EOK);
    CHECK(first.AddressLow == 0xfee02000 && first.AddressHigh == 0);
    CHECK(InterruptAllocationBind(first.Handle, 0, &resources) == OS_EOK);
    CHECK(ScInterruptEnable(first.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(first.Handle) == OS_EOK);
    CHECK(g_allocations == NULL && g_liveMappings == 0);
    tombstone = atomic_load(&g_interruptTable[first.Data].Descriptor);
    CHECK(tombstone != NULL && !tombstone->Bound && !atomic_load(&tombstone->Enabled));
    CHECK(tombstone->DeliveryGate == &g_retiredDeliveryGate);
    retired = tombstone->Id;
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(second.Data != first.Data);
    calls = g_handlerCalls;
    InterruptHandle(NULL, first.Data);
    CHECK(g_handlerCalls == calls);
    // Never-enabled allocations are reusable; late messages cannot name them.
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(second.Data != first.Data);
    CHECK(InterruptAllocationBind(second.Handle, 0, &resources) == OS_EOK);
    CHECK(ScInterruptEnable(second.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    uuid_t secondRetired = atomic_load(&g_interruptTable[second.Data].Descriptor)->Id;
    CHECK(ScInterruptAllocate(&request, &second) == OS_EBUSY);
    // Test-only cleanup of the intentionally permanent tombstone.
    CHECK(InterruptUnregister(retired) == OS_EOK);
    CHECK(InterruptUnregister(secondRetired) == OS_EOK);
    g_messageVectors = 0;
    __CheckEmpty();
    puts("PCI vector retirement: late messages suppressed and enabled vectors not reused passed");
}

int main(void)
{
    __TestRegistration();
    __TestLifecycle();
    __TestGate();
    __TestDdk();
    __TestAllocations();
    __TestPciRetirement();
#ifdef INTERRUPT_WINDOWS_HOST_TEST
    __TestConcurrentAllocations();
#endif
    return 0;
}
