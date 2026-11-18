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

#define DMA_BOUND_REAL_SHM
#define DMA_TEST_EXTERNAL_ENGINE
#define HANDLE_TEST_REAL_SHM
#include "dma_attachment_support.inc"
#define SHM_TEST_REAL_HANDLES
#include "shm_device_support.inc"

#include "../kernel/memory/shm/device/private.h"
#include <stdarg.h>

extern _Thread_local atomic_int g_rangeWaitError;

// Capture the final diagnostic so failures must identify the operation that
// failed. The fixtures still run the production cleanup and admission paths.
// g_boundErrorHistory keeps the messages since a test last cleared it, one per
// line, so a test can check that each failed call was logged on its own. It
// starts over when full. Tests clear it right before the calls they check.
static char g_lastBoundError[512];
static char g_boundErrorHistory[8192];
void TestDmaError(const char* format, ...)
{
    va_list arguments;
    size_t used = strlen(g_boundErrorHistory);
    int length;

    va_start(arguments, format);
    length = vsnprintf(g_lastBoundError, sizeof(g_lastBoundError), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(g_lastBoundError));
    if (used + (size_t)length + 2 > sizeof(g_boundErrorHistory)) {
        used = 0;
    }
    snprintf(g_boundErrorHistory + used, sizeof(g_boundErrorHistory) - used, "%s\n", g_lastBoundError);
}

oserr_t CapabilityVerify(Capabilities_t capability)
{ (void)capability; return OS_EOK; }
oserr_t MemorySpaceCopyFromUser(void* to, const void* from, size_t size)
{ return __CopyProviderUser((void*)from, to, size, false); }
static void (*g_boundCopyHook)(void);
static oserr_t (*g_boundCopyOverride)(void*, const void*, size_t);
oserr_t MemorySpaceCopyToUser(void* to, const void* from, size_t size)
{
    oserr_t status;

    if (g_boundCopyOverride != NULL) {
        return g_boundCopyOverride(to, from, size);
    }
    status = __CopyProviderUser(to, (void*)from, size, true);
    if (status == OS_EOK && g_boundCopyHook != NULL) {
        void (*hook)(void) = g_boundCopyHook;
        g_boundCopyHook = NULL;
        hook();
    }
    return status;
}
#ifndef DMA_API_SOURCE
#define DMA_API_SOURCE "../kernel/api/dma.c"
#endif
#include DMA_API_SOURCE
#ifndef SHM_API_SOURCE
#define SHM_API_SOURCE "../kernel/api/shm.c"
#endif
#include SHM_API_SOURCE
#include "../kernel/api/dma_attachment.c"

static void
__DeleteTestBuffer(void* resource)
{
    assert(resource == g_buffer);
    MutexDestruct(&g_buffer->Mutex);
    if (g_buffer->Regions != NULL) {
        kfree(g_buffer->Regions);
    }
    free(g_buffer);
    g_buffer = NULL;
}

static void
__RegisterTestBuffer(void)
{
    g_buffer->ID = CreateHandle(HandleTypeSHM, NULL, __DeleteTestBuffer, g_buffer);
    assert(g_buffer->ID != UUID_INVALID);
    MutexConstruct(&g_buffer->Mutex, MUTEX_FLAG_PLAIN);
}

#ifndef DMA_TEST_REAL_DEVICE_BUFFER
oserr_t
SHMCreateDeviceBuffer(size_t size, size_t alignment, size_t boundary, SHMHandle_t* handle)
{
    oserr_t status = __CreateDeviceBuffer(size, alignment, boundary, handle);

    if (status == OS_EOK) {
        __RegisterTestBuffer();
        handle->ID = g_buffer->ID;
    }
    return status;
}

oserr_t
SHMDetach(SHMHandle_t* handle)
{
    return DestroyHandle(handle->ID);
}

#endif

