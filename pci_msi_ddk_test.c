/**
 * Test the actual libddk lifecycle together with the PCI programming engine.
 * Kernel calls are checked state transitions; RPC failures can occur before or
 * after a real PCI operation, including a successful write with a lost response.
 */
#include "../librt/libddk/interrupt.c"
#include "../services/deviced/bus/pci/msi.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char config[256];
static struct PciMsiState pci;
static int allocated, bound, enabled, failCall = -1, failOp = -1, after, persistent;
static unsigned calls, failNth;
static int failStop;
static uint32_t allowed;

static uint32_t readConfig(void* context, size_t offset, size_t width)
{
    uint32_t value = 0;
    (void)context;
    for (size_t i = 0; i < width; i++) value |= (uint32_t)config[offset + i] << (8 * i);
    return value;
}
static void writeConfig(void* context, size_t offset, uint32_t value, size_t width)
{
    (void)context;
    for (size_t i = 0; i < width; i++) config[offset + i] = (unsigned char)(value >> (8 * i));
}
static struct PciMsiAccess access = {NULL, readConfig, writeConfig};

oserr_t OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest op, void* buffer, size_t length)
{
    CHECK(device == 42);
    if (op == OSIOCTLREQUEST_PCI_INTERRUPT_QUERY) {
        CHECK(length == sizeof(struct PciInterruptSupport));
        ((struct PciInterruptSupport*)buffer)->Strategies = allowed;
        return OS_EOK;
    }
    CHECK(op == OSIOCTLREQUEST_PCI_INTERRUPT && length == sizeof(struct PciInterruptRequest));
    struct PciInterruptRequest* request = buffer;
    CHECK(allocated);
    if (request->Operation == PciInterruptDisable && failStop) return OS_EUNKNOWN;
    if (request->Operation == PciInterruptEnable) CHECK(bound && enabled);
    int fail = request->Operation == (unsigned)failOp;
    if (fail && !persistent) failOp = -1;
    if (fail && !after) return OS_EUNKNOWN;
    oserr_t status = PciMsiControl(&access, &pci, request);
    return fail ? OS_EUNKNOWN : status;
}
uuid_t GetNativeHandle(int descriptor) { return (uuid_t)descriptor; }

SCTYPE syscall2(SCTYPE number, SCTYPE first, SCTYPE second)
{
    calls++;
    if (number == failCall || calls == failNth) return OS_EUNKNOWN;
    if (number == 16) {
        InterruptAllocationRequest_t* request = (void*)first;
        InterruptAllocationResult_t* result = (void*)second;
        CHECK(!allocated);
        *result = (InterruptAllocationResult_t) {
            .Handle = 1023, .Count = 1,
            .Strategy = (request->Strategies & INTERRUPT_STRATEGY_MSI)
                ? INTERRUPT_STRATEGY_MSI : INTERRUPT_STRATEGY_INTx,
            .AddressLow = 0xfee00000, .Data = 0x90
        };
        allocated = 1;
    } else {
        CHECK(number == 17 && allocated && !enabled);
        bound = 1;
    }
    return OS_EOK;
}
SCTYPE syscall1(SCTYPE number, SCTYPE handle)
{
    CHECK(allocated && handle == 1023);
    calls++;
    if (number == failCall || calls == failNth) return OS_EUNKNOWN;
    if (number == 18) {
        CHECK(bound);
        enabled = 1;
    } else if (number == 19) {
        CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
        enabled = 0;
    } else if (number == 20) {
        CHECK(!enabled);
        bound = 0;
    } else {
        CHECK(number == 21 && !pci.Active && !bound && !enabled);
        allocated = 0;
    }
    return OS_EOK;
}
static irqstatus_t handler(InterruptFunctionTable_t* functions, void* context)
{ (void)functions; (void)context; return IRQSTATUS_HANDLED; }

