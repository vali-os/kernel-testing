#include <ddk/types/interrupt-group.h>
#include <ddk/types/interrupt-controller.h>
#include <ddk/interrupts/provider.h>
// Exercise real component lookup, syscall copies, descriptor allocation and groups.
static void
__TestMixedControllerComponents(void)
{
    SystemInterruptController_t* first;
    SystemInterruptController_t* last;
    SystemInterruptController_t* message;

    CHECK(GetNextInterruptController(NULL)->Type == SystemInterruptControllerMessage);
    CHECK(GetInterruptControllerByLine(0) == NULL);
    CHECK(CreateInterruptController(0, -1, 1, 0) == OS_EINVALPARAMS);
    CHECK(CreateInterruptController(0, 0, 0, 0) == OS_EINVALPARAMS);
    CHECK(CreateInterruptController(0, INT_MAX, 2, 0) == OS_EINVALPARAMS);
    g_failAllocation = 1;
    CHECK(CreateInterruptController(0, 32, 16, 0) == OS_EOOM);
    g_failAllocation = 0;
    CHECK(CreateInterruptController(g_testMessageDomain, 32, 16, 0x1000) == OS_EOK);
    CHECK(CreateInterruptController(12, INT_MAX, 1, 0x2000) == OS_EOK);
    g_registryAllocations += 2;
    first = GetInterruptControllerByLine(32);
    last = GetNextInterruptController(first);
    CHECK(first->Id == g_testMessageDomain && last->Id == 12);
    CHECK(GetNextInterruptController(last) == NULL);
    CHECK(GetInterruptControllerByLine(32) == first && GetInterruptControllerByLine(47) == first);
    CHECK(GetInterruptControllerByLine(48) == NULL && GetInterruptControllerByLine(INT_MAX) == last);
    CHECK(GetPinOffsetByLine(47) == 15 && GetPinOffsetByLine(INTERRUPT_NONE) == INTERRUPT_NONE);
    CHECK(InterruptMessageControllerLookup(g_testMessageDomain, 0, &message) == OS_EOK);
    CHECK(message != first && message->Type == SystemInterruptControllerMessage);
    __CheckEmpty();
    puts("Mixed controllers: line iteration and lookup remain separate from message domain IDs passed");
}

static void
__TestControllerBoundary(void)
{
    InterruptMessageControllerIdentity_t query = {
        .Version = 1, .Kind = InterruptMessageControllerKindPlatform
    };
    InterruptGroupRequest_t request = {
        .ControllerID = g_testMessageDomain, .Minimum = 1, .Optimal = 1, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t first;
    DeviceInterruptMessage_t firstMessage, secondMessage;
    InterruptGroupResult_t second;
    uuid_t domain = 0xdead;
    unsigned int calls = g_allocationCalls;

    g_messageVectors = 2;
    g_denyInterruptControl = 1;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EPERMISSIONS);
    CHECK(domain == 0xdead);
    g_denyInterruptControl = 0;
    g_failCopy = 1;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    g_failCopy = 0;
    g_failCopyOut = 1;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    CHECK(domain == 0xdead && g_allocationCalls == calls);
    CHECK(DeviceInterruptMessageControllerResolve(NULL, &domain) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptMessageControllerResolve(&query, NULL) == OS_EINVALPARAMS);
    query.Version = 2;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    query.Version = 1;
    query.Reserved = 1;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    query.Reserved = 0;
    query.Kind = 3; // Former DeviceTreeLine value must not be a message identity.
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    query.Kind = 99;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    query.Kind = InterruptMessageControllerKindPlatform;
    query.FirmwareNode = 123;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EINVALPARAMS);
    query.Kind = InterruptMessageControllerKindDeviceTree;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_ENOTSUPPORTED);
    CHECK(domain == 0xdead && g_allocationCalls == calls);
    query.Kind = InterruptMessageControllerKindPlatform;
    query.FirmwareNode = 0;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EOK && domain == g_testMessageDomain);
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EOK);
    CHECK(g_allocationCalls == calls);

    // Domains exist independently of service publication and caller identity.
    // Allocated groups still enforce ownership by the requesting memory space.
    CHECK(ScInterruptAllocate(&request, &first) == OS_EOK);
    CHECK(ScInterruptGetMessage(first.Handle, 0, &firstMessage) == OS_EOK);
    g_currentSpace = 3;
    CHECK(DeviceInterruptMessageControllerResolve(&query, &domain) == OS_EOK && domain == g_testMessageDomain);
    CHECK(ScInterruptRelease(first.Handle) == OS_EPERMISSIONS);
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    CHECK(firstMessage.AddressLow == 0xfee02000 && secondMessage.AddressLow == firstMessage.AddressLow);
    CHECK(firstMessage.Data != secondMessage.Data);
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(first.Handle) == OS_EOK);
    request.ControllerID = 0;
    CHECK(ScInterruptAllocate(&request, &first) == OS_ENOTSUPPORTED);
    request.ControllerID = UINT32_MAX;
    CHECK(ScInterruptAllocate(&request, &first) == OS_ENOENT);
    request.ControllerID = 12;
    CHECK(ScInterruptAllocate(&request, &first) == OS_ENOENT);
    request.ControllerID = 0;
    CHECK(InterruptGroupCreate(&request, 1, &first) == OS_EOK);
    CHECK(InterruptGroupRelease(first.Handle) == OS_EOK);
    g_messageVectors = 0;
    __CheckEmpty();
    puts("Message domains: privilege, copy faults, read-only resolution, caller-independent IDs and shared x86 pool passed");
}

