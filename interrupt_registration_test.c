#ifdef INTERRUPT_WINDOWS_HOST_TEST
#undef mb
#define mb() atomic_thread_fence(memory_order_seq_cst)
#endif
#include <ddk/types/interrupt-group.h>
#include <ddk/types/interrupt-controller.h>
#include <ddk/barrier.h>
#include "../kernel/interrupts/interrupts.c"
#include "../kernel/interrupts/chain.c"
#include "../kernel/components/ic.c"
#include "../kernel/interrupts/group.c"
#include "../kernel/api/priv.c"
#include <stdio.h>
#include <stdarg.h>
#include <os/device.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

// Test fixed INTx and the kernel vector backend here. pci_msi_ddk_test.c covers
// PCI RPC programming with the service's configuration engine.
static oserr_t __NativeTestIoctl(uuid_t device, enum OSIOCtlRequest request, void* buffer, size_t length);
oserr_t OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest request, void* buffer, size_t length)
{
    return __NativeTestIoctl(device, request, buffer, length);
}

static _Atomic(unsigned int) g_liveDescriptors;
static unsigned int g_registryAllocations;
static int          g_denyInterruptControl;
static int          g_denyDeviceManagement;
static uuid_t       g_testMessageDomain;
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
static void*        g_failCopyAddress;
static int          g_failSyscall = -1;
static uuid_t       g_updateProbe = UUID_INVALID;
static const uuid_t g_testIndex = 64;
static uuid_t       g_testResolveIndex = g_testIndex;
static uint32_t     g_messageVectors;
static unsigned int g_failEnableCall;
static int          g_dispatchOnConfigure;
static _Atomic(unsigned int) g_allocationCalls;
static unsigned int g_failAllocationCall;
static int          g_failIoMap;
static uintptr_t    g_expectedIoUnmap;
static size_t       g_expectedIoLength;
static int          g_captureDiagnostics;
static const char*  g_expectedDiagnostic;
static enum OSSysLogLevel g_expectedDiagnosticLevel;
static unsigned int g_matchingDiagnostics;
static unsigned int g_problemDiagnostics;
static unsigned int g_traceDiagnostics;

static SystemMachine_t g_testMachine;

void InitializeInterruptTable(void) { }
void InitializeInterruptHandlers(void) { }
oserr_t PlatformInterruptInitialize(void) { return OS_EOK; }
void ArchProcessorHalt(void) { abort(); }

SystemMachine_t*
GetMachine(void)
{
    return &g_testMachine;
}

void
LogAppendMessage(
    enum OSSysLogLevel level,
    const char* format, ...)
{
    char    message[512];
    va_list arguments;

    if (!g_captureDiagnostics) {
        return;
    }
    if (level == OSSYSLOGLEVEL_WARNING || level == OSSYSLOGLEVEL_ERROR) {
        g_problemDiagnostics++;
    }
    if (level == OSSYSLOGLEVEL_TRACE) {
        g_traceDiagnostics++;
    }
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (g_expectedDiagnostic != NULL && level == g_expectedDiagnosticLevel &&
        strstr(message, g_expectedDiagnostic) != NULL) {
        g_matchingDiagnostics++;
    }
}

void SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    LogAppendMessage(level, "%s", message);
}

static void
__CaptureDiagnostics(
    _In_ const char*         expected,
    _In_ enum OSSysLogLevel level)
{
    g_expectedDiagnostic = expected;
    g_expectedDiagnosticLevel = level;
    g_matchingDiagnostics = 0;
    g_problemDiagnostics = 0;
    g_traceDiagnostics = 0;
    g_captureDiagnostics = 1;
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
static uuid_t       g_deadSpace;
static uuid_t       g_processPrincipal;
static DeviceIo_t   g_completionIo;
static unsigned int g_completionAcks;
static unsigned int g_completionReads;
static unsigned int g_completionHandlerBefore;
static unsigned int g_completionParentBefore;
static int          g_identityClone;

oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    CHECK(capabilities == CAPABILITY_INTERRUPT_CONTROL || capabilities == CAPABILITY_DEVICEIO_ACCESS ||
        capabilities == CAPABILITY_DEVICEIO_MANAGEMENT);
    if (capabilities == CAPABILITY_INTERRUPT_CONTROL && g_denyInterruptControl) {
        return OS_EPERMISSIONS;
    }
    if (capabilities == CAPABILITY_DEVICEIO_MANAGEMENT && g_denyDeviceManagement) {
        return OS_EPERMISSIONS;
    }
    return OS_EOK;
}

oserr_t
CapabilityGrant(
    _In_ Capabilities_t capabilities,
    _In_ uuid_t         memorySpaceHandle)
{
    abort();
}

uuid_t
GetCurrentMemorySpaceHandle(void)
{
    return g_currentSpace;
}

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    static _Thread_local MSContextSecurity_t security;
    security.Handle = g_processPrincipal != 0 ? g_processPrincipal : g_currentSpace;
    return &security;
}