static void
__TestNativeBoundStop(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    DMAManager_t* manager;
    uuid_t managerHandle;
    OSDmaAttachmentParameters_t route = __Parameters(500, 1, 1);
    OSDmaBindParameters_t request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    OSDmaBindingResult_t bound;
    OSDmaBoundOpenParameters_t open;
    SHMDeviceContext_t* context = NULL;
    SHMDeviceContext_t* duplicate = NULL;
    SHMDeviceMapping_t* mapping = NULL;
    SHMDeviceOperation_t* operation = NULL;
    SHMRecord_t contextRecord = { .State = SHMRecordUpdating };
    SHMRecord_t mappingRecord = { .State = SHMRecordUpdating };
    OSDmaStopTicket_t ticket;
    OSDmaStopTicket_t repeat;
    DMAAttachmentEventBatch_t events;
    paddr_t pages[] = { 0x1000 };
    uuid_t buffer;

    g_current = service;
    manager = __CreateManager(&managerHandle);
    route.Ranges[0] = (struct SHMDeviceRange) { 0x1000, 0x4000, 0x1000 };
    route.AddressLimit = 0x4fff;
    request.Attachment = __RegisterAttachment(manager, &route);
    bound = __BindAttachment(manager, &request);
    open = (OSDmaBoundOpenParameters_t) { bound.Generation, UINT64_MAX };
    assert(SHMDeviceContextPrepareBound(bound.Handle, &open, &context) == OS_ENOENT);
    g_current = driver;
    assert(SHMDeviceContextPrepareBound(bound.Handle, &open, &context) == OS_EOK);
    assert(context->AddressLimit == route.AddressLimit);
    assert(SHMDeviceContextPrepareBound(bound.Handle, &open, &duplicate) == OS_EBUSY);
    assert(SHMDeviceContextPublishBound(context) == OS_EOK);
    contextRecord.Object = context;
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    buffer = g_buffer->ID;
    assert(SHMDeviceMap(NULL, context, buffer, 0, 256, DMAFromDevice,
        SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EINVALPARAMS);
    assert(mapping == NULL && context->Mappings == NULL);
    assert(SHMDeviceContextBeginOperation(&contextRecord, context, SHMDeviceOperationSubmit, &operation) == OS_EOK);
    assert(SHMDeviceMap(operation, context, buffer, 0, 256, DMAFromDevice, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    // A construction token may inspect or roll back its new mapping, but it
    // cannot authorize a transfer on the mapping's separately serialized record.
    assert(SHMDeviceSyncForDevice(operation, mapping, 64, 64, NULL) == OS_EINVALPARAMS);
    SHMDeviceOperationEnd(&operation);
    mappingRecord.Object = mapping;
    assert(SHMDeviceSyncForDevice(NULL, mapping, 64, 64, NULL) == OS_EINVALPARAMS);
    assert(SHMDeviceUnmap(NULL, &mapping) == OS_EINVALPARAMS);
    assert(SHMDeviceMappingBeginOperation(&mappingRecord, mapping, SHMDeviceOperationSubmit, &operation) == OS_EOK);
    assert(SHMDeviceSyncForDevice(operation, mapping, 64, 64, NULL) == OS_EOK);

    g_current = service;
    assert(DMAManagerPrepareStop(manager, request.Attachment, &ticket) == OS_EOK);
    assert(SHMDeviceOperationCheck(operation) == OS_ECANCELLED);
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EBUSY);
    assert(g_buffer->Access.Faults == 1);
    SHMDeviceOperationEnd(&operation);
    assert(DMAManagerPrepareStop(manager, request.Attachment, &repeat) == OS_EOK);
    assert(memcmp(&ticket, &repeat, sizeof(ticket)) == 0);
    g_cacheCallCount = 0;
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EOK);
    assert(mapping->State == SHMDeviceMappingStopped && g_buffer->Access.Faults == 0);
    assert(g_cacheCallCount == 1 && g_cacheCalls[0].Length == 256);
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EOK && g_cacheCallCount == 1);
    assert(SHMDeviceMappingBeginOperation(&mappingRecord, mapping, SHMDeviceOperationSubmit, &operation) == OS_ECANCELLED);
    assert(SHMDeviceContextClose(&context) == OS_EBUSY);
    assert(SHMDeviceMappingBeginOperation(&mappingRecord, mapping, SHMDeviceOperationRelease, &operation) == OS_EOK);
    assert(SHMDeviceUnmap(operation, &mapping) == OS_EOK);
    SHMDeviceOperationEnd(&operation);
    assert(SHMDeviceContextClose(&context) == OS_EOK);

    g_current = driver;
    assert(DestroyHandle(buffer) == OS_EOK);
    assert(DestroyHandle(bound.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(manager, 32, &events) == OS_EOK);
    assert(events.Count == 1 && events.Entries[0].StopEpoch == ticket.Epoch);
    assert((events.Entries[0].Reasons & DMA_ATTACHMENT_IDLE) != 0);
    assert(DMAManagerFinishEvents(manager, &events, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(manager, request.Attachment) == OS_EOK);
    assert(DestroyHandle(managerHandle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

// Each case starts with an independent manager/group. Tests release resources
// through the real interfaces, then check the global handle/allocation totals.
typedef struct __BoundFixture {
    MSContextSecurity_t* Service;
    MSContextSecurity_t* Driver;
    DMAManager_t*        Manager;
    uuid_t               ManagerHandle;
    OSDmaAttachmentId_t  Attachment;
    OSDmaBindingResult_t Binding;
    uuid_t               ContextHandle;
    SHMDeviceContext_t*  Context;
} __BoundFixture_t;

static __BoundFixture_t* g_copyFixture;
static OSDmaStopTicket_t g_copyTicket;
static bool g_failAddressRelease;
static unsigned int g_addressReleaseCalls;
// Linker seams run callbacks at named production transitions. Clear each hook
// before calling it because stop preparation may itself allocate or clean.
static void (*g_constructionHook)(void);
static void (*g_beforeLinkHook)(void);
static void (*g_afterLinkHook)(void);
static void (*g_releaseHook)(void);
static void (*g_afterRevokeHook)(void);

static void
__RunHook(void (**slot)(void))
{
    void (*hook)(void) = *slot;

    *slot = NULL;
    if (hook != NULL) {
        hook();
    }
}

// The sizes of the most recent heap allocations, so a fixture can check that a
// list handed to it is long enough before it reads the list.
struct __RecentAllocation {
    void*  Pointer;
    size_t Size;
};
static struct __RecentAllocation g_recentAllocations[32];
static atomic_uint               g_recentAllocationIndex;

void* __real_kmalloc(size_t size);
void* __wrap_kmalloc(size_t size)
{
    void*        pointer;
    unsigned int slot;

    __RunHook(&g_constructionHook);
    pointer = __real_kmalloc(size);
    if (pointer != NULL) {
        slot = atomic_fetch_add(&g_recentAllocationIndex, 1) % 32;
        g_recentAllocations[slot] = (struct __RecentAllocation){ pointer, size };
    }
    return pointer;
}

// Search newest first, because freed memory can come back at the same address.
static size_t
__RecentAllocationSize(const void* pointer)
{
    unsigned int newest = atomic_load(&g_recentAllocationIndex);

    for (unsigned int i = 1; i <= 32; i++) {
        const struct __RecentAllocation* entry = &g_recentAllocations[(newest - i) % 32];

        if (entry->Pointer == pointer) {
            return entry->Size;
        }
    }
    return 0;
}
void __real_SHMDeviceMappingLink(SHMDeviceMapping_t* mapping);
void __wrap_SHMDeviceMappingLink(SHMDeviceMapping_t* mapping)
{
    __RunHook(&g_beforeLinkHook);
    __real_SHMDeviceMappingLink(mapping);
    __RunHook(&g_afterLinkHook);
}
oserr_t __real_DMAAddressMappingRelease(struct DMAAddressMapping** mapping);
oserr_t __wrap_DMAAddressMappingRelease(struct DMAAddressMapping** mapping)
{
    g_addressReleaseCalls++;
    __RunHook(&g_releaseHook);
    return g_failAddressRelease ? OS_EUNKNOWN : __real_DMAAddressMappingRelease(mapping);
}

oserr_t __real_SHMDeviceContextRevoke(SHMDeviceContext_t* context);
oserr_t __wrap_SHMDeviceContextRevoke(SHMDeviceContext_t* context)
{
    oserr_t status = __real_SHMDeviceContextRevoke(context);

    __RunHook(&g_afterRevokeHook);
    return status;
}

static __BoundFixture_t
__PrepareBoundPolicyFixture(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool coherent)
{
    __BoundFixture_t fixture = { .Service = service, .Driver = driver };
    OSDmaAttachmentParameters_t route = __Parameters(700, 20, 30);
    OSDmaBindParameters_t request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };

    g_current = service;
    fixture.Manager = __CreateManager(&fixture.ManagerHandle);
    route.Ranges[0] = (struct SHMDeviceRange) { 0x1000, 0x4000, 0x1000 };
    route.AddressLimit = 0x4fff;
    route.CachePolicy = coherent ? SHMDeviceCacheCoherent : SHMDeviceCacheNonCoherent;
    fixture.Attachment.Value = __RegisterAttachment(fixture.Manager, &route);
    request.Attachment = fixture.Attachment.Value;
    fixture.Binding = __BindAttachment(fixture.Manager, &request);
    g_current = driver;
    return fixture;
}

static __BoundFixture_t
__PrepareBoundFixture(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    return __PrepareBoundPolicyFixture(service, driver, false);
}

static void
__OpenBoundFixture(__BoundFixture_t* fixture)
{
    OSDmaBoundOpenParameters_t open = { fixture->Binding.Generation, UINT64_MAX };
    SHMRecord_t* record;

    assert(ScDmaOpenBound(fixture->Binding.Handle, &open, &fixture->ContextHandle) == OS_EOK);
    record = LookupHandle(fixture->ContextHandle, HandleTypeSHMAccess);
    assert(record != NULL && record->PendingOperation == NULL);
    fixture->Context = record->Object;
}

static struct OSDmaMappingResult
__MapBoundFixture(__BoundFixture_t* fixture, unsigned int flags)
{
    paddr_t pages[] = { 0x1000 };
    struct OSDmaMapParameters parameters = {
        .Offset = 0, .Length = 256, .Direction = DMAFromDevice, .Flags = flags
    };
    struct OSDmaMappingResult result;

    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    assert(ScDmaMapShm(fixture->ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    return result;
}

static void
__FinishBoundFixture(__BoundFixture_t* fixture)
{
    DMAAttachmentEventBatch_t batch;
    OSDmaStopTicket_t ticket;

    g_current = fixture->Service;
    assert(DMAManagerPrepareStop(fixture->Manager, fixture->Attachment.Value, &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture->Manager, &ticket) == OS_EOK);
    g_current = fixture->Driver;
    if (fixture->ContextHandle != 0) {
        assert(ScResourceRelease(fixture->ContextHandle) == OS_EOK);
    }
    if (g_buffer != NULL) {
        assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    }
    assert(DestroyHandle(fixture->Binding.Handle) == OS_EOK);
    __Drain();
    g_current = fixture->Service;
    assert(DMAManagerSnapshotEvents(fixture->Manager, 32, &batch) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture->Manager, &batch, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(fixture->Manager, fixture->Attachment.Value) == OS_EOK);
    assert(DestroyHandle(fixture->ManagerHandle) == OS_EOK);
    DMAManagerRelease(&fixture->Manager);
    __Drain();
}

static void
__StopDuringCopy(void)
{
    assert(DMAManagerPrepareStop(g_copyFixture->Manager, g_copyFixture->Attachment.Value, &g_copyTicket) == OS_EOK);
}

static void
__TestBoundOpenFailures(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    OSDmaBoundOpenParameters_t open = { 0, UINT64_MAX };
    uuid_t output = 0xfeed;
    SHMRecord_t* record;

    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_EINVALPARAMS);
    assert(output == 0xfeed);
    open.Generation = fixture.Binding.Generation + (1ULL << 32);
    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_ECANCELLED);
    open.Generation = fixture.Binding.Generation;
    // Keep the real driver grant valid while testing the separate principal
    // comparison. A grant alone must never authorize another principal.
    __FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->Binding.Current->OwnerSecurityID = service->GlobalHandleID;
    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_EPERMISSIONS);
    __FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->Binding.Current->OwnerSecurityID = driver->GlobalHandleID;
    g_failCopy = 1;
    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_EINVALPARAMS);
    assert(strstr(g_lastBoundError, "ScDmaOpenBound: MemorySpaceCopyToUser") != NULL);
    g_failCopy = -1;
    __Drain();
    assert(fixture.Manager->Registry.Attachments.head != NULL);
    assert(__FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->RecoveryGroup->Access.Operations == 0);
    __OpenBoundFixture(&fixture);
    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_EBUSY);
    record = LookupHandle(fixture.ContextHandle, HandleTypeSHMAccess);
    assert(record->Object == fixture.Context && fixture.Context->AddressLimit == 0x4fff);
    __FinishBoundFixture(&fixture);

    fixture = __PrepareBoundFixture(service, driver);
    open.Generation = fixture.Binding.Generation;
    g_copyFixture = &fixture;
    g_boundCopyHook = __StopDuringCopy;
    assert(ScDmaOpenBound(fixture.Binding.Handle, &open, &output) == OS_ECANCELLED);
    __Drain();
    assert(__FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->Binding.Current->Context.Pointer == NULL);
    assert(__FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->RecoveryGroup->Access.Operations == 0);
    __FinishBoundFixture(&fixture);
}

static void
__TestPreparingOpenDrain(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    OSDmaBoundOpenParameters_t open = { fixture.Binding.Generation, UINT64_MAX };
    SHMDeviceContext_t* context = NULL;
    OSDmaStopTicket_t ticket;
    DMAAttachmentEventBatch_t batch;

    assert(SHMDeviceContextPrepareBound(fixture.Binding.Handle, &open, &context) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY);
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &batch) == OS_EOK && batch.Count == 1);
    assert(DMAManagerFinishEvents(fixture.Manager, &batch, true) == OS_EOK);
    assert(fixture.Manager->PendingEvents.count == 0);
    SHMDeviceContextCancelBound(&context);
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &batch) == OS_EOK && batch.Count == 1);
    assert(batch.Entries[0].StopEpoch == ticket.Epoch && (batch.Entries[0].Reasons & DMA_ATTACHMENT_STOP_REQUIRED));
    __FinishBoundFixture(&fixture);
}

