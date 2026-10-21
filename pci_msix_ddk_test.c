// Share the modeled hardware, bounds and masked-write assertions with the
// provider engine suite, then drive it through the actual public DDK lifecycle.
#define main __MsixEngineTests
#include "pci_msix_test.c"
#undef main

static void
__DdkFree(void* allocation)
{
    if (allocation != NULL) {
        __MsixTestFree(allocation);
    }
}

#define calloc __MsixTestAllocate
#define free __DdkFree
#include "../librt/libddk/interrupt.c"
#undef calloc
#undef free

static uint32_t g_modes;
static uint32_t g_capacity;
static uint32_t g_available;
static uint32_t g_count;
static uint32_t g_strategy;
static unsigned g_bound;
static int g_enabled;
static int g_kernelAllocated;
static int g_failCall;
static int g_failIndex;
static int g_failOperation;
static int g_lostReply;
static int g_stopFailure;
static int g_releaseFailure;
static int g_unbindFailure;
static int g_msiReserved;
static oserr_t g_queryFailure;
static oserr_t g_reserveFailure;

uuid_t
GetNativeHandle(int descriptor)
{
    return descriptor;
}

SCTYPE
syscall2(SCTYPE number, SCTYPE first, SCTYPE second)
{
    InterruptGroupRequest_t* request = (void*)first;
    InterruptGroupResult_t* result = (void*)second;
    InterruptBindRequest_t* bind = (void*)second;

    if (number == 19) {
        CHECK(first == 1023 && g_kernelAllocated && !g_enabled);
        if (g_failCall == 19 && (int)bind->Index == g_failIndex) {
            return OS_EOOM;
        }
        CHECK(bind->Index < g_count);
        g_bound |= 1U << bind->Index;
        return OS_EOK;
    }
    CHECK(number == 18 && !g_kernelAllocated);
    if (g_failCall == 18) {
        return OS_EOOM;
    }
    g_count = 1;
    if ((request->Strategies & INTERRUPT_STRATEGY_MSIX) && g_available >= request->Minimum) {
        CHECK(request->Optimal <= g_capacity);
        g_strategy = INTERRUPT_STRATEGY_MSIX;
        g_count = request->Optimal < g_available ? request->Optimal : g_available;
    } else if (request->Minimum == 1 && (request->Strategies & INTERRUPT_STRATEGY_MSI) && g_available) {
        g_strategy = INTERRUPT_STRATEGY_MSI;
    } else if (request->Minimum == 1 && (request->Strategies & INTERRUPT_STRATEGY_INTx)) {
        g_strategy = INTERRUPT_STRATEGY_INTx;
    } else {
        return OS_EBUSY;
    }
    *result = (InterruptGroupResult_t) { .Handle = 1023, .Count = g_count, .Strategy = g_strategy };
    g_kernelAllocated = 1;
    return OS_EOK;
}

SCTYPE
syscall3(SCTYPE number, SCTYPE handle, SCTYPE index, SCTYPE output)
{
    CHECK(number == 24 && handle == 1023 && g_kernelAllocated && index < g_count);
    if (g_failCall == 24 && (int)index == g_failIndex) {
        return OS_EUNKNOWN;
    }
    *(DeviceInterruptMessage_t*)output = (DeviceInterruptMessage_t) {
        .AddressLow = 0xfee02000, .AddressHigh = 0x1234, .Data = 0xe0 + index
    };
    return OS_EOK;
}

SCTYPE
syscall1(SCTYPE number, SCTYPE handle)
{
    CHECK(handle == 1023 && g_kernelAllocated);
    if (number == 22 && g_unbindFailure) {
        return OS_EOOM;
    }
    if (number == g_failCall) {
        return OS_EUNKNOWN;
    }
    if (number == 20) {
        CHECK(g_bound == (1U << g_count) - 1);
        g_enabled = 1;
    } else if (number == 21) {
        CHECK(g_state == NULL || !g_state->MayDeliver);
        g_enabled = 0;
    } else if (number == 22) {
        CHECK(!g_enabled);
        g_bound = 0;
    } else {
        CHECK(number == 23 && !g_enabled && !g_bound && !g_msiReserved && g_state == NULL);
        g_kernelAllocated = 0;
    }
    return OS_EOK;
}