void* LookupHandleOfType(uuid_t id, HandleType_t type)
{
    CHECK(type == HandleTypeMSContext);
    return id == g_deadSpace ? NULL : (void*)(uintptr_t)1;
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
    *destinationAddress = g_identityClone ? sourceAddress - sourceAddress % 4096 : g_cloneCalls * 4096;
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
    if (g_expectedIoUnmap != 0) {
        CHECK(address == g_expectedIoUnmap && size == g_expectedIoLength);
        g_expectedIoUnmap = 0;
    }
    if (g_failUnmap) {
        return OS_EUNKNOWN;
    }
    g_liveMappings--;
    return OS_EOK;
}

oserr_t
MapSystemDeviceIo(uuid_t id, size_t offset, size_t length, uintptr_t* mapping)
{
    CHECK(id == 77 && offset == 0x128 && length == 64);
    if (g_failIoMap) {
        return OS_EOOM;
    }
    *mapping = 0x8128;
    g_liveMappings++;
    return OS_EOK;
}

static void
__TestIoMapSyscall(void)
{
    uintptr_t mapping = 0;

    CHECK(ScIoSpaceMap(77, 0x128, 64, NULL) == OS_EINVALPARAMS);
    g_failIoMap = 1;
    CHECK(ScIoSpaceMap(77, 0x128, 64, &mapping) == OS_EOOM && mapping == 0);
    g_failIoMap = 0;
    g_failCopyOut = 1;
    g_expectedIoUnmap = 0x8000;
    g_expectedIoLength = 0x168;
    CHECK(ScIoSpaceMap(77, 0x128, 64, &mapping) != OS_EOK);
    CHECK(g_expectedIoUnmap == 0 && g_liveMappings == 0 && mapping == 0);
    g_failCopyOut = 0;
    CHECK(ScIoSpaceMap(77, 0x128, 64, &mapping) == OS_EOK && mapping == 0x8128);
    CHECK(MemorySpaceUnmap(NULL, mapping - 0x128, 0x168) == OS_EOK);
    puts("MMIO mapping syscall: invalid output, map failure and copyout rollback passed");
}

oserr_t
CreateKernelSystemDeviceIo(
    DeviceIo_t* source,
    DeviceIo_t** destination)
{
    if (++g_ioCalls == g_failIo) {
        return OS_EUNKNOWN;
    }
    *destination = source->Id == 900 ? &g_completionIo : source;
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
    CHECK(!(flags & INTERRUPT_MSI));
    *tableIndex = g_testResolveIndex;
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
    const InterruptControllerResource_t* interrupt,
    int enable)
{
    CHECK(enable == 0 || enable == 1);
    if (enable) {
#ifdef INTERRUPT_MIP_HOST_TEST
        CHECK(interrupt->TableIndex >= 32 && interrupt->TableIndex < 1020);
#else
    CHECK(interrupt->TableIndex == g_testResolveIndex || (interrupt->Flags & INTERRUPT_MSI));
#endif
        g_enableCalls++;
        if (g_dispatchOnConfigure) {
            unsigned int calls = g_handlerCalls;
            InterruptHandle(NULL, interrupt->TableIndex);
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
    CHECK(g_liveDescriptors == g_registryAllocations + 0 && g_liveMappings == 0 && g_liveIo == 0);
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
    CHECK(existing->Routing.Source == 17);
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
#ifdef INTERRUPT_MIP_HOST_TEST
    CHECK(index >= 32 && index < MAX_SUPPORTED_INTERRUPTS);
#else
    CHECK(index == g_testResolveIndex || index == 200 || (index >= 224 && index < 240));
#endif
    if (index >= 224 && index < 240) {
        CHECK(source == INTERRUPT_NONE);
    }
    g_ackCalls++;
}
static oserr_t __CompletionWrite(DeviceIo_t* io, size_t offset, size_t value, size_t width)
{
    CHECK(io == &g_completionIo && offset == 24 && value == 0x5a && width == 4);
    CHECK(g_handlerCalls == g_completionHandlerBefore + 1);
    CHECK(g_ackCalls == g_completionParentBefore);
    g_completionAcks++;
    return OS_EOK;
}
static size_t __CompletionRead(DeviceIo_t* io, size_t offset, size_t width)
{
    CHECK(io == &g_completionIo && offset == 28 && width == 4);
    CHECK(g_completionAcks == g_completionReads + 1);
    g_completionReads++;
    return 0;
}
InterruptFunctionTable_t* GetFastInterruptTable(void)
{
    static InterruptFunctionTable_t functions = { .ReadIoSpace = __CompletionRead, .WriteIoSpace = __CompletionWrite };
    return &functions;
}

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
    if (g_failCopy || user == g_failCopyAddress || (toUser && g_failCopyOut)) {
        return OS_EINVALPARAMS;
    }
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}

/** Replaces only the machine trap: the real DDK requests enter the real gate. */
#include <internal/_syscalls.h>
SCTYPE
syscall3(SCTYPE number, SCTYPE handle, SCTYPE index, SCTYPE output)
{
    if (number == g_failSyscall) {
        return OS_EUNKNOWN;
    }
    switch (number) {
        case 24:
            return ScInterruptGetMessage(handle, index, (void*)(uintptr_t)output);
        case 30:
            return ScInterruptProviderCheckEnabled(handle, index, output);
        case 31:
            return ScInterruptProviderClose(handle, index, output);
        default:
            CHECK(0);
            return OS_EINVALPARAMS;
    }
}

SCTYPE
syscall2(SCTYPE number, SCTYPE first, SCTYPE second)
{
    switch (number) {
        case 22:
            return ScInterruptLineSourceResolve((void*)(uintptr_t)first, (void*)(uintptr_t)second);
        case 32:
            return ScInterruptMessageControllerResolve((void*)(uintptr_t)first, (void*)(uintptr_t)second);
        case 18:
            return ScInterruptAllocate((void*)(uintptr_t)first, (void*)(uintptr_t)second);
        case 19:
            return ScInterruptBind(first, (void*)(uintptr_t)second);
        case 27:
            return ScInterruptCompletionBind(first, (void*)(uintptr_t)second);
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
        case 20:
            return ScInterruptEnable(handle);
        case 21:
            return ScInterruptDisable(handle);
        case 23:
            return ScInterruptRelease(handle);
        case 25:
            return ScInterruptReap(handle);
        case 26:
            return ScInterruptCompletionCreate((void*)(uintptr_t)handle);
        case 28:
            return ScInterruptCompletionRelease(handle);
        case 29:
            return ScInterruptProviderAttach((void*)(uintptr_t)handle);
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
#include "../librt/libddk/interrupt-pci.c"
#undef calloc
#undef free
#include "../librt/libddk/interrupt-provider.c"

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
    CHECK(g_liveDescriptors == g_registryAllocations + 1 && g_liveMappings == 1);
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
    CHECK(g_liveDescriptors == g_registryAllocations + 1 && g_interruptTable[g_testIndex].Penalty == 1);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE, &rejected) == OS_EBUSY);
    InterruptReadExit();
    g_active = 0;
    InterruptReclaimRetired();
    CHECK(g_liveDescriptors == g_registryAllocations + 1);
    InterruptPut(entry);
    __CheckEmpty();

    g_testResolveIndex = MAX_SUPPORTED_INTERRUPTS;
    CHECK(InterruptReserve(&descriptor, 0, &first) == OS_EINVALPARAMS);
    g_testResolveIndex = g_testIndex;
    __CheckEmpty();
    puts("INTx lifecycle: dispatch, sharing, reservations, ownership, retries and reader lifetime passed");
}

