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
 *
 * Shared access counters and fault tracking for one SHM region. Enforces
 * multiple-reader / single-writer rules across CPU and device sessions,
 * tracks stopped device transfers, and gates region reopening until cache
 * recovery completes.
 */

#define main __RunBoundRegionBaseline
#include "dma_bound_test.c"
#undef main
#ifndef DMA_REGION_API_SOURCE
#define DMA_REGION_API_SOURCE "../kernel/api/dma_region.c"
#endif
#include DMA_REGION_API_SOURCE
#include <arch/interrupts.h>
#include <setjmp.h>

static _Thread_local bool g_irqDisabled;
static _Thread_local bool g_regionFast;
static int g_interruptAt = -1;
static unsigned int g_restores;
static jmp_buf g_interruption;

void TestRegionMutex(void) { assert(!g_irqDisabled && !g_regionFast); }
irqstate_t InterruptDisable(void)
{
    bool previous = g_irqDisabled;
    g_irqDisabled = true;
    return previous;
}
irqstate_t InterruptRestoreState(irqstate_t state)
{
    assert(g_irqDisabled);
    g_irqDisabled = state;
    if (!state) {
        g_restores++;
        if (g_interruptAt == 0) {
            g_interruptAt = -1;
            longjmp(g_interruption, 1);
        }
        if (g_interruptAt > 0) {
            g_interruptAt--;
        }
    }
    return state;
}
void MemorySpaceSyncProcessPending(void) {}

