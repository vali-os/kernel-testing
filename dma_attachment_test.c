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

//#define __TRACE

#define DMA_ATTACHMENT_WARNING_TEST
#include "dma_attachment_support.inc"

static unsigned int g_warningCount;

void
TestDmaWarning(
    _In_ const char* format,
    ...)
{
    (void)format;
    g_warningCount++;
}

static void
__TestManagerLifetime(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* stranger)
{
    DMAManager_t* manager = NULL;
    DMAManager_t* other = NULL;
    struct ResourceHandle* record;
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeProvider,
        .TargetType = GrantTokenTargetTypeAny,
        .Scope = GrantTokenScopeRead
    };
    GrantToken_t token = { .ControlHandleID = 123 };
    uuid_t handle = UUID_INVALID;
    uuid_t untouched = 123;
    uuid_t binding;
    uint64_t principal;
    size_t before = g_allocations;
    int failure;

    g_current = NULL;
    assert(DMAManagerPrepareCreate(&manager, &handle) == OS_EPERMISSIONS);
    assert(manager == NULL && handle == UUID_INVALID);
    g_current = service;
    for (failure = 0; failure < 2; failure++) {
        g_failAllocation = failure;
        assert(DMAManagerPrepareCreate(&manager, &handle) == OS_EOOM);
        assert(manager == NULL && handle == UUID_INVALID && g_manager == NULL);
        assert(g_allocations == before);
    }
    g_failAllocation = -1;

    for (failure = 0; failure < 2; failure++) {
        g_failDmaReference = failure;
        g_dmaReferenceCalls = 0;
        assert(DMAManagerPrepareCreate(&manager, &handle) == OS_EUNKNOWN);
        assert(g_dmaReferenceCalls == failure + 1);
        assert(manager == NULL && handle == UUID_INVALID && g_manager == NULL);
        assert(!g_managerRegistryLock.held);
        g_failDmaReference = -1;
        __Drain();
        assert(g_allocations == before);
    }

    assert(DMAManagerPrepareCreate(&manager, &handle) == OS_EOK);
    record = __LookupGlobalSafe(handle);
    assert(record->References == 3);
    assert(DMAManagerReference(handle, &other) == OS_EBUSY && other == NULL);
    assert(DMAManagerPrepareCreate(&other, &untouched) == OS_EBUSY);
    assert(other == NULL && untouched == 123);
    DMAManagerCancelCreate(manager);
    assert(manager->State == DMAManagerClosed && g_manager == NULL);
    DMAManagerRelease(&manager);
    __Drain();
    assert(g_allocations == before);

    manager = __CreateManager(&handle);
    assert(DMAManagerReference(handle, &other) == OS_EOK);
    DMAManagerRelease(&other);
    assert(GrantHandle(stranger, handle) == OS_EPERMISSIONS);
    assert(GrantTokenCreate(handle, &options, &token) == OS_EUNKNOWN);
    assert(token.ControlHandleID == 123);
    g_current = stranger;
    assert(DMAManagerReference(handle, &other) == OS_ENOENT && other == NULL);
    g_current = NULL;
    assert(DMAManagerReference(handle, &other) == OS_EPERMISSIONS && other == NULL);
    g_current = service;
    principal = manager->OwnerSecurityID;
    manager->OwnerSecurityID |= 1ULL << 32;
    assert(DMAManagerReference(handle, &other) == OS_EPERMISSIONS && other == NULL);
    manager->OwnerSecurityID = principal;

    binding = CreateHandle(HandleTypeDMABinding, NULL, NULL, NULL);
    assert(GrantHandle(stranger, binding) == OS_EPERMISSIONS);
    assert(GrantTokenCreate(binding, &options, &token) == OS_EUNKNOWN);
    assert(token.ControlHandleID == 123);
    assert(DMAManagerReference(binding, &other) == OS_EUNKNOWN && other == NULL);
    assert(DestroyHandle(binding) == OS_EOK);

    assert(DestroyHandle(handle) == OS_EOK);
    assert(manager->State == DMAManagerClosed && g_manager == NULL);
    assert(DMAManagerReference(handle, &other) == OS_ENOENT);
    DMAManagerRelease(&manager);
    __Drain();
    assert(g_allocations == before);

    // Close between prepare and publication. The independent operation survives
    // while the grant callback removes the singleton registry reference.
    assert(DMAManagerPrepareCreate(&manager, &handle) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EOK);
    assert(DMAManagerPublish(manager) == OS_ECANCELLED);
    DMAManagerCancelCreate(manager);
    DMAManagerRelease(&manager);
    __Drain();
    assert(g_allocations == before);
}

