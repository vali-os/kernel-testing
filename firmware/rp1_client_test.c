// Real DDK + RP1 provider/MSI-X engine + provider completion, modeled traps.
#define main __RuntimeTests
#include "rp1_runtime_test.c"
#undef main
#include "../../librt/libddk/interrupts/interrupt.c"
#include "../../librt/libddk/interrupts/pci.c"
#include <ddk/barrier.h>
#undef mb
#define mb() atomic_thread_fence(memory_order_seq_cst)

static unsigned int g_rpcCalls;
static int g_lostOperation = -1;
static int g_loseBefore;
static int g_losePersistent;

uuid_t GetNativeHandle(int descriptor) { return descriptor == 7 ? 900 : UUID_INVALID; }
InterruptFunctionTable_t* GetFastInterruptTable(void)
{
    static InterruptFunctionTable_t functions = { ReadDeviceIo, WriteDeviceIo, __Signal, NULL };
    return &functions;
}

oserr_t
OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest operation, void* buffer, size_t length)
{
    struct DeviceInterruptSourceQuery* support = buffer;
    struct DeviceInterruptSourceRequest* request = buffer;
    int lost;
    oserr_t status;

    CHECK(device == 42);
    g_rpcCalls++;
    if (operation == OSIOCTLREQUEST_INTERRUPT_QUERY) {
        CHECK(length == sizeof(*support));
        *support = (struct DeviceInterruptSourceQuery) {
            .Index = support->Index, .Kind = InterruptSourceMessage, .ControllerID = 1234
        };
        return OS_EOK;
    }
    CHECK(operation == OSIOCTLREQUEST_INTERRUPT_CONTROL && length == sizeof(*request));
    lost = (int)request->Operation == g_lostOperation;
    if (lost && !g_losePersistent) {
        g_lostOperation = -1;
    }
    if (lost && g_loseBefore) {
        return OS_EPROTOCOL;
    }
    status = Rp1RuntimeControl(&g_child, 700, request);
    return lost ? OS_EPROTOCOL : status;
}

static irqstatus_t
__ChildFastHandler(InterruptFunctionTable_t* functions, void* context)
{
    InterruptResourceTable_t* resources = context;
    g_model.LevelCleared = 1;
    functions->EventSignal(resources->HandleResource);
    return IRQSTATUS_HANDLED;
}

static void
__ChildInterrupt(DeviceInterrupt_t* interrupt)
{
    InterruptResourceTable_t* resources;
    InterruptResourceTable_t* completion;
    uuid_t completionHandle;
    unsigned int calls = g_rpcCalls;
    unsigned int acks = g_model.Acks;
    unsigned int events = g_model.Events;
    unsigned int index = interrupt->State->Handle - 1;

    CHECK(g_model.Groups[index].Enabled);
    resources = &g_model.Groups[index].Resources;
    completionHandle = g_model.Groups[index].Completions[0];
    g_model.LevelCleared = 0;
    resources->Handler(GetFastInterruptTable(), resources);
    if (completionHandle != UUID_INVALID) {
        completion = &g_model.Completions[completionHandle - 1].Resources;
        completion->Handler(GetFastInterruptTable(), completion);
    }
    CHECK(g_model.LevelCleared && g_model.Events == events + 1);
    CHECK(g_model.Acks == acks + (completionHandle != UUID_INVALID));
    CHECK(g_rpcCalls == calls);
}

static void
__PrepareChildSource(DeviceInterrupt_t* interrupt, uint32_t sourceIndex)
{
    uint32_t count;
    DeviceInterruptInitialize(interrupt, NULL);
    interrupt->DeviceId = 42;
    CHECK(DeviceInterruptSelectSource(interrupt, sourceIndex) == OS_EOK);
    CHECK(DeviceInterruptAllocate(interrupt, 1, 1, &count) == OS_EOK);
    CHECK(count == 1 && interrupt->Kind == InterruptSourceMessage);
    CHECK(DeviceInterruptEnable(interrupt) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptSetHandler(interrupt, 0, __ChildFastHandler) == OS_EOK);
    CHECK(DeviceInterruptSetDescriptor(interrupt, 0, 7) == OS_EOK);
}

static void
__PrepareChild(DeviceInterrupt_t* interrupt)
{
    __PrepareChildSource(interrupt, 0);
}

int main(void)
{
    DeviceInterrupt_t interrupt;
    uint32_t count;
    unsigned int before;
    unsigned int operation;

    __Setup();
    CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
    __PrepareChild(&interrupt);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(g_bus.Runtime->Sources[6].Allocation == interrupt.State->Handle);
    __ChildInterrupt(&interrupt);
    __ChildInterrupt(&interrupt);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(g_model.Table[6][3] & 1);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    __ChildInterrupt(&interrupt);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(g_model.Allocated == 1 && g_bus.Runtime->Sources[6].Owner == NULL);
    CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);

    // A nonzero firmware source still uses handler vector zero. Selecting it
    // must not reserve or enable the preceding source of the same child.
    __Setup();
    CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
    __PrepareChildSource(&interrupt, 1);
    CHECK(g_bus.Runtime->Sources[31].Allocation == interrupt.State->Handle);
    CHECK(g_bus.Runtime->Sources[6].Owner == NULL);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(!g_model.Table[31][3] && g_model.Table[6][3]);
    __ChildInterrupt(&interrupt);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);

    // Edge sources use the same client lifecycle without a completion callback.
    __Setup();
    g_child.Firmware.Interrupts[0].Type = 1;
    CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
    __PrepareChild(&interrupt);
    CHECK(g_model.Groups[interrupt.State->Handle - 1].Completions[0] == UUID_INVALID);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    __ChildInterrupt(&interrupt);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
    CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);

    for (before = 0; before < 2; before++) {
        for (operation = DeviceInterruptSourceReserve; operation <= DeviceInterruptSourceRelease; operation++) {
            __Setup();
            CHECK(Rp1RuntimeStart(&g_bus) == OS_EOK);
            g_lostOperation = (int)operation;
            g_loseBefore = before;
            if (operation == DeviceInterruptSourceReserve) {
                DeviceInterruptInitialize(&interrupt, NULL);
                interrupt.DeviceId = 42;
                CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EPROTOCOL);
                CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
            } else {
                __PrepareChild(&interrupt);
                if (operation == DeviceInterruptSourceEnable) {
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EPROTOCOL);
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
                    __ChildInterrupt(&interrupt);
                } else {
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
                }
                if (operation == DeviceInterruptSourceDisable || operation == DeviceInterruptSourceRelease) {
                    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EPROTOCOL);
                    CHECK(interrupt.State != NULL);
                }
                CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK);
            }
            CHECK(g_model.Allocated == 1 && g_bus.Runtime->Sources[6].Owner == NULL);
            CHECK(Rp1RuntimeStop(&g_bus) == OS_EOK);
        }
    }
    puts("RP1 DDK chain: fast-handler delivery without IPC, lifecycle and eight uncertain RPC recovery cases passed");
    return 0;
}