static void
__TestBoundCopyAndRetry(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMDeviceMapping_t* mapping;
    OSDmaStopTicket_t ticket;
    OSDmaStopTicket_t wrong;
    uint32_t count = 0;
    unsigned int releases;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    g_copyFixture = &fixture;
    g_boundCopyHook = __StopDuringCopy;
    assert(ScDmaGetSegments(result.Handle, NULL, 0, &count) == OS_ECANCELLED);
    assert(mapping->Access.Cancelled && SHMAccessGetPhase(&mapping->Access) == SHMAccessQuarantined);
    assert(ScResourceRelease(result.Handle) == OS_EBUSY);
    g_current = service;
    g_failCopy = 1;
    assert(ScDmaAttachmentPrepareStop(fixture.ManagerHandle, &fixture.Attachment, &ticket) == OS_EINVALPARAMS);
    assert(strstr(g_lastBoundError, "ScDmaAttachmentPrepareStop: MemorySpaceCopyToUser") != NULL);
    g_failCopy = -1;
    assert(ScDmaAttachmentPrepareStop(fixture.ManagerHandle, &fixture.Attachment, &ticket) == OS_EOK);
    assert(memcmp(&ticket, &g_copyTicket, sizeof(ticket)) == 0);
    wrong = ticket;
    wrong.Epoch++;
    assert(ScDmaAttachmentCompleteStop(fixture.ManagerHandle, &wrong) == OS_ECANCELLED);
    atomic_store(&mapping->Lifetime.References, UINT_MAX);
    assert(ScDmaAttachmentCompleteStop(fixture.ManagerHandle, &ticket) == OS_EOVERFLOW);
    assert(__FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->RecoveryGroup->Access.State == DMARecoveryBlocked);
    atomic_store(&mapping->Lifetime.References, 1);
    g_cacheCallCount = 0;
    g_failAddressRelease = true;
    assert(ScDmaAttachmentCompleteStop(fixture.ManagerHandle, &ticket) == OS_EUNKNOWN);
    assert(mapping->State == SHMDeviceMappingReleasingAfterStop && g_buffer->Access.Faults == 1);
    assert(g_cacheCallCount == 1 && g_cacheCalls[0].Length == 256);
    releases = g_addressReleaseCalls;
    g_failAddressRelease = false;
    g_current = driver;
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(g_addressReleaseCalls == releases + 1 && g_cacheCallCount == 1);
    __Drain();
    assert(g_buffer->Access.Faults == 0);
    __FinishBoundFixture(&fixture);
}