static void
__TestDiagnostics(void)
{
    const InterruptMessageControllerIdentity_t query = {
        .Version = 1,
        .Kind = InterruptMessageControllerKindDeviceTree,
        .FirmwareNode = 0xfedc
    };
    InterruptDescriptor_t descriptor = { 0 };
    uuid_t                id;
    uuid_t                rejected;
    char                  expected[256];

    descriptor.Line = g_testIndex;
    descriptor.ResourceTable.Handler = __CountHandler;
    descriptor.ResourceTable.MemoryResources[0].Address = 0x2005;
    descriptor.ResourceTable.MemoryResources[0].Length = 128;

    __CaptureDiagnostics(NULL, OSSYSLOGLEVEL_ERROR);
    CHECK(InterruptMessageControllerResolve(&query, &rejected) == OS_ENOTSUPPORTED);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE, &id) == OS_EOK);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL, &rejected) == OS_EBUSY);
    CHECK(g_problemDiagnostics == 0);
#if defined(__TRACE) && defined(__OSCONFIG_LOGGING_KTRACE)
    CHECK(g_traceDiagnostics > 0);
#else
    CHECK(g_traceDiagnostics == 0);
#endif
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EOK);
    CHECK(InterruptSetEnabled(id, 1) == OS_EOK);
    __CaptureDiagnostics(NULL, OSSYSLOGLEVEL_ERROR);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_problemDiagnostics == 0 && g_traceDiagnostics == 0);
    CHECK(InterruptUnregister(id) == OS_EOK);

    snprintf(expected, sizeof(expected),
             "InterruptControllerResolve: InterruptResolve line %u, flags 0x0 failed: %u",
             g_testIndex, OS_EUNKNOWN);
    __CaptureDiagnostics(expected, OSSYSLOGLEVEL_ERROR);
    g_failResolve = 1;
    CHECK(InterruptReserve(&descriptor, 0, &id) == OS_EUNKNOWN);
    CHECK(g_matchingDiagnostics == 1);
    g_failResolve = 0;
    __CheckEmpty();

    CHECK(InterruptReserve(&descriptor, 0, &id) == OS_EOK);
    snprintf(expected, sizeof(expected),
             "InterruptResolveMemoryResources: MemorySpaceCloneMapping id %u, resource 0, address 0x2005, length 133 failed:",
             id);
    __CaptureDiagnostics(expected, OSSYSLOGLEVEL_ERROR);
    g_cloneCalls = 0;
    g_failClone = 2;
    g_failUnmap = 1;
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EUNKNOWN);
    CHECK(g_matchingDiagnostics == 1);
    CHECK(g_liveMappings == 1);

    snprintf(expected, sizeof(expected),
             "InterruptUnbind: InterruptReleaseResources id %u failed, cleanup remains retryable: %u",
             id, OS_EUNKNOWN);
    __CaptureDiagnostics(expected, OSSYSLOGLEVEL_ERROR);
    CHECK(InterruptUnbind(id) == OS_EUNKNOWN);
    CHECK(g_matchingDiagnostics == 1);
    CHECK(g_liveMappings == 1);
    g_failUnmap = 0;
    g_failClone = 0;
    CHECK(InterruptUnbind(id) == OS_EOK);
    CHECK(InterruptBind(id, &descriptor.ResourceTable) == OS_EOK);

    snprintf(expected, sizeof(expected),
             "InterruptControllerSetEnabled: InterruptConfigure line %u, enable 1 failed: %u",
             g_testIndex, OS_EUNKNOWN);
    __CaptureDiagnostics(expected, OSSYSLOGLEVEL_ERROR);
    g_failConfigure = 1;
    CHECK(InterruptSetEnabled(id, 1) == OS_EUNKNOWN);
    CHECK(g_matchingDiagnostics == 1);
    CHECK(g_interruptTable[g_testIndex].EnabledCount == 0);
    g_failConfigure = 0;
    CHECK(InterruptSetEnabled(id, 1) == OS_EOK);
    CHECK(InterruptUnregister(id) == OS_EOK);
    g_captureDiagnostics = 0;
    __CheckEmpty();
    puts("Interrupt diagnostics: operation/resource context, cleanup retry, quiet misses and dispatch passed");
}

