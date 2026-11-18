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

// Included by dma_bound_test.c after the real record, binding and SHM fixtures.
// Keep a handle reference across failed publication so deferred destruction
// cannot hide missing synchronous cleanup in the syscall rollback path.
static SHMRecord_t*  g_setupRecord;
static size_t        g_setupCopyLength;
static unsigned int g_setupRevokeChecks;
static unsigned int g_setupCleanupCalls;
static oserr_t       g_setupCleanupStatus;

static oserr_t
__RejectSetupRelease(SHMRecord_t* record)
{
    (void)record;
    assert(false && "setup rollback must call Cleanup, not Release");
    return OS_EUNKNOWN;
}

static oserr_t
__CleanupSetupRecord(SHMRecord_t* record)
{
    void* object = SHMRecordDetachObject(record);

    assert(object == &g_setupCleanupCalls);
    assert(record->State == SHMRecordUpdating && record->PendingOperation == NULL);
    g_setupCleanupCalls++;
    if (g_setupCleanupStatus != OS_EOK) {
        SHMRecordAttachObject(record, object);
        return g_setupCleanupStatus;
    }
    return OS_EOK;
}

static void
__TestRecordSetupCleanup(void)
{
    const SHMRecordOps_t operations = {
        .Release = __RejectSetupRelease,
        .Cleanup = __CleanupSetupRecord
    };
    SHMRecord_t record = { .State = SHMRecordUpdating, .Ops = &operations };

    assert(SHMRecordCleanup(&record) == OS_EOK && g_setupCleanupCalls == 0);
    record.Object = &g_setupCleanupCalls;
    g_setupCleanupStatus = OS_EBUSY;
    assert(SHMRecordCleanup(&record) == OS_EBUSY);
    assert(record.Object == &g_setupCleanupCalls && g_setupCleanupCalls == 1);
    assert(record.State == SHMRecordUpdating && record.PendingOperation == NULL);
    g_setupCleanupStatus = OS_EOK;
    assert(SHMRecordCleanup(&record) == OS_EOK);
    assert(record.Object == NULL && g_setupCleanupCalls == 2);
    assert(record.State == SHMRecordUpdating && record.PendingOperation == NULL);
    assert(SHMRecordCleanup(&record) == OS_EOK && g_setupCleanupCalls == 2);
}

static oserr_t
__FailMappingResultCopy(void* to, const void* from, size_t size)
{
    // Allocation puts the CPU buffer before its mapping result. Capture the
    // record even when no bytes reach user space, then copy the chosen prefix.
    const struct OSDmaMappingResult* result;
    SHMRecord_t*                    parent = LookupHandle(g_copyFixture->ContextHandle, HandleTypeSHMAccess);

    assert(size == sizeof(struct OSDmaMappingResult) || size == sizeof(struct OSDmaAllocationResult));
    result = size == sizeof(struct OSDmaAllocationResult) ?
        &((const struct OSDmaAllocationResult*)from)->Mapping : from;
    assert(g_setupCopyLength < size && result->SegmentCount == 1);
    assert(parent != NULL && parent->PendingOperation != NULL);
    assert(AcquireHandle(result->Handle, HandleTypeSHMAccess, (void**)&g_setupRecord) == OS_EOK);
    assert(g_setupRecord->State == SHMRecordUpdating && g_setupRecord->Object != NULL);
    assert(ScResourceRelease(result->Handle) == OS_EBUSY);
    assert(g_copyFixture->Context->Binding->Group->Access.Operations == 1);
    memcpy(to, from, g_setupCopyLength);
    g_boundCopyOverride = NULL;
    return OS_EINVALPARAMS;
}

static void
__CheckPublicationRevoke(void)
{
    SHMRecord_t*         parent = LookupHandle(g_copyFixture->ContextHandle, HandleTypeSHMAccess);
    DMARecoveryGroup_t* group = g_copyFixture->Context->Binding->Group;

    // The mapping callback has a detached slot, but the parent token must
    // still prevent physical-stop cleanup from overtaking this rollback.
    assert(g_setupRecord != NULL);
    assert(g_setupRecord->Object == NULL && g_setupRecord->State == SHMRecordUpdating);
    assert(parent != NULL && parent->PendingOperation != NULL);
    assert(group->Access.State != DMARecoveryLive && group->Access.Operations == 1);
    assert(g_buffer != NULL);
    assert(DMAManagerPrepareStop(g_copyFixture->Manager, g_copyFixture->Attachment.Value, &g_copyTicket) == OS_EOK);
    assert(DMAManagerCompleteStop(g_copyFixture->Manager, &g_copyTicket) == OS_EBUSY);
    g_setupRevokeChecks++;
}