static void
__FinishStoppedRegistry(__BoundFixture_t* fixture)
{
    DMAAttachmentEventBatch_t batch;

    __Drain();
    g_current = fixture->Service;
    assert(DMAManagerSnapshotEvents(fixture->Manager, 32, &batch) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture->Manager, &batch, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(fixture->Manager, fixture->Attachment.Value) == OS_EOK);
    assert(DestroyHandle(fixture->ManagerHandle) == OS_EOK);
    DMAManagerRelease(&fixture->Manager);
    __Drain();
}

static void
__TestStoppedOperation(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool rangeWait)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMRecord_t* record;
    SHMDeviceOperation_t* operation = NULL;
    struct SHMAccess cpu;
    struct SHMAccessBeginParameters request = {
        .Kind = DMAAccessTransfer, .Mode = SHMWaitUntilAvailable, .Length = 256
    };
    OSDmaStopTicket_t ticket;
    HandleGrantsTable_t* table;
    DMARecoveryGroup_t* group;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    record = LookupHandle(result.Handle, HandleTypeSHMAccess);
    group = fixture.Context->Binding->Group;
    if (rangeWait) {
        SHMAccessInitialize(&cpu, g_buffer, 0, 256);
        assert(SHMAccessBegin(&cpu, 0, 256, SHMAccessHolderCpu, true, NULL) == OS_EOK);
        request.Deadline = *OS_DEADLINE_INFINITE;
        // The host seam models the forked parent's return. No child can resume
        // when the real grant table is torn down below.
        atomic_store(&g_rangeWaitError, OS_EFORKED);
        assert(ScShmAccessBegin(NULL, result.Handle, &request) == OS_EFORKED);
        assert(record->PendingOperation != NULL);
        assert(((SHMDeviceOperation_t*)record->PendingOperation)->Phase == SHMDeviceOperationRangeAdmission);
    } else {
        assert(SHMRecordBegin(result.Handle, &g_mappingOps, &record) == OS_EOK);
        assert(SHMDeviceMappingBeginOperation(record, record->Object, SHMDeviceOperationSubmit, &operation) == OS_EOK);
        assert(operation->Phase == SHMDeviceOperationRunning);
    }
    assert(group->Access.Operations == 1);
    table = driver->HandleTable;
    DestroyHandleGrantsTable(table);
    driver->HandleTable = CreateHandleGrantsTable();
    __Drain();
    assert(group->Access.State == DMARecoveryBlocked && g_buffer->Access.Faults == 1);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    if (rangeWait) {
        assert(group->Access.Operations == 0);
        SHMAccessEnd(&cpu);
    } else {
        assert(group->Access.Operations == 1 && record->PendingOperation == operation);
        assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY);
        // Test-only reboot disposal after proving production refuses to infer
        // an unwind. The stopped stack is never resumed by this fixture.
        SHMDeviceOperationEnd(&operation);
        assert(__ReleaseObject(record, record->Ops->Cleanup) == OS_EOK);
        kfree(record);
    }
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    __FinishStoppedRegistry(&fixture);
    assert(g_buffer == NULL);
}

static void
__TestMapCopyClosure(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    paddr_t pages[] = { 0x1000 };
    struct OSDmaMapParameters parameters = {
        .Offset = 0, .Length = 256, .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT
    };
    struct OSDmaMappingResult result;

    __OpenBoundFixture(&fixture);
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    g_copyFixture = &fixture;
    g_boundCopyHook = __StopDuringCopy;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_ECANCELLED);
    __Drain();
    assert(fixture.Context->Mappings != NULL && fixture.Context->Mappings->Lifetime.Abandoned);
    assert(g_buffer->Access.Faults == 1 && fixture.Context->Binding->Group->Access.Operations == 0);
    __FinishBoundFixture(&fixture);
}