/** Checks malformed requests cannot bypass the public wrapper's constraints. */
static void
__TestGate(void)
{
    InterruptGroupRequest_t request = {0};
    InterruptGroupResult_t result = {0};
    InterruptBindRequest_t bind = {0};
    InterruptDescriptor_t kernel = {0};
    uuid_t id;

    request.Line.Line = g_testIndex;
    request.Minimum = 1;
    request.Optimal = 1;
    request.Kind = InterruptSourceLine;
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
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    request.Minimum = 1;
    g_failCopy = 1;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_failCopy = 0;
    g_failCopyOut = 1;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    __CheckEmpty();
    CHECK(ScInterruptAllocate(&request, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Kind == InterruptSourceLine);
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
    CHECK(ScInterruptRelease(result.Handle) == OS_EPERMISSIONS);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(result.Handle) == OS_ENOENT);
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_ENOENT);
    CHECK(ScInterruptEnable(result.Handle) == OS_ENOENT);
    CHECK(ScInterruptDisable(result.Handle) == OS_ENOENT);
    CHECK(InterruptReserve(&kernel, INTERRUPT_KERNEL, &id) == OS_EOK);
    CHECK(ScInterruptBind(id, &bind) == OS_ENOENT);
    CHECK(ScInterruptEnable(id) == OS_ENOENT);
    CHECK(ScInterruptDisable(id) == OS_ENOENT);
    CHECK(ScInterruptRelease(id) == OS_ENOENT);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();
    puts("Interrupt syscall gate: copied requests, copy-out rollback, index and owner checks passed");
}