static DeviceInterrupt_t setup(void)
{
    DeviceInterrupt_t irq;
    BusDevice_t device = {.Base.Id = 42, .IsPci = 1};
    memset(config, 0, sizeof(config));
    memset(&pci, 0, sizeof(pci));
    writeConfig(NULL, 6, 0x10, 2); writeConfig(NULL, 0x34, 0x40, 1);
    writeConfig(NULL, 0x40, 5, 1); writeConfig(NULL, 0x42, MSI_64, 2);
    allocated = bound = enabled = after = persistent = 0;
    failStop = 0;
    failCall = failOp = -1; calls = failNth = 0;
    allowed = INTERRUPT_STRATEGY_MSI;
    DeviceInterruptInitialize(&irq, &device);
    return irq;
}
static void reserve(DeviceInterrupt_t* irq)
{
    uint32_t count;
    CHECK(DeviceInterruptAllocate(irq, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_EOK);
    CHECK(count == 1 && irq->Strategy == INTERRUPT_STRATEGY_MSI);
    CHECK(DeviceInterruptSetHandler(irq, 0, handler) == OS_EOK);
}
static void gone(DeviceInterrupt_t* irq)
{
    CHECK(DeviceInterruptDestroy(irq) == OS_EOK);
    CHECK(DeviceInterruptDestroy(irq) == OS_EOK);
    CHECK(!irq->State && !irq->Vectors && !irq->Count);
    CHECK(!allocated && !bound && !enabled && !pci.Active);
}

int main(void)
{
    DeviceInterrupt_t irq;
    uint32_t count;
    // Lost responses on Reserve, Enable, Disable and Release remain retryable.
    for (int op = 0; op < 4; op++) for (int applied = 0; applied < 2; applied++) {
        irq = setup();
        if (op == PciInterruptReserve) {
            failOp = op; after = applied;
            CHECK(DeviceInterruptAllocate(&irq, 1, 1, INTERRUPT_STRATEGY_MSI, &count) != OS_EOK);
            CHECK(count == 0);
        } else {
            reserve(&irq);
            if (op != PciInterruptEnable) CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
            failOp = op; after = applied;
            if (op == PciInterruptEnable) CHECK(DeviceInterruptEnable(&irq) != OS_EOK);
            if (op == PciInterruptDisable) {
                CHECK(DeviceInterruptDisable(&irq) != OS_EOK);
                CHECK(bound && enabled && allocated); // No premature unmapping.
            }
            if (op == PciInterruptRelease) CHECK(DeviceInterruptDestroy(&irq) != OS_EOK);
        }
        gone(&irq);
    }
    // A persistent failure to stop hardware must preserve all handler resources.
    irq = setup(); reserve(&irq); CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
    failOp = PciInterruptDisable; persistent = 1;
    CHECK(DeviceInterruptDestroy(&irq) != OS_EOK);
    CHECK(bound && enabled && allocated && pci.Active && irq.State);
    failOp = -1; gone(&irq);
    // Enable applied remotely but its response and the subsequent stop fail.
    // A second Enable must retry cleanup instead of claiming success.
    irq = setup(); reserve(&irq);
    failOp = PciInterruptEnable; after = 1; failStop = 1;
    CHECK(DeviceInterruptEnable(&irq) != OS_EOK);
    CHECK(bound && enabled && irq.State->PendingCleanup);
    CHECK(DeviceInterruptEnable(&irq) != OS_EOK);
    failStop = 0;
    CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
    gone(&irq);
    // Disable applied remotely but the response is lost. Re-enable must restore
    // both sides instead of returning early from the stale local Enabled bit.
    irq = setup(); reserve(&irq); CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
    failOp = PciInterruptDisable; after = 1;
    CHECK(DeviceInterruptDisable(&irq) != OS_EOK);
    CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
    CHECK(readConfig(NULL, 0x42, 2) & MSI_ENABLE);
    gone(&irq);
    // Failed allocation rollback must remain visible and cannot be enabled.
    irq = setup(); failOp = PciInterruptReserve; after = 1;
    failCall = 21;
    CHECK(DeviceInterruptAllocate(&irq, 1, 1, INTERRUPT_STRATEGY_MSI, &count) != OS_EOK);
    CHECK(irq.State && allocated && !pci.Active);
    CHECK(DeviceInterruptEnable(&irq) == OS_EBUSY);
    failCall = -1; gone(&irq);
    // Fail each kernel bind/enable/disable/unbind/release boundary, then retry.
    for (int call = 17; call <= 21; call++) {
        irq = setup(); reserve(&irq);
        if (call >= 19) CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
        failCall = call;
        if (call < 19) CHECK(DeviceInterruptEnable(&irq) != OS_EOK);
        else CHECK(DeviceInterruptDestroy(&irq) != OS_EOK);
        CHECK(allocated);
        failCall = -1; gone(&irq);
    }
    irq = setup(); reserve(&irq);
    for (unsigned i = 0; i < 20; i++) {
        CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
        CHECK(DeviceInterruptEnable(&irq) == OS_EOK);
        CHECK(DeviceInterruptDisable(&irq) == OS_EOK);
        CHECK(DeviceInterruptDisable(&irq) == OS_EOK);
    }
    gone(&irq);
    // Unsupported MSI may fall back only when the caller allows INTx.
    irq = setup(); allowed = 0;
    CHECK(DeviceInterruptAllocate(&irq, 1, 4, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(irq.Strategy == INTERRUPT_STRATEGY_INTx && !pci.Active); gone(&irq);
    irq = setup(); allowed = 0;
    CHECK(DeviceInterruptAllocate(&irq, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_ENOTSUPPORTED);
    CHECK(!allocated);
    irq = setup();
    CHECK(DeviceInterruptAllocate(&irq, 2, 2, INTERRUPT_STRATEGY_ANY, &count) == OS_ENOTSUPPORTED);
    CHECK(DeviceInterruptAllocate(&irq, 1, 1, INTERRUPT_STRATEGY_MSIX, &count) == OS_ENOTSUPPORTED);
    printf("PCI MSI libddk: RPC uncertainty, rollback, retained cleanup, fallback and re-enable PASS\n");
    return 0;
}