enum __SetupMappingKind {
    __SetupMappingDirect,
    __SetupMappingBounce,
    __SetupMappingPersistent,
    __SetupMappingAllocation
};

static void
__TestMappingPublicationFailure(
    MSContextSecurity_t*    service,
    MSContextSecurity_t*    driver,
    enum __SetupMappingKind kind,
    bool                    partialCopy)
{
    __BoundFixture_t             fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaAllocationResult result = { 0 };
    struct DMARequirements      requirements = { .Length = 256 };
    struct OSDmaMapParameters    parameters = {
        .Length = 256, .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT
    };
    SHMDeviceMapping_t* mapping;
    SHMRecord_t*        parent;
    unsigned int       releases = g_addressReleaseCalls;
    int                cacheCalls;
    paddr_t            pages[] = { kind == __SetupMappingBounce ? 0x9000 : 0x1000 };

    g_cacheCallCount = 0;
    __UsePool(0x1000, true);
    __OpenBoundFixture(&fixture);
    g_copyFixture = &fixture;
    g_setupRecord = NULL;
    g_setupRevokeChecks = 0;
    g_setupCopyLength = partialCopy ? offsetof(struct OSDmaMappingResult, InlineSegment.Length) : 0;
    if (partialCopy && kind == __SetupMappingAllocation) {
        g_setupCopyLength += offsetof(struct OSDmaAllocationResult, Mapping);
    }
    g_boundCopyOverride = __FailMappingResultCopy;
    g_afterRevokeHook = __CheckPublicationRevoke;
    if (kind == __SetupMappingAllocation) {
        g_createPages[0] = 0x1000;
        assert(ScDmaAllocate(fixture.ContextHandle, &requirements, &result) == OS_EINVALPARAMS);
    } else {
        __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
        __RegisterTestBuffer();
        if (kind == __SetupMappingBounce) {
            parameters.Flags = 0;
        } else if (kind == __SetupMappingPersistent) {
            parameters.Flags |= DMA_MAP_PERSISTENT;
        }
        assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result.Mapping) == OS_EINVALPARAMS);
    }

    // No drain runs here. The extra handle reference keeps the failed record
    // allocated, so these checks require cleanup before syscall return.
    assert(g_setupRevokeChecks == 1 && g_boundCopyOverride == NULL && g_afterRevokeHook == NULL);
    assert(g_setupRecord->Object == NULL && g_setupRecord->State == SHMRecordClosed);
    parent = LookupHandle(fixture.ContextHandle, HandleTypeSHMAccess);
    assert(parent != NULL && parent->PendingOperation == NULL && parent->State == SHMRecordIdle);
    assert(fixture.Context->Binding->Group->Access.Operations == 0);
    assert(strstr(g_lastBoundError, "__PublishMapping: MemorySpaceCopyToUser") != NULL);
    if (partialCopy) {
        assert(result.Mapping.Handle == g_setupRecord->Handle);
        assert(result.Mapping.InlineSegment.Address >= 0x4000 && result.Mapping.InlineSegment.Address <= 0x4fff);
        assert(result.Mapping.InlineSegment.Length == 0);
    } else {
        assert(result.Mapping.Handle == 0 && result.Mapping.InlineSegment.Address == 0);
    }
    mapping = fixture.Context->Mappings;
    assert(mapping != NULL && mapping->Lifetime.Abandoned && mapping->AddressMapping != NULL);
    assert(g_addressReleaseCalls == releases && g_buffer->Access.Faults == 1);
    if (kind == __SetupMappingBounce) {
        assert(mapping->Stream.Bounce.BounceMapping != 0 && !__PoolAllFree());
    }
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result.Mapping) == OS_ECANCELLED);

    // Allocation rollback already detached the CPU view. Drop the test's
    // source reference for other mappings so recovery is the last buffer user.
    if (kind != __SetupMappingAllocation) {
        assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    }
    assert(DestroyHandle(g_setupRecord->Handle) == OS_EOK);
    g_setupRecord = NULL;
    __Drain();
    assert(g_buffer != NULL && g_addressReleaseCalls == releases);

    // A failed address release after confirmed stop must keep the mapping and
    // its storage. Retrying must not repeat completed cache maintenance.
    g_current = service;
    g_failAddressRelease = true;
    assert(DMAManagerCompleteStop(fixture.Manager, &g_copyTicket) == OS_EUNKNOWN);
    assert(mapping->AddressMapping != NULL && mapping->State == SHMDeviceMappingReleasingAfterStop);
    assert(g_buffer != NULL);
    if (kind == __SetupMappingBounce) {
        assert(!__PoolAllFree());
    }
    cacheCalls = g_cacheCallCount;
    g_failAddressRelease = false;
    assert(DMAManagerCompleteStop(fixture.Manager, &g_copyTicket) == OS_EOK);
    assert(g_cacheCallCount == cacheCalls && fixture.Context->Mappings == NULL);
    __Drain();
    assert(g_buffer == NULL && __PoolAllFree());
    __FinishBoundFixture(&fixture);
}