static void
__TestDdkDisableRetry(void)
{
    DeviceInterrupt_t interrupt;
    uint32_t         count;
    unsigned int     calls;

    g_identityClone = 1;
    DeviceInterruptInitialize(&interrupt, NULL);
    interrupt.Line.Line = g_testIndex;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EOK);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    calls = g_handlerCalls;
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 1);

    // The group closes dispatch before the controller disable can fail. Enable
    // must finish that incomplete stop and reopen dispatch, even without a bus
    // provider to set the DDK's pending-cleanup state.
    g_failConfigure = 1;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EUNKNOWN);
    CHECK(ScInterruptEnable(interrupt.State->Handle) == OS_EBUSY);
    calls = g_handlerCalls;
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls);
    g_failConfigure = 0;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 1);

    // The combined syscall can also fail after dispatch stopped, while removing
    // mappings. Neither direct enable nor the DDK may reuse this partial group.
    g_failUnmap = 1;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EUNKNOWN);
    CHECK(ScInterruptEnable(interrupt.State->Handle) == OS_EBUSY);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    g_failUnmap = 0;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    calls = g_handlerCalls;
    InterruptHandle(NULL, g_testIndex);
    CHECK(g_handlerCalls == calls + 1);

    // Kernel callers may stop dispatch without unmapping. A failed stop still
    // requires cleanup rather than allowing an old descriptor bit to reopen it.
    g_failConfigure = 1;
    CHECK(InterruptGroupSetEnabled(interrupt.State->Handle, 0) == OS_EUNKNOWN);
    CHECK(ScInterruptEnable(interrupt.State->Handle) == OS_EBUSY);
    g_failConfigure = 0;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);

    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    g_identityClone = 0;
    __CheckEmpty();
    puts("DDK line disable retry: Enable restores delivery after controller stop failure passed");
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
    bus.Base.Length = sizeof(bus);
    DeviceInterruptInitialize(&interrupt, &bus.Base);
    CHECK(interrupt.DeviceId == 123 && interrupt.Line.Line == g_testIndex);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAllocate(&interrupt, 0, 1, &count) == OS_EINVALPARAMS);
    CHECK(count == 0);
    CHECK(DeviceInterruptAllocate(&interrupt, 2, 1, &count) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAllocate(&interrupt, 2, 8, &count) == OS_ENOTSUPPORTED);
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, 0x80, &count) == OS_EINVALPARAMS);
    for (i = 1; i <= 2; i++) {
        g_localAllocationCalls = 0;
        g_failLocalAllocation = i;
        CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EOOM);
        CHECK(g_localAllocations == 0 && interrupt.State == NULL);
        __CheckEmpty();
    }
    g_failLocalAllocation = 0;
    // Vector storage now follows kernel reservation. Keep that reservation
    // reachable if both storage allocation and rollback fail.
    g_localAllocationCalls = 0;
    g_failLocalAllocation = 2;
    g_failSyscall = 23;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, &count) == OS_EOOM);
    CHECK(count == 0 && interrupt.State != NULL && interrupt.Vectors == NULL);
    CHECK(g_localAllocations == 1);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EBUSY);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EBUSY);
    g_failLocalAllocation = 0;
    g_failSyscall = -1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    __CheckEmpty();
    g_failAllocation = 1;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EOOM);
    CHECK(g_localAllocations == 0);
    g_failAllocation = 0;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Count == 1 && interrupt.Kind == InterruptSourceLine);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EBUSY);
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
    CHECK(interrupt.State->Bound && interrupt.State->PendingCleanup && g_liveMappings != 0);
    g_failUnmap = 0;
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(!interrupt.State->Bound && !interrupt.State->PendingCleanup && g_liveMappings == 0);
    g_failClone = 0;
    g_failConfigure = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(g_liveMappings == 0 && !interrupt.State->Bound);
    g_failUnmap = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State->Bound && !interrupt.State->Enabled && interrupt.State->PendingCleanup);
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
    CHECK(g_liveMappings == 0 && g_liveDescriptors == g_registryAllocations + 2);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    // This object has begun Destroy, unlike an ordinary reusable Disable.
    // Finishing its stop does not undo the terminal teardown decision.
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EBUSY);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EBUSY);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    g_failSyscall = 23;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EUNKNOWN);
    CHECK(interrupt.State != NULL && g_localAllocations == 2);
    g_failSyscall = -1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(interrupt.State == NULL && interrupt.Vectors == NULL && interrupt.Count == 0);
    CHECK(interrupt.DeviceId == 123 && g_localAllocations == 0);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EOK);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    __CheckEmpty();
    puts("DDK interrupt lifecycle: allocation failures, setters, retries and reallocation passed");
}

static void
__TestMessageQuery(void)
{
    InterruptGroupRequest_t request = {
        .ControllerID = g_testMessageDomain,
        .Minimum = 2,
        .Optimal = 8,
        .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t result;
    DeviceInterrupt_t interrupt = {0};
    DeviceInterruptMessage_t message;
    DeviceInterruptMessage_t sentinel = { 0x11, 0x22, 0x33 };
    uint32_t i;

    g_messageVectors = 3;
    // Exercise the production syscall with a partial multi-vector grant.
    g_failCopyOut = 1;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    __CheckEmpty();
    CHECK(ScInterruptAllocate(&request, &result) == OS_EOK);
    CHECK(result.Count == 3);
    interrupt.State = __TestCalloc(1, sizeof(*interrupt.State));
    interrupt.State->Handle = result.Handle;
    interrupt.Count = result.Count;
    interrupt.Kind = result.Kind;
    CHECK(__AllocateVectors(&interrupt) == OS_EOK);
    interrupt.State->Phase = InterruptPhaseReady;

    for (i = 0; i < interrupt.Count; i++) {
        CHECK(interrupt.Vectors[i].AffinityMask == UINT32_MAX);
        CHECK(interrupt.Vectors[i].ResourceTable.HandleResource == UUID_INVALID);
        CHECK(interrupt.Vectors[i].ResourceTable.Handler == NULL);
        CHECK(DeviceInterruptSetHandler(&interrupt, i, __CountHandler) == OS_EOK);
        CHECK(DeviceInterruptGetMessage(&interrupt, i, &message) == OS_EOK);
        CHECK(message.AddressLow == 0xfee02000 && message.AddressHigh == 0);
        CHECK(message.Data == 224 + i);
    }
    g_groups->Vectors[2].Message.Address = 0x12345678fee02000ULL;
    CHECK(DeviceInterruptGetMessage(&interrupt, 2, &message) == OS_EOK);
    CHECK(message.AddressLow == 0xfee02000 && message.AddressHigh == 0x12345678);
    CHECK(message.Data == 226);

    message = sentinel;
    CHECK(DeviceInterruptGetMessage(NULL, 0, &message) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptGetMessage(&interrupt, 0, NULL) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptGetMessage(&interrupt, 3, &message) == OS_EINVALPARAMS);
    CHECK(ScInterruptGetMessage(result.Handle, 3, &message) == OS_EINVALPARAMS);
    CHECK(ScInterruptGetMessage(result.Handle, 0, NULL) == OS_EINVALPARAMS);
    CHECK(ScInterruptGetMessage(UUID_INVALID, 0, &message) == OS_ENOENT);
    g_currentSpace = 3;
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_EPERMISSIONS);
    g_currentSpace = 2;
    g_failCopyOut = 1;
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptGetMessage(&interrupt, 0, &message) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    g_failSyscall = 24;
    CHECK(DeviceInterruptGetMessage(&interrupt, 0, &message) == OS_EUNKNOWN);
    g_failSyscall = -1;
    g_groups->Updating = 1;
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_EBUSY);
    g_groups->Updating = 0;
    CHECK(memcmp(&message, &sentinel, sizeof(message)) == 0);
    CHECK(!interrupt.State->Bound && !interrupt.State->Enabled && g_liveMappings == 0);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_ENOENT);
    CHECK(DeviceInterruptGetMessage(&interrupt, 0, &message) == OS_EINVALPARAMS);
    CHECK(g_localAllocations == 0);

    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_EPERMISSIONS);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    request.Minimum = 1;
    request.Kind = InterruptSourceLine;
    request.Optimal = 1;
    request.ControllerID = 0;
    request.Line.Line = g_testIndex;
    CHECK(InterruptGroupCreate(&request, 0, &result) == OS_EOK);
    CHECK(ScInterruptGetMessage(result.Handle, 0, &message) == OS_ENOTSUPPORTED);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    CHECK(memcmp(&message, &sentinel, sizeof(message)) == 0);
    __CheckEmpty();
    g_messageVectors = 0;
    puts("Indexed messages: ownership, bounds, copy-out, independent vectors and DDK storage passed");
}

