#include <ddk/types/interrupt-group.h>
// Production completion, group and syscall paths; only hardware and VM are mocked.
static int g_completionChildHandled;
static unsigned int g_otherCompletionCalls;

static irqstatus_t
__OtherCompletionCallback(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                     context)
{
    (void)functions;
    (void)context;
    CHECK(g_handlerCalls == g_completionHandlerBefore + 1);
    CHECK(g_ackCalls == g_completionParentBefore);
    g_otherCompletionCalls++;
    return IRQSTATUS_HANDLED;
}

static irqstatus_t
__CompletionChild(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                     context)
{
    __CountHandler(functions, context);
    return g_completionChildHandled ? IRQSTATUS_HANDLED : IRQSTATUS_NOT_HANDLED;
}

static irqstatus_t
__CompletionCallback(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                     context)
{
    InterruptResourceTable_t* resources = context;

    // Arbitrary test registers prove that the core invokes provider code;
    // no register layout or trigger encoding belongs in the generic kernel.
    functions->WriteIoSpace(resources->IoResources[0], 24, 0x5a, 4);
    functions->ReadIoSpace(resources->IoResources[0], 28, 4);
    return IRQSTATUS_HANDLED;
}

static void
__TestCompletionBindings(void)
{
    InterruptGroupRequest_t allocation = {
        .Minimum = 1, .Optimal = 1, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t first;
    DeviceInterruptMessage_t firstMessage, secondMessage;
    InterruptGroupResult_t second;
    InterruptBindRequest_t bind = { .Resources.Handler = __CompletionChild };
    InterruptResourceTable_t resources = { .Handler = __CompletionCallback };
    struct InterruptBusVector vectors[2] = { 0 };
    struct InterruptBusAttachRequest provider = {
        .Principal = 2, .Device = 42, .Count = 1, .Vectors = vectors
    };
    uuid_t closeHandle;
    DeviceInterruptMessage_t message;
    SystemInterrupt_t* entry;
    uuid_t completion;
    uuid_t failed;
    uuid_t id;
    unsigned int handled;

    g_identityClone = 1;
    g_messageVectors = 2;
    allocation.ControllerID = g_testMessageDomain;
    provider.ControllerID = g_testMessageDomain;
    g_completionIo = (DeviceIo_t) {
        .Id = 900, .Type = DeviceIoMemoryBased,
        .Access.Memory.Length = 64, .Access.Memory.VirtualBase = 0x1000
    };
    resources.IoResources[0] = &g_completionIo;
    g_currentSpace = 3;
    g_denyInterruptControl = 1;
    CHECK(DeviceInterruptBusHandlerCreate(&failed) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusStatus(0, 2, 42) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusClose(0, 2, 42) == OS_EPERMISSIONS);
    g_denyInterruptControl = 0;
    g_denyDeviceManagement = 1;
    CHECK(DeviceInterruptBusHandlerCreate(&failed) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusStatus(0, 2, 42) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusClose(0, 2, 42) == OS_EPERMISSIONS);
    g_denyDeviceManagement = 0;
    CHECK(DeviceInterruptBusAttach(NULL) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptBusStatus(0, 2, 42) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptBusClose(0, 2, 42) == OS_EINVALPARAMS);
    g_failCopyOut = 1;
    CHECK(DeviceInterruptBusHandlerCreate(&failed) != OS_EOK);
    CHECK(g_busHandlers == NULL);
    g_failCopyOut = 0;
    CHECK(DeviceInterruptBusHandlerCreate(&failed) == OS_EOK);
    g_failIo = g_ioCalls + 1;
    g_failUnmap = 1;
    CHECK(DeviceInterruptBusHandlerBind(failed, &resources) != OS_EOK);
    CHECK(DeviceInterruptBusHandlerRelease(failed) != OS_EOK);
    CHECK(g_liveMappings != 0);
    g_failUnmap = 0;
    g_failIo = 0;
    CHECK(DeviceInterruptBusHandlerRelease(failed) == OS_EOK);
    CHECK(g_liveMappings == 0);
    CHECK(DeviceInterruptBusHandlerCreate(&completion) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(completion, &resources) == OS_EOK);
    CHECK(g_liveIo == 1);
    g_currentSpace = 4;
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EPERMISSIONS);

    g_currentSpace = 2;
    CHECK(ScInterruptAllocate(&allocation, &first) == OS_EOK);
    CHECK(ScInterruptGetMessage(first.Handle, 0, &firstMessage) == OS_EOK);
    id = g_groups->Vectors[0].Id;
    provider.Allocation = first.Handle;
    g_currentSpace = 3;
    provider.Allocation++;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_ENOENT);
    provider.Allocation--;
    provider.Principal = 9;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EPERMISSIONS);
    provider.Principal = 2;
    provider.Count = 2;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EINVALPARAMS);
    provider.Count = 1;
    provider.ControllerID++;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EINVALPARAMS);
    provider.ControllerID--;
    vectors[0].Completion = completion;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EOK);
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EOK);
    g_currentSpace = 4;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusStatus(first.Handle, 2, 42) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusClose(first.Handle, 2, 42) == OS_EPERMISSIONS);
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusStatus(first.Handle, 9, 42) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusClose(first.Handle, 9, 42) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusStatus(first.Handle, 2, 43) == OS_EPERMISSIONS);
    CHECK(DeviceInterruptBusClose(first.Handle, 2, 43) == OS_EPERMISSIONS);
    provider.Device++;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EPERMISSIONS);
    provider.Device--;
    g_failCopyOut = 1;
    CHECK(DeviceInterruptBusAttach(&provider) != OS_EOK);
    g_failCopyOut = 0;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EOK);
    CHECK(vectors[0].Message.Data == firstMessage.Data);
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EBUSY);
    closeHandle = first.Handle;

    g_currentSpace = 2;
    CHECK(ScInterruptBind(first.Handle, &bind) == OS_EOK);
    CHECK(ScInterruptRelease(first.Handle) == OS_EBUSY);
    CHECK(ScInterruptEnable(first.Handle) == OS_EOK);
    for (handled = 0; handled < 2; handled++) {
        g_completionChildHandled = handled;
        g_completionAcks = g_completionReads = 0;
        g_completionHandlerBefore = g_handlerCalls;
        g_completionParentBefore = g_ackCalls;
        InterruptHandle(NULL, firstMessage.Data);
        CHECK(g_completionAcks == 1 && g_completionReads == 1);
        CHECK(g_ackCalls == g_completionParentBefore + 1);
    }
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusClose(closeHandle, 2, 42) == OS_EBUSY);
    g_currentSpace = 2;
    InterruptReadEnter();
    g_holdReader = 1;
    CHECK(ScInterruptDisable(first.Handle) == OS_EOK);
    CHECK(!g_holdReader && g_liveIo == 1);
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusClose(closeHandle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusClose(closeHandle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusAttach(&provider) == OS_ENOENT);

    // Closing a newer allocation must not erase the older one's closed state.
    g_currentSpace = 2;
    CHECK(ScInterruptAllocate(&allocation, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    g_currentSpace = 3;
    closeHandle = second.Handle;
    CHECK(DeviceInterruptBusClose(closeHandle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusAttach(&provider) == OS_ENOENT);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    CHECK(ScInterruptRelease(first.Handle) == OS_EOK);
    CHECK(InterruptUnregister(id) == OS_EOK);
    g_currentSpace = 3;
    g_failUnmap = 1;
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EUNKNOWN);
    g_failUnmap = 0;
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EOK);
    CHECK(g_busHandlers == NULL);

    // Process identity remains stable across native threads. A failed reap
    // must leave that original identity intact so another reaper can retry.
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusHandlerCreate(&completion) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(completion, &resources) == OS_EOK);
    g_currentSpace = 2;
    CHECK(ScInterruptAllocate(&allocation, &first) == OS_EOK);
    CHECK(ScInterruptGetMessage(first.Handle, 0, &firstMessage) == OS_EOK);
    id = g_groups->Vectors[0].Id;
    CHECK(ScInterruptAllocate(&allocation, &second) == OS_EOK);
    CHECK(ScInterruptGetMessage(second.Handle, 0, &secondMessage) == OS_EOK);
    g_currentSpace = 3;
    provider.Allocation = first.Handle;
    vectors[0].Completion = completion;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EOK);
    provider.Allocation = second.Handle;
    CHECK(DeviceInterruptBusAttach(&provider) == OS_EOK);
    g_currentSpace = 2;
    CHECK(ScInterruptBind(first.Handle, &bind) == OS_EOK);
    CHECK(ScInterruptEnable(first.Handle) == OS_EOK);
    g_processPrincipal = 2;
    g_currentSpace = 9;
    CHECK(ScInterruptGetMessage(first.Handle, 0, &message) == OS_EOK);
    CHECK(DeviceInterruptReap(first.Handle) == OS_EBUSY);
    g_processPrincipal = 0;
    g_currentSpace = 3;
    g_deadSpace = 2;
    g_failUnmap = 1;
    CHECK(DeviceInterruptReap(first.Handle) == OS_EUNKNOWN);
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EBUSY);
    CHECK(g_liveIo == 1);
    entry = InterruptGet(id);
    CHECK(entry != NULL && entry->Owner == 2 && !atomic_load(&entry->Enabled));
    InterruptPut(entry);
    g_failUnmap = 0;
    g_currentSpace = 4;
    CHECK(DeviceInterruptReap(first.Handle) == OS_EOK);
    CHECK(DeviceInterruptReap(first.Handle) == OS_ENOENT);
    g_currentSpace = 3;
    // Reaping one group cannot release a callback still attached to another.
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EBUSY);
    closeHandle = second.Handle;
    CHECK(DeviceInterruptBusClose(closeHandle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerRelease(completion) == OS_EOK);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(second.Handle) == OS_EOK);
    CHECK(InterruptUnregister(id) == OS_EOK);
    g_deadSpace = 0;
    g_identityClone = 0;
    g_messageVectors = 0;
    __CheckEmpty();
    puts("Provider completions: callback/EOI order, ownership, replay, mapping failure and cross-thread reap passed");
}

