/**
 * Copyright 2026, Philip Meulengracht
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
 *
 *
 * Exercise deviced registration, binding and recovery against the native kernel.
 */

// Use the real kernel manager, grants, grouping and binding implementation.
// Only scheduler, hardware and the syscall transport are replaced here.
#include "dma_attachment_support.inc"

oserr_t CapabilityVerify(Capabilities_t capability)
{ (void)capability; return OS_EOK; }
oserr_t MemorySpaceCopyFromUser(void* to, const void* from, size_t size)
{ return __CopyProviderUser((void*)from, to, size, false); }
oserr_t MemorySpaceCopyToUser(void* to, const void* from, size_t size)
{ return __CopyProviderUser(to, (void*)from, size, true); }
#include "../kernel/api/dma_attachment.c"

#define Syscall_DmaManagerCreate ScDmaManagerCreate
#define Syscall_DmaAttachmentRegister ScDmaAttachmentRegister
#define Syscall_DmaManagerReadEvents ScDmaManagerReadEvents
#define Syscall_DmaAttachmentRemove ScDmaAttachmentRemove
#define Syscall_DmaAttachmentBind ScDmaAttachmentBind
#define Syscall_DmaAttachmentPrepareStop ScDmaAttachmentPrepareStop
static oserr_t __TestCompleteStop(uuid_t manager, const OSDmaStopTicket_t* ticket);
#define Syscall_DmaAttachmentCompleteStop __TestCompleteStop
#include "../librt/libddk/dma/attachment.c"

#ifndef DMA_SERVICE_SOURCE
#define DMA_SERVICE_SOURCE "../services/deviced/core/dma.c"
#endif
#include DMA_SERVICE_SOURCE
#include "../services/deviced/core/dma-device.c"
#ifndef DMA_SERVICE_RECOVERY_SOURCE
#define DMA_SERVICE_RECOVERY_SOURCE "../services/deviced/core/dma-recovery.c"
#endif
#include DMA_SERVICE_RECOVERY_SOURCE

#include <sys_device_service_server.h>
static int __ReplyBinding(struct gracht_message* message, oserr_t result, uuid_t binding, uint64_t generation);
#define sys_device_dma_bind_response __ReplyBinding
#include "../services/deviced/core/requests.c"
#undef sys_device_dma_bind_response
static bool g_failBindingReply;
static oserr_t g_bindingReplyStatus;
static OSDmaBindingResult_t g_bindingReply;
static int __ReplyBinding(struct gracht_message* message, oserr_t result, uuid_t binding, uint64_t generation)
{
    (void)message;
    g_bindingReplyStatus = result;
    g_bindingReply = (OSDmaBindingResult_t) { .Handle = binding, .Generation = generation };
    return g_failBindingReply ? -1 : 0;
}

struct usched_mtx g_dmDevicesLock;
static DmDevice_t g_testDevices[4];
static Device_t g_testDescriptions[4];
static DmDmaDescription_t g_testRoutes[4];
static unsigned int g_prepares;
static unsigned int g_releases;
static bool g_subscription;
static unsigned int g_describes;
static oserr_t g_queueCreateStatus;
static oserr_t g_queueSubscribeStatus;
static unsigned int g_queueReleases;
static bool g_failRecoveryJob;
static unsigned int g_stopCalls;
static unsigned int g_completeCalls;
static oserr_t g_stopStatus;
static oserr_t g_completeStatus;
static OSTimestamp_t g_testTime = { .Seconds = 100 };
static unsigned int g_sleepCalls;
int usched_job_sleep(const struct timespec* until)
{
    assert(g_dmDmaLock.references == 0 && until->tv_sec > g_testTime.Seconds);
    g_testTime.Seconds = until->tv_sec;
    g_testTime.Nanoseconds = until->tv_nsec;
    return ++g_sleepCalls == 2 ? -1 : 0;
}