static void
__TestGroupEpochs(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    OSDmaAttachmentParameters_t route = __Parameters(701, 20, 31);
    OSDmaAttachmentId_t peer;
    OSDmaBindingResult_t peerBinding;
    OSDmaBindParameters_t bind = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    DMAAttachmentEventBatch_t before;
    DMAAttachmentEventBatch_t after;
    OSDmaStopTicket_t ticket;
    OSDmaStopTicket_t next;
    DMARecoveryGroup_t* group;
    uint64_t peerGeneration;

    // Topology changes require no current bindings, so prepare the connected
    // peer in a fresh registry before binding either member.
    assert(DestroyHandle(fixture.Binding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &before) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture.Manager, &before, true) == OS_EOK);
    peer.Value = __RegisterAttachment(fixture.Manager, &route);
    bind.Attachment = fixture.Attachment.Value;
    bind.RequestId = 2;
    fixture.Binding = __BindAttachment(fixture.Manager, &bind);
    bind.Attachment = peer.Value;
    bind.RequestId = 1;
    peerBinding = __BindAttachment(fixture.Manager, &bind);
    peerGeneration = peerBinding.Generation;
    g_current = driver;
    __OpenBoundFixture(&fixture);
    assert(DestroyHandle(peerBinding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &before) == OS_EOK && before.Count == 1);
    assert(before.Entries[0].StopEpoch == 0);
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture.Manager, &before, true) == OS_EOK);
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &after) == OS_EOK && after.Count == 2);
    assert(after.Entries[0].Generation == peerGeneration && after.Entries[0].StopEpoch == ticket.Epoch);
    assert(after.Entries[0].Reasons == (DMA_ATTACHMENT_IDLE | DMA_ATTACHMENT_STOP_REQUIRED));
    assert(after.Sequences[0] > before.Sequences[0]);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(DMAManagerRemoveAttachment(fixture.Manager, peer.Value) == OS_EBUSY);
    g_current = driver;
    assert(ScResourceRelease(fixture.ContextHandle) == OS_EOK);
    fixture.ContextHandle = 0;
    assert(DestroyHandle(fixture.Binding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &after) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture.Manager, &after, true) == OS_EOK);
    bind.Attachment = fixture.Attachment.Value;
    bind.RequestId = 3;
    fixture.Binding = __BindAttachment(fixture.Manager, &bind);
    group = __FindTestAttachment(fixture.Manager, fixture.Attachment.Value)->RecoveryGroup;
    assert(group->Access.State == DMARecoveryLive && group->Stop.Round == ticket.Epoch + 1);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &next) == OS_EOK);
    assert(next.RecoveryGroup == ticket.RecoveryGroup && next.Epoch == ticket.Epoch + 1);
    assert(DMAManagerCompleteStop(fixture.Manager, &next) == OS_EOK);
    g_current = driver;
    assert(DestroyHandle(fixture.Binding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &after) == OS_EOK);
    assert(DMAManagerFinishEvents(fixture.Manager, &after, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(fixture.Manager, peer.Value) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &next) == OS_ENOENT);
    __FinishStoppedRegistry(&fixture);
}

typedef struct __BoundWait {
    SHMRecord_t* Record;
    atomic_bool      Returned;
    atomic_bool      Finish;
    oserr_t          Status;
} __BoundWait_t;

static void*
__RunBoundWait(void* argument)
{
    __BoundWait_t* wait = argument;
    SHMDeviceOperation_t* operation = NULL;
    struct SHMAccessWait accessWait = { .Blocking = true };

    assert(SHMDeviceMappingBeginOperation(wait->Record, wait->Record->Object,
        SHMDeviceOperationSubmit, &operation) == OS_EOK);
    wait->Status = SHMDeviceSyncForDevice(operation, wait->Record->Object, 64, 64, &accessWait);
    atomic_store(&wait->Returned, true);
    while (!atomic_load(&wait->Finish)) {
        sched_yield();
    }
    SHMDeviceOperationEnd(&operation);
    return NULL;
}

