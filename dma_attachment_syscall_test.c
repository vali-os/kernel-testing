/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Keep the real manager, grant table and handle-set machinery in boundary tests.
#define DMA_ATTACHMENT_LOG_TEST
#include "dma_attachment_support.inc"
#include <stdarg.h>

static bool g_allowManager;
static bool g_expectUpdateLock;
static void (*g_afterOutputCopy)(void);
static unsigned int g_outputCopies;
static char g_lastError[512];
static char g_previousError[512];

void
TestDmaError(
    _In_ const char* format,
    ...)
{
    va_list args;
    int     length;

    memcpy(g_previousError, g_lastError, sizeof(g_previousError));
    va_start(args, format);
    length = vsnprintf(g_lastError, sizeof(g_lastError), format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(g_lastError));
}

static void
__ExpectError(
    _In_ const char* prefix)
{
    if (strncmp(g_lastError, prefix, strlen(prefix)) != 0) {
        fprintf(stderr, "Expected diagnostic prefix '%s', received '%s'\n", prefix, g_lastError);
        assert(false);
    }
    g_lastError[0] = '\0';
}

oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    assert(capabilities == CAPABILITY_DEVICEIO_MANAGEMENT);
    return g_allowManager ? OS_EOK : OS_EPERMISSIONS;
}

oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       kernel,
    _In_  const void* user,
    _In_  size_t      length)
{
    assert(g_manager == NULL || !g_manager->Lock.held);
    if (user == NULL) {
        return OS_EINVALPARAMS;
    }
    return __CopyProviderUser((void*)user, kernel, length, false);
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       user,
    _In_  const void* kernel,
    _In_  size_t      length)
{
    oserr_t status;
    void (*hook)(void) = g_afterOutputCopy;

    g_outputCopies++;
    assert(g_manager == NULL || !g_manager->Lock.held);
    if (g_expectUpdateLock) {
        assert(g_manager != NULL && g_manager->UpdateLock.held);
    }
    status = user == NULL ? OS_EINVALPARAMS : __CopyProviderUser(user, (void*)kernel, length, true);
    g_afterOutputCopy = NULL;
    if (hook != NULL) {
        hook();
    }
    return status;
}

#ifndef DMA_ATTACHMENT_API_SOURCE
#define DMA_ATTACHMENT_API_SOURCE "../kernel/api/dma_attachment.c"
#endif
#include DMA_ATTACHMENT_API_SOURCE

static void
__CloseDuringCopy(void)
{
    assert(DestroyHandle(g_manager->ID) == OS_EOK);
}

static void
__ChangeDuringCopy(void)
{
    DMAAttachment_t* attachment = g_manager->Registry.Attachments.head->value;

    __QueueAttachmentEvent(g_manager, attachment->ID, DMA_ATTACHMENT_STOP_REQUIRED);
}

static void
__TestBoundaryCreation(void)
{
    uuid_t handle = 123;
    size_t before = g_allocations;
    int failure;

    g_allowManager = false;
    assert(ScDmaManagerCreate(&handle) == OS_EPERMISSIONS && handle == 123);
    __ExpectError("ScDmaManagerCreate: CapabilityVerify ");
    g_allowManager = true;
    g_failAllocation = 0;
    assert(ScDmaManagerCreate(&handle) == OS_EOOM && handle == 123);
    __ExpectError("ScDmaManagerCreate: DMAManagerPrepareCreate ");
    g_failAllocation = -1;
    for (failure = 0; failure < 2; failure++) {
        g_failDmaReference = failure;
        assert(ScDmaManagerCreate(&handle) == OS_EUNKNOWN && handle == 123);
        __ExpectError("ScDmaManagerCreate: DMAManagerPrepareCreate ");
        assert(strstr(g_previousError, failure == 0 ? "operation reference" : "registry reference") != NULL);
        g_failDmaReference = -1;
        __Drain();
        assert(g_manager == NULL && g_allocations == before);
    }

    g_failCopy = 0;
    assert(ScDmaManagerCreate(&handle) == OS_EINVALPARAMS && handle == 123);
    __ExpectError("ScDmaManagerCreate: MemorySpaceCopyToUser ");
    g_failCopy = -1;
    __Drain();
    assert(g_manager == NULL && g_allocations == before);

    g_afterOutputCopy = __CloseDuringCopy;
    assert(ScDmaManagerCreate(&handle) == OS_ECANCELLED);
    __ExpectError("ScDmaManagerCreate: DMAManagerPublish ");
    __Drain();
    assert(g_manager == NULL && g_allocations == before);
    assert(ScDmaManagerCreate(NULL) == OS_EINVALPARAMS);
    __Drain();
    assert(g_manager == NULL && g_allocations == before);
}