static void
__TestGroups(void)
{
    InterruptGroupRequest_t request = {
        .Minimum = 2, .Optimal = 4, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t result = {0}, second = {0};
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    InterruptMessage_t messages[4], sentinel = { .Address = 123, .Data = 456 };
    unsigned int calls, enables;

    g_messageVectors = 4;
    for (unsigned int stage = 1; stage <= 5; stage++) {
        g_allocationCalls = 0;
        g_failAllocationCall = stage;
        CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOOM);
        CHECK(result.Handle == 0 && g_groups == NULL);
        __CheckEmpty();
    }
    g_failAllocationCall = 0;
    CHECK(InterruptGroupCreate(NULL, 1, &result) == OS_EINVALPARAMS);
    request.Minimum = 0;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EINVALPARAMS);
    request.Minimum = 2;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Count == 4 && result.Kind == InterruptSourceMessage);
    CHECK(ScInterruptEnable(result.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptRelease(result.Handle) == OS_EPERMISSIONS);
    CHECK(InterruptGroupGetMessage(result.Handle, 4, &sentinel) == OS_EINVALPARAMS);
    CHECK(sentinel.Address == 123 && sentinel.Data == 456);
    CHECK(InterruptGroupBind(result.Handle, 4, &resources) == OS_EINVALPARAMS);
    CHECK(InterruptGroupCreate(&request, 1, &second) == OS_EBUSY);
    CHECK(second.Handle == 0);
    for (uint32_t i = 0; i < result.Count; i++) {
        CHECK(InterruptGroupGetMessage(result.Handle, i, &messages[i]) == OS_EOK);
        CHECK(messages[i].Address == 0xFEE02000 && messages[i].Data == 224 + i);
        CHECK(InterruptGroupBind(result.Handle, i, &resources) == OS_EOK);
        if (i + 1 < result.Count) {
            enables = g_enableCalls;
            CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EINVALPARAMS);
            CHECK(g_enableCalls == enables);
        }
    }
    calls = g_handlerCalls;
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls);
    // Inject an interrupt each time hardware is enabled. The group gate must
    // block it, even if a later enable fails and triggers rollback.
    g_dispatchOnConfigure = 1;
    g_failEnableCall = g_enableCalls + 2;
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EUNKNOWN);
    CHECK(g_handlerCalls == calls);
    for (uint32_t i = 0; i < result.Count; i++) {
        CHECK(g_interruptTable[messages[i].Data].EnabledCount == 0);
    }
    g_failEnableCall = 0;
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EOK);
    g_dispatchOnConfigure = 0;
    CHECK(InterruptGroupUnbind(result.Handle) == OS_EBUSY);
    CHECK(InterruptGroupBind(result.Handle, 0, &resources) == OS_EBUSY);
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls + 4);
    CHECK(InterruptGroupSetEnabled(result.Handle, 0) == OS_EOK);
    for (uint32_t i = 0; i < result.Count; i++) {
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls + 4);
    CHECK(InterruptGroupUnbind(result.Handle) == OS_EOK);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    CHECK(InterruptGroupRelease(result.Handle) == OS_ENOENT);
    __CheckEmpty();

    // Single-message policy is expressed by an exact count, not a PCI mode.
    request.Kind = InterruptSourceMessage;
    request.Optimal = 1;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EINVALPARAMS);
    request.Minimum = 1;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Kind == InterruptSourceMessage);
    // Skip an occupied vector and negotiate independent message vectors.
    request.Optimal = 4;
    request.Minimum = 2;
    request.Kind = InterruptSourceMessage;
    CHECK(InterruptGroupCreate(&request, 1, &second) == OS_EOK);
    CHECK(second.Count == 3 && second.Handle != result.Handle);
    CHECK(InterruptGroupGetMessage(second.Handle, 0, &sentinel) == OS_EOK);
    CHECK(sentinel.Data == 225);
    CHECK(InterruptGroupRelease(second.Handle) == OS_EOK);
    request.Minimum = 4;
    CHECK(InterruptGroupCreate(&request, 1, &second) == OS_EBUSY);
    CHECK(g_liveDescriptors == g_registryAllocations + 2); // Only the first group and its vector remain.
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    for (uint32_t i = 224; i < 228; i++) {
        CHECK(g_interruptTable[i].Penalty == 0);
        CHECK(atomic_load(&g_interruptTable[i].Descriptor) == NULL);
    }
    __CheckEmpty();

    // Keep the whole group when only some user mappings could be cleaned up.
    request.Minimum = 2;
    request.ControllerID = g_testMessageDomain;
    CHECK(InterruptGroupCreate(&request, 0, &result) == OS_EOK);
    g_cloneCalls = 0;
    g_failClone = 2;
    CHECK(InterruptGroupBind(result.Handle, 0, &resources) == OS_EOK);
    CHECK(InterruptGroupBind(result.Handle, 1, &resources) == OS_EUNKNOWN);
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EINVALPARAMS);
    g_failClone = 0;
    g_failUnmap = 1;
    CHECK(InterruptGroupRelease(result.Handle) == OS_EUNKNOWN);
    CHECK(g_liveDescriptors == g_registryAllocations + 5 && g_liveMappings == 1);
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EBUSY);
    g_failUnmap = 0;
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    request.ControllerID = UUID_INVALID;
    __CheckEmpty();

    // The kernel never falls back from messages to lines. PCI policy owns that.
    g_messageVectors = 0;
    request.Minimum = 1;
    request.Kind = InterruptSourceMessage;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_ENOTSUPPORTED);
    request.Kind = InterruptSourceLine;
    request.Line.Line = g_testIndex;
    request.Optimal = 1;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Kind == InterruptSourceLine && result.Count == 1);
    CHECK(InterruptGroupGetMessage(result.Handle, 0, &sentinel) == OS_ENOTSUPPORTED);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    request.Minimum = 2;
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EINVALPARAMS);
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
__GroupWorker(void* context)
{
    (void)context;
    for (unsigned int iteration = 0; iteration < 250; iteration++) {
        InterruptGroupRequest_t request = {
            .Minimum = 1, .Optimal = iteration % 2 ? 1 : 2,
            .Kind = InterruptSourceMessage
        };
        InterruptGroupResult_t result;
        InterruptMessage_t messages[2];
        CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
        CHECK(result.Count == (iteration % 2 ? 1U : 2U));
        for (uint32_t i = 0; i < result.Count; i++) {
            CHECK(InterruptGroupGetMessage(result.Handle, i, &messages[i]) == OS_EOK);
            CHECK(atomic_exchange(&g_claimedMessages[messages[i].Data - 224], 1) == 0);
        }
        for (uint32_t i = 0; i < result.Count; i++) {
            CHECK(atomic_exchange(&g_claimedMessages[messages[i].Data - 224], 0) == 1);
        }
        CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    }
    return 0;
}

