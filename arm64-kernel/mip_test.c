// Use the real interrupt core with the existing host kernel-service stubs.
// Model the ARM GIC's full ID space even when running under x64 ASan.
#include <arch/platform.h>
#undef MAX_SUPPORTED_INTERRUPTS
#define MAX_SUPPORTED_INTERRUPTS 1020
#define main InterruptRegressionMain
#define INTERRUPT_MIP_HOST_TEST
#define InterruptConfigure __HarnessInterruptConfigure
#include "../interrupt_registration_test.c"
#undef InterruptConfigure
#undef main
#include "../../kernel/arch/aarch64/rpi/mip.h"

// Hardware mappings and barriers are modeled by this host test. The actual
// AArch64 implementation is separately compiled by the architecture checks.
#define __VALI_AARCH64_PRIVATE_H__
#include <ddk/io.h>
#undef mb
#define mb() atomic_thread_fence(memory_order_seq_cst)
uintptr_t Arm64MapDevice(uint64_t physical, uint64_t length);
oserr_t InterruptConfigure(const InterruptControllerResource_t* resource, int enable);
#include "../../kernel/arch/aarch64/rpi/mip.c"

static uint32_t g_mipRegisters[4][ARM64_MIP_REGISTER_LENGTH / 4];
static unsigned int g_mipMaps;
static unsigned int g_mipWrites;
static int g_failMipMap;
static int g_failMipReadback;

uintptr_t
Arm64MapDevice(uint64_t physical, uint64_t length)
{
    CHECK((physical & 3) == 0 && length == ARM64_MIP_REGISTER_LENGTH);
    if (g_failMipMap) {
        return 0;
    }
    CHECK(g_mipMaps < 4);
    return (uintptr_t)g_mipRegisters[g_mipMaps++];
}

void
WriteVolatileMemory(volatile void* pointer, void* data, size_t length)
{
    CHECK(length == 4);
    *(volatile uint32_t*)pointer = *(uint32_t*)data;
    g_mipWrites++;
}

void
ReadVolatileMemory(const volatile void* pointer, volatile void* data, size_t length)
{
    CHECK(length == 4);
    *(volatile uint32_t*)data = g_failMipReadback ? 1 : *(const volatile uint32_t*)pointer;
}

oserr_t
InterruptConfigure(const InterruptControllerResource_t* resource, int enable)
{
    CHECK(resource->Flags & INTERRUPT_MSI);
    CHECK(resource->TableIndex == (uuid_t)resource->Line);
    CHECK(resource->AcpiConform == 0);
    CHECK(resource->Line >= 32 && resource->Line < 1020);
    return __HarnessInterruptConfigure(resource, enable);
}

