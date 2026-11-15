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

// Shared manager/binding setup without test cases. DMA_TEST_EXTERNAL_ENGINE
// links the DMA implementation separately. DMA_BOUND_REAL_SHM omits the SHM
// stubs when the suite links production SHM sources.

#define HANDLE_TEST_CUSTOM_SYSCALLS
#include "handle_test_support.inc"

#include "../kernel/memory/dma/attachment_private.h"
#ifndef DMA_TEST_EXTERNAL_ENGINE
static int g_failDmaReference = -1;
static int g_dmaReferenceCalls;

static oserr_t
__AcquireDmaReference(
    _In_  uuid_t           handle,
    _In_  enum HandleType type,
    _Out_ void**          resourceOut)
{
    g_dmaReferenceCalls++;
    if (g_failDmaReference == 0) {
        return OS_EUNKNOWN;
    }
    if (g_failDmaReference > 0) {
        g_failDmaReference--;
    }
    return AcquireGlobalHandle(handle, type, resourceOut);
}

#define AcquireGlobalHandle __AcquireDmaReference
#ifndef DMA_MANAGER_SOURCE
#define DMA_MANAGER_SOURCE "../kernel/memory/dma/manager.c"
#endif
#include DMA_MANAGER_SOURCE
#ifndef DMA_BINDING_SOURCE
#define DMA_BINDING_SOURCE "../kernel/memory/dma/binding.c"
#endif
#include DMA_BINDING_SOURCE
#undef AcquireGlobalHandle
#include "../kernel/memory/dma/groups.c"
#ifndef DMA_RECOVERY_SOURCE
#define DMA_RECOVERY_SOURCE "../kernel/memory/dma/recovery.c"
#endif
#include DMA_RECOVERY_SOURCE
#include "../kernel/memory/dma/direct.c"
#ifndef DMA_EVENTS_SOURCE
#define DMA_EVENTS_SOURCE "../kernel/memory/dma/events.c"
#endif
#include DMA_EVENTS_SOURCE

#endif

// Registry lookup for test assertions. Production keeps this helper private.
static DMAAttachment_t*
__FindTestAttachment(DMAManager_t* manager, uint64_t id)
{
    foreach (node, &manager->Registry.Attachments) {
        DMAAttachment_t* attachment = node->value;

        if (attachment->ID == id) {
            return attachment;
        }
    }
    return NULL;
}

// This suite exercises administrative records without constructing SHM contexts.
// The bound integration suite supplies the real SHM implementation instead.
#ifndef DMA_BOUND_REAL_SHM
oserr_t SHMDeviceContextAcquire(SHMDeviceContext_t* context)
{ (void)context; assert(false); return OS_EUNKNOWN; }
void SHMDeviceContextRelease(SHMDeviceContext_t** context)
{ (void)context; assert(false); }
oserr_t SHMDeviceContextRevoke(SHMDeviceContext_t* context)
{ (void)context; assert(false); return OS_EUNKNOWN; }
oserr_t SHMDeviceContextCompleteStop(SHMDeviceContext_t* context)
{ (void)context; assert(false); return OS_EUNKNOWN; }
#endif

static DMAManager_t*
__CreateManager(
    _Out_ uuid_t* handleOut)
{
    DMAManager_t* manager = NULL;

    assert(DMAManagerPrepareCreate(&manager, handleOut) == OS_EOK);
    assert(DMAManagerPublish(manager) == OS_EOK);
    return manager;
}

static OSDmaAttachmentParameters_t
__Parameters(
    _In_ uuid_t   device,
    _In_ uint64_t requester,
    _In_ uint64_t reset)
{
    OSDmaAttachmentParameters_t parameters = {
        .Version = DMA_ATTACHMENT_VERSION,
        .Device = device,
        .Host = 1,
        .RequesterGroup = requester,
        .ResetGroup = reset,
        .AddressLimit = UINT32_MAX,
        .CachePolicy = SHMDeviceCacheNonCoherent,
        .RangeCount = 1,
        .Ranges = { { 0x1000, 0x2000, 0x1000 } }
    };
    return parameters;
}

static uint64_t
__RegisterAttachment(
    _In_ DMAManager_t*                      manager,
    _In_ const OSDmaAttachmentParameters_t* parameters)
{
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentId_t          id = { 0 };

    assert(DMAManagerPrepareAttachment(manager, parameters, &registration, &id) == OS_EOK);
    assert(DMAManagerPublishAttachment(&registration) == OS_EOK && registration == NULL);
    return id.Value;
}

static void
__RemoveAllAttachments(
    _In_ DMAManager_t* manager)
{
    DMAAttachment_t* attachment;

    while (manager->Registry.Attachments.head != NULL) {
        attachment = manager->Registry.Attachments.head->value;
        assert(DMAManagerRemoveAttachment(manager, attachment->ID) == OS_EOK);
    }
}

static void
__QueueAttachmentEvent(
    _In_ DMAManager_t* manager,
    _In_ uint64_t      id,
    _In_ uint32_t      reasons)
{
    MutexLock(&manager->Lock);
    DMAAttachmentQueueEventLocked(manager, __FindTestAttachment(manager, id), 7, 3, reasons);
    MutexUnlock(&manager->Lock);
    MarkHandle(manager->ID, IOSETIN);
}

static void
__ConsumeBindingIdle(
    _In_ DMAManager_t* manager,
    _In_ uint64_t      attachment,
    _In_ uint64_t      generation)
{
    DMAAttachmentEventBatch_t batch;

    assert(DMAManagerSnapshotEvents(manager, 1, &batch) == OS_EOK && batch.Count == 1);
    assert(batch.Entries[0].Attachment == attachment && batch.Entries[0].Generation == generation);
    assert(batch.Entries[0].StopEpoch == 0 && batch.Entries[0].Reasons == DMA_ATTACHMENT_IDLE);
    assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    assert(manager->PendingEvents.count == 0);
}

static OSDmaBindingResult_t
__BindAttachment(
    _In_ DMAManager_t*                manager,
    _In_ const OSDmaBindParameters_t* parameters)
{
    DMABindingRegistration_t* registration = NULL;
    OSDmaBindingResult_t      result;

    assert(DMAManagerPrepareBinding(manager, parameters, &registration, &result) == OS_EOK);
    assert(DMAManagerPublishBinding(&registration) == OS_EOK && registration == NULL);
    return result;
}