static void
__TestManagerGroupExit(void)
{
    MSContextSecurity_t service;
    DMAManager_t* manager;
    DMAManager_t* other = NULL;
    uuid_t handle;

    __NewGroup(&service);
    g_current = &service;
    manager = __CreateManager(&handle);
    // Group destruction can be queued before its grant callbacks run.
    assert(ReleaseGlobalHandle(service.GlobalHandleID) == OS_EOK);
    assert(DMAManagerReference(handle, &other) == OS_ECANCELLED);
    assert(other == NULL && manager->State == DMAManagerClosed && g_manager == NULL);
    DestroyHandleGrantsTable(service.HandleTable);
    g_current = NULL;
    DMAManagerRelease(&manager);
    __Drain();
}







static void
__TestRegistrationValidation(void)
{
    DMAManager_t*                 manager;
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentParameters_t   parameters;
    OSDmaAttachmentId_t           id = { 999 };
    DMAAttachment_t*             attachment;
    uuid_t                            handle;
    uint64_t                          saved;
    size_t                            before;
    int                               failure;

    manager = __CreateManager(&handle);
    before = g_allocations;
    for (failure = 0; failure < 12; failure++) {
        parameters = __Parameters(10, 20, 30);
        switch (failure) {
            case 0: parameters.Version++; break;
            case 1: parameters.Reserved = 1; break;
            case 2: parameters.Device = UUID_INVALID; break;
            case 3: parameters.Host = UUID_INVALID; break;
            case 4: parameters.RequesterGroup = 0; break;
            case 5: parameters.ResetGroup = 0; break;
            case 6: parameters.CachePolicy = SHMDeviceCacheUnknown; break;
            case 7: parameters.RangeCount = 0; break;
            case 8: parameters.Ranges[1].Length = 1; break;
            case 9: parameters.Ranges[0].Length = 0; break;
            case 10: parameters.Ranges[0].PhysicalBase = UINT64_MAX; break;
            case 11: parameters.Ranges[0].DeviceBase = UINT64_MAX; break;
        }
        assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EINVALPARAMS);
        assert(registration == NULL && id.Value == 999 && manager->Registry.Count == 0);
        assert(g_allocations == before && !manager->UpdateLock.held);
    }
    parameters = __Parameters(10, 20, 30);
    parameters.RangeCount = DMA_MAX_RANGES + 1;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_ENOTSUPPORTED);
    parameters.RangeCount = 2;
    parameters.Ranges[1] = parameters.Ranges[0];
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EINVALPARAMS);
    parameters.Ranges[1].DeviceBase = 0x4000;
    // Physical aliases remain valid, and a zero address limit is not widened.
    parameters.AddressLimit = 0;
    saved = __RegisterAttachment(manager, &parameters);
    attachment = __FindTestAttachment(manager, saved);
    assert(attachment->Configuration.AddressLimit == 0);
    assert(__RegisterAttachment(manager, &parameters) == saved && manager->Registry.Count == 1);
    parameters.Host++;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EBUSY);
    parameters.Host--;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EOK);
    DMAManagerCancelAttachment(&registration);
    assert(manager->Registry.Count == 1 && __FindTestAttachment(manager, saved) == attachment);
    assert(DMAManagerRemoveAttachment(manager, saved) == OS_EOK);
    assert(DMAManagerRemoveAttachment(manager, saved) == OS_ENOENT);
    assert(DMAManagerRemoveAttachment(manager, 0) == OS_EINVALPARAMS);
    assert(g_allocations == before);

    parameters = __Parameters(10, 20, 30);
    id.Value = 999;
    for (failure = 0; failure < 4; failure++) {
        g_failAllocation = failure;
        assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EOOM);
        assert(registration == NULL && id.Value == 999 && manager->Registry.Count == 0);
        assert(g_allocations == before && !manager->UpdateLock.held);
    }
    g_failAllocation = -1;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EOK);
    saved = id.Value;
    assert(manager->Registry.Count == 0 && __FindTestAttachment(manager, saved) == NULL);
    memset(&parameters, 0xee, sizeof(parameters));
    assert(DMAManagerPublishAttachment(&registration) == OS_EOK);
    attachment = __FindTestAttachment(manager, saved);
    assert(attachment->Configuration.Device == 10 && attachment->Configuration.Ranges[0].Length == 0x1000);
    __RemoveAllAttachments(manager);

    parameters = __Parameters(10, 20, 30);
    manager->Registry.NextAttachmentID = UINT64_MAX;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EOVERFLOW);
    manager->Registry.NextAttachmentID = 1000;
    manager->Registry.NextRecoveryGroupID = UINT64_MAX;
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_EOVERFLOW);
    assert(manager->Registry.Count == 0 && g_allocations == before);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

