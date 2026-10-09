#include "../kernel/interrupts.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static unsigned int g_liveDescriptors;
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
static const uuid_t g_testIndex = 64;
static uuid_t       g_testResolveIndex = g_testIndex;

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

    if (g_failAllocation) {
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
    CHECK(atomic_load(&lock->Current) == 0);
    atomic_store(&lock->Current, 1);
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
    return 0;
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
    DeviceInterrupt_t* interrupt,
    unsigned int flags,
    uuid_t* tableIndex)
{
    (void)interrupt;
    (void)flags;
    *tableIndex = g_testResolveIndex;
    return g_failResolve ? OS_EUNKNOWN : OS_EOK;
}

oserr_t
InterruptConfigure(
    SystemInterrupt_t* interrupt,
    int enable)
{
    CHECK(enable == 0 || enable == 1);
    if (enable) {
        CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == interrupt);
        CHECK(LOWORD(interrupt->Id) == g_testIndex);
    } else {
        CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == NULL);
    }
    if (g_holdReader) {
        InterruptReadEnter();
    }
    return g_failConfigure ? OS_EUNKNOWN : OS_EOK;
}

static void
__CheckEmpty(void)
{
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == NULL);
    CHECK(g_interruptTable[g_testIndex].Penalty == 0);
    CHECK(g_interruptTable[g_testIndex].Sharable == 0);
    CHECK(g_retiredInterrupts == NULL);
    CHECK(g_liveDescriptors == 0 && g_liveMappings == 0 && g_liveIo == 0);
}

int
main(void)
{
    DeviceIo_t         io = {0};
    DeviceInterrupt_t  interrupt = {0};
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
    g_holdReader = 1;
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == NULL);
    CHECK(g_retiredInterrupts != NULL && g_liveDescriptors == 1);
    CHECK(g_liveMappings == 3 && g_liveIo == 2);
    InterruptReadExit();
    InterruptReclaimRetired();
    __CheckEmpty();
    g_holdReader = g_failConfigure = 0;

    id = InterruptRegister(&interrupt, INTERRUPT_KERNEL);
    CHECK(id != UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Id == id);
    CHECK(InterruptRegister(&interrupt, INTERRUPT_EXCLUSIVE) == UUID_INVALID);
    g_failConfigure = 1;
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == existing);
    CHECK(g_interruptTable[g_testIndex].Penalty == 1);
    CHECK(g_interruptTable[g_testIndex].Sharable == 1);
    CHECK(g_liveDescriptors == 1 && g_liveMappings == 0 && g_liveIo == 0);
    CHECK(g_retiredInterrupts == NULL);

    atomic_store(&g_interruptTable[g_testIndex].Descriptor, NULL);
    g_interruptTable[g_testIndex].Penalty = 0;
    g_interruptTable[g_testIndex].Sharable = 0;
    InterruptRetire(existing);
    InterruptReclaimRetired();
    __CheckEmpty();

    g_failConfigure = 0;
    id = InterruptRegister(&interrupt, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE);
    CHECK(id != UUID_INVALID);
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Id == id);
    CHECK(g_interruptTable[g_testIndex].Penalty == 1);
    CHECK(g_interruptTable[g_testIndex].Sharable == 0);
    CHECK(g_liveDescriptors == 1 && g_liveMappings == 0 && g_liveIo == 0);
    atomic_store(&g_interruptTable[g_testIndex].Descriptor, NULL);
    g_interruptTable[g_testIndex].Penalty = 0;
    InterruptRetire(existing);
    InterruptReclaimRetired();
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
    return 0;
}