static struct OSDmaMappingResult
__RegionMapping(__BoundFixture_t* fixture, enum DMADirection direction)
{
    paddr_t pages[] = { 0x1000 };
    struct SHMRegionRange ranges[] = { { 0, 128 }, { 128, 128 }, { 256, 3840 } };
    struct OSDmaMapParameters parameters = {
        .Length = 4096, .Direction = direction,
        .Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_REGIONS
    };
    struct OSDmaMappingResult result;

    __OpenBoundFixture(fixture);
    __CreateBuffer(pages, 1, 0, 4096, SHM_COMMIT | SHM_REGIONS);
    __RegisterTestBuffer();
    assert(SHMRegionRegister(g_buffer->ID, ranges, 3) == OS_EOK);
    assert(ScDmaMapShm(fixture->ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    return result;
}

static OSDmaRegionSession_t
__RegionSession(uuid_t mapping, unsigned int region)
{
    OSBufferIdentity_t identity = { g_buffer->ID, region, 1 };
    OSDmaRegionSession_t result;

    assert(ScDmaRegionSessionOpen(mapping, &identity, 4, &result) == OS_EOK);
    assert(result.Identity.Buffer == identity.Buffer && result.Identity.Region == region);
    assert(result.Identity.Generation == 1 && result.TokenCapacity == 4);
    return result;
}

static void __RejectRegionAllocation(void) { assert(!"region fast path allocated"); }
static void __CheckCacheUnlocked(void) { assert(!g_irqDisabled); }

static void
__TestRegionAccess(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool coherent)
{
    __BoundFixture_t fixture = __PrepareBoundPolicyFixture(service, driver, coherent);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t first = __RegionSession(mapping.Handle, 0);
    OSDmaRegionSession_t second = __RegionSession(mapping.Handle, 1);
    OSDmaRegionSession_t queue = __RegionSession(mapping.Handle, 0);
    struct SHMRegionCpuSession* cpu = NULL;
    OSBufferIdentity_t identity;
    size_t allocations = g_allocations;
    unsigned int releases = g_addressReleaseCalls;

    assert(first.Offset == 0 && first.Length == 128 && second.Offset == 128 && second.Length == 128);
    assert(ScResourceRelease(mapping.Handle) == OS_EBUSY);
    assert(SHMRegionCpuCreate(g_buffer->ID, 0, 2, &cpu, &identity) == OS_EOK);
    assert(SHMRegionCpuBegin(cpu, 0, 1, SHMCpuAccessRead) == OS_EOK);
    allocations = g_allocations;
    g_cacheCallCount = 0;
    g_cacheHook = __CheckCacheUnlocked;
    g_constructionHook = __RejectRegionAllocation;
    g_regionFast = true;
    assert(ScDmaRegionBeginUse(first.Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionBeginUse(queue.Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionBeginUse(first.Handle, 1, 1, DMAFromDevice) == OS_EBUSY);
    assert(ScDmaRegionBeginUse(second.Handle, 0, 1, DMAFromDevice) == OS_EOK);
    assert(ScDmaRegionEndUse(first.Handle, 0, 2) == OS_ENOENT);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_EOK);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_EOK);
    assert(ScDmaRegionEndUse(queue.Handle, 0, 1) == OS_EOK);
    assert(ScDmaRegionEndUse(second.Handle, 0, 1) == OS_EOK);
    g_regionFast = false;
    g_constructionHook = NULL;
    g_cacheHook = NULL;
    assert(g_allocations == allocations && g_addressReleaseCalls == releases);
    if (!coherent) {
        const struct __CacheCall expected[] = {
            { __CacheClean, 0x1000, 128 }, { __CacheClean, 0x1000, 128 },
            { __CacheCleanInvalidate, 0x1080, 128 }, { __CacheInvalidate, 0x1080, 128 }
        };
        __ExpectCacheCalls(expected, 4);
    } else {
        assert(g_cacheCallCount == 0);
    }
    assert(SHMRegionCpuBegin(cpu, 1, 1, SHMCpuAccessWrite) == OS_EBUSY);
    assert(SHMRegionCpuEnd(cpu, 0, 1) == OS_EOK);
    g_cacheCallCount = 0;
    assert(ScDmaRegionBeginUse(first.Handle, 0, 2, DMABidirectional) == OS_EOK);
    assert(SHMRegionCpuBegin(cpu, 0, 2, SHMCpuAccessRead) == OS_EBUSY);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_ENOENT);
    assert(ScResourceRelease(first.Handle) == OS_EBUSY);
    assert(ScDmaRegionEndUse(first.Handle, 0, 2) == OS_EOK);
    assert(ScDmaRegionBeginUse(first.Handle, 0, 2, DMAToDevice) == OS_ENOENT);
    assert(SHMRegionCpuDestroy(&cpu, false) == OS_EOK);
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(ScResourceRelease(queue.Handle) == OS_EOK);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    g_cacheCallCount = 0;
    __FinishBoundFixture(&fixture);
}

static void
__RevokeAtCache(void)
{
    assert(!g_irqDisabled);
    DMABindingRevoke(g_copyFixture->Context->Binding);
}

static void
__TestRegionRecovery(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t first = __RegionSession(mapping.Handle, 0);
    OSDmaRegionSession_t second = __RegionSession(mapping.Handle, 1);
    SHMRecord_t* record = LookupHandle(first.Handle, HandleTypeSHMAccess);
    SHMDeviceRegionSession_t* session = record->Object;
    OSDmaStopTicket_t ticket;
    struct SHMRegionCpuSession* cpu = NULL;
    OSBufferIdentity_t identity;
    int cacheCount;

    assert(SHMRegionCpuCreate(g_buffer->ID, 2, 1, &cpu, &identity) == OS_EOK);
    g_cacheCallCount = 0;
    assert(ScDmaRegionBeginUse(first.Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_EOK);
    assert(ScDmaRegionBeginUse(second.Handle, 0, 1, DMAFromDevice) == OS_EOK);
    g_copyFixture = &fixture;
    g_cacheHook = __RevokeAtCache;
    assert(ScDmaRegionBeginUse(first.Handle, 0, 2, DMAToDevice) == OS_ECANCELLED);
    assert(session->Tokens[0].Generation == 1 && session->Tokens[0].Access.Generation == 1);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_EOK);
    assert(SHMRegionCpuBegin(cpu, 0, 1, SHMCpuAccessRead) == OS_EDEVFAULT);
    // Exact hardware completion is accepted while Blocked.
    assert(ScDmaRegionEndUse(second.Handle, 0, 1) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    g_cacheCallCount = 0;
    g_failAddressRelease = true;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EUNKNOWN);
    assert(g_cacheCallCount == 3 && session->Region->Fault.Region != NULL);
    cacheCount = g_cacheCallCount;
    g_failAddressRelease = false;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(g_cacheCallCount == cacheCount && session->Mapping->State == SHMDeviceMappingStopped);
    g_current = driver;
    assert(ScResourceRelease(mapping.Handle) == OS_EBUSY);
    assert(ScDmaRegionEndUse(first.Handle, 0, 1) == OS_EOK);
    assert(SHMRegionCpuBegin(cpu, 0, 1, SHMCpuAccessRead) == OS_EOK);
    assert(SHMRegionCpuEnd(cpu, 0, 1) == OS_EOK);
    assert(SHMRegionCpuDestroy(&cpu, false) == OS_EOK);
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    g_cacheCallCount = 0;
    __FinishBoundFixture(&fixture);
}

static void
__TestRegionInterruption(MSContextSecurity_t* service, MSContextSecurity_t* driver, int point, bool end)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t opened = __RegionSession(mapping.Handle, 0);
    SHMRecord_t* record = LookupHandle(opened.Handle, HandleTypeSHMAccess);
    SHMDeviceRegionSession_t* session = record->Object;
    OSDmaStopTicket_t ticket;
    oserr_t result;

    g_cacheCallCount = 0;
    if (end) {
        assert(ScDmaRegionBeginUse(opened.Handle, 0, 1, DMAFromDevice) == OS_EOK);
    }
    atomic_store(&record->State, SHMRecordUpdating);
    g_interruptAt = point;
    if (setjmp(g_interruption) == 0) {
        if (end) {
            assert(SHMDeviceRegionEnd(record, session, 0, 1) == OS_EOK);
        } else {
            assert(SHMDeviceRegionBegin(record, session, 0, 1, DMAFromDevice) == OS_EOK);
        }
    }
    g_interruptAt = -1;
    assert(SHMDeviceRegionCancel(record) == OS_EOK);
    assert(!session->Counted && session->Record == NULL && record->PendingOperation == NULL);
    atomic_store(&record->State, SHMRecordIdle);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    g_cacheCallCount = 0;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    g_current = driver;
    result = ScDmaRegionEndUse(opened.Handle, 0, 1);
    assert(result == OS_EOK || result == OS_EDEVFAULT || (!end && result == OS_ENOENT));
    assert(session->Tokens[0].Access.State == SHMRegionAccessIdle);
    assert(ScResourceRelease(opened.Handle) == OS_EOK);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    g_cacheCallCount = 0;
    __FinishBoundFixture(&fixture);
}