uuid_t usched_job_queue(usched_task_fn entry, void* argument)
{
    (void)argument;
    assert(entry == __DmaRecoveryJob && g_subscription);
    assert(g_dmDmaLock.references == 1);
    return g_failRecoveryJob ? UUID_INVALID : 91;
}
void usched_job_yield(void) {}
uuid_t usched_notification_handle(void) { return UUID_INVALID; }
oserr_t OSGetTime(enum OSTimeSource source, OSTimestamp_t* time)
{ assert(source == OSTimeSource_UTC); *time = g_testTime; return OS_EOK; }
oserr_t OSNotificationQueueWait(OSHandle_t* queue, struct ioset_event* events, int maxEvents,
                               int pollEvents, OSTimestamp_t* deadline, int* count, OSAsyncContext_t* context)
{
    (void)queue; (void)events; (void)maxEvents; (void)pollEvents; (void)deadline; (void)count; (void)context;
    assert(g_dmDmaLock.references == 0);
    return OS_EUNKNOWN;
}
static oserr_t __TestCompleteStop(uuid_t manager, const OSDmaStopTicket_t* ticket)
{
    assert(g_dmDmaLock.references == 1 && g_stopCalls != 0 && g_stopStatus == OS_EOK);
    g_completeCalls++;
    if (g_completeStatus != OS_EOK) return g_completeStatus;
    return ScDmaAttachmentCompleteStop(manager, ticket);
}

void usched_mtx_init(struct usched_mtx* mutex, int type)
{ memset(mutex, 0, sizeof(*mutex)); mutex->type = type; }
void usched_mtx_lock(struct usched_mtx* mutex)
{ assert(mutex->references == 0); mutex->references = 1; }
void usched_mtx_unlock(struct usched_mtx* mutex)
{ assert(mutex->references == 1); mutex->references = 0; }
oserr_t Syscall_HandleDestroy(uuid_t handle)
{ return DestroyHandle(handle); }

// Queue transport is inspected separately from kernel event contents. Native
// manager tests already exercise handle-set subscription and notification.
oserr_t OSNotificationQueueCreate(unsigned int flags, OSHandle_t* queue)
{ (void)flags; if (g_queueCreateStatus != OS_EOK) return g_queueCreateStatus; queue->ID = 90; return OS_EOK; }
oserr_t OSNotificationQueueCtrl(OSHandle_t* queue, int operation, OSHandle_t* handle, struct ioset_event* event)
{
    assert(queue->ID == 90 && operation == IOSET_ADD && event->events == IOSETIN);
    assert(handle->ID == g_dmDmaState.Manager && g_dmDmaState.Devices == NULL);
    if (g_queueSubscribeStatus != OS_EOK) return g_queueSubscribeStatus;
    g_subscription = true;
    return OS_EOK;
}
oserr_t OSHandleRelease(OSHandle_t* handle)
{ g_queueReleases++; handle->ID = UUID_INVALID; return OS_EOK; }

DmDevice_t* DmDeviceFindLocked(uuid_t id)
{
    assert(g_dmDevicesLock.references == 1);
    return id < 4 && g_testDevices[id].Device != NULL ? &g_testDevices[id] : NULL;
}
oserr_t DmDeviceRequestAcquire(uuid_t id, DmDevice_t** output)
{
    DmDevice_t* device;
    usched_mtx_lock(&g_dmDevicesLock);
    device = DmDeviceFindLocked(id);
    if (device == NULL || device->State == DmDeviceRemoving) {
        usched_mtx_unlock(&g_dmDevicesLock);
        return OS_EBUSY;
    }
    device->ActiveRequests++;
    *output = device;
    usched_mtx_unlock(&g_dmDevicesLock);
    return OS_EOK;
}
void DmDeviceRequestRelease(DmDevice_t* device)
{ assert(device->ActiveRequests != 0); device->ActiveRequests--; }