/** @brief Independent kernel backend with a distinct range and retryable cleanup. */
struct __TestMessageController {
    unsigned int Base;
    unsigned int Calls;
    unsigned int Resources;
    unsigned int Releases;
    unsigned int Retirements;
    unsigned int ActiveReleases;
    int FailRelease;
};

static uint32_t
__TestControllerCapacity(void* context)
{
    (void)context;
    return 2;
}

static oserr_t
__TestControllerReserve(
    _In_    void*                          context,
    _In_    uint32_t                       ordinal,
    _InOut_ InterruptControllerResource_t* resource)
{
    struct __TestMessageController* controller = context;
    InterruptMessageControllerIdentity_t query = { .Version = 1, .Kind = InterruptMessageControllerKindPlatform };
    uuid_t domain;

    // Reentrant lookup proves callbacks execute outside the registry lock.
    CHECK(InterruptMessageControllerResolve(&query, &domain) == OS_EOK);
    controller->Calls++;
    controller->Resources++;
    resource->Token = controller->Calls;
    resource->TableIndex = controller->Base + ordinal;
    resource->Line = (int)resource->TableIndex;
    resource->Message.Address = 0x100000000ULL + controller->Base * 4096;
    resource->Message.Data = ordinal;
    return OS_EOK;
}

static oserr_t
__TestControllerEnable(
    _In_ void*                                context,
    _In_ const InterruptControllerResource_t* resource,
    _In_ int                                  enable)
{
    (void)context;
    return InterruptConfigure(resource, enable);
}

static oserr_t
__TestControllerRelease(
    _In_ void*                                context,
    _In_ const InterruptControllerResource_t* resource,
    _In_ int                                  retire)
{
    struct __TestMessageController* controller = context;

    CHECK(resource->Token != 0 && controller->Resources != 0);
    if (controller->FailRelease) {
        return OS_EUNKNOWN;
    }
    controller->Resources--;
    controller->Releases++;
    controller->Retirements += !!retire;
    controller->ActiveReleases += !!InterruptGetActiveStatus();
    return OS_EOK;
}

/** @brief State kept alive while a kernel message handler unregisters itself. */
struct __SelfReleaseMessage {
    uuid_t Domain;
    uuid_t Id;
};

