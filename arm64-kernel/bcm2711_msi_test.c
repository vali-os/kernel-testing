// Run the production core and BCM2711 backend against a W1C/mask MMIO model.
#include <ddk/types/interrupt-group.h>
#include <ddk/types/interrupt-controller.h>
#include <arch/platform.h>
#undef MAX_SUPPORTED_INTERRUPTS
#undef INTERRUPT_MESSAGE_BASE
#define INTERRUPT_MESSAGE_BASE 1024
#define MAX_SUPPORTED_INTERRUPTS 1056
#define main InterruptRegressionMain
#define INTERRUPT_MIP_HOST_TEST
#include "../interrupt_registration_test.c"
#undef main
#define __VALI_AARCH64_PRIVATE_H__
#include <ddk/io.h>
#undef mb
#define mb() atomic_thread_fence(memory_order_seq_cst)
uintptr_t Arm64MapDevice(uint64_t physical, uint64_t length);
#include "../../kernel/arch/aarch64/rpi/bcm2711-msi.c"

static uint32_t g_registers[BCM2711_MSI_REGISTER_LENGTH / 4];
static unsigned int g_writes;
static int g_failMap;
static int g_failReadback;
static uint32_t g_seen;
static uint32_t g_inject;
static uuid_t g_selfRemove = UUID_INVALID;
static uuid_t g_removePeer = UUID_INVALID;
static uuid_t g_domainId;

static oserr_t
__ReservedTag(void* context, uint32_t ordinal, InterruptControllerResource_t* resource)
{
    oserr_t status = __BcmMsiAllocate(context, ordinal, resource);

    resource->TableIndex = 1023;
    return status;
}

uintptr_t
Arm64MapDevice(uint64_t physical, uint64_t length)
{
    CHECK(physical == 0xfd500000ULL && length == 0x9310);
    return g_failMap ? 0 : (uintptr_t)g_registers;
}

void
WriteVolatileMemory(volatile void* pointer, void* data, size_t length)
{
    uint32_t offset = (uint32_t)((uintptr_t)pointer - (uintptr_t)g_registers);
    uint32_t value = *(uint32_t*)data;

    CHECK(length == 4 && offset < sizeof(g_registers));
    g_writes++;
    if (offset == 0x4508) {
        g_registers[0x4500 / 4] &= ~value;
    } else if (offset == 0x4510) {
        g_registers[0x450c / 4] |= value;
    } else if (offset == 0x4514) {
        g_registers[0x450c / 4] &= ~value;
    } else {
        g_registers[offset / 4] = value;
    }
}

void
ReadVolatileMemory(const volatile void* pointer, volatile void* data, size_t length)
{
    uint32_t offset = (uint32_t)((uintptr_t)pointer - (uintptr_t)g_registers);

    CHECK(length == 4 && offset < sizeof(g_registers));
    *(volatile uint32_t*)data = g_failReadback && offset == 0x4044 ? 0 : g_registers[offset / 4];
}

static void
__HostReady(uint64_t base, unsigned int order)
{
    memset(g_registers, 0, sizeof(g_registers));
    g_registers[0x4068 / 4] = 0xb0;
    g_registers[0x406c / 4] = 0x0303;
    g_registers[0x4034 / 4] = (uint32_t)base | (order - 15);
    g_registers[0x4038 / 4] = (uint32_t)(base >> 32);
}

static irqstatus_t
__Child(InterruptFunctionTable_t* functions, void* context)
{
    uint32_t bit = 1U << (uintptr_t)context;
    InterruptMessage_t message;
    uuid_t id;

    (void)functions;
    CHECK(g_active && atomic_load(&g_interruptReaders) == 2);
    CHECK(!(g_registers[0x4500 / 4] & bit));
    g_seen |= bit;
    g_handlerCalls++;
    if (g_removePeer != UUID_INVALID) {
        CHECK(InterruptUnregister(g_removePeer) == OS_EOK);
        g_removePeer = UUID_INVALID;
        // The parent already captured this child's pending bit. Core retirement
        // must stop the slot being reused until the entire snapshot is done.
        CHECK(InterruptReserveMessage(g_domainId, 1, INTERRUPT_KERNEL, &id, &message) == OS_EBUSY);
    }
    if (g_inject) {
        g_registers[0x4500 / 4] |= g_inject;
        g_inject = 0;
    }
    if (g_selfRemove != UUID_INVALID) {
        CHECK(InterruptUnregister(g_selfRemove) == OS_EOK);
        g_selfRemove = UUID_INVALID;
    }
    return IRQSTATUS_HANDLED;
}