static void
__TestRecoveryGroups(void)
{
    DMAManager_t* manager;
    OSDmaAttachmentParameters_t parameters;
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
    uint64_t e;
    uint64_t f;
    uint64_t beforeA;
    uint64_t beforeC;
    uint64_t beforeD;
    uint64_t combined;
    uint64_t next;
    uuid_t handle;

    manager = __CreateManager(&handle);
    parameters = __Parameters(10, 10, 20);
    a = __RegisterAttachment(manager, &parameters);
    beforeA = __FindTestAttachment(manager, a)->RecoveryGroup->ID;
    parameters = __Parameters(12, 40, 30);
    c = __RegisterAttachment(manager, &parameters);
    beforeC = __FindTestAttachment(manager, c)->RecoveryGroup->ID;
    parameters = __Parameters(13, 50, 60);
    d = __RegisterAttachment(manager, &parameters);
    beforeD = __FindTestAttachment(manager, d)->RecoveryGroup->ID;
    parameters = __Parameters(11, 10, 30);
    b = __RegisterAttachment(manager, &parameters);
    combined = __FindTestAttachment(manager, b)->RecoveryGroup->ID;
    assert(combined != beforeA && combined != beforeC);
    assert(__FindTestAttachment(manager, a)->RecoveryGroup->ID == combined);
    assert(__FindTestAttachment(manager, c)->RecoveryGroup->ID == combined);
    assert(__FindTestAttachment(manager, d)->RecoveryGroup->ID == beforeD);

    // A split needs two fresh IDs. Preflight failure must not change any member.
    next = manager->Registry.NextRecoveryGroupID;
    manager->Registry.NextRecoveryGroupID = UINT64_MAX - 1;
    assert(DMAManagerRemoveAttachment(manager, b) == OS_EOVERFLOW);
    assert(manager->Registry.Count == 4 && __FindTestAttachment(manager, a)->RecoveryGroup->ID == combined);
    manager->Registry.NextRecoveryGroupID = next;
    g_failAllocation = 0;
    assert(DMAManagerRemoveAttachment(manager, b) == OS_EOOM);
    g_failAllocation = -1;
    assert(DMAManagerRemoveAttachment(manager, b) == OS_EOK);
    assert(__FindTestAttachment(manager, a)->RecoveryGroup->ID != __FindTestAttachment(manager, c)->RecoveryGroup->ID);
    assert(__FindTestAttachment(manager, a)->RecoveryGroup->ID != combined);
    assert(__FindTestAttachment(manager, c)->RecoveryGroup->ID != combined);
    assert(__FindTestAttachment(manager, d)->RecoveryGroup->ID == beforeD);
    assert(__RegisterAttachment(manager, &parameters) > b);

    // Equality between different key namespaces does not connect the records.
    parameters = __Parameters(14, 7, 70);
    e = __RegisterAttachment(manager, &parameters);
    parameters = __Parameters(15, 80, 7);
    f = __RegisterAttachment(manager, &parameters);
    assert(__FindTestAttachment(manager, e)->RecoveryGroup->ID != __FindTestAttachment(manager, f)->RecoveryGroup->ID);
    __RemoveAllAttachments(manager);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

static void
__TestRegistryLimit(void)
{
    DMAManager_t* manager;
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentParameters_t parameters;
    OSDmaAttachmentId_t id = { 999 };
    uint64_t first = 0;
    uint32_t i;
    uuid_t handle;

    manager = __CreateManager(&handle);
    for (i = 1; i <= DMA_MANAGER_MAX_ATTACHMENTS; i++) {
        parameters = __Parameters(i, i, i);
        if (i == 1) {
            first = __RegisterAttachment(manager, &parameters);
        } else {
            __RegisterAttachment(manager, &parameters);
        }
    }
    parameters = __Parameters(1, 1, 1);
    assert(__RegisterAttachment(manager, &parameters) == first);
    parameters = __Parameters(1000, 1000, 1000);
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &id) == OS_ENOTSUPPORTED);
    assert(registration == NULL && id.Value == 999 && manager->Registry.Count == DMA_MANAGER_MAX_ATTACHMENTS);
    __RemoveAllAttachments(manager);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