static irqstatus_t
__SelfReleaseMessageHandler(
    _In_ InterruptFunctionTable_t* table,
    _In_ void*                     context)
{
    struct __SelfReleaseMessage* self = context;
    InterruptMessage_t message;
    uuid_t rejected;

    (void)table;
    CHECK(InterruptGetActiveStatus());
    CHECK(InterruptUnregister(self->Id) == OS_EOK);
    CHECK(InterruptReserveMessage(self->Domain, 0, INTERRUPT_KERNEL, &rejected, &message) == OS_EBUSY);
    return IRQSTATUS_HANDLED;
}

static void
__TestControllerBackends(void)
{
    static struct __TestMessageController left = { .Base = 200 };
    static struct __TestMessageController right = { .Base = 210 };
    struct InterruptMessageBackend backend = {
        .Operations = { __TestControllerCapacity, __TestControllerReserve,
                        __TestControllerEnable, __TestControllerRelease },
        .Context = &left
    };
    InterruptMessageControllerIdentity_t query = {
        .Version = 1, .Kind = InterruptMessageControllerKindDeviceTree, .FirmwareNode = 123
    };
    InterruptGroupRequest_t request = {
        .Minimum = 2, .Optimal = 2, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t first;
    DeviceInterruptMessage_t firstMessage, secondMessage;
    InterruptGroupResult_t second;
    DeviceInterruptMessage_t message;
    InterruptMessage_t coreMessage;
    InterruptResourceTable_t resources = { .Handler = __CountHandler };
    SystemInterrupt_t* entry;
    uuid_t reservation;
    uuid_t rejected;
    uuid_t leftDomain;
    uuid_t rightDomain;
    uuid_t sentinel = 0xdead;
    uuid_t generation;
    unsigned int failure;
    unsigned int releases;
    struct __SelfReleaseMessage self;

    query.Version = 2;
    CHECK(CreateInterruptMessageController(&query, &backend, &sentinel) == OS_EINVALPARAMS);
    CHECK(sentinel == 0xdead);
    query.Version = 1;
    g_failAllocation = 1;
    CHECK(CreateInterruptMessageController(&query, &backend, &sentinel) == OS_EOOM);
    g_failAllocation = 0;
    CHECK(sentinel == 0xdead);
    CHECK(CreateInterruptMessageController(&query, &backend, &leftDomain) == OS_EOK);
    CHECK(CreateInterruptMessageController(&query, &backend, &sentinel) == OS_EEXISTS);
    CHECK(sentinel == 0xdead);
    CHECK(DeviceInterruptMessageControllerResolve(&query, &sentinel) == OS_EOK && sentinel == leftDomain);
    query.FirmwareNode++;
    backend.Context = &right;
    generation = g_messageControllerIdGen;
    g_messageControllerIdGen = UINT32_MAX;
    CHECK(CreateInterruptMessageController(&query, &backend, &sentinel) == OS_EOVERFLOW);
    g_messageControllerIdGen = generation;
    CHECK(CreateInterruptMessageController(&query, &backend, &rightDomain) == OS_EOK);
    CHECK(leftDomain != rightDomain && leftDomain != g_testMessageDomain);
    g_registryAllocations += 2;
    memset(&backend, 0, sizeof(backend));
    request.ControllerID = leftDomain;
    for (failure = 1; failure <= 3; failure++) {
        g_allocationCalls = 0;
        g_failAllocationCall = failure;
        CHECK(ScInterruptAllocate(&request, &first) == OS_EOOM);
        CHECK(g_liveDescriptors == g_registryAllocations && g_groups == NULL);
        CHECK(left.Resources == 0);
    }
    g_failAllocationCall = 0;
    CHECK(ScInterruptAllocate(&request, &first) == OS_EOK);
    CHECK(ScInterruptGetMessage(first.Handle, 0, &firstMessage) == OS_EOK);
    CHECK(firstMessage.AddressHigh == 1 && firstMessage.AddressLow == left.Base * 4096 && firstMessage.Data == 0);
    CHECK(ScInterruptGetMessage(first.Handle, 1, &message) == OS_EOK && message.Data == 1);
    request.ControllerID = rightDomain;
    CHECK(ScInterruptAllocate(&request, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    CHECK(secondMessage.AddressLow == right.Base * 4096 && secondMessage.Data == 0);
    CHECK(left.Calls != 0 && right.Calls == 2);
    left.FailRelease = 1;
    CHECK(ScInterruptRelease(first.Handle) == OS_EUNKNOWN);
    CHECK(left.Resources == 2);
    left.FailRelease = 0;
    CHECK(ScInterruptRelease(first.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    CHECK(left.Resources == 0 && right.Resources == 0);

    // An occupied core slot rolls back only the new backend allocation.
    CHECK(InterruptReserveMessage(leftDomain, 0, INTERRUPT_KERNEL, &reservation, &coreMessage) == OS_EOK);
    releases = left.Releases;
    CHECK(InterruptReserveMessage(leftDomain, 0, INTERRUPT_KERNEL, &rejected, &coreMessage) == OS_EBUSY);
    CHECK(rejected == UUID_INVALID && left.Resources == 1 && left.Releases == releases + 1);
    CHECK(InterruptBind(reservation, &resources) == OS_EOK);
    CHECK(InterruptSetEnabled(reservation, 1) == OS_EOK);
    CHECK(InterruptRelease(reservation, 1) == OS_EINVALPARAMS);
    CHECK(InterruptSetEnabled(reservation, 0) == OS_EOK);
    CHECK(InterruptUnbind(reservation) == OS_EOK);

    // Backend retirement failure keeps the core handle and resources retryable.
    left.FailRelease = 1;
    CHECK(InterruptRelease(reservation, 1) == OS_EUNKNOWN);
    CHECK(left.Resources == 1);
    left.FailRelease = 0;
    CHECK(InterruptRelease(reservation, 1) == OS_EOK);
    CHECK(left.Resources == 0 && left.Retirements == 1);
    entry = InterruptGet(reservation);
    CHECK(entry != NULL && entry->ControllerReleased);
    CHECK(entry->GroupEnabled == &g_retiredGroupEnabled);
    InterruptPut(entry);
    CHECK(InterruptBind(reservation, &resources) == OS_EBUSY);
    CHECK(InterruptSetEnabled(reservation, 1) == OS_EINVALPARAMS);
    CHECK(InterruptReserveMessage(leftDomain, 0, INTERRUPT_KERNEL, &rejected, &coreMessage) == OS_EBUSY);
    releases = left.Releases;
    // Harness shutdown removes the tombstone without releasing the backend twice.
    CHECK(InterruptUnregister(reservation) == OS_EOK);
    CHECK(left.Releases == releases && left.Resources == 0);

    CHECK(InterruptReserveMessage(leftDomain, 0, INTERRUPT_KERNEL, &self.Id, &coreMessage) == OS_EOK);
    self.Domain = leftDomain;
    entry = InterruptGet(self.Id);
    entry->Context = &self;
    InterruptPut(entry);
    resources.Handler = __SelfReleaseMessageHandler;
    CHECK(InterruptBind(self.Id, &resources) == OS_EOK);
    CHECK(InterruptSetEnabled(self.Id, 1) == OS_EOK);
    InterruptHandle(NULL, LOWORD(self.Id));
    CHECK(left.ActiveReleases == 2 && left.Resources == 0);
    CHECK(InterruptReserveMessage(leftDomain, 0, INTERRUPT_KERNEL, &reservation, &coreMessage) == OS_EOK);
    CHECK(InterruptUnregister(reservation) == OS_EOK);
    CHECK(DeviceInterruptMessageControllerResolve(&query, &sentinel) == OS_EOK && sentinel == rightDomain);
    __CheckEmpty();
    puts("Message backends: distinct firmware identities, copied callbacks, allocation rollback, cleanup retry and permanent domains passed");
}

static void
__TestCommonMessageReservations(void)
{
    InterruptDescriptor_t descriptor = { .Line = 224, .Pin = INTERRUPT_NONE };
    InterruptDescriptor_t automatic = { .Line = INTERRUPT_NONE, .Pin = INTERRUPT_NONE };
    InterruptGroupRequest_t request = {
        .Minimum = 2, .Optimal = 2, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t group;
    InterruptMessage_t message;
    SystemInterrupt_t* entry;
    uuid_t standalone;
    uuid_t second;
    uuid_t rejected;
    unsigned int calls;

    g_messageVectors = 3;
    descriptor.ResourceTable.Handler = __CountHandler;
    standalone = InterruptRegister(&descriptor, INTERRUPT_KERNEL | INTERRUPT_MSI);
    CHECK(standalone != UUID_INVALID && LOWORD(standalone) == 224);
    CHECK(descriptor.MsiAddress == 0xfee02000 && descriptor.MsiValue == 224);
    entry = InterruptGet(standalone);
    CHECK(entry->Routing.Controller->Type == SystemInterruptControllerMessage);
    CHECK(entry->Flags & INTERRUPT_EXCLUSIVE);
    InterruptPut(entry);
    calls = g_handlerCalls;
    InterruptHandle(NULL, 224);
    CHECK(g_handlerCalls == calls + 1);

    CHECK(InterruptGroupCreate(&request, 1, &group) == OS_EOK);
    CHECK(InterruptGroupGetMessage(group.Handle, 0, &message) == OS_EOK);
    CHECK(message.Address == descriptor.MsiAddress && message.Data == 225);
    CHECK(InterruptReserve(&automatic, INTERRUPT_KERNEL | INTERRUPT_MSI, &rejected) == OS_EBUSY);
    CHECK(InterruptGroupRelease(group.Handle) == OS_EOK);
    CHECK(InterruptReserve(&automatic, INTERRUPT_KERNEL | INTERRUPT_MSI, &second) == OS_EOK);
    CHECK(automatic.Line == 225 && automatic.MsiValue == 225);
    CHECK(InterruptUnregister(second) == OS_EOK);

    // Candidate-list requests use the same message encoder and exclusive pool.
    automatic.Line = INTERRUPT_NONE;
    automatic.Vectors[0] = 226;
    automatic.Vectors[1] = INTERRUPT_NONE;
    CHECK(InterruptReserve(&automatic, INTERRUPT_KERNEL | INTERRUPT_MSI | INTERRUPT_VECTOR, &second) == OS_EOK);
    CHECK(LOWORD(second) == 226 && automatic.MsiValue == 226);
    CHECK(InterruptUnregister(second) == OS_EOK);
    CHECK(InterruptUnregister(standalone) == OS_EOK);
    g_messageVectors = 0;
    CHECK(InterruptReserve(&automatic, INTERRUPT_KERNEL | INTERRUPT_MSI, &rejected) == OS_ENOTSUPPORTED);
    __CheckEmpty();
    puts("Common reservations: standalone MSI, group contention, encoding, fixed/candidate vectors and reuse passed");
}

static void
__TestLocalReservationBeforeDiscovery(void)
{
    InterruptDescriptor_t descriptor = { .Line = 64, .Pin = INTERRUPT_NONE };
    SystemInterrupt_t* entry;
    uuid_t id;

    CHECK(GetMachine()->InterruptController == NULL);
    CHECK(InterruptReserve(&descriptor, INTERRUPT_KERNEL | INTERRUPT_SOFT, &id) == OS_EOK);
    entry = InterruptGet(id);
    CHECK(entry->Routing.Controller->Type == SystemInterruptControllerLocal);
    InterruptPut(entry);
    CHECK(InterruptUnregister(id) == OS_EOK);
    __CheckEmpty();
    puts("CPU-local reservation before controller discovery passed");
}