static void
__TestMappingPublicationFailures(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    for (int kind = __SetupMappingDirect; kind <= __SetupMappingAllocation; kind++) {
        __TestMappingPublicationFailure(service, driver, kind, false);
        __TestMappingPublicationFailure(service, driver, kind, true);
    }
}

static void
__TestEmptyMappingRollback(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t             fixture = __PrepareBoundFixture(service, driver);
    struct DMARequirements      requirements = { .Length = 256 };
    struct OSDmaAllocationResult result = { 0 };
    SHMRecord_t*                parent;

    __OpenBoundFixture(&fixture);
    g_createStatus = OS_EOOM;
    assert(ScDmaAllocate(fixture.ContextHandle, &requirements, &result) == OS_EOOM);
    g_createStatus = OS_EOK;
    assert(result.Mapping.Handle == 0 && fixture.Context->Mappings == NULL);
    assert(fixture.Context->Binding->Group->Access.State == DMARecoveryLive);
    assert(fixture.Context->Binding->Group->Access.Operations == 0);
    parent = LookupHandle(fixture.ContextHandle, HandleTypeSHMAccess);
    assert(parent != NULL && parent->PendingOperation == NULL && parent->State == SHMRecordIdle);
    __FinishBoundFixture(&fixture);
}

enum __InstallTestPoint {
    __InstallCancelBeforeCall,
    __InstallFailBeforeTransition,
    __InstallFailAfterTransition,
    __InstallCancelBeforeTransition,
    __InstallCancelAfterTransition,
    __InstallSucceed
};

static enum __InstallTestPoint g_installPoint;
static unsigned int           g_installCalls;
static unsigned int           g_installChecks;
static oserr_t (*g_installOverride)(struct DMAAddressMapping*);

oserr_t __real_DMAAddressMappingInstall(struct DMAAddressMapping* mapping);
oserr_t __wrap_DMAAddressMappingInstall(struct DMAAddressMapping* mapping)
{
    g_installCalls++;
    if (g_installOverride != NULL) {
        return g_installOverride(mapping);
    }
    return __real_DMAAddressMappingInstall(mapping);
}

static void
__CheckPreparingLink(void)
{
    SHMDeviceMapping_t* mapping = g_copyFixture->Context->Mappings;

    assert(mapping != NULL && mapping->State == SHMDeviceMappingPreparing);
    assert(mapping->Access.Buffer == g_buffer && mapping->Access.ReferenceID == g_buffer->ID);
    assert((mapping->Access.Scope & GrantTokenScopeWrite) != 0);
    assert(mapping->Extents != NULL && mapping->ExtentCount > 0 && mapping->AddressMapping != NULL);
    if (g_installPoint == __InstallCancelBeforeCall) {
        __StopDuringOperation();
    }
}

static oserr_t
__InspectInstallation(struct DMAAddressMapping* addressMapping)
{
    SHMDeviceMapping_t*      mapping = g_copyFixture->Context->Mappings;
    SHMRecord_t*             parent = LookupHandle(g_copyFixture->ContextHandle, HandleTypeSHMAccess);
    const struct DMASegment* segments;
    uint32_t                count;
    oserr_t                 status;

    g_installChecks++;
    assert(mapping != NULL && mapping->AddressMapping == addressMapping);
    assert(mapping->State == SHMDeviceMappingPreparing);
    assert(parent != NULL && parent->PendingOperation != NULL);
    assert(g_copyFixture->Context->Binding->Group->Access.Operations == 1);
    assert(!g_copyFixture->Context->MappingLock.held && !g_buffer->Mutex.held);
    segments = DMAAddressMappingSegments(addressMapping, &count);
    assert(segments != NULL && count == 1);
    assert(SHMDeviceMappingSegments(parent->PendingOperation, mapping, &count) == NULL && count == 0);

    // Both failures are permitted results from a future backend. Neither can
    // justify a bounce retry, even when no direct transition was performed.
    if (g_installPoint == __InstallFailBeforeTransition) {
        return OS_ENOENT;
    }
    if (g_installPoint == __InstallCancelBeforeTransition) {
        __StopDuringOperation();
    }
    status = __real_DMAAddressMappingInstall(addressMapping);
    assert(status == OS_EOK);
    assert(mapping->State == SHMDeviceMappingPreparing);
    if (g_installPoint == __InstallFailAfterTransition) {
        return OS_ENOTSUPPORTED;
    }
    if (g_installPoint == __InstallCancelAfterTransition) {
        __StopDuringOperation();
    }
    return OS_EOK;
}