static void
__TestGroupAttachment(void)
{
    InterruptGroupRequest_t allocation = {
        .Minimum = 3, .Optimal = 3, .Kind = InterruptSourceMessage
    };
    InterruptGroupResult_t result;
    InterruptBindRequest_t bind = { .Resources.Handler = __CompletionChild };
    InterruptResourceTable_t resources = { .Handler = __CompletionCallback };
    struct InterruptBusVector vectors[3] = { 0 };
    struct InterruptBusAttachRequest attach = {
        .Principal = 2, .Device = 42,
        .Count = 3, .Vectors = vectors
    };
    DeviceInterruptMessage_t message;
    uuid_t completions[2];
    uuid_t ids[3];
    uuid_t foreign;
    uuid_t unready;
    uint32_t i;
    unsigned int cycle;

    g_identityClone = 1;
    g_messageVectors = 3;
    allocation.ControllerID = g_testMessageDomain;
    attach.ControllerID = g_testMessageDomain;
    resources.IoResources[0] = &g_completionIo;
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusHandlerCreate(&completions[0]) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(completions[0], &resources) == OS_EOK);
    resources.Handler = __OtherCompletionCallback;
    CHECK(DeviceInterruptBusHandlerCreate(&completions[1]) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(completions[1], &resources) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerCreate(&unready) == OS_EOK);
    g_currentSpace = 4;
    CHECK(DeviceInterruptBusHandlerCreate(&foreign) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(foreign, &resources) == OS_EOK);

    g_currentSpace = 2;
    CHECK(ScInterruptAllocate(&allocation, &result) == OS_EOK);
    attach.Allocation = result.Handle;
    g_currentSpace = 3;
    vectors[0].Completion = completions[0];
    vectors[2].Completion = completions[1];
    attach.Count = 0;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EINVALPARAMS);
    attach.Count = INTERRUPT_BUS_MAX_VECTORS + 1;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EINVALPARAMS);
    attach.Count = 3;
    attach.Vectors = NULL;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EINVALPARAMS);
    attach.Vectors = vectors;
    g_failAllocation = 1;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EOOM);
    g_failAllocation = 0;
    g_failCopyAddress = vectors;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EINVALPARAMS);
    g_failCopyAddress = NULL;

    // Failure at the last entry must undo the first reference and the claim,
    // including when the middle vector intentionally has no completion.
    vectors[2].Completion = UINT32_MAX;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_ENOENT);
    vectors[2].Completion = foreign;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EPERMISSIONS);
    vectors[2].Completion = unready;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EBUSY);
    CHECK(DeviceInterruptBusHandlerRelease(completions[0]) == OS_EOK);
    resources.Handler = __CompletionCallback;
    CHECK(DeviceInterruptBusHandlerCreate(&completions[0]) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerBind(completions[0], &resources) == OS_EOK);
    vectors[0].Completion = completions[0];
    vectors[2].Completion = completions[1];
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);
    CHECK(ScInterruptAllocate(&allocation, &result) == OS_EOK);
    attach.Allocation = result.Handle;
    for (i = 0; i < 3; i++) {
        ids[i] = g_groups->Vectors[i].Id;
    }

    // Attach must not change a group which already has a child binding.
    CHECK(ScInterruptBind(result.Handle, &bind) == OS_EOK);
    g_currentSpace = 3;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EBUSY);
    g_currentSpace = 2;
    CHECK(ScInterruptDisable(result.Handle) == OS_EOK);
    g_currentSpace = 3;
    g_failCopyOut = 1;
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    CHECK(DeviceInterruptBusHandlerRelease(completions[0]) == OS_EBUSY);
    CHECK(DeviceInterruptBusHandlerRelease(completions[1]) == OS_EBUSY);
    for (i = 0; i < 3; i++) {
        vectors[i].Message = (DeviceInterruptMessage_t) { 123, 456, 789 };
    }
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EOK);
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EOK);
    CHECK(DeviceInterruptBusStatus(result.Handle, 2, 42) == OS_EBUSY);
    vectors[1].Completion = completions[0];
    CHECK(DeviceInterruptBusAttach(&attach) == OS_EBUSY);
    vectors[1].Completion = UUID_INVALID;

    for (cycle = 0; cycle < 2; cycle++) {
        g_currentSpace = 2;
        for (i = 0; i < 3; i++) {
            CHECK(ScInterruptGetMessage(result.Handle, i, &message) == OS_EOK);
            CHECK(memcmp(&message, &vectors[i].Message, sizeof(message)) == 0);
            bind.Index = i;
            CHECK(ScInterruptBind(result.Handle, &bind) == OS_EOK);
        }
        CHECK(ScInterruptEnable(result.Handle) == OS_EOK);
        g_currentSpace = 3;
        CHECK(DeviceInterruptBusAttach(&attach) == OS_EOK);
        CHECK(DeviceInterruptBusStatus(result.Handle, 2, 42) == OS_EOK);
        for (i = 0; i < 3; i++) {
            g_completionChildHandled = cycle;
            g_completionAcks = 0;
            g_completionReads = 0;
            g_otherCompletionCalls = 0;
            g_completionHandlerBefore = g_handlerCalls;
            g_completionParentBefore = g_ackCalls;
            InterruptHandle(NULL, vectors[i].Message.Data);
            CHECK(g_handlerCalls == g_completionHandlerBefore + 1);
            CHECK(g_ackCalls == g_completionParentBefore + 1);
            CHECK(g_completionAcks == (i == 0 ? 1U : 0U));
            CHECK(g_otherCompletionCalls == (i == 2 ? 1U : 0U));
        }
        g_currentSpace = 2;
        CHECK(ScInterruptDisable(result.Handle) == OS_EOK);
    }

    g_currentSpace = 3;
    CHECK(DeviceInterruptBusClose(result.Handle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusClose(result.Handle, 2, 42) == OS_EOK);
    CHECK(DeviceInterruptBusAttach(&attach) == OS_ENOENT);
    CHECK(DeviceInterruptBusHandlerRelease(completions[0]) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerRelease(completions[1]) == OS_EOK);
    CHECK(DeviceInterruptBusHandlerRelease(unready) == OS_EOK);
    g_currentSpace = 4;
    CHECK(DeviceInterruptBusHandlerRelease(foreign) == OS_EOK);
    g_currentSpace = 2;
    CHECK(ScInterruptRelease(result.Handle) == OS_EOK);
    for (i = 0; i < 3; i++) {
        CHECK(InterruptUnregister(ids[i]) == OS_EOK);
    }
    g_identityClone = 0;
    g_messageVectors = 0;
    __CheckEmpty();
    puts("Group attachment: atomic rollback, copied arrays, retries, per-vector completion selection and rebind passed");
}