static void
__TestMipGroup(SystemInterruptController_t* controller)
{
    InterruptGroupRequest_t request = {
        .ControllerID = controller->Id,
        .Minimum = 1,
        .Optimal = InterruptMessageControllerCapacity(controller),
        .Strategies = INTERRUPT_STRATEGY_MSIX
    };
    InterruptGroupResult_t result;
    InterruptGroupResult_t other;
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    InterruptControllerResource_t route;
    InterruptMessage_t message;
    SystemInterruptController_t* peer;
    SystemInterrupt_t* tombstone;
    uuid_t id;
    uint32_t index;
    uint32_t line;
    unsigned int calls;
    unsigned int baseline = g_liveDescriptors;
    unsigned int writes = g_mipWrites;

    CHECK(InterruptControllerAllocateMessage(controller, request.Optimal,
        INTERRUPT_MSI, &route) == OS_EINVALPARAMS);

    for (index = 1; index <= request.Optimal + 1; index++) {
        g_allocationCalls = 0;
        g_failAllocationCall = index;
        CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOOM);
        CHECK(g_liveDescriptors == baseline && g_groups == NULL);
    }
    g_failAllocationCall = 0;

    CHECK(InterruptGroupCreate(&request, 1, &result) == OS_EOK);
    CHECK(result.Count == request.Optimal && result.Strategy == INTERRUPT_STRATEGY_MSIX);
    CHECK(InterruptGroupCreate(&request, 1, &other) == OS_EBUSY);
    for (index = 0; index < result.Count; index++) {
        CHECK(InterruptControllerAllocateMessage(controller, index, INTERRUPT_MSI, &route) == OS_EOK);
        CHECK(InterruptGroupGetMessage(result.Handle, index, &message) == OS_EOK);
        CHECK(message.Address == route.Message.Address);
        CHECK(message.Data == route.Message.Data);
        CHECK(InterruptGroupBind(result.Handle, index, &resources) == OS_EOK);
        calls = g_handlerCalls;
        InterruptHandle(NULL, route.TableIndex);
        CHECK(g_handlerCalls == calls);
    }
    // Independent MIPs remain allocatable; aliases of this MIP share its slots.
    for (peer = GetMachine()->InterruptController; peer; peer = peer->Link) {
        if (peer == controller) {
            continue;
        }
        CHECK(InterruptControllerAllocateMessage(peer, 0, INTERRUPT_MSI, &route) == OS_EOK);
        if (atomic_load(&g_interruptTable[route.TableIndex].Descriptor)) {
            CHECK(InterruptReserveMessage(peer->Id, 0, INTERRUPT_KERNEL, &id, &route.Message) == OS_EBUSY);
        } else {
            CHECK(InterruptReserveMessage(peer->Id, 0, INTERRUPT_KERNEL, &id, &route.Message) == OS_EOK);
            CHECK(InterruptRelease(id, 0) == OS_EOK);
        }
    }
    g_failEnableCall = g_enableCalls + 2;
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EUNKNOWN);
    g_failEnableCall = 0;
    calls = g_handlerCalls;
    for (index = 0; index < result.Count; index++) {
        CHECK(InterruptControllerAllocateMessage(controller, index, INTERRUPT_MSI, &route) == OS_EOK);
        InterruptHandle(NULL, route.TableIndex);
    }
    CHECK(g_handlerCalls == calls);
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EOK);
    for (index = 0; index < result.Count; index++) {
        CHECK(InterruptControllerAllocateMessage(controller, index, INTERRUPT_MSI, &route) == OS_EOK);
        calls = g_handlerCalls;
        InterruptHandle(NULL, route.TableIndex);
        CHECK(g_handlerCalls == calls + 1);
    }
    CHECK(InterruptGroupSetEnabled(result.Handle, 0) == OS_EOK);
    calls = g_handlerCalls;
    InterruptHandle(NULL, route.TableIndex);
    CHECK(g_handlerCalls == calls);
    CHECK(InterruptGroupSetEnabled(result.Handle, 1) == OS_EOK);
    InterruptHandle(NULL, route.TableIndex);
    CHECK(g_handlerCalls == calls + 1);
    CHECK(InterruptGroupRelease(result.Handle) == OS_EOK);
    CHECK(g_mipWrites == writes);

    // Single-message MSI uses the same controller and composes the same payload.
    request.Strategies = INTERRUPT_STRATEGY_MSI;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EOK);
    CHECK(result.Count == 1 && result.Strategy == INTERRUPT_STRATEGY_MSI);
    CHECK(InterruptControllerAllocateMessage(controller, 0, INTERRUPT_MSI, &route) == OS_EOK);
    line = route.TableIndex;
    CHECK(result.Data == route.Message.Data);
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);

    // Once exposed, the core's tombstone prevents a late MIP message from
    // reaching a newly allocated handler, even through another host handle.
    CHECK(InterruptReserveMessage(controller->Id, 0, 0, &id, &route.Message) == OS_EOK);
    CHECK(InterruptRelease(id, 1) == OS_EOK);
    tombstone = atomic_load(&g_interruptTable[line].Descriptor);
    CHECK(tombstone && tombstone->ControllerReleased && !tombstone->Bound);
    calls = g_handlerCalls;
    InterruptHandle(NULL, line);
    CHECK(g_handlerCalls == calls);
    CHECK(InterruptReserveMessage(controller->Id, 0, 0, &id, &route.Message) == OS_EBUSY);
    for (peer = GetMachine()->InterruptController; peer; peer = peer->Link) {
        CHECK(InterruptControllerAllocateMessage(peer, 0, INTERRUPT_MSI, &route) == OS_EOK);
        if (route.TableIndex == line) {
            CHECK(InterruptReserveMessage(peer->Id, 0, 0, &id, &route.Message) == OS_EBUSY);
        }
    }
    CHECK(InterruptUnregister(tombstone->Id) == OS_EOK);
}