static void
__TestMappingInstallationAt(
    MSContextSecurity_t*     service,
    MSContextSecurity_t*     driver,
    enum __SetupMappingKind  kind,
    enum __InstallTestPoint  point)
{
    __BoundFixture_t             fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaAllocationResult result = { .Mapping.Handle = 0xdead, .Buffer.ID = 0xbeef };
    struct DMARequirements      requirements = { .Length = 256 };
    struct OSDmaMapParameters    parameters = { .Length = 256, .Direction = DMAFromDevice };
    SHMDeviceMapping_t*         mapping;
    unsigned int                installs = g_installCalls;
    unsigned int                releases = g_addressReleaseCalls;
    int                         cacheCalls;
    oserr_t                     status;
    paddr_t                     pages[] = { kind == __SetupMappingBounce ? 0x9000 : 0x1000 };

    __UsePool(0x1000, true);
    __OpenBoundFixture(&fixture);
    g_cacheCallCount = 0;
    g_copyFixture = &fixture;
    g_installPoint = point;
    g_installChecks = 0;
    g_installOverride = __InspectInstallation;
    g_afterLinkHook = __CheckPreparingLink;
    if (kind == __SetupMappingAllocation) {
        g_createPages[0] = 0x1000;
        status = ScDmaAllocate(fixture.ContextHandle, &requirements, &result);
    } else {
        __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
        __RegisterTestBuffer();
        if (kind == __SetupMappingDirect) {
            parameters.Flags = DMA_MAP_REQUIRE_DIRECT;
        } else if (kind == __SetupMappingPersistent) {
            parameters.Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;
        }
        status = ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result.Mapping);
    }
    g_installOverride = NULL;
    assert(g_afterLinkHook == NULL);
    assert(g_installCalls == installs + (point == __InstallCancelBeforeCall ? 0U : 1U));
    assert(g_installChecks == (point == __InstallCancelBeforeCall ? 0U : 1U));
    assert(fixture.Context->Binding->Group->Access.Operations == 0);
    mapping = fixture.Context->Mappings;
    assert(mapping != NULL && mapping->AddressMapping != NULL);

    if (point == __InstallSucceed) {
        assert(status == OS_EOK && result.Mapping.Handle != 0xdead);
        assert(mapping->State == SHMDeviceMappingReady && !mapping->Lifetime.Abandoned);
        assert(ScResourceRelease(result.Mapping.Handle) == OS_EOK);
        if (kind == __SetupMappingAllocation) {
            assert(SHMDetach(&result.Buffer) == OS_EOK);
            __Drain();
        }
        __FinishBoundFixture(&fixture);
        return;
    }

    assert(status == (point == __InstallFailBeforeTransition ? OS_ENOENT :
        point == __InstallFailAfterTransition ? OS_ENOTSUPPORTED : OS_ECANCELLED));
    assert(result.Mapping.Handle == 0xdead && result.Buffer.ID == 0xbeef);
    assert(mapping->State == SHMDeviceMappingPreparing && mapping->Lifetime.Abandoned);
    assert(fixture.Context->Binding->Group->Access.State != DMARecoveryLive);
    assert(g_buffer->Access.Faults == 1 && g_addressReleaseCalls == releases);
    if (kind == __SetupMappingBounce) {
        assert(mapping->Stream.Bounce.BounceMapping != 0 && !__PoolAllFree());
    }

    // Remove the normal buffer reference, leaving only recovery to protect
    // its pages. A failed allocation already detached its CPU buffer handle.
    if (kind != __SetupMappingAllocation) {
        assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    }
    __Drain();
    assert(g_buffer != NULL);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &g_copyTicket) == OS_EOK);
    // Prepared direct release is infallible. Inject a release failure only
    // when this case completed the real installation transition.
    if (point != __InstallCancelBeforeCall && point != __InstallFailBeforeTransition) {
        g_failAddressRelease = true;
        assert(DMAManagerCompleteStop(fixture.Manager, &g_copyTicket) == OS_EUNKNOWN);
        assert(mapping->State == SHMDeviceMappingReleasingAfterStop && mapping->AddressMapping != NULL);
        assert(g_buffer != NULL);
        g_failAddressRelease = false;
    }
    cacheCalls = g_cacheCallCount;
    assert(DMAManagerCompleteStop(fixture.Manager, &g_copyTicket) == OS_EOK);
    if (point != __InstallCancelBeforeCall && point != __InstallFailBeforeTransition) {
        assert(g_cacheCallCount == cacheCalls);
    }
    assert(fixture.Context->Mappings == NULL);
    __Drain();
    assert(g_buffer == NULL && __PoolAllFree());
    __FinishBoundFixture(&fixture);
}