static void
__TestIndependentRegionStop(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool readOnly)
{
    __BoundFixture_t first = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult firstMapping = __RegionMapping(&first, readOnly ? DMAToDevice : DMABidirectional);
    OSDmaRegionSession_t firstSession = __RegionSession(firstMapping.Handle, 0);
    __BoundFixture_t second = { .Service = service, .Driver = driver, .Manager = first.Manager };
    OSDmaAttachmentParameters_t route = __Parameters(701, 21, 31);
    OSDmaBindParameters_t request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    struct OSDmaMapParameters parameters = { .Length = 128, .Direction = DMABidirectional,
        .Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_REGIONS };
    struct OSDmaMappingResult secondMapping;
    OSDmaRegionSession_t secondSession;
    OSDmaStopTicket_t firstTicket, secondTicket;
    DMAAttachmentEventBatch_t batch;

    g_current = service;
    second.Attachment.Value = __RegisterAttachment(first.Manager, &route);
    request.Attachment = second.Attachment.Value;
    second.Binding = __BindAttachment(first.Manager, &request);
    g_current = driver;
    __OpenBoundFixture(&second);
    assert(ScDmaMapShm(second.ContextHandle, g_buffer->ID, &parameters, &secondMapping) == OS_EOK);
    secondSession = __RegionSession(secondMapping.Handle, 0);
    g_cacheCallCount = 0;
    if (readOnly) {
        assert(ScDmaRegionBeginUse(firstSession.Handle, 0, 1, DMAFromDevice) == OS_EPERMISSIONS);
    }
    assert(ScDmaRegionBeginUse(firstSession.Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionBeginUse(secondSession.Handle, 0, 1, DMAToDevice) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(first.Manager, first.Attachment.Value, &firstTicket) == OS_EOK);
    g_cacheCallCount = 0;
    assert(DMAManagerCompleteStop(first.Manager, &firstTicket) == (readOnly ? OS_EOK : OS_EBUSY));
    assert(DMAManagerPrepareStop(first.Manager, second.Attachment.Value, &secondTicket) == OS_EOK);
    assert(DMAManagerCompleteStop(first.Manager, &secondTicket) == OS_EOK);
    if (!readOnly) {
        assert(DMAManagerCompleteStop(first.Manager, &firstTicket) == OS_EOK);
    }
    g_current = driver;
    assert(ScDmaRegionEndUse(firstSession.Handle, 0, 1) == OS_EDEVFAULT);
    assert(ScDmaRegionEndUse(secondSession.Handle, 0, 1) == OS_EDEVFAULT);
    assert(ScResourceRelease(firstSession.Handle) == OS_EOK);
    assert(ScResourceRelease(secondSession.Handle) == OS_EOK);
    assert(ScResourceRelease(firstMapping.Handle) == OS_EOK);
    assert(ScResourceRelease(secondMapping.Handle) == OS_EOK);
    assert(ScResourceRelease(second.ContextHandle) == OS_EOK);
    assert(DestroyHandle(second.Binding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    assert(DMAManagerSnapshotEvents(first.Manager, 32, &batch) == OS_EOK);
    assert(DMAManagerFinishEvents(first.Manager, &batch, true) == OS_EOK);
    assert(DMAManagerRemoveAttachment(first.Manager, second.Attachment.Value) == OS_EOK);
    g_cacheCallCount = 0;
    __FinishBoundFixture(&first);
}

static oserr_t
__RejectSessionCopy(void* destination, const void* source, size_t size)
{
    (void)destination; (void)source; (void)size;
    return OS_EINVALPARAMS;
}

static void
__TestRegionSetupFailures(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t opened;
    OSBufferIdentity_t identity = { g_buffer->ID, 0, 1 };
    SHMRecord_t* parent = LookupHandle(mapping.Handle, HandleTypeSHMAccess);
    SHMDeviceMapping_t* object = parent->Object;
    size_t allocations = g_allocations;
    struct OSDmaMapParameters parameters = { .Length = 4096, .Direction = DMABidirectional,
        .Flags = DMA_MAP_REGIONS };
    struct OSDmaMappingResult rejected;

    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &rejected) == OS_EINVALPARAMS);
    parameters.Flags |= DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &rejected) == OS_EINVALPARAMS);
    parameters.Flags = DMA_MAP_REQUIRE_DIRECT;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &rejected) == OS_ENOTSUPPORTED);
    parameters.Flags |= DMA_MAP_REGIONS;
    parameters.Offset = 64;
    parameters.Length = 64;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &rejected) == OS_EINVALPARAMS);
    parameters.Offset = 0;
    parameters.Length = 4096;
    parameters.Requirements.AddressAlignment = 256;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &rejected) == OS_ENOTSUPPORTED);

    assert(ScDmaRegionSessionOpen(fixture.ContextHandle, &identity, 1, &opened) == OS_EINVALPARAMS);
    assert(ScDmaRegionSessionOpen(mapping.Handle, &identity, 0, &opened) == OS_EINVALPARAMS);
    assert(ScDmaRegionSessionOpen(mapping.Handle, &identity, 4097, &opened) == OS_EINVALPARAMS);
    identity.Generation++;
    assert(ScDmaRegionSessionOpen(mapping.Handle, &identity, 1, &opened) == OS_ENOENT);
    identity.Generation--;
    g_boundCopyOverride = __RejectSessionCopy;
    assert(ScDmaRegionSessionOpen(mapping.Handle, &identity, 1, &opened) == OS_EINVALPARAMS);
    g_boundCopyOverride = NULL;
    __Drain();
    assert(object->Regions->SessionCount == 0 && atomic_load(&object->Lifetime.References) == 1);
    assert(fixture.Context->Binding->Group->Access.State == DMARecoveryLive && g_allocations == allocations);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}