static oserr_t __Describe(void* context, DmDmaDescription_t* description)
{
    unsigned int index = (unsigned int)(uintptr_t)context;
    assert(g_dmDevicesLock.references == 0 && g_testDevices[index].ActiveRequests == 1);
    g_describes++;
    *description = g_testRoutes[index];
    return OS_EOK;
}
static oserr_t __Prepare(void* context, uuid_t principal, DmDmaDescription_t* description)
{
    assert(principal != UUID_INVALID);
    g_prepares++;
    return __Describe(context, description);
}
static void __Release(void* context)
{ (void)context; g_releases++; }
static const DmDeviceBusOperations_t g_testOperations = {
    .DescribeDma = __Describe, .PrepareDma = __Prepare, .ReleaseDma = __Release
};

static void __TestStartupFailures(void)
{
    unsigned int releases = g_queueReleases;

    g_failAllocation = 0;
    assert(DmDmaInitialize() == OS_EOOM && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    g_failAllocation = -1;
    assert(DmDmaInitialize() == OS_ECANCELLED);
    __Drain();
    assert(g_manager == NULL);
    memset(&g_dmDmaState, 0, sizeof(g_dmDmaState));

    g_queueCreateStatus = OS_EOOM;
    assert(DmDmaInitialize() == OS_EOOM && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    assert(g_dmDmaState.Manager == UUID_INVALID && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    assert(DmDmaInitialize() == OS_ECANCELLED && g_queueReleases == releases);
    __Drain();
    assert(g_manager == NULL);
    memset(&g_dmDmaState, 0, sizeof(g_dmDmaState));
    g_queueCreateStatus = OS_EOK;

    g_queueSubscribeStatus = OS_EUNKNOWN;
    assert(DmDmaInitialize() == OS_EUNKNOWN && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    assert(g_dmDmaState.Manager == UUID_INVALID && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    assert(DmDmaInitialize() == OS_ECANCELLED && g_queueReleases == releases + 1);
    __Drain();
    assert(g_manager == NULL);
    memset(&g_dmDmaState, 0, sizeof(g_dmDmaState));
    g_queueSubscribeStatus = OS_EOK;
    g_failRecoveryJob = true;
    assert(DmDmaInitialize() == OS_EOOM && g_dmDmaState.ManagerState == DmDmaManagerUnavailable);
    assert(g_queueReleases == releases + 2);
    assert(DmDmaInitialize() == OS_ECANCELLED);
    __Drain();
    assert(g_manager == NULL);
    memset(&g_dmDmaState, 0, sizeof(g_dmDmaState));
    g_failRecoveryJob = false;
}

static void __TestRegistration(void)
{
    OSDmaAttachmentId_t first;
    DmDmaAttachment_t* entry;
    DMAAttachment_t* native;
    unsigned int i;
    unsigned int describes;

    for (i = 1; i < 4; i++) {
        g_testDescriptions[i].Id = i;
        g_testDevices[i].Device = &g_testDescriptions[i];
        g_testDevices[i].Bus.Operations = &g_testOperations;
        g_testDevices[i].Bus.Context = (void*)(uintptr_t)i;
        g_testRoutes[i] = (DmDmaDescription_t) {
            .HostId = 1, .AddressLimit = 0x2fff, .CachePolicy = DmDmaCacheNonCoherent,
            .Map = { .Count = 1, .Ranges = {{ .PhysicalBase = 0x1000, .DeviceBase = 0x2000, .Length = 0x1000 }} }
        };
    }
    g_testDevices[1].Bus.Operations = NULL;
    assert(DmDmaInitialize() == OS_EOK && g_subscription);
    assert(DmDmaRegisterDevice(1) == OS_EOK);
    assert(__FindDmaDevice(1)->RouteState == DmDmaRouteUnavailable && g_manager->Registry.Attachments.count == 0);
    assert(DmDmaRegisterDevice(2) == OS_EOK);
    entry = __FindDmaDevice(2);
    first = entry->Id;
    native = __FindTestAttachment(g_manager, first.Value);
    assert(native != NULL && native->Configuration.AddressLimit == 0x2fff);
    assert(DmDmaRegisterDevice(2) == OS_EOK && entry->Id.Value == first.Value);
    assert(g_manager->Registry.Attachments.count == 1 && g_prepares == 0 && g_releases == 0);
    g_failAllocation = 0;
    assert(DmDmaRegisterDevice(3) == OS_EOOM);
    assert(__FindDmaDevice(3)->Id.Value == 0);
    assert(g_manager->Registry.Attachments.count == 1);
    g_failAllocation = -1;
    describes = g_describes;
    g_testRoutes[3].AddressLimit = UINT64_MAX;
    g_testRoutes[3].Map.Ranges[0].DeviceBase = 0x3000;
    // Failed output copying must also preserve the saved registration input.
    g_failCopy = 1;
    assert(DmDmaRegisterDevice(3) == OS_EINVALPARAMS);
    assert(__FindDmaDevice(3)->Id.Value == 0);
    g_failCopy = -1;
    assert(DmDmaRegisterDevice(3) == OS_EOK);
    assert(g_manager->Registry.Attachments.count == 2 && g_describes == describes);
    assert(__FindTestAttachment(g_manager, __FindDmaDevice(3)->Id.Value)->Configuration.AddressLimit == 0x2fff);
    assert(__FindTestAttachment(g_manager, __FindDmaDevice(3)->Id.Value)->Configuration.Ranges[0].DeviceBase == 0x2000);
    assert(__FindTestAttachment(g_manager, __FindDmaDevice(3)->Id.Value)->RecoveryGroup == native->RecoveryGroup);
    assert(DmDmaRemoveDevice(2) == OS_EBUSY);
    g_testDevices[2].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(2) == OS_EOK);
    assert(__FindDmaDevice(2) == NULL && g_dmDmaState.Hosts->Devices == 1);
    g_testDevices[3].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(3) == OS_EOK && g_dmDmaState.Hosts == NULL);
    // Invalid and failed-copy registrations can be removed without trying to
    // register again. Keep the fault enabled to catch a hidden registration call.
    g_testDevices[2].State = DmDeviceNoDriver;
    g_testRoutes[2].Map.Count = 0;
    assert(DmDmaRegisterDevice(2) == OS_EINVALPARAMS);
    g_testDevices[2].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(2) == OS_EOK);
    g_testDevices[2].State = DmDeviceNoDriver;
    g_testRoutes[2].Map.Count = 1;
    g_failCopy = 1;
    assert(DmDmaRegisterDevice(2) == OS_EINVALPARAMS);
    g_testDevices[2].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(2) == OS_EOK);
    g_failCopy = -1;
    g_testDevices[1].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(1) == OS_EOK && g_dmDmaState.Devices == NULL);
}