static void
__TestBoundaryRegistration(void)
{
    DMAManager_t* manager = NULL;
    OSDmaAttachmentParameters_t parameters = __Parameters(1, 2, 3);
    OSDmaAttachmentId_t id = { 987 };
    OSDmaAttachmentId_t existing;
    uuid_t handle;
    int failure;

    assert(ScDmaManagerCreate(&handle) == OS_EOK);
    assert(DMAManagerReference(handle, &manager) == OS_EOK);
    parameters.Version = 0;
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_EINVALPARAMS);
    __ExpectError("ScDmaAttachmentRegister: DMAManagerPrepareAttachment ");
    parameters.Version = DMA_ATTACHMENT_VERSION;
    for (failure = 0; failure <= 1; failure++) {
        g_failCopy = failure;
        g_outputCopies = 0;
        assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_EINVALPARAMS);
        __ExpectError(failure == 0 ?
                      "ScDmaAttachmentRegister: MemorySpaceCopyFromUser " :
                      "ScDmaAttachmentRegister: MemorySpaceCopyToUser ");
        assert(g_outputCopies == (unsigned int)failure);
        assert(id.Value == 987 && manager->Registry.Count == 0);
        assert(!manager->UpdateLock.held);
    }
    g_failCopy = -1;
    g_expectUpdateLock = true;
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_EOK);
    g_expectUpdateLock = false;
    assert(manager->Registry.Count == 1);
    existing = id;
    g_failCopy = 1;
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_EINVALPARAMS);
    assert(id.Value == existing.Value && manager->Registry.Count == 1);
    g_failCopy = 0;
    assert(ScDmaAttachmentRemove(handle, &id) == OS_EINVALPARAMS);
    __ExpectError("ScDmaAttachmentRemove: MemorySpaceCopyFromUser ");
    assert(manager->Registry.Count == 1);
    g_failCopy = -1;
    assert(ScDmaAttachmentRemove(handle, &id) == OS_EOK);
    assert(ScDmaAttachmentRemove(handle, &id) == OS_ENOENT);
    __ExpectError("ScDmaAttachmentRemove: DMAManagerRemoveAttachment ");

    g_afterOutputCopy = __CloseDuringCopy;
    g_expectUpdateLock = true;
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_ECANCELLED);
    __ExpectError("ScDmaAttachmentRegister: DMAManagerPublishAttachment ");
    g_expectUpdateLock = false;
    assert(manager->Registry.Count == 0 && g_manager == NULL);
    DMAManagerRelease(&manager);
    __Drain();
}