static void
__TestSessionInterruption(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool close)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t opened;
    OSBufferIdentity_t identity = { g_buffer->ID, 0, 1 };
    SHMRecord_t* parent;
    SHMRecord_t* record;
    SHMDeviceOperation_t* operation = NULL;
    SHMDeviceMapping_t* object;

    assert(SHMRecordBegin(mapping.Handle, &g_mappingOps, &parent) == OS_EOK);
    object = parent->Object;
    assert(SHMDeviceMappingBeginOperation(parent, object, SHMDeviceOperationSubmit, &operation) == OS_EOK);
    assert(SHMRecordCreate(&g_deviceRegionOps, &record) == OS_EOK);
    if (close) {
        assert(SHMDeviceRegionSessionCreate(object, &identity, 1, record, &opened) == OS_EOK);
    }
    g_interruptAt = 0;
    if (setjmp(g_interruption) == 0) {
        if (close) {
            assert(SHMRecordRelease(record) == OS_EOK);
        } else {
            assert(SHMDeviceRegionSessionCreate(object, &identity, 1, record, &opened) == OS_EOK);
        }
        assert(!"expected interruption at the session publication transition");
    }
    g_interruptAt = -1;
    assert(record->Object != NULL);
    assert(((SHMDeviceRegionSession_t*)record->Object)->Unlinked == close);
    assert(SHMRecordCleanup(record) == OS_EOK && record->Object == NULL);
    SHMRecordRemove(record);
    SHMDeviceOperationEnd(&operation);
    SHMRecordEnd(parent);
    assert(object->Regions->SessionCount == 0 && atomic_load(&object->Lifetime.References) == 1);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}