oserr_t
OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest operation, void* buffer, size_t length)
{
    struct PciInterruptRequest* request = buffer;
    int fail;
    oserr_t status;

    CHECK(device == 42);
    if (operation == OSIOCTLREQUEST_PCI_INTERRUPT_QUERY) {
        CHECK(length == sizeof(struct PciInterruptSupport));
        *(struct PciInterruptSupport*)buffer = (struct PciInterruptSupport) {
            .Strategies = g_modes, .MsixCapacity = g_capacity, .MsiCapacity = 1
        };
        return g_queryFailure;
    }
    CHECK(g_kernelAllocated);
    CHECK(operation == OSIOCTLREQUEST_PCI_INTERRUPT && length == sizeof(*request));
    CHECK(request->Strategy == g_strategy);
    if (request->Strategy == INTERRUPT_STRATEGY_MSI) {
        if (request->Operation == PciInterruptReserve) {
            g_msiReserved = 1;
        } else if (request->Operation == PciInterruptRelease) {
            g_msiReserved = 0;
        }
        return OS_EOK;
    }
    if (request->Operation == PciInterruptEnable) {
        CHECK(g_enabled && g_bound == (1U << g_count) - 1);
    }
    if (request->Operation == PciInterruptReserve && g_reserveFailure != OS_EOK) {
        return g_reserveFailure;
    }
    if (request->Operation == PciInterruptDisable && g_stopFailure) {
        return OS_EPROTOCOL;
    }
    if (request->Operation == PciInterruptRelease && g_releaseFailure) {
        return OS_EPROTOCOL;
    }
    fail = request->Operation == (unsigned)g_failOperation;
    if (request->Operation == PciInterruptProgram && (int)request->Index != g_failIndex) {
        fail = 0;
    }
    if (fail) {
        g_failOperation = -1;
        if (!g_lostReply) {
            return OS_EPROTOCOL;
        }
    }
    status = PciMsixControl(&g_access, &g_state, request);
    return fail ? OS_EPROTOCOL : status;
}

static irqstatus_t
__Handler(InterruptFunctionTable_t* functions, void* context)
{
    (void)functions;
    (void)context;
    return IRQSTATUS_HANDLED;
}

static DeviceInterrupt_t
__DdkSetup(void)
{
    BusDevice_t device = { .Base.Id = 42, .Type = BusTypePci, .InterruptLine = 17 };
    DeviceInterrupt_t interrupt;

    __Setup();
    CHECK(!g_kernelAllocated && !g_enabled && !g_bound);
    g_modes = INTERRUPT_STRATEGY_MSI | INTERRUPT_STRATEGY_MSIX;
    g_capacity = g_available = 4;
    g_failCall = g_failIndex = g_failOperation = -1;
    g_lostReply = g_stopFailure = g_releaseFailure = g_unbindFailure = 0;
    g_queryFailure = g_reserveFailure = OS_EOK;
    DeviceInterruptInitialize(&interrupt, &device);
    return interrupt;
}

static void
__AllocateDdk(DeviceInterrupt_t* interrupt)
{
    uint32_t count;
    uint32_t i;

    CHECK(DeviceInterruptAllocate(interrupt, 2, 3, INTERRUPT_STRATEGY_MSIX, &count) == OS_EOK);
    CHECK(count == 3 && interrupt->Strategy == INTERRUPT_STRATEGY_MSIX);
    for (i = 0; i < count; i++) {
        CHECK(DeviceInterruptSetHandler(interrupt, i, __Handler) == OS_EOK);
        CHECK(g_table[i][0] == 0xfee02000 && g_table[i][1] == 0x1234 && g_table[i][2] == 0xe0 + i);
    }
}

static void
__Gone(DeviceInterrupt_t* interrupt)
{
    g_failCall = g_failOperation = -1;
    g_stopFailure = g_releaseFailure = g_failUnmap = g_unbindFailure = 0;
    CHECK(DeviceInterruptDestroy(interrupt) == OS_EOK);
    CHECK(DeviceInterruptDestroy(interrupt) == OS_EOK);
    CHECK(!g_kernelAllocated && !g_enabled && !g_bound && !g_allocations);
    CHECK(interrupt->State == NULL && interrupt->Vectors == NULL && interrupt->Count == 0);
    __Restored();
}