static void
__TestBoundaryEvents(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* stranger,
    _In_ bool                failCopyOnClose)
{
    DMAManager_t* manager = NULL;
    DMAAttachment_t* attachment;
    OSDmaAttachmentParameters_t parameters = __Parameters(1, 2, 3);
    OSDmaAttachmentId_t id;
    OSDmaAttachmentEvent_t events[32];
    struct ioset_event watch;
    uint32_t count;
    uuid_t handle;
    uuid_t queue;
    int failure;
    oserr_t status;

    assert(ScDmaManagerCreate(&handle) == OS_EOK);
    assert(DMAManagerReference(handle, &manager) == OS_EOK);
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_EOK);
    queue = CreateHandleSet(0);
    watch = (struct ioset_event) { .events = IOSETIN, .data.handle = handle };
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    __QueueAttachmentEvent(manager, id.Value, DMA_ATTACHMENT_IDLE);
    __ExpectWakeup(queue, handle);
    for (failure = 0; failure <= 1; failure++) {
        count = 999;
        g_failCopy = failure;
        g_outputCopies = 0;
        assert(ScDmaManagerReadEvents(handle, events, 32, &count) == OS_EINVALPARAMS);
        __ExpectError(failure == 0 ?
                      "ScDmaManagerReadEvents: MemorySpaceCopyToUser 1 events " :
                      "ScDmaManagerReadEvents: MemorySpaceCopyToUser count ");
        assert(g_outputCopies == (unsigned int)failure + 1);
        assert(count == 999 && manager->PendingEvents.count == 1);
        __ExpectWakeup(queue, handle);
    }
    g_failCopy = -1;
    g_afterOutputCopy = __ChangeDuringCopy;
    assert(ScDmaManagerReadEvents(handle, events, 32, &count) == OS_EOK && count == 1);
    assert(events[0].Reasons == DMA_ATTACHMENT_IDLE && manager->PendingEvents.count == 1);
    __ExpectWakeup(queue, handle);
    assert(ScDmaManagerReadEvents(handle, events, 32, &count) == OS_EOK && count == 1);
    assert(events[0].Reasons == (DMA_ATTACHMENT_IDLE | DMA_ATTACHMENT_STOP_REQUIRED));
    assert(manager->PendingEvents.count == 0);
    assert(ScDmaManagerReadEvents(handle, NULL, UINT32_MAX, &count) == OS_EOK && count == 0);
    assert(ScDmaManagerReadEvents(handle, events, 0, &count) == OS_EINVALPARAMS);
    __ExpectError("ScDmaManagerReadEvents: DMAManagerSnapshotEvents ");

    g_current = stranger;
    assert(ScDmaAttachmentRegister(handle, &parameters, &id) == OS_ENOENT);
    __ExpectError("ScDmaAttachmentRegister: DMAManagerReference ");
    assert(ScDmaManagerReadEvents(handle, events, 32, &count) == OS_ENOENT);
    __ExpectError("ScDmaManagerReadEvents: DMAManagerReference ");
    assert(ScDmaAttachmentRemove(handle, &id) == OS_ENOENT);
    __ExpectError("ScDmaAttachmentRemove: DMAManagerReference ");
    g_current = service;
    __QueueAttachmentEvent(manager, id.Value, DMA_ATTACHMENT_IDLE);
    __ExpectWakeup(queue, handle);
    g_afterOutputCopy = __CloseDuringCopy;
    g_failCopy = failCopyOnClose ? 0 : -1;
    status = ScDmaManagerReadEvents(handle, events, 32, &count);
    assert(status == (failCopyOnClose ? OS_EINVALPARAMS : OS_ECANCELLED));
    __ExpectError("ScDmaManagerReadEvents: DMAManagerFinishEvents ");
    if (failCopyOnClose) {
        assert(strstr(g_previousError, "ScDmaManagerReadEvents: MemorySpaceCopyToUser 1 events ") != NULL);
    } else {
        assert(count == 1);
    }
    g_failCopy = -1;
    assert(manager->State == DMAManagerClosed && manager->PendingEvents.count == 1);
    assert(DestroyHandle(queue) == OS_EOK);

    // Simulate reboot after checking that close preserved the pending record.
    attachment = manager->Registry.Attachments.head->value;
    list_remove(&manager->Registry.Attachments, &attachment->Link);
    list_remove(&manager->PendingEvents, &attachment->Event.Link);
    manager->Registry.Count = 0;
    while (manager->Registry.Groups.head != NULL) {
        DMARecoveryGroup_t* group = manager->Registry.Groups.head->value;

        list_remove(&manager->Registry.Groups, &group->Link);
        DMARecoveryGroupRelease(&group);
    }
    kfree(attachment);
    DMAManagerRelease(&manager);
    __Drain();
}

static MSContextSecurity_t* g_bindingReceiver;
static OSDmaBindingResult_t* g_copiedBinding;

static void
__CloseReceiverDuringCopy(void)
{
    assert(DestroyHandleInContext(g_bindingReceiver, g_copiedBinding->Handle) == OS_EOK);
}