static void __TestBinding(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    OSDmaBindingResult_t first = { 0 };
    OSDmaBindingResult_t result = { 0 };
    DmDmaAttachment_t* entry;
    uint64_t principal = driver->GlobalHandleID;
    struct gracht_message message = { 0 };
    unsigned int prepares = g_prepares;
    unsigned int releases = g_releases;

    g_testDevices[1].State = DmDeviceNoDriver;
    g_testDevices[2].State = DmDeviceNoDriver;
    g_testDevices[2].DriverId = 99;
    g_testDevices[2].DriverPrincipal = principal;
    assert(DmDmaRegisterDevice(2) == OS_EOK);
    entry = __FindDmaDevice(2);
    assert(DmDmaBind(principal, 2, 0, &result) == OS_EINVALPARAMS);
    assert(DmDmaBind(principal, 2, UINT64_MAX, &result) == OS_EOVERFLOW);
    assert(DmDmaBind(principal + (1ULL << 32), 2, 1, &result) == OS_EPERMISSIONS);
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaCheckControlLocked(2, principal + (1ULL << 32), false) == OS_EPERMISSIONS);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(g_prepares == prepares && entry->Binding.RequestId == 0);
    // An assigned full-width identity must fail the narrow bus boundary, never
    // silently operate as the driver whose low 32 bits happen to match.
    g_testDevices[2].DriverPrincipal = principal + (1ULL << 32);
    assert(DmDmaBind(principal + (1ULL << 32), 2, 1, &result) == OS_EOVERFLOW);
    assert(g_prepares == prepares);
    g_testDevices[2].DriverPrincipal = principal;

    g_failAllocation = 0;
    assert(DmDmaBind(principal, 2, 1, &result) == OS_EOOM);
    g_failAllocation = -1;
    assert(entry->Binding.Lease == NULL && entry->Binding.RequestId == 0);
    assert(g_prepares == prepares + 1 && g_releases == releases + 1);
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaCheckControlLocked(2, principal, true) == OS_ECANCELLED);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(DmDmaBeginHostStart(1) == OS_EOK && g_dmDmaLock.references == 1);
    DmDmaEndHostStart();
    message.security.principal = principal;
    sys_device_dma_bind_invocation(&message, 2, 1);
    assert(g_bindingReplyStatus == OS_EPERMISSIONS && g_bindingReply.Handle == UUID_INVALID);
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal += 1ULL << 32;
    sys_device_dma_bind_invocation(&message, 2, 1);
    assert(g_bindingReplyStatus == OS_EPERMISSIONS && g_prepares == prepares + 1);
    message.security.principal = principal;
    g_failBindingReply = true;
    sys_device_dma_bind_invocation(&message, 2, 1);
    assert(g_bindingReplyStatus == OS_EOK && entry->Binding.Lease != NULL);
    first = g_bindingReply;
    g_failBindingReply = false;
    sys_device_dma_bind_invocation(&message, 2, 1);
    result = g_bindingReply;
    assert(g_bindingReplyStatus == OS_EOK);
    assert(first.Handle == result.Handle && first.Generation == result.Generation);
    assert(g_prepares == prepares + 2 && g_testDevices[2].DmaLeases == 1);
    assert(DmDmaBind(principal, 2, 2, &result) == OS_EBUSY);
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaCheckAssignmentLocked(2, principal + 1) == OS_EPERMISSIONS);
    assert(DmDmaCheckControlLocked(2, principal, true) == OS_EOK);
    usched_mtx_unlock(&g_dmDmaLock);

    // Close in the driver security group. The next bind must drain idle itself,
    // release exactly one lease and preserve history before preparing another.
    g_current = driver;
    assert(DestroyHandle(first.Handle) == OS_EOK);
    g_current = service;
    __Drain();
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaCheckControlLocked(2, principal, true) != OS_EOK);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(DmDmaBind(principal, 2, 1, &result) == OS_ECANCELLED);
    assert(entry->Binding.Lease == NULL && entry->Binding.RequestId == 1);
    assert(DmDmaBind(principal, 2, 2, &result) == OS_EOK);
    assert(result.Generation > first.Generation);
    g_current = driver;
    assert(DestroyHandle(result.Handle) == OS_EOK);
    g_current = service;
    __Drain();
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaCheckControlLocked(2, principal, true) != OS_EOK);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(DmDmaBind(principal, 2, 1, &result) == OS_ECANCELLED);
    g_testRoutes[2].AddressLimit++;
    assert(DmDmaBind(principal, 2, 3, &result) == OS_ECANCELLED);
    assert(entry->RouteState == DmDmaRouteChanged && entry->Binding.Lease == NULL && entry->Binding.RequestId == 2);
    g_testRoutes[2].AddressLimit--;
    prepares = g_prepares;
    assert(DmDmaBind(principal, 2, 3, &result) == OS_ECANCELLED && g_prepares == prepares);
    g_testDevices[2].State = DmDeviceRemoving;
    assert(DmDmaRemoveDevice(2) == OS_EOK);
    g_testDevices[1].State = DmDeviceRemoving;
}