static void __TestConcurrentGroups(void)
{
    void* threads[4];
    g_messageVectors = 16;
    for (unsigned int i = 0; i < 4; i++) {
        threads[i] = CreateThread(NULL, 0, __GroupWorker, NULL, 0, NULL);
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
    CHECK(g_groups == NULL);
    g_messageVectors = 0;
    __CheckEmpty();
    puts("Interrupt groups: 1000 allocations/releases across four concurrent threads passed");
}
#endif

static void __TestPciRetirement(void)
{
    InterruptGroupRequest_t request = {
        .ControllerID = g_testMessageDomain,
        .Minimum = 1, .Optimal = 1, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t first, second;
    DeviceInterruptMessage_t firstMessage, secondMessage;
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    SystemInterrupt_t* tombstone;
    uuid_t retired;
    unsigned calls;
    g_messageVectors = 2;
    CHECK(ScInterruptAllocate(&request, &first) == OS_EOK);
    CHECK(ScInterruptGetMessage(first.Handle, 0, &firstMessage) == OS_EOK);
    CHECK(firstMessage.AddressLow == 0xfee02000 && firstMessage.AddressHigh == 0);
    CHECK(InterruptGroupBind(first.Handle, 0, &resources) == OS_EOK);
    CHECK(ScInterruptEnable(first.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(first.Handle) == OS_EOK);
    CHECK(g_groups == NULL && g_liveMappings == 0);
    tombstone = atomic_load(&g_interruptTable[firstMessage.Data].Descriptor);
    CHECK(tombstone != NULL && !tombstone->Bound && !atomic_load(&tombstone->Enabled));
    CHECK(tombstone->GroupEnabled == &g_retiredGroupEnabled);
    retired = tombstone->Id;
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    CHECK(secondMessage.Data != firstMessage.Data);
    calls = g_handlerCalls;
    InterruptHandle(NULL, firstMessage.Data);
    CHECK(g_handlerCalls == calls);
    // These groups were never enabled, so no late messages prevent reuse.
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    CHECK(secondMessage.Data != firstMessage.Data);
    CHECK(InterruptGroupBind(second.Handle, 0, &resources) == OS_EOK);
    CHECK(ScInterruptEnable(second.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    uuid_t secondRetired = atomic_load(&g_interruptTable[secondMessage.Data].Descriptor)->Id;
    CHECK(ScInterruptAllocate(&request, &second) == OS_EBUSY);
    calls = g_handlerCalls;
    InterruptHandle(NULL, LOWORD(retired));
    InterruptHandle(NULL, LOWORD(secondRetired));
    CHECK(g_handlerCalls == calls);
    // Remove the retired descriptor here. In production it stays until reboot.
    CHECK(InterruptUnregister(retired) == OS_EOK);
    CHECK(InterruptUnregister(secondRetired) == OS_EOK);
    g_messageVectors = 0;
    __CheckEmpty();
    puts("PCI vector retirement: late messages suppressed and enabled vectors not reused passed");
}

static void
__TestPciMsixRetirement(void)
{
    InterruptGroupRequest_t request = {
        .ControllerID = g_testMessageDomain,
        .Minimum = 2, .Optimal = 2, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t result;
    InterruptBindRequest_t bind = { .Resources.Handler = __CountHandler };
    DeviceInterruptMessage_t messages[2];
    SystemInterrupt_t* tombstone;
    uuid_t retired[2];
    unsigned int calls;
    unsigned int i;

    g_messageVectors = 2;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EOK && result.Count == 2);
    for (i = 0; i < 2; i++) {
        CHECK(ScInterruptGetMessage(result.Handle, i, &messages[i]) == OS_EOK);
        bind.Index = i;
        CHECK(ScInterruptBind(result.Handle, &bind) == OS_EOK);
    }
    CHECK(ScInterruptEnable(result.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);
    CHECK(g_groups == NULL && g_liveMappings == 0);
    calls = g_handlerCalls;
    for (i = 0; i < 2; i++) {
        tombstone = atomic_load(&g_interruptTable[messages[i].Data].Descriptor);
        CHECK(tombstone != NULL && !tombstone->Bound && !atomic_load(&tombstone->Enabled));
        CHECK(tombstone->GroupEnabled == &g_retiredGroupEnabled);
        retired[i] = tombstone->Id;
        InterruptHandle(NULL, messages[i].Data);
    }
    CHECK(g_handlerCalls == calls);
    CHECK(ScInterruptAllocate(&request, &result) == OS_EBUSY);
    for (i = 0; i < 2; i++) {
        InterruptHandle(NULL, messages[i].Data);
        CHECK(g_handlerCalls == calls);
        CHECK(InterruptUnregister(retired[i]) == OS_EOK);
    }
    g_messageVectors = 0;
    __CheckEmpty();
    puts("PCI MSI-X retirement: all enabled vectors retained and late dispatch suppressed passed");
}

#include "interrupt_controller_test.inc"

#include "interrupt_completion_test.inc"
#include "native_interrupt_test.inc"

int main(void)
{
    const InterruptMessageControllerIdentity_t query = {
        .Version = 1, .Kind = InterruptMessageControllerKindPlatform
    };
    SystemInterruptController_t* controller;

    // Platform discovery happens before any user requests, even on a host with
    // no published PCI devices. Tests vary capacity afterwards to model exhaustion.
    __TestLocalReservationBeforeDiscovery();
    CHECK(SystemInterruptsInitialize() == OS_EOK);
    CHECK(GetMachine()->InterruptController == NULL);
    g_messageVectors = 2;
    CHECK(SystemInterruptsInitialize() == OS_EOK);
    CHECK(InterruptMessageControllerResolve(&query, &g_testMessageDomain) == OS_EOK);
    g_registryAllocations = 1;
    g_messageVectors = 0;
    __TestIoMapSyscall();
    __TestRegistration();
    __TestLifecycle();
    __TestDiagnostics();
    __TestGate();
    __TestDdk();
    __TestDdkDisableRetry();
    __TestMixedControllerComponents();
    __TestControllerBoundary();
    __TestControllerBackends();
    __TestMessageQuery();
    __TestGroups();
    __TestCommonMessageReservations();
    __TestPciRetirement();
    __TestPciMsixRetirement();
    __TestCompletionBindings();
    __TestGroupAttachment();
    __TestNativeInterrupts();
#ifdef INTERRUPT_WINDOWS_HOST_TEST
    __TestConcurrentGroups();
#endif
    // Only the harness simulates shutdown; production domains live until reboot.
    while (GetMachine()->InterruptController != NULL) {
        controller = GetMachine()->InterruptController;
        GetMachine()->InterruptController = controller->Link;
        kfree(controller);
    }
    g_registryAllocations = 0;
    __CheckEmpty();
    return 0;
}