static void
__TestConcurrentBoundWait(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    struct SHMAccess cpu;
    __BoundWait_t wait = { 0 };
    pthread_t thread;
    OSDmaStopTicket_t ticket;
    bool waiting;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    SHMAccessInitialize(&cpu, g_buffer, 0, 256);
    assert(SHMAccessBegin(&cpu, 0, 256, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    assert(SHMRecordBegin(result.Handle, &g_mappingOps, &wait.Record) == OS_EOK);
    assert(pthread_create(&thread, NULL, __RunBoundWait, &wait) == 0);
    do {
        MutexLock(&g_buffer->Mutex);
        waiting = g_buffer->Access.Waiting == 1;
        MutexUnlock(&g_buffer->Mutex);
        sched_yield();
    } while (!waiting);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    while (!atomic_load(&wait.Returned)) {
        sched_yield();
    }
    assert(wait.Status == OS_ECANCELLED && g_buffer->Access.Faults == 1);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY);
    atomic_store(&wait.Finish, true);
    assert(pthread_join(thread, NULL) == 0);
    g_current = driver;
    SHMRecordEnd(wait.Record);
    SHMAccessEnd(&cpu);
    g_current = service;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    g_current = driver;
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}

static void
__DisposeClosedFixture(__BoundFixture_t* fixture, SHMDeviceContext_t* context)
{
    DMARecoveryGroup_t* group = context->Binding->Group;
    DMAAttachment_t*    attachment;

    // Production leaves this state until reboot. The fixture now supplies a
    // separate stop/exclusive interval only to dispose of its simulated memory.
    assert(group->Access.Operations == 0);
    assert(SHMDeviceContextCompleteStop(context) == OS_EOK);
    SHMDeviceContextRelease(&context);
    __Drain();
    attachment = fixture->Manager->Registry.Attachments.head->value;
    assert(attachment->Binding.Current == NULL && g_buffer == NULL);
    if (attachment->Event.Queued) {
        list_remove(&fixture->Manager->PendingEvents, &attachment->Event.Link);
    }
    list_remove(&fixture->Manager->Registry.Attachments, &attachment->Link);
    list_remove(&fixture->Manager->Registry.Groups, &group->Link);
    fixture->Manager->Registry.Count = 0;
    kfree(attachment);
    DMARecoveryGroupRelease(&group);
    DMAManagerRelease(&fixture->Manager);
    __Drain();
}

static void
__TestManagerLoss(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool confirmed)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMDeviceContext_t* context;
    DMARecoveryGroup_t* group;
    OSDmaStopTicket_t ticket;
    DMAManager_t* other = NULL;
    uuid_t unused;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    context = fixture.Context;
    group = context->Binding->Group;
    // Keep a native reference solely for test-only reboot disposal below.
    assert(SHMDeviceContextAcquire(context) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    if (confirmed) {
        g_failAddressRelease = true;
        assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EUNKNOWN);
        g_failAddressRelease = false;
    }
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    assert(group->Access.ManagerClosed);
    assert(group->Access.State == (confirmed ? DMARecoveryStopConfirmed : DMARecoveryBlocked));
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    assert(DMAManagerPrepareCreate(&other, &unused) == OS_EBUSY && other == NULL);
    g_current = driver;
    assert(ScResourceRelease(result.Handle) == OS_ECANCELLED);
    assert(DestroyHandle(result.Handle) == OS_EOK);
    assert(DestroyHandle(fixture.ContextHandle) == OS_EOK);
    assert(DestroyHandle(fixture.Binding.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(context->Mappings != NULL && g_buffer->Access.Faults == 1);

    __DisposeClosedFixture(&fixture, context);
}

typedef struct __ReleaseDuringCancel {
    uuid_t      Mapping;
    atomic_bool Entered;
    atomic_bool Continue;
    atomic_bool Returned;
    oserr_t     Status;
} __ReleaseDuringCancel_t;

static __ReleaseDuringCancel_t* g_cancellingRelease;

static void
__WaitForCancellation(void)
{
    atomic_store(&g_cancellingRelease->Entered, true);
    while (!atomic_load(&g_cancellingRelease->Continue)) {
        sched_yield();
    }
}

static void
__FinishReleaseDuringCancellation(void)
{
    // Revoke has observed Cleaning and returned busy. Complete the release
    // before recovery stores that result, reproducing the lost-retry window.
    atomic_store(&g_cancellingRelease->Continue, true);
    while (!atomic_load(&g_cancellingRelease->Returned)) {
        sched_yield();
    }
}

static void*
__RunReleaseDuringCancellation(void* argument)
{
    __ReleaseDuringCancel_t* release = argument;

    release->Status = ScResourceRelease(release->Mapping);
    atomic_store(&release->Returned, true);
    return NULL;
}

static void
__TestCancellationRetryOnManagerLoss(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult waitingMapping;
    struct OSDmaMappingResult releasingMapping;
    struct OSDmaMapParameters parameters = {
        .Offset = 512, .Length = 256, .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT
    };
    struct SHMAccess        cpu;
    __BoundWait_t           wait = { 0 };
    __ReleaseDuringCancel_t release = { 0 };
    SHMDeviceMapping_t*     mapping;
    DMARecoveryGroup_t*     group;
    pthread_t              waiterThread;
    pthread_t              releaseThread;
    bool                   waiting;

    __OpenBoundFixture(&fixture);
    waitingMapping = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &releasingMapping) == OS_EOK);
    group = fixture.Context->Binding->Group;
    assert(SHMDeviceContextAcquire(fixture.Context) == OS_EOK);
    SHMAccessInitialize(&cpu, g_buffer, 0, 256);
    assert(SHMAccessBegin(&cpu, 0, 256, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    assert(SHMRecordBegin(waitingMapping.Handle, &g_mappingOps, &wait.Record) == OS_EOK);
    mapping = wait.Record->Object;
    assert(pthread_create(&waiterThread, NULL, __RunBoundWait, &wait) == 0);
    do {
        MutexLock(&g_buffer->Mutex);
        waiting = g_buffer->Access.Waiting == 1;
        MutexUnlock(&g_buffer->Mutex);
        sched_yield();
    } while (!waiting);

    release.Mapping = releasingMapping.Handle;
    g_cancellingRelease = &release;
    g_releaseHook = __WaitForCancellation;
    g_afterRevokeHook = __FinishReleaseDuringCancellation;
    assert(pthread_create(&releaseThread, NULL, __RunReleaseDuringCancellation, &release) == 0);
    while (!atomic_load(&release.Entered)) {
        sched_yield();
    }
    assert(DestroyHandleInContext(service, fixture.ManagerHandle) == OS_EOK);
    assert(group->Access.ManagerClosed && group->Stop.Phase == DMARecoveryStopIdle);
    assert(group->Stop.CancellationError == OS_EOK);
    assert(SHMAccessGetPhase(&mapping->Access) == SHMAccessQuarantined);
    assert(pthread_join(releaseThread, NULL) == 0 && release.Status == OS_EOK);
    while (!atomic_load(&wait.Returned)) {
        sched_yield();
    }
    assert(wait.Status == OS_ECANCELLED);
    atomic_store(&wait.Finish, true);
    assert(pthread_join(waiterThread, NULL) == 0);
    SHMRecordEnd(wait.Record);
    SHMAccessEnd(&cpu);
    assert(DestroyHandle(waitingMapping.Handle) == OS_EOK);
    assert(DestroyHandle(fixture.ContextHandle) == OS_EOK);
    assert(DestroyHandle(fixture.Binding.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    __DisposeClosedFixture(&fixture, fixture.Context);
    g_cancellingRelease = NULL;
}

static void
__TestReleaseSkippedMapping(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMDeviceMapping_t* mapping;
    SHMDeviceMapping_t* temporary;
    OSDmaStopTicket_t ticket;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    assert(SHMDeviceMappingAcquire(mapping) == OS_EOK);
    temporary = mapping;
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    g_cacheCallCount = 0;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EBUSY);
    assert(mapping->State == SHMDeviceMappingReady && g_cacheCallCount == 0);
    assert(fixture.Context->Binding->Group->Access.State == DMARecoveryStopConfirmed);
    SHMDeviceMappingRelease(&temporary);
    g_current = driver;
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(g_cacheCallCount == 1 && g_cacheCalls[0].Length == 256);
    assert(g_buffer->Access.Faults == 0);
    __FinishBoundFixture(&fixture);
}

// Every callback runs inside an admitted operation. Even when cancellation
// cannot inspect a mapping being released, stop confirmation must wait for it.
static void
__StopDuringOperation(void)
{
    __StopDuringCopy();
    assert(g_copyFixture->Context->Binding->Group->Access.Operations == 1);
    assert(DMAManagerCompleteStop(g_copyFixture->Manager, &g_copyTicket) == OS_EBUSY);
}

static void
__TestMapConstructionClosure(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    for (int point = 0; point < 3; point++) {
        __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
        struct OSDmaMapParameters parameters = {
            .Length = 256, .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT
        };
        struct OSDmaMappingResult result;
        paddr_t pages[] = { 0x1000 };

        __OpenBoundFixture(&fixture);
        __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
        __RegisterTestBuffer();
        g_copyFixture = &fixture;
        if (point == 0) {
            g_constructionHook = __StopDuringOperation;
        } else if (point == 1) {
            g_beforeLinkHook = __StopDuringOperation;
        } else {
            g_afterLinkHook = __StopDuringOperation;
        }
        assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_ECANCELLED);
        assert(g_constructionHook == NULL && g_beforeLinkHook == NULL && g_afterLinkHook == NULL);
        assert(fixture.Context->Binding->Group->Access.Operations == 0);
        assert(g_buffer->Access.Waiting == 0 && g_buffer->Access.Faults == 1);
        assert(fixture.Context->Mappings->Lifetime.Abandoned);
        __FinishBoundFixture(&fixture);
    }
}

static void
__TestCacheAndReleaseClosure(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    for (int point = 0; point < 3; point++) {
        __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
        struct OSDmaMappingResult result;
        struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer, .Length = 256 };

        __OpenBoundFixture(&fixture);
        result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
        g_copyFixture = &fixture;
        g_cacheCallCount = 0;
        if (point == 0) {
            g_cacheHook = __StopDuringOperation;
            assert(ScShmAccessBegin(NULL, result.Handle, &begin) == OS_ECANCELLED);
        } else if (point == 1) {
            assert(ScShmAccessBegin(NULL, result.Handle, &begin) == OS_EOK);
            g_cacheHook = __StopDuringOperation;
            // This completion carries the driver's completed-prefix promise.
            // Revocation preserves quarantine until the whole device stops.
            assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 64) == OS_EOK);
        } else {
            g_releaseHook = __StopDuringOperation;
            assert(ScResourceRelease(result.Handle) == OS_EOK);
            assert(fixture.Context->Mappings == NULL);
        }
        assert(g_cacheHook == NULL && g_releaseHook == NULL);
        assert(fixture.Context->Binding->Group->Access.Operations == 0);
        assert(g_buffer->Access.Faults == (point == 2 ? 0U : 1U));
        if (point != 2) {
            g_current = service;
            g_cacheCallCount = 0;
            assert(DMAManagerCompleteStop(fixture.Manager, &g_copyTicket) == OS_EOK);
            assert(g_cacheCallCount == 1 && g_cacheCalls[0].Length == 256);
            g_current = driver;
            assert(ScResourceRelease(result.Handle) == OS_EOK);
        }
        __FinishBoundFixture(&fixture);
    }
}

