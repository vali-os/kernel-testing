#include <ddk/types/interrupt-source.h>
#include <ddk/types/interrupt-group.h>
#include <ddk/types/interrupt-controller.h>
#include <ddk/interrupts/provider.h>
// Run the real DDK, syscall copy, group and provider-attachment paths together.
static int g_nativeTestActive;
static int g_nativeWrongLine;
static int g_nativeWrongFlags;

static oserr_t __NativeTestIoctl(uuid_t device, enum OSIOCtlRequest operation, void* buffer, size_t length)
{
    struct DeviceInterruptSourceQuery* support = buffer;
    const struct DeviceInterruptSourceRequest* request = buffer;
    struct InterruptProviderVector vector = { 0 };
    struct InterruptProviderAttachRequest attach = {
        .Principal = 2, .Device = device, .ControllerID = 77,
        .Count = 1, .Vectors = &vector
    };
    oserr_t status;
    CHECK(g_nativeTestActive && device == 42 && g_currentSpace == 2);
    if (operation == OSIOCTLREQUEST_INTERRUPT_QUERY) {
        CHECK(length == sizeof(*support));
        CHECK(support->Index < 2);
        support->Kind = InterruptSourceFixedLine;
        support->ControllerID = 77;
        support->Line = (DeviceInterruptLine_t) {
            64 + support->Index, INTERRUPT_NONE, INTERRUPT_ACPICONFORM_PRESENT | INTERRUPT_ACPICONFORM_FIXED
        };
        return OS_EOK;
    }
    CHECK(operation == OSIOCTLREQUEST_INTERRUPT_CONTROL && length == sizeof(*request));
    g_currentSpace = 3;
    if (request->Operation == DeviceInterruptSourceReserve) {
        attach.Allocation = request->Allocation;
        attach.Line = 64 + request->Index + g_nativeWrongLine;
        attach.LineFlags = INTERRUPT_ACPICONFORM_PRESENT | INTERRUPT_ACPICONFORM_FIXED;
        if (g_nativeWrongFlags) {
            attach.LineFlags |= INTERRUPT_ACPICONFORM_TRIGGERMODE;
        }
        status = DeviceInterruptProviderAttach(&attach);
    } else if (request->Operation == DeviceInterruptSourceEnable) {
        status = DeviceInterruptProviderCheckEnabled(request->Allocation, 2, device);
    } else if (request->Operation == DeviceInterruptSourceRelease) {
        status = DeviceInterruptProviderClose(request->Allocation, 2, device);
    } else {
        CHECK(request->Operation == DeviceInterruptSourceDisable);
        status = OS_EOK;
    }
    g_currentSpace = 2;
    return status;
}

static void __TestNativeInterrupts(void)
{
    InterruptLineSourceIdentity_t identity = {
        .Version = 1,
        .FirmwareController = 77, .Line = 65
    };
    InterruptGroupRequest_t request = {
        .Line = { 65, INTERRUPT_NONE, INTERRUPT_ACPICONFORM_FIXED },
        .Minimum = 1, .Optimal = 1, .Kind = InterruptSourceFixedLine,
        .ControllerID = 78
    };
    InterruptGroupResult_t result;
    Device_t device = { .Id = 42, .Length = sizeof(Device_t) };
    DeviceInterrupt_t interrupt;
    uuid_t id = 0xdead;
    uint32_t count;
    unsigned int calls;

    CHECK(CreateInterruptController(77, 64, 8, 0x123000) == OS_EOK);
    g_registryAllocations++;
    // The controller registry can include CPU-local and reserved GIC IDs.
    // Owning such a number must not make it a valid fixed device source.
    CHECK(CreateInterruptController(79, 16, 16, 0x124000) == OS_EOK);
    CHECK(CreateInterruptController(80, 1020, 4, 0x125000) == OS_EOK);
    g_registryAllocations += 2;
    request.ControllerID = 79;
    request.Line.Line = 31;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    request.ControllerID = 80;
    request.Line.Line = 1020;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    request.ControllerID = 78;
    request.Line.Line = 65;
    // The new resolver keeps the same copied-input, privilege and read-only
    // output guarantees as message resolution, without overloading Reserved.
    g_denyInterruptControl = 1;
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_EPERMISSIONS);
    g_denyInterruptControl = 0;
    g_failCopy = 1;
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_EINVALPARAMS);
    g_failCopy = 0;
    identity.Reserved = 65;
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_EINVALPARAMS);
    identity.Reserved = 0;
    CHECK(DeviceInterruptLineSourceResolve(NULL, &id) == OS_EINVALPARAMS);
    CHECK(DeviceInterruptLineSourceResolve(&identity, NULL) == OS_EINVALPARAMS);
    CHECK(id == 0xdead);
#if defined(__aarch64__)
    g_failCopyOut = 1;
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_EINVALPARAMS);
    g_failCopyOut = 0;
    CHECK(id == 0xdead);
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_EOK && id == 77);
    identity.Line = 72;
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_ENOTSUPPORTED);
    CHECK(ScInterruptAllocate(&request, &result) == OS_ENOTSUPPORTED);
    request.ControllerID = 77;
    request.Line.AcpiConform = 0;
    CHECK(ScInterruptAllocate(&request, &result) == OS_EINVALPARAMS);
    g_nativeTestActive = 1;
    g_identityClone = 1;
    g_testResolveIndex = 65;
    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(DeviceInterruptSelectSource(&interrupt, 1) == OS_EOK);
    g_nativeWrongLine = 1;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EINVALPARAMS);
    CHECK(interrupt.State == NULL);
    g_nativeWrongLine = 0;
    g_nativeWrongFlags = 1;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EINVALPARAMS);
    CHECK(interrupt.State == NULL);
    g_nativeWrongFlags = 0;
    CHECK(DeviceInterruptAllocate(&interrupt, 1, 1, &count) == OS_EOK && count == 1);
    CHECK(DeviceInterruptSelectSource(&interrupt, 0) == OS_EBUSY);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, __CountHandler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    calls = g_handlerCalls;
    InterruptHandle(NULL, 65);
    CHECK(g_handlerCalls == calls + 1);
    CHECK(DeviceInterruptDestroy(&interrupt) == OS_EOK && interrupt.State == NULL);
    g_nativeTestActive = g_identityClone = 0;
    g_testResolveIndex = g_testIndex;
#else
    CHECK(DeviceInterruptLineSourceResolve(&identity, &id) == OS_ENOTSUPPORTED && id == 0xdead);
    // Bypassing firmware resolution must not turn a fixed source into an x86
    // route that can subsequently be remapped by an ACPI interrupt override.
    request.ControllerID = 77;
    CHECK(ScInterruptAllocate(&request, &result) == OS_ENOTSUPPORTED);
#endif
    __CheckEmpty();
    puts("Native IRQs: explicit GIC identity, source selection, provider attachment and DDK lifecycle passed");
}