static void
__Dispatch(uint32_t incoming, uint32_t expected)
{
    unsigned int acks = g_ackCalls;

    g_seen = 0;
    g_registers[0x4500 / 4] |= incoming;
    InterruptHandle(NULL, 180);
    CHECK(g_seen == expected && g_ackCalls == acks + 1);
    CHECK(!g_active && atomic_load(&g_interruptReaders) == 0);
}

static void
__TestReceiver(SystemInterruptController_t* domain, uint64_t base, unsigned int order, uint64_t address)
{
    struct __BcmMsiController* controller = domain->Message.Backend.Context;
    InterruptMessage_t message;
    InterruptResourceTable_t resources = { .Handler = __Child };
    InterruptGroupRequest_t request = { .ControllerID = domain->Id, .Minimum = 32,
        .Optimal = 32, .Kind = InterruptSourceMessage };
    InterruptGroupResult_t result;
    SystemInterrupt_t* entry;
    uuid_t ids[32];
    uuid_t id;
    unsigned int index;
    unsigned int baseline = g_liveDescriptors;
    unsigned int writes;
    uint32_t originalBar;

    __HostReady(base, order);
    originalBar = g_registers[0x4034 / 4];
    g_domainId = domain->Id;
    CHECK(InterruptReserveMessage(domain->Id, 32, INTERRUPT_KERNEL, &id, &message) == OS_EINVALPARAMS);
    // No receiver write is permitted while host reset/link/window setup is incomplete.
    g_registers[0x9210 / 4] = 3;
    writes = g_writes;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EBUSY);
    CHECK(g_writes == writes);
    g_registers[0x9210 / 4] = 0;
    g_registers[0x4068 / 4] = 0x80;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EBUSY);
    g_registers[0x4068 / 4] = 0xb0;
    g_registers[0x406c / 4] = 0x0302;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_ENOTSUPPORTED);
    g_registers[0x406c / 4] = 0x0303;
    g_registers[0x402c / 4] = 1;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_ENOTSUPPORTED);
    g_registers[0x402c / 4] = 0;
    g_registers[0x4034 / 4] = 0;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_ENOTSUPPORTED);
    g_registers[0x4034 / 4] = 22;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_ENOTSUPPORTED);
    g_registers[0x4034 / 4] = 21;
    g_registers[0x4038 / 4] = 0;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_ENOTSUPPORTED);
    g_registers[0x4038 / 4] = (uint32_t)(base >> 32);
    g_registers[0x4034 / 4] = originalBar;
    atomic_store(&controller->State, BcmMsiStarting);
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EBUSY);
    atomic_store(&controller->State, BcmMsiUninitialized);
    CHECK(g_writes == writes);
    g_failReadback = 1;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EUNKNOWN);
    g_failReadback = 0;
    CHECK(g_registers[0x4044 / 4] == 0 && g_registers[0x450c / 4] == UINT32_MAX);
    g_failEnableCall = g_enableCalls + 1;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EUNKNOWN);
    g_failEnableCall = 0;
    CHECK(atomic_load(&controller->State) == BcmMsiUninitialized);
    CHECK(g_liveDescriptors == baseline && controller->Claimed == 0);

    // Group allocation failure at each stage must release every hardware bit.
    for (index = 1; index <= 33; index++) {
        g_allocationCalls = 0;
        g_failAllocationCall = index;
        CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOOM);
        g_failAllocationCall = 0;
        CHECK(controller->Claimed == 0 && g_liveDescriptors == baseline);
    }
    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK && result.Count == 32);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK && controller->Claimed == 0);
    domain->Message.Backend.Operations.AllocateOrdinal = __ReservedTag;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EINVALPARAMS);
    CHECK(controller->Claimed == 0 && g_liveDescriptors == baseline);
    CHECK(!atomic_load(&g_interruptTable[1023].Descriptor));
    domain->Message.Backend.Operations.AllocateOrdinal = __BcmMsiAllocate;
    for (index = 0; index < 32; index++) {
        CHECK(InterruptReserveMessage(domain->Id, index, INTERRUPT_KERNEL, &ids[index], &message) == OS_EOK);
        CHECK(LOWORD(ids[index]) == 1024 + index);
        CHECK(message.Address == address && message.Data == (0x6540 | index));
        entry = InterruptGet(ids[index]);
        entry->Context = (void*)(uintptr_t)index;
        InterruptPut(entry);
        CHECK(InterruptBind(ids[index], &resources) == OS_EOK);
        CHECK(InterruptSetEnabled(ids[index], 1) == OS_EOK);
    }
    CHECK(g_registers[0x4044 / 4] == ((uint32_t)address | 1));
    CHECK(g_registers[0x4048 / 4] == (uint32_t)(address >> 32));
    CHECK(g_registers[0x404c / 4] == 0xffe06540);
    CHECK(g_registers[0x450c / 4] == 0);
    // Exhaustion/collision must not acknowledge another owner's pending bit.
    g_registers[0x4500 / 4] = 1;
    writes = g_writes;
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &id, &message) == OS_EBUSY);
    CHECK(g_writes == writes && g_registers[0x4500 / 4] == 1);
    __Dispatch(UINT32_MAX, UINT32_MAX);
    CHECK(InterruptSetEnabled(ids[7], 0) == OS_EOK);
    __Dispatch((1U << 7) | (1U << 31), 1U << 31);
    CHECK(g_registers[0x4500 / 4] == (1U << 7));
    CHECK(InterruptSetEnabled(ids[7], 1) == OS_EOK);
    __Dispatch(0, 1U << 7);
    g_inject = 1;
    __Dispatch(1, 1);
    CHECK(g_registers[0x4500 / 4] == 1);
    __Dispatch(0, 1);
    __Dispatch(0, 0);
    g_removePeer = ids[1];
    __Dispatch(3, 1);
    CHECK(InterruptReserveMessage(domain->Id, 1, INTERRUPT_KERNEL, &ids[1], &message) == OS_EOK);
    g_selfRemove = ids[0];
    __Dispatch(1, 1);
    CHECK(InterruptReserveMessage(domain->Id, 0, INTERRUPT_KERNEL, &ids[0], &message) == OS_EOK);
    for (index = 0; index < 32; index++) {
        CHECK(InterruptRelease(ids[index], 0) == OS_EOK);
    }
    // A stale edge is cleared on fresh ownership, before the new handler binds.
    g_registers[0x4500 / 4] = 1;
    CHECK(InterruptReserveMessage(domain->Id, 0, 0, &id, &message) == OS_EOK);
    CHECK(g_registers[0x4500 / 4] == 0);
    CHECK(InterruptRelease(id, 1) == OS_EOK);
    CHECK(InterruptReserveMessage(domain->Id, 0, 0, &ids[0], &message) == OS_EBUSY);
    __Dispatch(1, 0);
    // Explicit shutdown cleanup of the core tombstone and permanent backend.
    CHECK(InterruptUnregister(id) == OS_EOK);
    CHECK(InterruptSetEnabled(controller->ParentId, 0) == OS_EOK);
    atomic_store(&controller->State, BcmMsiUninitialized);
    atomic_store(&controller->Claimed, 0);
    CHECK(g_liveDescriptors == baseline);
}