static void
__TestBoundMappingPolicies(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    // Each policy reaches recovery through the public syscall boundary. Shared
    // allocations are uncached. Bounce storage uses both pool cache policies.
    for (int kind = 0; kind < 5; kind++) {
        __BoundFixture_t fixture = __PrepareBoundPolicyFixture(service, driver, kind == 4);
        struct OSDmaMappingResult result;
        struct OSDmaAllocationResult allocation;
        struct DMARequirements requirements = { .Length = 256 };
        struct OSDmaMapParameters parameters = { .Length = 256, .Direction = DMAFromDevice };
        struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer, .Length = 256 };
        SHMDeviceMapping_t* mapping;
        OSDmaStopTicket_t ticket;
        paddr_t pages[] = { kind == 2 || kind == 3 ? 0x9000 : 0x1000 };
        uint32_t count = 0;

        g_cacheCallCount = 0;
        __UsePool(0x1000, kind == 3);
        __OpenBoundFixture(&fixture);
        if (kind == 1) {
            g_createPages[0] = 0x1000;
            assert(ScDmaAllocate(fixture.ContextHandle, &requirements, &allocation) == OS_EOK);
            result = allocation.Mapping;
        } else {
            parameters.Flags = kind == 0 ? DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT : 0;
            __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
            __RegisterTestBuffer();
            assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
            if (kind == 0) {
                begin.Kind = DMAAccessUse;
                begin.Length = 0;
            }
            assert(ScShmAccessBegin(NULL, result.Handle, &begin) == OS_EOK);
            if (kind == 0) {
                assert(ScShmAccessBegin(NULL, result.Handle, &begin) == OS_EOK);
            }
        }
        mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
        if (kind == 2 || kind == 3) {
            assert(mapping->Stream.Bounce.BounceMapping != 0);
            memset(g_sourceMemory, 0x5a, 256);
            memset(g_bounceMemory, 0xa5, 256);
        }
        g_current = service;
        assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
        assert(g_buffer->Access.Faults == 1 && g_buffer->Access.Waiting == 0);
        g_cacheCallCount = 0;
        assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
        assert(mapping->State == SHMDeviceMappingStopped && g_buffer->Access.Faults == 0);
        assert(g_cacheCallCount == (kind == 0 || kind == 3 ? 1 : 0));
        if (kind == 0) {
            assert(mapping->Persistent.Uses == 0 && g_cacheCalls[0].Length == 256);
        }
        if (kind == 2 || kind == 3) {
            for (size_t i = 0; i < 256; i++) {
                assert(g_sourceMemory[i] == 0x5a);
            }
            assert(g_bounceMemory == NULL && __PoolAllFree() && g_liveViews == 0);
        }
        g_current = driver;
        assert(ScDmaGetSegments(result.Handle, NULL, 0, &count) == OS_ECANCELLED);
        assert(ScResourceRelease(result.Handle) == OS_EOK);
        __FinishBoundFixture(&fixture);
    }
    __UsePool(0x100000, false);
}