static void
__ReleaseMipControllers(void)
{
    SystemInterruptController_t* controller;

    // Simulate shutdown only; production controllers live until reboot.
    while (GetMachine()->InterruptController) {
        controller = GetMachine()->InterruptController;
        GetMachine()->InterruptController = controller->Link;
        kfree(controller->Message.Backend.Context);
        kfree(controller);
    }
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
    const DeviceTreeProperty_t* property;
    SystemInterruptController_t* controller;
    struct Arm64MipRoute route;
    InterruptMessageControllerIdentity_t query = { .Version = 1,
        .Kind = InterruptMessageControllerKindDeviceTree };
    struct FDTHeader header;
    unsigned int domains = 0;
    unsigned int baseline;
    unsigned int index;
    uuid_t id;
    oserr_t expected;
    oserr_t status;
    struct Arm64MipRoute sentinel;

    CHECK(argc == 4 || argc == 5);
    expected = (oserr_t)atoi(argv[2]);
    file = fopen(argv[1], "rb");
    CHECK(file);
    size = fread(blob, 1, sizeof(blob), file);
    fclose(file);
    CHECK(DeviceTreeCreate(blob, (uint32_t)size, &tree) == OS_EOK);
    if (argc == 5) {
        CHECK(!strcmp(argv[4], "--external"));
        node = DeviceTreeFindPath(tree, "/axi/pcie@1000110000");
        CHECK(node);
        property = DeviceTreeGetProperty(node, "status");
        CHECK(property && property->Length >= 5);
        memcpy((void*)property->Value, "okay", 5);
    }
    CHECK(FdtParseHeader(blob, (uint32_t)size, &header) == OS_EOK);
    for (node = tree->Nodes; node; node = node->NextNode) {
        CHECK(FdtReadBe32(blob + header.OffDtStruct + node->NodeOffset) == FDT_BEGIN_NODE);
        CHECK(!strcmp((const char*)blob + header.OffDtStruct + node->NodeOffset + 4, node->Name));
        if (DeviceTreeIsCompatible(node, "arm,gic-400")) {
            gic = node;
        }
    }
    CHECK(gic);
    // Check fixture expectations independently of the backend's composition.
    for (node = tree->Nodes; node; node = node->NextNode) {
        memset(&route, 0xa5, sizeof(route));
        sentinel = route;
        status = Arm64MipReadRoute(tree, node, gic, 1020, &route);
        if (status != OS_EOK) {
            CHECK(!memcmp(&route, &sentinel, sizeof(route)));
            continue;
        }
        if (expected == OS_EOK) {
            if (strstr(argv[1], "bcm2712")) {
                CHECK(route.MessageAddress == 0xfffffff000ULL);
                if (route.RegisterBase == 0x1000130000ULL) {
                    CHECK(route.Count == 64 && route.MessageOffset == 0);
                    CHECK(route.FirstInterrupt == 160);
                } else {
                    CHECK(route.RegisterBase == 0x1000131000ULL);
                    CHECK(route.Count == 8 && route.MessageOffset == 8);
                    CHECK(route.FirstInterrupt == 287);
                }
            } else {
                CHECK(route.Count == 8 && route.MessageOffset == 8);
                if (route.Provider->Phandle == 2) {
                    CHECK(route.FirstInterrupt == 104 && route.MessageAddress == 0xffffffe000ULL);
                    CHECK(route.RegisterBase == 0x1000130000ULL);
                } else {
                    CHECK(route.FirstInterrupt == 168 && route.MessageAddress == 0xfffffff000ULL);
                    CHECK(route.RegisterBase == 0x1000131000ULL);
                }
            }
            CHECK(Arm64MipReadRoute(tree, node, gic, route.FirstInterrupt, &sentinel) == OS_EINVALPARAMS);
        }
    }
    baseline = g_liveDescriptors;
    if (expected == OS_EOK && atoi(argv[3]) != 0) {
        g_failAllocation = 1;
        CHECK(Arm64MipInitialize(tree, gic->Phandle, 1020) == OS_EOOM);
        g_failAllocation = 0;
        CHECK(g_liveDescriptors == baseline && g_mipMaps == 0);
        g_failMipMap = 1;
        CHECK(Arm64MipInitialize(tree, gic->Phandle, 1020) == OS_EOOM);
        g_failMipMap = 0;
        CHECK(g_liveDescriptors == baseline && g_mipMaps == 0);
        g_failMipReadback = 1;
        CHECK(Arm64MipInitialize(tree, gic->Phandle, 1020) == OS_EUNKNOWN);
        g_failMipReadback = 0;
        CHECK(g_liveDescriptors == baseline && GetMachine()->InterruptController == NULL);
        g_mipMaps = 0;
        g_mipWrites = 0;
        // Fail each publication allocation. Earlier published contexts remain
        // alive for the fatal boot path; unpublished contexts are reclaimed.
        for (index = 0; index < (unsigned int)atoi(argv[3]); index++) {
            g_allocationCalls = 0;
            g_failAllocationCall = (unsigned int)atoi(argv[3]) + index + 1;
            CHECK(Arm64MipInitialize(tree, gic->Phandle, 1020) == OS_EOOM);
            CHECK(g_liveDescriptors == baseline + index * 2);
            g_failAllocationCall = 0;
            __ReleaseMipControllers();
            g_mipMaps = 0;
            g_mipWrites = 0;
        }
    }
    status = Arm64MipInitialize(tree, gic->Phandle, 1020);
    if (status != expected) {
        printf("initialize returned %u, expected %u\n", status, expected);
    }
    CHECK(status == expected);
    if (status == OS_EOK) {
        for (controller = GetMachine()->InterruptController; controller; controller = controller->Link) {
            domains++;
            query.FirmwareNode = controller->Message.Source.FirmwareNode;
            CHECK(InterruptMessageControllerResolve(&query, &id) == OS_EOK && id == controller->Id);
            __TestMipGroup(controller);
        }
        CHECK(domains == (unsigned int)atoi(argv[3]));
        for (index = 0; index < g_mipMaps; index++) {
            CHECK(g_mipRegisters[index][0x20 / 4] == UINT32_MAX);
            CHECK(g_mipRegisters[index][0x30 / 4] == UINT32_MAX);
            CHECK(g_mipRegisters[index][0x40 / 4] == 0);
            CHECK(g_mipRegisters[index][0x50 / 4] == 0);
            CHECK(g_mipRegisters[index][0x60 / 4] == UINT32_MAX);
            CHECK(g_mipRegisters[index][0x70 / 4] == UINT32_MAX);
        }
        CHECK(g_mipWrites == g_mipMaps * 8);
        for (node = tree->Nodes; node; node = node->NextNode) {
            query.FirmwareNode = node->NodeOffset;
            status = Arm64MipReadRoute(tree, node, gic, 1020, &route);
            CHECK(InterruptMessageControllerResolve(&query, &id) ==
                (status == OS_EOK ? OS_EOK : OS_ENOTSUPPORTED));
        }
    } else {
        CHECK(GetMachine()->InterruptController == NULL && g_mipWrites == 0);
        CHECK(g_liveDescriptors == baseline);
    }
    __ReleaseMipControllers();
    DeviceTreeDestroy(tree);
    CHECK(g_liveDescriptors == 0 && g_liveMappings == 0 && g_groups == NULL);
    printf("MIP firmware/core checks passed: %s (%u domains, %u blocks)\n", argv[1], domains, g_mipMaps);
    return 0;
}
