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
static int          g_expectedParentLine = 64;
static unsigned int g_eventSignals;
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

oserr_t
AcquireHandleOfType(
    uuid_t       handleId,
    HandleType_t handleType,
    void**       resourceOut)
{
    CHECK((handleId == 0x55 || handleId == 0x66) && handleType == HandleTypeUserEvent);
    if (resourceOut != NULL) {
        *resourceOut = NULL;
    }
    return OS_EOK;
}

oserr_t
DestroyHandle(
    uuid_t handleId)
{
    (void)handleId;
    return OS_EOK;
}

oserr_t
UserEventSignal(
    uuid_t handleId)
{
    CHECK(handleId == 0x55 || handleId == 0x66);
    g_eventSignals++;
    return OS_EOK;
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

uuid_t
GetCurrentMemorySpaceHandle(void)
{
    return 2;
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
PlatformMsiAllocate(
    DeviceInterrupt_t* interrupt)
{
    InterruptMsiRoute_t route = {
        .ControllerId = INTERRUPT_MSI_CONTROLLER_X86_LAPIC,
        .HwIrq = g_testIndex,
        .Index = g_testIndex,
        .ParentLine = INTERRUPT_NONE
    };
    oserr_t oserr = InterruptMsiReserveTableRoute(&route);

    if (oserr != OS_EOK) {
        return oserr;
    }
    interrupt->MsiControllerId = route.ControllerId;
    interrupt->MsiHwIrq = route.HwIrq;
    interrupt->MsiIndex = route.Index;
    interrupt->MsiParentLine = route.ParentLine;
    interrupt->MsiRouteFlags = route.Flags;
    interrupt->MsiAddress = 0xFEE00000;
    interrupt->MsiValue = (uintptr_t)(0x100 | (route.HwIrq & 0xFF));
    return OS_EOK;
}

void
PlatformMsiRelease(
    const InterruptMsiRoute_t* route)
{
    (void)route;
}

oserr_t
InterruptConfigure(
    SystemInterrupt_t* interrupt,
    int enable)
{
    CHECK(enable == 0 || enable == 1);
    if (enable) {
        CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == interrupt);
        CHECK(interrupt->Index == g_testIndex);
        CHECK(interrupt->ParentLine == g_expectedParentLine);
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
    CHECK(InterruptUnregisterOwned(id, 3) == OS_ENOENT);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == existing);
    CHECK(InterruptUnregisterOwned(id, 2) == OS_EOK);
    __CheckEmpty();

    interrupt.Line = 17;
    g_expectedParentLine = 17;
    id = InterruptRegister(&interrupt, 0);
    CHECK(id != UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Index == g_testIndex);
    CHECK(existing->ParentLine == 17);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();

    g_expectedParentLine = INTERRUPT_NONE;
    id = InterruptRegister(&interrupt, INTERRUPT_MSI);
    CHECK(id != UUID_INVALID);
    CHECK(InterruptMsiCommitRoutes(&id, 1) == OS_EOK);
    CHECK(InterruptRegister(&interrupt, 0) == UUID_INVALID);
    existing = atomic_load(&g_interruptTable[g_testIndex].Descriptor);
    CHECK(existing != NULL && existing->Index == g_testIndex);
    CHECK(existing->ParentLine == INTERRUPT_NONE);
    CHECK(InterruptUnregister(id) == OS_EOK);
    CHECK(atomic_load(&g_interruptTable[g_testIndex].Descriptor) == NULL);
    CHECK(g_interruptTable[g_testIndex].Penalty == 1);
    CHECK(g_interruptTable[g_testIndex].Sharable == 0);
    CHECK(g_interruptTable[g_testIndex].MsiQuarantined == 1);
    CHECK(InterruptGetPenalty(g_testIndex) == INTERRUPT_NONE);
    CHECK(InterruptRegister(&interrupt, INTERRUPT_MSI) == UUID_INVALID);

    CHECK(InterruptMsiQuiesceRegister(2, 0x55) == OS_EOK);
    DeviceInterruptQuiesceRequest_t request = {
        .DeviceId = 0x1234,
        .Segment = 0,
        .Bus = 2,
        .Slot = 3,
        .Function = 1
    };
    InterruptMsiRoute_t routes[] = {
        {
            .ControllerId = INTERRUPT_MSI_CONTROLLER_X86_LAPIC,
            .HwIrq = g_testIndex,
            .Index = g_testIndex,
            .ParentLine = INTERRUPT_NONE
        }
    };
    uuid_t token;
    CHECK(InterruptMsiQuiesceEnqueue(&request, 2, routes, 1, &token) == OS_EOK);
    CHECK(g_eventSignals == 1);
    CHECK(InterruptMsiQuiesceNext(3, &request) == OS_EPERMISSIONS);
    CHECK(InterruptMsiQuiesceNext(2, &request) == OS_EOK);
    CHECK(request.Token == token && request.DeviceId == 0x1234);
    InterruptMsiQuiesceOwnerExit(2);
    CHECK(InterruptMsiQuiesceRegister(3, 0x66) == OS_EOK);
    CHECK(g_eventSignals == 2);
    CHECK(InterruptMsiQuiesceNext(2, &request) == OS_EPERMISSIONS);
    CHECK(InterruptMsiQuiesceNext(3, &request) == OS_EOK);
    CHECK(request.Token == token && request.DeviceId == 0x1234);
    CHECK(InterruptMsiQuiesceFinish(2, token) == OS_EPERMISSIONS);
    CHECK(g_interruptTable[g_testIndex].MsiQuarantined == 1);
    CHECK(InterruptMsiQuiesceFinish(3, token) == OS_EOK);
    __CheckEmpty();

    DeviceMsiControllerDescription_t mip = {
        .Type = DEVICE_MSI_CONTROLLER_MIP,
        .ProviderId = 17,
        .Segment = 9,
        .BusStart = 4,
        .BusEnd = 4,
        .ParentLine = 80,
        .MessageOffset = 3,
        .MessageCount = 2,
        .DoorbellAddress = 0xF1000000,
        .DoorbellLength = 0x1000
    };
    uuid_t mipController = InterruptMsiControllerRegister(&mip);
    CHECK(mipController != UUID_INVALID);
    CHECK(InterruptMsiControllerRegister(&mip) == mipController);
    mip.BusStart = 4;
    mip.BusEnd = 5;
    CHECK(InterruptMsiControllerRegister(&mip) == UUID_INVALID);
    mip.Segment = 10;
    mip.BusStart = 0;
    mip.BusEnd = 0;
    CHECK(InterruptMsiControllerRegister(&mip) == mipController);

    DeviceInterrupt_t mipInterrupt = { 0 };
    mipInterrupt.IsPci = 1;
    mipInterrupt.Segment = 9;
    mipInterrupt.Bus = 4;
    mipInterrupt.DeviceId = 0x9876;
    CHECK(InterruptMsiControllerAllocate(&mipInterrupt) == OS_EOK);
    CHECK(mipInterrupt.MsiControllerId == mipController);
    CHECK(mipInterrupt.MsiHwIrq == 3 && mipInterrupt.MsiIndex == 83);
    CHECK(mipInterrupt.MsiParentLine == 83 && mipInterrupt.MsiRouteFlags == INTERRUPT_MSI_ROUTE_PARENT_RESERVED);
    CHECK(mipInterrupt.MsiAddress == mip.DoorbellAddress && mipInterrupt.MsiValue == 3);
    CHECK(InterruptGetPenalty(83) == INTERRUPT_NONE);

    DeviceInterrupt_t mipSecondInterrupt = { 0 };
    mipSecondInterrupt.IsPci = 1;
    mipSecondInterrupt.Segment = 9;
    mipSecondInterrupt.Bus = 4;
    mipSecondInterrupt.DeviceId = 0x9877;
    CHECK(InterruptMsiControllerAllocate(&mipSecondInterrupt) == OS_EOK);
    CHECK(mipSecondInterrupt.MsiHwIrq == 4 && mipSecondInterrupt.MsiParentLine == 84);
    CHECK(InterruptMsiControllerAllocate(&mipInterrupt) == OS_EOOM);

    g_testResolveIndex = 83;
    mipInterrupt.Line = 83;
    CHECK(InterruptRegister(&mipInterrupt, INTERRUPT_EXCLUSIVE) == UUID_INVALID);
    g_testResolveIndex = g_testIndex;

    InterruptMsiRoute_t mipRoute = {
        .ControllerId = mipInterrupt.MsiControllerId,
        .HwIrq = mipInterrupt.MsiHwIrq,
        .Index = mipInterrupt.MsiIndex,
        .ParentLine = mipInterrupt.MsiParentLine,
        .Flags = mipInterrupt.MsiRouteFlags
    };
    CHECK(InterruptMsiReleaseTableRoute(&mipRoute) == OS_EOK);
    InterruptMsiControllerRelease(&mipRoute);
    DeviceInterrupt_t mipOtherHost = { 0 };
    mipOtherHost.IsPci = 1;
    mipOtherHost.Segment = 10;
    mipOtherHost.Bus = 0;
    mipOtherHost.DeviceId = 0x9878;
    CHECK(InterruptMsiControllerAllocate(&mipOtherHost) == OS_EOK);
    CHECK(mipOtherHost.MsiControllerId == mipController);
    CHECK(mipOtherHost.MsiHwIrq == 3 && mipOtherHost.MsiParentLine == 83);
    mipRoute.ControllerId = mipOtherHost.MsiControllerId;
    mipRoute.HwIrq = mipOtherHost.MsiHwIrq;
    mipRoute.Index = mipOtherHost.MsiIndex;
    mipRoute.ParentLine = mipOtherHost.MsiParentLine;
    mipRoute.Flags = mipOtherHost.MsiRouteFlags;
    CHECK(InterruptMsiReleaseTableRoute(&mipRoute) == OS_EOK);
    InterruptMsiControllerRelease(&mipRoute);
    mipRoute.HwIrq = mipSecondInterrupt.MsiHwIrq;
    mipRoute.Index = mipSecondInterrupt.MsiIndex;
    mipRoute.ParentLine = mipSecondInterrupt.MsiParentLine;
    CHECK(InterruptMsiReleaseTableRoute(&mipRoute) == OS_EOK);
    InterruptMsiControllerRelease(&mipRoute);
    memset(&mipInterrupt.MsiControllerId, 0, sizeof(mipInterrupt.MsiControllerId));
    CHECK(InterruptMsiControllerAllocate(&mipInterrupt) == OS_EOK);
    CHECK(mipInterrupt.MsiHwIrq == 3 && mipInterrupt.MsiParentLine == 83);
    mipRoute.ControllerId = mipInterrupt.MsiControllerId;
    mipRoute.HwIrq = mipInterrupt.MsiHwIrq;
    mipRoute.Index = mipInterrupt.MsiIndex;
    mipRoute.ParentLine = mipInterrupt.MsiParentLine;
    mipRoute.Flags = mipInterrupt.MsiRouteFlags;
    CHECK(InterruptMsiReleaseTableRoute(&mipRoute) == OS_EOK);
    InterruptMsiControllerRelease(&mipRoute);

    puts("Interrupt registration: failure returns, resource cleanup, shared-line rollback and reader lifetime passed");
    return 0;
}