static void
__TestTransitiveBoundGroups(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    DMAManager_t* manager;
    uuid_t managerHandle;
    uint64_t attachments[4];
    OSDmaBindingResult_t bindings[4];
    SHMDeviceContext_t* contexts[4] = { 0 };
    SHMRecord_t records[4] = { 0 };
    SHMDeviceOperation_t* operation = NULL;
    OSDmaStopTicket_t ticket;
    DMAAttachmentEventBatch_t events;
    OSDmaAttachmentParameters_t routes[] = {
        __Parameters(801, 10, 20), __Parameters(802, 10, 30),
        __Parameters(803, 40, 30), __Parameters(804, 50, 60)
    };
    OSDmaBindParameters_t bind = { .Principal = driver->GlobalHandleID, .RequestId = 1 };

    g_current = service;
    manager = __CreateManager(&managerHandle);
    for (int i = 0; i < 4; i++) {
        attachments[i] = __RegisterAttachment(manager, &routes[i]);
    }
    for (int i = 0; i < 4; i++) {
        bind.Attachment = attachments[i];
        bindings[i] = __BindAttachment(manager, &bind);
    }
    g_current = driver;
    for (int i = 0; i < 4; i++) {
        OSDmaBoundOpenParameters_t open = { bindings[i].Generation, UINT64_MAX };

        assert(SHMDeviceContextPrepareBound(bindings[i].Handle, &open, &contexts[i]) == OS_EOK);
        assert(SHMDeviceContextPublishBound(contexts[i]) == OS_EOK);
        records[i].State = SHMRecordUpdating;
        records[i].Object = contexts[i];
    }
    g_current = service;
    assert(DMAManagerPrepareStop(manager, attachments[0], &ticket) == OS_EOK);
    for (int i = 0; i < 4; i++) {
        assert(SHMDeviceContextBeginOperation(&records[i], contexts[i], SHMDeviceOperationSubmit,
            &operation) == (i == 3 ? OS_EOK : OS_ECANCELLED));
        SHMDeviceOperationEnd(&operation);
    }
    assert(DMAManagerSnapshotEvents(manager, 32, &events) == OS_EOK && events.Count == 3);
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EOK);
    assert(DMAManagerPrepareStop(manager, attachments[3], &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EOK);
    g_current = driver;
    for (int i = 0; i < 4; i++) {
        assert(SHMDeviceContextClose(&contexts[i]) == OS_EOK);
        assert(DestroyHandle(bindings[i].Handle) == OS_EOK);
    }
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(manager, 32, &events) == OS_EOK && events.Count == 4);
    assert(DMAManagerFinishEvents(manager, &events, true) == OS_EOK);
    for (int i = 0; i < 4; i++) {
        assert(DMAManagerRemoveAttachment(manager, attachments[i]) == OS_EOK);
    }
    assert(DestroyHandle(managerHandle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

static bool g_failRecordOperation;
static unsigned int g_recordObjectCalls;

static oserr_t
__CheckRecordOperationFailure(SHMRecord_t* record)
{
    assert(record->PendingOperation != NULL);
    return g_failRecordOperation ? OS_EBUSY : OS_EOK;
}

static oserr_t
__CheckRecordObjectFailure(SHMRecord_t* record)
{
    assert(record->Object != NULL);
    g_recordObjectCalls++;
    return OS_EUNKNOWN;
}

static void
__TestTeardownDiagnostics(void)
{
    const SHMRecordOps_t operations = {
        .Cleanup = __CheckRecordObjectFailure,
        .CancelOperation = __CheckRecordOperationFailure
    };
    SHMRecord_t record = {
        .Handle = 0x1234, .State = SHMRecordUpdating, .Ops = &operations,
        .Object = &record, .PendingOperation = &record
    };

    // Failure at either stage must keep the record and its object. Each path
    // names the failed callback, including the final destructor retry.
    g_failRecordOperation = true;
    __ExitRecord(&record);
    assert(strstr(g_lastBoundError, "__ExitRecord: CancelOperation") != NULL);
    assert(g_recordObjectCalls == 0 && record.Object == &record);
    __DestroyRecord(&record);
    assert(strstr(g_lastBoundError, "__DestroyRecord: CancelOperation") != NULL);
    assert(g_recordObjectCalls == 0 && record.PendingOperation == &record);
    g_failRecordOperation = false;
    __ExitRecord(&record);
    assert(strstr(g_lastBoundError, "__ExitRecord: object cleanup") != NULL);
    assert(g_recordObjectCalls == 1 && record.Object == &record);
    __DestroyRecord(&record);
    assert(strstr(g_lastBoundError, "__DestroyRecord: object cleanup") != NULL);
    assert(g_recordObjectCalls == 2 && record.Object == &record);
}

#include "dma_operation_test.inc"
#include "dma_recovery_test.inc"
#include "dma_mapping_setup_test.inc"
#include "dma_bounce_release_test.inc"

int
main(void)
{
    MSContextSecurity_t service;
    MSContextSecurity_t driver;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    __UsePool(0x100000, false);
    __TestTeardownDiagnostics();
    __TestRecordSetupCleanup();
    __TestMappingPublicationFailures(&service, &driver);
    __TestEmptyMappingRollback(&service, &driver);
    __TestMappingInstallation(&service, &driver);
    __TestAllocationPreparationConstraints(&service, &driver);
    __TestNativeBoundStop(&service, &driver);
    __TestOperationSetupFailures(&service, &driver);
    __TestPartialContextRejectsOperations();
    __TestOperationReferences(&service, &driver);
    __TestRecoveryPassExclusion(&service, &driver);
    __TestBoundOpenFailures(&service, &driver);
    __TestPreparingOpenDrain(&service, &driver);
    __TestBoundCopyAndRetry(&service, &driver);
    __TestMapCopyClosure(&service, &driver);
    __TestGroupEpochs(&service, &driver);
    __TestStoppedOperation(&service, &driver, true);
    __TestStoppedOperation(&service, &driver, false);
    __TestConcurrentBoundWait(&service, &driver);
    __TestCancellationRetryOnManagerLoss(&service, &driver);
    __TestManagerLoss(&service, &driver, false);
    __TestManagerLoss(&service, &driver, true);
    __TestReleaseSkippedMapping(&service, &driver);
    __TestMapConstructionClosure(&service, &driver);
    __TestCacheAndReleaseClosure(&service, &driver);
    __TestBoundMappingPolicies(&service, &driver);
    __TestBounceReleaseFailures(&service, &driver);
    __TestTransitiveBoundGroups(&service, &driver);
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(driver.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("Bound DMA: real grants, mapping admission, quarantine and confirmed cleanup passed");
    return 0;
}