int
main(int argc, char** argv)
{
    unsigned char blob[262144];
    FILE* file;
    size_t size;
    DeviceTree_t* tree;
    const DeviceTreeNode_t* node;
    const DeviceTreeNode_t* gic = NULL;
    struct Arm64Bcm2711MsiRoute route;
    struct Arm64Bcm2711MsiRoute sentinel;
    SystemInterruptController_t* domain;
    struct __BcmMsiController* controller;
    InterruptDescriptor_t occupied = { .Line = 180, .Pin = INTERRUPT_NONE };
    InterruptMessageControllerIdentity_t query = { .Version = 1,
        .Kind = InterruptMessageControllerKindDeviceTree };
    unsigned int baseline;
    unsigned int index;
    uuid_t id;
    uuid_t occupiedId;
    oserr_t status;

    CHECK(argc == 4);
    file = fopen(argv[1], "rb");
    CHECK(file);
    size = fread(blob, 1, sizeof(blob), file);
    fclose(file);
    CHECK(DeviceTreeCreate(blob, (uint32_t)size, &tree) == OS_EOK);
    for (node = tree->Nodes; node; node = node->NextNode) {
        if (DeviceTreeIsCompatible(node, "arm,gic-400")) {
            gic = node;
        }
    }
    CHECK(gic);
    g_testResolveIndex = 180;
    baseline = g_liveDescriptors;
    if (atoi(argv[3])) {
        for (node = tree->Nodes; node; node = node->NextNode) {
            if (Arm64Bcm2711MsiReadRoute(tree, node, gic, 1020, &route) == OS_EOK) {
                CHECK(route.RegisterBase == 0xfd500000 && route.ParentInterrupt == 180);
                memset(&sentinel, 0xa5, sizeof(sentinel));
                route = sentinel;
                CHECK(Arm64Bcm2711MsiReadRoute(tree, node, gic, 180, &route) == OS_EINVALPARAMS);
                CHECK(!memcmp(&route, &sentinel, sizeof(route)));
            }
        }
        for (index = 1; index <= 3; index++) {
            g_allocationCalls = 0;
            g_failAllocationCall = index;
            CHECK(Arm64Bcm2711MsiInitialize(tree, gic->Phandle, 1020) == OS_EOOM);
            g_failAllocationCall = 0;
            CHECK(g_liveDescriptors == baseline && GetMachine()->InterruptController == NULL);
        }
        g_failMap = 1;
        CHECK(Arm64Bcm2711MsiInitialize(tree, gic->Phandle, 1020) == OS_EOOM);
        g_failMap = 0;
        CHECK(g_liveDescriptors == baseline);
        // The aggregate parent must be exclusive, even before the receiver is
        // initialized. A failed claim must leave the previous owner intact.
        CHECK(InterruptReserve(&occupied, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE, &occupiedId) == OS_EOK);
        CHECK(Arm64Bcm2711MsiInitialize(tree, gic->Phandle, 1020) == OS_EBUSY);
        CHECK(atomic_load(&g_interruptTable[180].Descriptor)->Id == occupiedId);
        CHECK(InterruptUnregister(occupiedId) == OS_EOK);
        CHECK(g_liveDescriptors == baseline);
    }
    status = Arm64Bcm2711MsiInitialize(tree, gic->Phandle, 1020);
    CHECK(status == (oserr_t)atoi(argv[2]) && g_writes == 0);
    domain = GetMachine()->InterruptController;
    CHECK((domain != NULL) == !!atoi(argv[3]));
    if (domain) {
        query.FirmwareNode = domain->Message.Source.FirmwareNode;
        CHECK(InterruptMessageControllerResolve(&query, &id) == OS_EOK && id == domain->Id);
        CHECK(!atomic_load(&g_interruptTable[180].Descriptor)->Enabled);
        CHECK(atomic_load(&g_interruptTable[180].Descriptor)->Routing.Line == 180);
        CHECK(atomic_load(&g_interruptTable[180].Descriptor)->Routing.AcpiConform &
            INTERRUPT_ACPICONFORM_TRIGGERMODE);
        __TestReceiver(domain, 0, 32, 0xffffffffcULL);
        __TestReceiver(domain, 0, 31, 0xfffffffcULL);
        __TestReceiver(domain, 0x100000000ULL, 32, 0xfffffffcULL);
        controller = domain->Message.Backend.Context;
        CHECK(InterruptUnregister(controller->ParentId) == OS_EOK);
        kfree(controller);
        kfree(domain);
        GetMachine()->InterruptController = NULL;
    }
    CHECK(g_liveDescriptors == baseline);
    DeviceTreeDestroy(tree);
    CHECK(g_liveDescriptors == 0);
    printf("BCM2711 MSI checks passed: %s\n", argv[1]);
    return 0;
}