static void
__TestRegionAbandon(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool mappingFirst)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult mapping = __RegionMapping(&fixture, DMABidirectional);
    OSDmaRegionSession_t session = __RegionSession(mapping.Handle, 0);
    SHMRecord_t* mappingRecord = LookupHandle(mapping.Handle, HandleTypeSHMAccess);
    SHMRecord_t* sessionRecord = LookupHandle(session.Handle, HandleTypeSHMAccess);
    OSDmaStopTicket_t ticket;

    g_cacheCallCount = 0;
    assert(ScDmaRegionBeginUse(session.Handle, 0, 1, DMAFromDevice) == OS_EOK);
    __ExitRecord(mappingFirst ? mappingRecord : sessionRecord);
    __ExitRecord(mappingFirst ? sessionRecord : mappingRecord);
    assert(mappingRecord->Object == NULL && sessionRecord->Object == NULL);
    assert(DestroyHandle(mapping.Handle) == OS_EOK);
    assert(DestroyHandle(session.Handle) == OS_EOK);
    __Drain();
    assert(fixture.Context->Mappings != NULL);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &ticket) == OS_EOK);
    g_cacheCallCount = 0;
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(fixture.Context->Mappings == NULL);
    g_cacheCallCount = 0;
    __FinishBoundFixture(&fixture);
}

int main(void)
{
    MSContextSecurity_t service, driver;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    __UsePool(0x100000, false);
    __TestRegionAccess(&service, &driver, false);
    __TestRegionAccess(&service, &driver, true);
    __TestRegionRecovery(&service, &driver);
    __TestRegionSetupFailures(&service, &driver);
    __TestSessionInterruption(&service, &driver, false);
    __TestSessionInterruption(&service, &driver, true);
    __TestRegionAbandon(&service, &driver, true);
    __TestRegionAbandon(&service, &driver, false);
    __TestIndependentRegionStop(&service, &driver, true);
    __TestIndependentRegionStop(&service, &driver, false);
    for (int point = 0; point < 12; point++) {
        __TestRegionInterruption(&service, &driver, point, false);
        __TestRegionInterruption(&service, &driver, point, true);
    }
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(driver.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0 && g_restores != 0);
    puts("Device regions: cache direction/bounds, sharing, exact receipts, recovery, IRQ interruption and fast-path budgets passed");
    return 0;
}