// Tests call the same internal producer that later binding lifecycle code uses.
// Notifications remain outside Lock, as required by the native contract.


static void
__TestEventDelivery(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* stranger)
{
    DMAManager_t* manager;
    DMAManager_t* reference = NULL;
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentParameters_t parameters;
    OSDmaAttachmentId_t result;
    DMAAttachmentEventBatch_t first;
    DMAAttachmentEventBatch_t second;
    struct ioset_event watch;
    uint64_t id;
    uint64_t replacement;
    uint64_t sequence;
    uuid_t handle;
    uuid_t queue;
    uuid_t foreignQueue;
    uint32_t i;

    manager = __CreateManager(&handle);
    queue = CreateHandleSet(0);
    watch = (struct ioset_event) { .events = IOSETIN, .data.handle = handle };
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    parameters = __Parameters(1, 10, 20);
    id = __RegisterAttachment(manager, &parameters);
    __ExpectNoWakeup(queue);
    assert(DMAManagerSnapshotEvents(manager, 1, &first) == OS_EOK && first.Count == 0);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);

    // A handle-set subscription and a forged wake do not supply a manager grant.
    g_current = stranger;
    foreignQueue = CreateHandleSet(0);
    assert(ControlHandleSet(foreignQueue, IOSET_ADD, handle, &watch) == OS_ENOENT);
    MarkHandle(handle, IOSETIN);
    assert(DMAManagerReference(handle, &reference) == OS_ENOENT && reference == NULL);
    __ExpectNoWakeup(foreignQueue);
    assert(DestroyHandle(foreignQueue) == OS_EOK);
    g_current = service;
    __ExpectWakeup(queue, handle);
    __ExpectNoWakeup(queue);

    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_IDLE);
    __ExpectWakeup(queue, handle);
    first.Count = 99;
    assert(DMAManagerSnapshotEvents(manager, 0, &first) == OS_EINVALPARAMS && first.Count == 99);
    __ExpectWakeup(queue, handle);
    assert(DMAManagerSnapshotEvents(manager, 1, &first) == OS_EOK && first.Count == 1);
    assert(first.Entries[0].Attachment == id && first.Entries[0].Generation == 7);
    assert(first.Entries[0].StopEpoch == 3 && first.Entries[0].Reserved == 0);
    assert(DMAManagerFinishEvents(manager, &first, false) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    __ExpectWakeup(queue, handle);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EBUSY);

    // Extending any member of a component must wait for its queued events.
    parameters = __Parameters(2, 10, 30);
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &result) == OS_EBUSY);
    assert(registration == NULL && manager->Registry.Count == 1);
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_STOP_REQUIRED);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    assert(DMAManagerSnapshotEvents(manager, 1, &second) == OS_EOK);
    assert(second.Sequences[0] > first.Sequences[0]);
    assert(second.Entries[0].Reasons == (DMA_ATTACHMENT_IDLE | DMA_ATTACHMENT_STOP_REQUIRED));
    assert(DMAManagerFinishEvents(manager, &second, true) == OS_EOK);
    assert(manager->PendingEvents.count == 0);
    __ExpectWakeup(queue, handle);
    __ExpectNoWakeup(queue);

    // A new event after acknowledgement must not match an older reader's version.
    sequence = second.Sequences[0];
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_IDLE);
    assert(DMAManagerFinishEvents(manager, &second, true) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    assert(DMAManagerSnapshotEvents(manager, 1, &first) == OS_EOK);
    assert(first.Sequences[0] > sequence);
    assert(DMAManagerSnapshotEvents(manager, 1, &second) == OS_EOK);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EOK);
    parameters = __Parameters(1, 10, 20);
    replacement = __RegisterAttachment(manager, &parameters);
    assert(replacement > id);
    __QueueAttachmentEvent(manager, replacement, DMA_ATTACHMENT_IDLE);
    assert(DMAManagerFinishEvents(manager, &second, true) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    assert(DMAManagerSnapshotEvents(manager, 32, &first) == OS_EOK);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    __ExpectWakeup(queue, handle);

    // An event queued while the registration result is copied prevents publish.
    parameters = __Parameters(2, 10, 30);
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &result) == OS_EOK);
    __QueueAttachmentEvent(manager, replacement, DMA_ATTACHMENT_IDLE);
    assert(DMAManagerPublishAttachment(&registration) == OS_EBUSY);
    DMAManagerCancelAttachment(&registration);
    assert(manager->Registry.Count == 1);
    assert(DMAManagerSnapshotEvents(manager, 32, &first) == OS_EOK);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    __RemoveAllAttachments(manager);
    __ExpectWakeup(queue, handle);

    assert(ControlHandleSet(queue, IOSET_DEL, handle, NULL) == OS_EOK);
    parameters = __Parameters(1, 10, 20);
    id = __RegisterAttachment(manager, &parameters);
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_IDLE);
    assert(MarkHandle(handle, IOSETIN) == OS_ENOENT);
    assert(manager->PendingEvents.count == 1);
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    assert(DMAManagerSnapshotEvents(manager, 1, &first) == OS_EOK && first.Count == 1);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    __RemoveAllAttachments(manager);

    // Capacity is bounded independently of the caller's array size.
    for (i = 1; i <= 33; i++) {
        parameters = __Parameters(i, i, i);
        id = __RegisterAttachment(manager, &parameters);
        __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_IDLE);
    }
    __ExpectWakeup(queue, handle);
    assert(DMAManagerSnapshotEvents(manager, UINT32_MAX, &first) == OS_EOK && first.Count == 32);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    __ExpectWakeup(queue, handle);
    assert(DMAManagerSnapshotEvents(manager, 1, &first) == OS_EOK && first.Count == 1);
    assert(DMAManagerFinishEvents(manager, &first, true) == OS_EOK);
    __ExpectNoWakeup(queue);
    __RemoveAllAttachments(manager);

    // The native subscription is tied to the service grant, not the global ID.
    assert(DestroyHandle(handle) == OS_EOK);
    MarkHandle(handle, IOSETIN);
    __ExpectNoWakeup(queue);
    assert(DestroyHandle(queue) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

static void
__TestClosedRegistry(void)
{
    DMAManager_t* manager;
    DMAManager_t* other = NULL;
    DMAAttachment_t* attachment;
    DMAAttachmentEventBatch_t batch;
    OSDmaAttachmentParameters_t parameters;
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentId_t prepared;
    uint64_t id;
    uuid_t handle;
    uuid_t unused = 123;
    unsigned int warnings;

    manager = __CreateManager(&handle);
    parameters = __Parameters(1, 10, 20);
    id = __RegisterAttachment(manager, &parameters);
    attachment = __FindTestAttachment(manager, id);
    warnings = g_warningCount;
    attachment->Event.Sequence = UINT64_MAX - 3;
    // Acknowledgement must still work before the sequence counter is exhausted.
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_STOP_REQUIRED);
    assert(DMAManagerSnapshotEvents(manager, 1, &batch) == OS_EOK);
    assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    assert(manager->PendingEvents.count == 0 && attachment->Event.Sequence == UINT64_MAX - 2);
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_STOP_REQUIRED);
    assert(DMAManagerSnapshotEvents(manager, 1, &batch) == OS_EOK);
    assert(attachment->Event.Sequence == UINT64_MAX - 1 && g_warningCount == warnings);
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_CLEANUP_FAILED);
    assert(attachment->Event.Sequence == UINT64_MAX && g_warningCount == warnings + 1);
    assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    assert(DMAManagerSnapshotEvents(manager, 1, &batch) == OS_EOK);
    assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    assert(manager->PendingEvents.count == 1);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EOVERFLOW);
    __QueueAttachmentEvent(manager, id, DMA_ATTACHMENT_IDLE);
    assert(attachment->Event.Sequence == UINT64_MAX && attachment->Event.Value.Reasons == 7);
    assert(g_warningCount == warnings + 1);

    // Close can run while UpdateLock is held across an output copy. It keeps
    // committed metadata but must not publish another attachment afterwards.
    parameters = __Parameters(2, 40, 50);
    assert(DMAManagerPrepareAttachment(manager, &parameters, &registration, &prepared) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EOK);
    assert(manager->State == DMAManagerClosed && g_manager == manager);
    assert(DMAManagerPublishAttachment(&registration) == OS_ECANCELLED);
    DMAManagerCancelAttachment(&registration);
    assert(DMAManagerFinishEvents(manager, &batch, true) == OS_ECANCELLED);
    assert(DMAManagerSnapshotEvents(manager, 1, &batch) == OS_ECANCELLED);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_ECANCELLED);
    assert(DMAManagerPrepareCreate(&other, &unused) == OS_EBUSY);
    assert(other == NULL && unused == 123 && manager->Registry.Count == 1);
    DMAManagerRelease(&manager);
    __Drain();
    assert(g_manager != NULL && __LookupGlobalSafe(handle)->References == 1);

    // Test-only reboot cleanup. Production deliberately keeps this closed
    // registry until reboot. No API can clear exhaustion or revive authority.
    manager = g_manager;
    list_remove(&manager->PendingEvents, &attachment->Event.Link);
    list_remove(&manager->Registry.Attachments, &attachment->Link);
    manager->Registry.Count = 0;
    while (manager->Registry.Groups.head != NULL) {
        DMARecoveryGroup_t* group = manager->Registry.Groups.head->value;

        list_remove(&manager->Registry.Groups, &group->Link);
        DMARecoveryGroupRelease(&group);
    }
    kfree(attachment);
    DMAManagerDetachClosed(manager);
    __Drain();
    assert(g_manager == NULL);
}