static oserr_t __StopHost(void* context)
{
    DmDevice_t* root = &g_testDevices[1];
    (void)context;
    assert(g_dmDmaLock.references == 1 && g_dmDevicesLock.references == 0);
    assert(root->State == DmDeviceRemoving && root->ActiveRequests == 1);
    assert(g_dmDmaState.Hosts->State != DmDmaHostReady);
    g_stopCalls++;
    return g_stopStatus;
}

// The caller holds g_dmDmaLock. Repeat a stop event after a recovery stage to
// check that it preserves the ticket, completed callbacks and deferred lease.
static void __ReplayStopEvent(DmDmaAttachment_t* entry, const OSDmaStopTicket_t* ticket)
{
    MutexLock(&g_manager->Lock);
    DMAAttachmentQueueEventLocked(g_manager, __FindTestAttachment(g_manager, entry->Id.Value),
                                  entry->Binding.Result.Generation, ticket->Epoch, DMA_ATTACHMENT_STOP_REQUIRED);
    MutexUnlock(&g_manager->Lock);
    assert(DmDmaDrainEventsLocked() == OS_EOK);
}

static void __TestRecovery(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    static const DmDeviceBusOperations_t rootOperations = { .StopDma = __StopHost };
    OSDmaBindParameters_t request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    OSDmaStopTicket_t ticket;
    OSDmaAttachmentEvent_t events[DMA_MANAGER_MAX_EVENTS];
    uint32_t eventCount;
    DmDmaAttachment_t* entry;
    DmDmaHost_t* host;
    unsigned int i;
    unsigned int calls;
    unsigned int releases = g_releases;
    uint64_t oldGeneration;

    for (i = 2; i < 4; i++) {
        g_testDevices[i].State = DmDeviceNoDriver;
        g_testDevices[i].DriverPrincipal = driver->GlobalHandleID;
        g_testDevices[i].DriverId = 99;
        g_testRoutes[i].AddressLimit = 0x2fff;
        g_testRoutes[i].Map.Ranges[0].DeviceBase = 0x2000;
        assert(DmDmaRegisterDevice(i) == OS_EOK);
    }
    g_testDevices[1].Bus.Operations = &rootOperations;
    entry = __FindDmaDevice(2);
    host = entry->Host;
    request.Attachment = entry->Id.Value;
    assert(DmDevicePrepareDmaForPrincipal(2, driver->GlobalHandleID, &entry->Binding.Lease) == OS_EOK);
    assert(DMAAttachmentBind(g_dmDmaState.Manager, &request, &entry->Binding.Result) == OS_EOK);
    entry->Binding.RequestId = 1;
    entry->Binding.OwnerSecurityID = request.Principal;
    oldGeneration = entry->Binding.Result.Generation;
    g_current = driver;
    assert(DestroyHandle(entry->Binding.Result.Handle) == OS_EOK);
    g_current = service;
    __Drain();
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaDrainEventsLocked() == OS_EOK);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(entry->Binding.Lease == NULL && entry->Binding.Result.Generation == 0);
    assert(entry->Binding.RequestId == 1 && g_releases == releases + 1);

    request.RequestId = 2;
    assert(DmDevicePrepareDmaForPrincipal(2, driver->GlobalHandleID, &entry->Binding.Lease) == OS_EOK);
    assert(DMAAttachmentBind(g_dmDmaState.Manager, &request, &entry->Binding.Result) == OS_EOK);
    entry->Binding.RequestId = 2;
    // Replayed idle from the old generation must not release the new lease.
    MutexLock(&g_manager->Lock);
    DMAAttachmentQueueEventLocked(g_manager, __FindTestAttachment(g_manager, request.Attachment),
                                  oldGeneration, 0, DMA_ATTACHMENT_IDLE);
    MutexUnlock(&g_manager->Lock);
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaDrainEventsLocked() == OS_EOK && entry->Binding.Lease != NULL);
    usched_mtx_unlock(&g_dmDmaLock);

    assert(DMAAttachmentPrepareStop(g_dmDmaState.Manager, &entry->Id, &ticket) == OS_EOK);
    // Deliver the idle peer's generation-zero event on its own. It must block
    // the host even though the peer has never had a binding or active lease.
    assert(DMAManagerReadEvents(g_dmDmaState.Manager, events, DMA_MANAGER_MAX_EVENTS, &eventCount) == OS_EOK);
    assert(eventCount == 2);
    MutexLock(&g_manager->Lock);
    DMAAttachmentQueueEventLocked(g_manager, __FindTestAttachment(g_manager, __FindDmaDevice(3)->Id.Value),
                                  0, ticket.Epoch, DMA_ATTACHMENT_STOP_REQUIRED);
    MutexUnlock(&g_manager->Lock);
    // Drop the driver grant before stop succeeds. Native IDLE may now share a
    // batch with the idle peer's stop event, but the bus lease must stay pinned
    // until the trusted callback and kernel confirmation both finish.
    g_current = driver;
    assert(DestroyHandle(entry->Binding.Result.Handle) == OS_EOK);
    g_current = service;
    __Drain();
    g_stopStatus = OS_ENOTSUPPORTED;
    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaDrainEventsLocked() == OS_EOK && host->State == DmDmaHostStopPending);
    // A failed ticket copy must retry preparation before calling the bus.
    g_failCopy = 1;
    __ProcessDmaStops(&g_testTime);
    g_failCopy = -1;
    assert(host->State == DmDmaHostStopPending && host->Ticket.Epoch == 0);
    assert(g_stopCalls == 0 && g_completeCalls == 0 && entry->Binding.Lease != NULL);
    assert(host->RetryDelay == 1);
    __ReplayStopEvent(entry, &ticket);
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 0 && g_completeCalls == 0);
    g_testTime.Seconds++;
    __ProcessDmaStops(&g_testTime);
    assert(host->State == DmDmaHostStopUnsupported && g_completeCalls == 0);
    assert(host->Ticket.Epoch == ticket.Epoch && g_stopCalls == 1);
    __ReplayStopEvent(entry, &ticket);
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 1 && entry->Binding.Lease != NULL);

    // Permit the fake callback for the remaining fault cases and fixture
    // teardown. Production never leaves StopUnsupported or unblocks this host.
    host->State = DmDmaHostStopPrepared;
    host->RetryDelay = 0;
    g_stopStatus = OS_EBUSY;
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 2 && g_completeCalls == 0 && host->RetryDelay == 1);
    assert(host->State == DmDmaHostStopPrepared && host->Ticket.Epoch == ticket.Epoch);
    __ReplayStopEvent(entry, &ticket);
    calls = g_stopCalls;
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == calls);
    g_testTime.Seconds++;
    g_stopStatus = OS_EOK;
    g_completeStatus = OS_EBUSY;
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 3 && g_completeCalls == 1 && host->State == DmDmaHostHardwareStopped);
    assert(host->RetryDelay == 2 && host->Ticket.Epoch == ticket.Epoch);
    __ReplayStopEvent(entry, &ticket);
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 3 && g_completeCalls == 1 && entry->Binding.Lease != NULL);
    g_testTime.Seconds += 2;
    g_completeStatus = OS_EOK;
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 3 && g_completeCalls == 2 && host->State == DmDmaHostStopped);
    assert(entry->Binding.Lease == NULL);
    __ReplayStopEvent(entry, &ticket);
    __ProcessDmaStops(&g_testTime);
    assert(g_stopCalls == 3 && g_completeCalls == 2);
    g_testDevices[1].State = DmDeviceNoDriver;
    assert(!__DmaHostCanStart(host));
    g_testDevices[1].State = DmDeviceRemoving;
    usched_mtx_unlock(&g_dmDmaLock);

    usched_mtx_lock(&g_dmDmaLock);
    assert(DmDmaDrainEventsLocked() == OS_EOK && entry->Binding.Lease == NULL);
    usched_mtx_unlock(&g_dmDmaLock);
    assert(g_releases == releases + 2 && g_testDevices[1].ActiveRequests == 0);
    for (i = 2; i < 4; i++) {
        g_testDevices[i].State = DmDeviceRemoving;
        assert(DmDmaRemoveDevice(i) == OS_EOK);
    }
    assert(g_dmDmaState.Hosts == NULL && g_dmDmaState.Devices == NULL);
}