int
main(void)
{
    DeviceInterrupt_t interrupt;
    uint32_t count;
    unsigned int operation;
    unsigned int applied;
    unsigned int index;
    unsigned int call;

    interrupt = __DdkSetup();
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSIX, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Strategy == INTERRUPT_STRATEGY_MSIX);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __Handler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    __Gone(&interrupt);
    interrupt = __DdkSetup();
    g_available = 3;
    CHECK(DeviceInterruptAllocate(&interrupt, 2, UINT32_MAX, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(count == 3 && interrupt.Strategy == INTERRUPT_STRATEGY_MSIX);
    __Gone(&interrupt);
    interrupt = __DdkSetup();
    CHECK(DeviceInterruptAllocate(&interrupt, 5, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_ENOTSUPPORTED);
    g_capacity = 0;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Strategy == INTERRUPT_STRATEGY_MSI);
    __Gone(&interrupt);
    interrupt = __DdkSetup();
    g_available = 0;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Strategy == INTERRUPT_STRATEGY_INTx);
    __Gone(&interrupt);
    interrupt = __DdkSetup();
    g_queryFailure = OS_EPROTOCOL;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_EPROTOCOL);
    CHECK(!g_kernelAllocated && count == 0);
    g_queryFailure = OS_ENOTSUPPORTED;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 8, INTERRUPT_STRATEGY_ANY, &count) == OS_EOK);
    CHECK(interrupt.Strategy == INTERRUPT_STRATEGY_INTx);
    __Gone(&interrupt);

    // Failed local storage or provider allocations leave no hidden owner.
    for (index = 1; index <= 4; index++) {
        interrupt = __DdkSetup();
        g_failAllocation = index;
        CHECK(DeviceInterruptAllocate(&interrupt, 2, 3, INTERRUPT_STRATEGY_MSIX, &count) == OS_EOOM);
        CHECK(count == 0);
        __Gone(&interrupt);
    }
    interrupt = __DdkSetup();
    g_reserveFailure = OS_EBUSY;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 3, INTERRUPT_STRATEGY_ANY, &count) == OS_EBUSY);
    CHECK(count == 0 && !interrupt.State && !g_kernelAllocated);
    __Gone(&interrupt);

    // Lost replies and send failures at every operation, including each message.
    for (operation = PciInterruptReserve; operation <= PciInterruptRelease; operation++) {
        for (applied = 0; applied < 2; applied++) {
            for (index = 0; index < (operation == PciInterruptProgram ? 3U : 1U); index++) {
                interrupt = __DdkSetup();
                if (operation >= PciInterruptEnable) {
                    __AllocateDdk(&interrupt);
                }
                if (operation >= PciInterruptDisable) {
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
                }
                g_failOperation = operation;
                g_failIndex = index;
                g_lostReply = applied;
                if (operation <= PciInterruptProgram) {
                    g_releaseFailure = 1;
                    CHECK(DeviceInterruptAllocate(&interrupt, 2, 3, INTERRUPT_STRATEGY_MSIX, &count) == OS_EPROTOCOL);
                    CHECK(count == 0 && interrupt.State && g_kernelAllocated);
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EBUSY);
                } else if (operation == PciInterruptEnable) {
                    g_stopFailure = 1;
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EPROTOCOL);
                    CHECK(g_bound == 7 && g_enabled && interrupt.State->PendingCleanup);
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EPROTOCOL);
                    g_stopFailure = 0;
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
                } else if (operation == PciInterruptDisable) {
                    CHECK(DeviceInterruptDisable(&interrupt) == OS_EPROTOCOL);
                    CHECK(g_bound == 7 && g_enabled);
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
                } else {
                    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EPROTOCOL);
                    CHECK(DeviceInterruptEnable(&interrupt) == OS_EBUSY);
                }
                __Gone(&interrupt);
            }
        }
    }
    for (index = 0; index < 3; index++) {
        interrupt = __DdkSetup();
        g_failCall = 24;
        g_failIndex = index;
        g_releaseFailure = 1;
        CHECK(DeviceInterruptAllocate(&interrupt, 2, 3, INTERRUPT_STRATEGY_MSIX, &count) == OS_EUNKNOWN);
        CHECK(count == 0 && interrupt.State && g_state);
        __Gone(&interrupt);
        interrupt = __DdkSetup();
        __AllocateDdk(&interrupt);
        g_failCall = 19;
        g_failIndex = index;
        CHECK(DeviceInterruptEnable(&interrupt) == OS_EOOM);
        CHECK(!g_enabled && !g_bound);
        __Gone(&interrupt);
    }
    for (call = 20; call <= 23; call++) {
        interrupt = __DdkSetup();
        __AllocateDdk(&interrupt);
        if (call != 20) {
            CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
        }
        g_failCall = call;
        CHECK((call == 20 ? DeviceInterruptEnable(&interrupt) : DeviceInterruptDestroy(&interrupt)) == OS_EUNKNOWN);
        CHECK(g_kernelAllocated);
        __Gone(&interrupt);
    }
    interrupt = __DdkSetup();
    __AllocateDdk(&interrupt);
    g_failCall = 19;
    g_failIndex = 1;
    g_unbindFailure = 1;
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOOM);
    CHECK(g_bound == 1 && interrupt.State->Bound && !g_enabled);
    CHECK(DeviceInterruptSetHandler(&interrupt, 2, __Handler) == OS_EBUSY);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOOM);
    CHECK(g_kernelAllocated && g_state && g_bound == 1);
    __Gone(&interrupt);
    interrupt = __DdkSetup();
    __AllocateDdk(&interrupt);
    for (index = 0; index < 8; index++) {
        CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
        CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    }
    g_failUnmap = 1;
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EUNKNOWN);
    CHECK(g_state && g_mapped && g_kernelAllocated && !g_bound && !g_enabled);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EBUSY);
    __Gone(&interrupt);
    puts("MSI-X libddk: indexed programming, negotiation, fallback, RPC uncertainty and retryable teardown PASS");
    return 0;
}