static void
__TestBindingRequests(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* receiver)
{
    DMAManager_t* manager;
    DMABindingRegistration_t* registration = NULL;
    DMAAttachmentRegistration_t* addition = NULL;
    DMAAttachment_t* attachment;
    DMABinding_t* reference = NULL;
    OSDmaAttachmentParameters_t route = __Parameters(101, 10, 20);
    OSDmaAttachmentId_t other;
    OSDmaBindParameters_t parameters;
    OSDmaBindingResult_t result = { .Handle = 123, .Generation = 456 };
    OSDmaBindingResult_t first;
    uint64_t id;
    uint64_t next;
    uuid_t handle;
    size_t allocations;
    struct ioset_event watch;
    uuid_t queue;
    int failure;

    manager = __CreateManager(&handle);
    id = __RegisterAttachment(manager, &route);
    attachment = __FindTestAttachment(manager, id);
    parameters = (OSDmaBindParameters_t) {
        .Attachment = id, .Principal = receiver->GlobalHandleID, .RequestId = 41
    };
    next = manager->Registry.NextBindingGeneration;
    parameters.Principal |= 1ULL << 32;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EINVALPARAMS);
    parameters.Principal = 0;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EINVALPARAMS);
    parameters.Principal = receiver->GlobalHandleID;
    parameters.RequestId = 0;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EINVALPARAMS);
    parameters.RequestId = UINT64_MAX;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOVERFLOW);
    parameters.RequestId = 41;
    parameters.Attachment = 0;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EINVALPARAMS);
    parameters.Attachment = id + 1;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_ENOENT);
    parameters.Attachment = id;
    assert(result.Handle == 123 && result.Generation == 456 && registration == NULL);
    assert(manager->Registry.NextBindingGeneration == next);

    queue = CreateHandleSet(0);
    watch = (struct ioset_event) { .events = IOSETIN, .data.handle = handle };
    assert(ControlHandleSet(queue, IOSET_ADD, handle, &watch) == OS_EOK);
    allocations = g_allocations;
    for (failure = 0; failure < 3; failure++) {
        // Fail preparation storage, binding storage, then native handle creation.
        g_failAllocation = failure;
        assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOOM);
        g_failAllocation = -1;
        __Drain();
        assert(result.Handle == 123 && result.Generation == 456 && registration == NULL);
        assert(attachment->Binding.Current == NULL && attachment->Binding.RequestId == 0);
        assert(!manager->UpdateLock.held && g_allocations == allocations);
    }

    for (failure = 0; failure < 2; failure++) {
        g_failDmaReference = failure;
        g_dmaReferenceCalls = 0;
        assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EUNKNOWN);
        assert(g_dmaReferenceCalls == failure + 1);
        g_failDmaReference = -1;
        __Drain();
        assert(result.Handle == 123 && result.Generation == 456 && registration == NULL);
        assert(attachment->Binding.Current == NULL && attachment->Binding.RequestId == 0);
        assert(!manager->Lock.held && !manager->UpdateLock.held);
        assert(g_allocations == allocations);
    }

    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOK);
    assert(result.Reserved == 0 && attachment->Binding.Current == NULL);
    assert(AcquireHandleReference(result.Handle, HandleTypeDMABinding, (void**)&reference) == OS_ENOENT);
    assert(AcquireHandleReferenceInContext(receiver, result.Handle, HandleTypeDMABinding,
                                         (void**)&reference) == OS_EOK);
    assert(reference->State == DMABindingPreparing && reference->Attachment == NULL);
    DMAManagerCancelBinding(&registration);
    assert(reference->State == DMABindingClosed && registration == NULL);
    assert(ReleaseGlobalHandle(result.Handle) == OS_EOK);
    __Drain();
    assert(g_allocations == allocations && attachment->Binding.RequestId == 0);
    __ExpectNoWakeup(queue);

    first = __BindAttachment(manager, &parameters);
    assert(first.Generation > result.Generation);
    assert(attachment->Binding.OwnerSecurityID == receiver->GlobalHandleID);
    assert(attachment->Binding.RequestId == 41 && attachment->Binding.Generation == first.Generation);
    assert(__LookupGlobalSafe(first.Handle)->References == 1);
    assert(__LookupGlobalSafe(receiver->GlobalHandleID)->References == 1);
    assert(DestroyHandle(first.Handle) == OS_ENOENT);
    result = __BindAttachment(manager, &parameters);
    assert(result.Handle == first.Handle && result.Generation == first.Generation);
    assert(__LookupGlobalSafe(first.Handle)->References == 1);

    // Failed copy-out of an identical retry drops only its temporary reference.
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOK);
    DMAManagerCancelBinding(&registration);
    assert(__LookupGlobalSafe(first.Handle) != NULL);
    assert(__LookupGlobalSafe(first.Handle)->References == 1);
    assert(attachment->Binding.Current->State == DMABindingLive);
    parameters.Principal = service->GlobalHandleID;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EPERMISSIONS);
    parameters.Principal = receiver->GlobalHandleID;
    parameters.RequestId = 40;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_ECANCELLED);
    parameters.RequestId = 42;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EBUSY);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EBUSY);
    assert(__RegisterAttachment(manager, &route) == id);
    route.Device = 102;
    assert(DMAManagerPrepareAttachment(manager, &route, &addition, &other) == OS_EBUSY);
    assert(addition == NULL);

    assert(AcquireGlobalHandle(first.Handle, HandleTypeDMABinding, (void**)&reference) == OS_EOK);
    assert(DestroyHandleInContext(receiver, first.Handle) == OS_EOK);
    assert(reference->State == DMABindingClosed && attachment->Binding.Current == reference);
    __ExpectNoWakeup(queue);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EBUSY);
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EBUSY);
    parameters.RequestId = 41;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_ECANCELLED);
    assert(ReleaseGlobalHandle(first.Handle) == OS_EOK);
    __Drain();
    assert(attachment->Binding.Current == NULL && attachment->Binding.RequestId == 41);
    __ExpectWakeup(queue, handle);
    parameters.RequestId = 42;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EBUSY);
    __ConsumeBindingIdle(manager, id, first.Generation);
    parameters.Principal = service->GlobalHandleID;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EPERMISSIONS);
    parameters.Principal = receiver->GlobalHandleID;
    result = __BindAttachment(manager, &parameters);
    assert(result.Generation > first.Generation);
    assert(DestroyHandleInContext(receiver, result.Handle) == OS_EOK);
    __Drain();
    __ExpectWakeup(queue, handle);
    __ConsumeBindingIdle(manager, id, result.Generation);
    assert(DMAManagerRemoveAttachment(manager, id) == OS_EOK);

    // A new attachment can select a new principal. Its generation remains newer
    // even when its request sequence restarts, including a service-local receiver.
    id = __RegisterAttachment(manager, &route);
    parameters = (OSDmaBindParameters_t) {
        .Attachment = id, .Principal = service->GlobalHandleID, .RequestId = 1
    };
    first = __BindAttachment(manager, &parameters);
    assert(first.Generation > result.Generation);
    manager->Registry.NextBindingGeneration = UINT64_MAX;
    result = __BindAttachment(manager, &parameters);
    assert(result.Handle == first.Handle && result.Generation == first.Generation);
    assert(DestroyHandle(first.Handle) == OS_EOK);
    __Drain();
    __ExpectWakeup(queue, handle);
    __ConsumeBindingIdle(manager, id, first.Generation);
    parameters.RequestId = 2;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOVERFLOW);
    assert(DestroyHandle(queue) == OS_EOK);
    __RemoveAllAttachments(manager);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