static void __TestRepeatedEvent(void)
{
    DMAAttachment_t* native;
    DmDmaAttachment_t* entry;
    DmDmaHost_t* host;
    unsigned int stops = g_stopCalls;

    g_testDevices[2].State = DmDeviceNoDriver;
    assert(DmDmaRegisterDevice(2) == OS_EOK);
    entry = __FindDmaDevice(2);
    native = __FindTestAttachment(g_manager, entry->Id.Value);
    MutexLock(&g_manager->Lock);
    DMAAttachmentQueueEventLocked(g_manager, native, 0, 1, DMA_ATTACHMENT_STOP_REQUIRED);
    native->Event.Sequence = UINT64_MAX;
    MutexUnlock(&g_manager->Lock);
    g_stopStatus = OS_ENOTSUPPORTED;
    __DmaRecoveryJob(NULL, NULL);
    assert(g_sleepCalls == 2 && g_stopCalls == stops + 1);
    assert(native->Event.Sequence == UINT64_MAX && g_manager->PendingEvents.count != 0);
    assert(g_dmDmaState.ManagerState == DmDmaManagerUnavailable && g_dmDmaState.Manager == UUID_INVALID);
    // The fixture has no bindings. Model process exit for the service records,
    // which production keeps after failure because real bus guards may remain.
    entry = g_dmDmaState.Devices;
    host = g_dmDmaState.Hosts;
    assert(entry->Next == NULL && host->Next == NULL && entry->Binding.Lease == NULL);
    free(entry);
    free(host);
    g_dmDmaState.Devices = NULL;
    g_dmDmaState.Hosts = NULL;
    // Kernel exhaustion intentionally pins the closed manager until reboot.
    // Reuse the native suite's reboot cleanup after verifying that state.
    assert(g_manager != NULL && g_manager->State == DMAManagerClosed);
    list_remove(&g_manager->PendingEvents, &native->Event.Link);
    list_remove(&g_manager->Registry.Attachments, &native->Link);
    g_manager->Registry.Count = 0;
    while (g_manager->Registry.Groups.head != NULL) {
        DMARecoveryGroup_t* group = g_manager->Registry.Groups.head->value;
        list_remove(&g_manager->Registry.Groups, &group->Link);
        DMARecoveryGroupRelease(&group);
    }
    kfree(native);
    __DetachClosedManager(g_manager);
}

int main(void)
{
    MSContextSecurity_t service;
    MSContextSecurity_t driver;
    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    g_current = &service;
    usched_mtx_init(&g_dmDmaLock, USCHED_MUTEX_PLAIN);
    usched_mtx_init(&g_dmDevicesLock, USCHED_MUTEX_PLAIN);
    __TestStartupFailures();
    __TestRegistration();
    __TestBinding(&service, &driver);
    __TestRecovery(&service, &driver);
    // Repeated readiness must sleep. A sleep failure closes the manager.
    __TestRepeatedEvent();
    assert(g_dmDmaState.ManagerState == DmDmaManagerUnavailable && g_dmDmaState.Manager == UUID_INVALID);
    assert(DmDmaInitialize() == OS_ECANCELLED);
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(driver.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("DMA service: registration, grouping, retry history, idle generations and confirmed-stop ordering passed");
    return 0;
}