static void
__TestBoundaryBinding(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* receiver)
{
    DMAManager_t* manager;
    DMAAttachment_t* attachment;
    OSDmaAttachmentParameters_t route = __Parameters(301, 1, 2);
    OSDmaBindParameters_t parameters;
    OSDmaBindingResult_t result = { .Handle = 123, .Generation = 456 };
    OSDmaBindingResult_t first;
    uint64_t id;
    uuid_t handle;
    size_t allocations;
    int failure;

    manager = __CreateManager(&handle);
    id = __RegisterAttachment(manager, &route);
    attachment = __FindTestAttachment(manager, id);
    parameters = (OSDmaBindParameters_t) {
        .Attachment = id, .Principal = receiver->GlobalHandleID, .RequestId = 1
    };
    allocations = g_allocations;
    for (failure = 0; failure <= 1; failure++) {
        g_failCopy = failure;
        assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_EINVALPARAMS);
        __ExpectError(failure == 0 ?
                      "ScDmaAttachmentBind: MemorySpaceCopyFromUser " :
                      "ScDmaAttachmentBind: MemorySpaceCopyToUser ");
        g_failCopy = -1;
        __Drain();
        assert(result.Handle == 123 && result.Generation == 456);
        assert(attachment->Binding.Current == NULL && attachment->Binding.RequestId == 0);
        assert(g_allocations == allocations && manager->PendingEvents.count == 0);
        assert(!manager->UpdateLock.held);
    }
    assert(ScDmaAttachmentBind(handle, NULL, &result) == OS_EINVALPARAMS);
    assert(ScDmaAttachmentBind(handle, &parameters, NULL) == OS_EINVALPARAMS);
    __Drain();
    assert(g_allocations == allocations);

    // The complete raw output can be copied before the receiver closes its grant.
    // Publication must then fail, and the same request remains available to retry.
    g_bindingReceiver = receiver;
    g_copiedBinding = &result;
    g_afterOutputCopy = __CloseReceiverDuringCopy;
    g_expectUpdateLock = true;
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_ECANCELLED);
    __ExpectError("ScDmaAttachmentBind: DMAManagerPublishBinding ");
    g_expectUpdateLock = false;
    __Drain();
    assert(attachment->Binding.RequestId == 0 && manager->PendingEvents.count == 0);
    assert(g_allocations == allocations);

    // Subsequent administration requires the manager grant, not another check
    // of the capability used to create it. A receiver cannot use the service ID.
    g_allowManager = false;
    assert(ScDmaAttachmentBind(handle, &parameters, &first) == OS_EOK);
    g_current = receiver;
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_ENOENT);
    __ExpectError("ScDmaAttachmentBind: DMAManagerReference ");
    g_current = service;
    g_allowManager = true;
    result = (OSDmaBindingResult_t) { .Handle = 123, .Generation = 456 };
    g_failCopy = 1;
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_EINVALPARAMS);
    g_failCopy = -1;
    assert(result.Handle == 123 && result.Generation == 456);
    assert(attachment->Binding.Current != NULL && attachment->Binding.Current->State == DMABindingLive);
    assert(__LookupGlobalSafe(first.Handle)->References == 1);
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_EOK);
    assert(result.Handle == first.Handle && result.Generation == first.Generation);
    assert(DestroyHandleInContext(receiver, first.Handle) == OS_EOK);
    __Drain();
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_ECANCELLED);
    __ExpectError("ScDmaAttachmentBind: DMAManagerPrepareBinding ");
    __ConsumeBindingIdle(manager, id, first.Generation);

    // Preserve an earlier copy error when service close happens during that copy.
    parameters.RequestId = 2;
    g_failCopy = 1;
    g_afterOutputCopy = __CloseDuringCopy;
    assert(ScDmaAttachmentBind(handle, &parameters, &result) == OS_EINVALPARAMS);
    g_failCopy = -1;
    __Drain();
    assert(manager->State == DMAManagerClosed && attachment->Binding.Current == NULL);
    assert(attachment->Binding.RequestId == 1 && manager->PendingEvents.count == 0);

    // Test-only reboot cleanup. Production leaves the closed registry in place.
    list_remove(&manager->Registry.Attachments, &attachment->Link);
    manager->Registry.Count = 0;
    while (manager->Registry.Groups.head != NULL) {
        DMARecoveryGroup_t* group = manager->Registry.Groups.head->value;

        list_remove(&manager->Registry.Groups, &group->Link);
        DMARecoveryGroupRelease(&group);
    }
    kfree(attachment);
    DMAManagerRelease(&manager);
    __Drain();
}

int
main(void)
{
    MSContextSecurity_t service;
    MSContextSecurity_t stranger;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&stranger);
    g_current = &service;
    __TestBoundaryCreation();
    __TestBoundaryRegistration();
    __TestBoundaryEvents(&service, &stranger, true);
    __TestBoundaryEvents(&service, &stranger, false);
    __TestBoundaryBinding(&service, &stranger);
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(stranger.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(stranger.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("DMA attachment syscalls: authority, output faults and publication races passed");
    return 0;
}