static void
__TestBindingCloseRaces(
    _In_ MSContextSecurity_t* receiver)
{
    DMAManager_t* manager;
    DMAAttachment_t* attachment;
    DMABindingRegistration_t* registration = NULL;
    OSDmaAttachmentParameters_t route = __Parameters(201, 1, 2);
    OSDmaBindParameters_t parameters;
    OSDmaBindingResult_t result;
    OSDmaBindingResult_t published;
    MSContextSecurity_t exiting;
    uuid_t handle;

    manager = __CreateManager(&handle);
    parameters = (OSDmaBindParameters_t) {
        .Attachment = __RegisterAttachment(manager, &route),
        .Principal = receiver->GlobalHandleID, .RequestId = 1
    };
    attachment = __FindTestAttachment(manager, parameters.Attachment);
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOK);
    assert(DestroyHandleInContext(receiver, result.Handle) == OS_EOK);
    assert(DMAManagerPublishBinding(&registration) == OS_ECANCELLED);
    DMAManagerCancelBinding(&registration);
    __Drain();
    assert(attachment->Binding.Current == NULL && attachment->Binding.RequestId == 0);
    assert(manager->PendingEvents.count == 0);

    // No permanent receiver context reference may keep group-exit callbacks from
    // closing a published binding. The fixture performs the real table cleanup.
    __NewGroup(&exiting);
    parameters.Principal = exiting.GlobalHandleID;
    result = __BindAttachment(manager, &parameters);
    assert(__LookupGlobalSafe(exiting.GlobalHandleID)->References == 1);
    DestroyHandleGrantsTable(exiting.HandleTable);
    assert(attachment->Binding.Current->State == DMABindingClosed);
    ReleaseGlobalHandle(exiting.GlobalHandleID);
    __Drain();
    assert(attachment->Binding.Current == NULL);
    __ConsumeBindingIdle(manager, parameters.Attachment, result.Generation);
    __RemoveAllAttachments(manager);

    parameters.Principal = receiver->GlobalHandleID;
    parameters.Attachment = __RegisterAttachment(manager, &route);
    route = __Parameters(202, 3, 4);
    __RegisterAttachment(manager, &route);
    published = __BindAttachment(manager, &parameters);
    parameters.Attachment++;
    assert(DMAManagerPrepareBinding(manager, &parameters, &registration, &result) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EOK);
    assert(manager->Registry.Attachments.head != NULL);
    attachment = manager->Registry.Attachments.head->value;
    assert(attachment->Binding.Current->State == DMABindingClosed);
    assert(DMAManagerPublishBinding(&registration) == OS_ECANCELLED);
    DMAManagerCancelBinding(&registration);
    assert(DestroyHandleInContext(receiver, published.Handle) == OS_EOK);
    __Drain();
    assert(manager->State == DMAManagerClosed && manager->PendingEvents.count == 0);
    assert(g_manager == manager && manager->Registry.Count == 2);

    // Test-only reboot cleanup of the metadata that production keeps until reboot.
    while (manager->Registry.Attachments.head != NULL) {
        attachment = manager->Registry.Attachments.head->value;
        assert(attachment->Binding.Current == NULL);
        list_remove(&manager->Registry.Attachments, &attachment->Link);
        kfree(attachment);
    }
    manager->Registry.Count = 0;
    while (manager->Registry.Groups.head != NULL) {
        DMARecoveryGroup_t* group = manager->Registry.Groups.head->value;

        list_remove(&manager->Registry.Groups, &group->Link);
        DMARecoveryGroupRelease(&group);
    }
    DMAManagerRelease(&manager);
    __Drain();
    assert(g_manager == NULL);
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
    __TestManagerLifetime(&service, &stranger);
    __TestManagerGroupExit();
    g_current = &service;
    __TestRegistrationValidation();
    __TestRecoveryGroups();
    __TestRegistryLimit();
    __TestEventDelivery(&service, &stranger);
    __TestClosedRegistry();
    __TestBindingRequests(&service, &stranger);
    __TestBindingCloseRaces(&stranger);
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(stranger.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(stranger.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("DMA attachments: lifetime, registration, groups and event delivery passed");
    return 0;
}