static void
__TestMappingInstallation(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    for (int kind = __SetupMappingDirect; kind <= __SetupMappingAllocation; kind++) {
        for (int point = __InstallCancelBeforeCall; point <= __InstallSucceed; point++) {
            __TestMappingInstallationAt(service, driver, kind, point);
        }
    }
}

static void
__RejectPreparationLink(void)
{
    assert(false && "a rejected prepared layout must not reach the context list");
}

static void
__TestAllocationPreparationConstraints(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    // Physical pool pages meet each request. The device ranges below shift or
    // split their addresses so only validation of the prepared layout rejects it.
    for (int constraint = 0; constraint < 3; constraint++) {
        __BoundFixture_t             fixture = { .Service = service, .Driver = driver };
        OSDmaAttachmentParameters_t route = __Parameters(700, 20, 30);
        OSDmaBindParameters_t       bind = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
        struct OSDmaAllocationResult result = { .Mapping.Handle = 0xdead, .Buffer.ID = 0xbeef };
        struct DMARequirements      requirements = { .Length = 0x1000 };
        unsigned int                installs = g_installCalls;

        route.Ranges[0] = (struct SHMDeviceRange) { 0x2000, 0x5000, 0x2000 };
        if (constraint == 0) {
            requirements.Alignment = 0x2000;
        } else if (constraint == 1) {
            requirements.Length = 0x2000;
            requirements.Contiguous = true;
            route.RangeCount = 2;
            route.Ranges[0] = (struct SHMDeviceRange) { 0x2000, 0x4000, 0x1000 };
            route.Ranges[1] = (struct SHMDeviceRange) { 0x3000, 0x6000, 0x1000 };
        } else {
            requirements.Boundary = 0x1000;
            route.Ranges[0].DeviceBase = 0x5800;
        }
        g_current = service;
        fixture.Manager = __CreateManager(&fixture.ManagerHandle);
        fixture.Attachment.Value = __RegisterAttachment(fixture.Manager, &route);
        bind.Attachment = fixture.Attachment.Value;
        fixture.Binding = __BindAttachment(fixture.Manager, &bind);
        g_current = driver;
        __OpenBoundFixture(&fixture);
        g_createPages[0] = 0x2000;
        g_createPages[1] = 0x3000;
        g_beforeLinkHook = __RejectPreparationLink;
        assert(ScDmaAllocate(fixture.ContextHandle, &requirements, &result) == OS_ENOTSUPPORTED);
        g_beforeLinkHook = NULL;
        assert(result.Mapping.Handle == 0xdead && result.Buffer.ID == 0xbeef);
        assert(g_installCalls == installs && fixture.Context->Mappings == NULL);
        assert(fixture.Context->Binding->Group->Access.State == DMARecoveryLive);
        assert(fixture.Context->Binding->Group->Access.Operations == 0);
        __Drain();
        assert(g_buffer == NULL && __PoolAllFree());

        // The same binding can immediately allocate and publish a valid layout.
        requirements = (struct DMARequirements) { .Length = 0x1000 };
        assert(ScDmaAllocate(fixture.ContextHandle, &requirements, &result) == OS_EOK);
        assert(g_installCalls == installs + 1);
        assert(ScResourceRelease(result.Mapping.Handle) == OS_EOK);
        assert(SHMDetach(&result.Buffer) == OS_EOK);
        __Drain();
        assert(g_buffer == NULL);
        __FinishBoundFixture(&fixture);
    }
}
