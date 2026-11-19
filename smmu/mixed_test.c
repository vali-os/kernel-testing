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
 * Mixed native SMMU backends through real manager, grants and SHM operations.
 */

#define main __BoundRegressionMain
// mixed_support.inc wraps the pool release with its own memory checks.
#define DMA_TEST_EXTERNAL_POOL_RELEASE
#include "../dma_bound_test.c"
#undef main
#include "v3/model.h"
#include "mmu500/model.h"
#include "../../kernel/memory/dma/private.h"
#include "mixed_support.inc"
#include <sys/wait.h>
#include <unistd.h>

#define __MIXED_IOVA_LAST 0x1fffffULL
#define __MIXED_DOMAINS 4U

// Four independent domains let one generation fail while a reset peer and an
// unrelated domain on the other generation remain distinguishable.
typedef struct __MixedDomain {
    unsigned int          Backend;
    uint32_t              SID;
    DMATranslatedDomain_t* Domain;
    uint64_t              Attachment;
    OSDmaBindingResult_t   Binding;
    uuid_t                ContextHandle;
    SHMDeviceContext_t*    Context;
} __MixedDomain_t;

typedef struct __MixedFixture {
    MSContextSecurity_t* Service;
    MSContextSecurity_t* Driver;
    DMAManager_t*        Manager;
    uuid_t              ManagerHandle;
    unsigned int        Count;
    __MixedDomain_t      Domains[__MIXED_DOMAINS];
} __MixedFixture_t;

static void
__AckMixedEvents(
    _In_ DMAManager_t* manager)
{
    DMAAttachmentEventBatch_t batch;

    do {
        assert(DMAManagerSnapshotEvents(manager, DMA_MANAGER_MAX_EVENTS, &batch) == OS_EOK);
        assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    } while (batch.Count != 0);
}

static DMATranslatedDomainConfig_t
__MixedConfig(
    _In_ uint64_t        key,
    _In_ const uint32_t* sid,
    _In_ const uuid_t*   device,
    _In_ uint64_t        first)
{
    return (DMATranslatedDomainConfig_t) {
        .Host = 1, .RequesterGroup = key, .CachePolicy = SHMDeviceCacheCoherent,
        .AddressLimit = __MIXED_IOVA_LAST, .HardwareIDs = sid, .HardwareIDCount = 1,
        .Devices = device, .DeviceCount = 1,
        .AddressSpace = { .Bounds = { first, __MIXED_IOVA_LAST }, .Granularity = __MIXED_PAGE_SIZE }
    };
}

static void
__PrepareMixedFixture(
    _InOut_ __MixedFixture_t*    fixture,
    _In_    MSContextSecurity_t* service,
    _In_    MSContextSecurity_t* driver,
    _In_    unsigned int         count,
    _In_    bool                 sharedReset,
    _In_    bool                 equalAddresses)
{
    unsigned int index;

    assert(count == 2 || count == 4);
    memset(fixture, 0, sizeof(*fixture));
    fixture->Service = service;
    fixture->Driver = driver;
    fixture->Count = count;
    __MixedStart();
    g_current = service;
    fixture->Manager = __CreateManager(&fixture->ManagerHandle);
    for (index = 0; index < count; index++) {
        __MixedDomain_t* entry = &fixture->Domains[index];
        uuid_t device = 1800 + index;
        DMATranslatedDomainConfig_t config;
        DMATranslatedAttachmentParameters_t parameters;
        DMAAttachmentRegistration_t* registration = NULL;
        OSDmaAttachmentId_t attachment;

        entry->Backend = index % __MixedBackendCount;
        entry->SID = 7 + index / __MixedBackendCount;
        config = __MixedConfig(300 + index, &entry->SID, &device,
            equalAddresses ? 0x10000 : (index + 1) * 0x10000);
        assert(DMATranslatedDomainCreate(fixture->Manager, g_mixedUnits[entry->Backend],
            &config, &entry->Domain) == OS_EOK);
        assert(entry->Domain->ConnectionSpace == entry->Domain->AddressSpace);
        assert(atomic_load(&entry->Domain->AddressSpace->References) == 2);
        assert(DMATranslatedDomainConnect(entry->Domain) == OS_EOK);
        parameters = (DMATranslatedAttachmentParameters_t) {
            device, sharedReset && index < 2 ? 500 : 500 + index, __MIXED_IOVA_LAST
        };
        assert(DMAManagerPrepareTranslatedAttachment(fixture->Manager, entry->Domain,
            &parameters, &registration, &attachment) == OS_EOK);
        assert(DMAManagerPublishAttachment(&registration) == OS_EOK);
        entry->Attachment = attachment.Value;
    }
    // Finish reset-group membership before any driver opens a binding.
    for (index = 0; index < count; index++) {
        __MixedDomain_t* entry = &fixture->Domains[index];

        entry->Binding = __BindAttachment(fixture->Manager, &(OSDmaBindParameters_t) {
            .Attachment = entry->Attachment, .Principal = driver->GlobalHandleID, .RequestId = 1
        });
    }
    g_current = driver;
    for (index = 0; index < count; index++) {
        __MixedDomain_t* entry = &fixture->Domains[index];
        OSDmaBoundOpenParameters_t parameters = { entry->Binding.Generation, __MIXED_IOVA_LAST };
        SHMRecord_t* record;

        assert(ScDmaOpenBound(entry->Binding.Handle, &parameters, &entry->ContextHandle) == OS_EOK);
        record = LookupHandle(entry->ContextHandle, HandleTypeSHMAccess);
        assert(record != NULL);
        entry->Context = record->Object;
        assert(entry->Context->AddressSpace == entry->Domain->AddressSpace);
    }
}

static void
__CloseMixedClients(
    _InOut_ __MixedFixture_t* fixture)
{
    unsigned int index;

    g_current = fixture->Driver;
    for (index = 0; index < fixture->Count; index++) {
        __MixedDomain_t* entry = &fixture->Domains[index];

        assert(ScResourceRelease(entry->ContextHandle) == OS_EOK);
        assert(DestroyHandle(entry->Binding.Handle) == OS_EOK);
    }
    __Drain();
    g_current = fixture->Service;
    __AckMixedEvents(fixture->Manager);
    __RemoveAllAttachments(fixture->Manager);
}

static void
__FinishMixedFixture(
    _InOut_ __MixedFixture_t* fixture)
{
    unsigned int index;

    __CloseMixedClients(fixture);
    for (index = 0; index < fixture->Count; index++) {
        assert(DMATranslatedDomainDestroy(&fixture->Domains[index].Domain) == OS_EOK);
    }
    assert(DestroyHandle(fixture->ManagerHandle) == OS_EOK);
    DMAManagerRelease(&fixture->Manager);
    __Drain();
    __MixedFinish();
}

// A second space without a manager registry entry must still be unable to use
// an existing SID. This tests the native reservation independently of the
// manager's own duplicate-domain checks.
static void
__CheckMixedSID(
    _In_ __MixedDomain_t* entry,
    _In_ oserr_t         expected)
{
    DMAIOVAConfig_t config = { { 0x10000, __MIXED_IOVA_LAST }, __MIXED_PAGE_SIZE, NULL, 0 };
    DMAIOMMUUnit_t* unit = g_mixedUnits[entry->Backend];
    struct DMAAddressSpace* space = NULL;
    void* connection = NULL;

    assert(DMAAddressSpaceCreateTranslated(unit, &config, &space) == OS_EOK);
    MutexLock(&unit->Lock);
    assert(unit->Ops.PrepareHardwareIDs(space->Translated.BackendSpace,
        &entry->SID, 1, &connection) == expected);
    if (expected == OS_EOK) unit->Ops.DestroyHardwareIDs(space->Translated.BackendSpace, connection);
    else assert(connection == NULL);
    MutexUnlock(&unit->Lock);
    assert(DMAAddressSpaceDestroy(&space) == OS_EOK);
}

static void
__HoldMixedWalks(
    _In_ unsigned int backend,
    _In_ bool         hold)
{
    if (backend == __MixedV3) g_smmuv3Model.HeldWalks = hold ? 1 : 0;
    else g_smmu500Model.HeldWalks = hold ? 1 : 0;
}

static struct OSDmaMappingResult
__MapMixed(
    _In_ __MixedDomain_t*  entry,
    _In_ uuid_t           source,
    _In_ size_t           offset,
    _In_ enum DMADirection direction)
{
    struct OSDmaMapParameters parameters = { .Offset = offset, .Length = __MIXED_PAGE_SIZE,
        .Direction = direction };
    struct OSDmaMappingResult result;

    assert(ScDmaMapShm(entry->ContextHandle, source, &parameters, &result) == OS_EOK);
    assert(result.SegmentCount == 1 && result.InlineSegment.Length == __MIXED_PAGE_SIZE);
    return result;
}

static oserr_t
__MixedAccess(
    _In_  const __MixedDomain_t* entry,
    _In_  uint64_t               address,
    _In_  bool                   write,
    _Out_ uint64_t*              physicalOut)
{
    if (entry->Backend == __MixedV3) {
        return SMMUv3ModelAccess(entry->SID, address, write, physicalOut);
    }
    return SMMU500ModelAccess(entry->SID, address, write, physicalOut);
}

static void
__TestSharedBuffer(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ enum DMADirection    direction)
{
    __MixedFixture_t fixture;
    SHMHandle_t buffer;
    struct OSDmaMappingResult mappings[2];
    uint64_t physical[2];
    unsigned int index;

    __PrepareMixedFixture(&fixture, service, driver, 2, false, false);
    assert(SHMCreateDeviceBuffer(__MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    for (index = 0; index < 2; index++) {
        mappings[index] = __MapMixed(&fixture.Domains[index], buffer.ID, 0, direction);
        assert(__MixedAccess(&fixture.Domains[index], mappings[index].InlineSegment.Address,
            direction != DMAToDevice, &physical[index]) == OS_EOK);
        if (direction == DMABidirectional) {
            uint64_t readPhysical;

            assert(__MixedAccess(&fixture.Domains[index], mappings[index].InlineSegment.Address,
                false, &readPhysical) == OS_EOK);
            assert(readPhysical == physical[index]);
        }
    }
    assert(mappings[0].InlineSegment.Address != mappings[1].InlineSegment.Address);
    assert(physical[0] == physical[1]);
    assert(SMMUTestPhysical(physical[0], __MIXED_PAGE_SIZE) == buffer.Buffer);
    for (index = 0; index < 2; index++) {
        if (direction != DMABidirectional) {
            uint64_t untouched = UINT64_MAX;

            assert(__MixedAccess(&fixture.Domains[index], mappings[index].InlineSegment.Address,
                direction == DMAToDevice, &untouched) == OS_EDEVFAULT);
            assert(untouched == UINT64_MAX);
        }
        assert(ScResourceRelease(mappings[index].Handle) == OS_EOK);
    }
    assert(SHMDetach(&buffer) == OS_EOK);
    __Drain();
    __FinishMixedFixture(&fixture);
}

static void
__TestEqualAddresses(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver)
{
    __MixedFixture_t fixture;
    SHMHandle_t buffer;
    struct OSDmaMappingResult mappings[2];
    uint64_t physical[2];
    unsigned int index;

    __PrepareMixedFixture(&fixture, service, driver, 2, false, true);
    assert(SHMCreateDeviceBuffer(2 * __MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    for (index = 0; index < 2; index++) {
        memset((uint8_t*)buffer.Buffer + index * __MIXED_PAGE_SIZE, 0x31 + index, __MIXED_PAGE_SIZE);
        mappings[index] = __MapMixed(&fixture.Domains[index], buffer.ID, index * __MIXED_PAGE_SIZE, DMABidirectional);
        assert(__MixedAccess(&fixture.Domains[index], mappings[index].InlineSegment.Address,
            false, &physical[index]) == OS_EOK);
        assert(*(uint8_t*)SMMUTestPhysical(physical[index], 1) == 0x31 + index);
        __CheckMixedSID(&fixture.Domains[index], OS_EEXISTS);
        {
            uuid_t device = 2200 + index;
            DMATranslatedDomainConfig_t config = __MixedConfig(800 + index,
                &fixture.Domains[index].SID, &device, 0x10000);
            DMATranslatedDomain_t* duplicate = NULL;

            g_current = service;
            // Native reservation runs before manager registry publication.
            assert(DMATranslatedDomainCreate(fixture.Manager, g_mixedUnits[index],
                &config, &duplicate) == OS_EEXISTS);
            assert(duplicate == NULL);
            g_current = driver;
        }
    }
    assert(mappings[0].InlineSegment.Address == mappings[1].InlineSegment.Address);
    assert(physical[0] + __MIXED_PAGE_SIZE == physical[1]);
    *(uint8_t*)SMMUTestPhysical(physical[0], 1) = 0x55;
    assert(*((uint8_t*)buffer.Buffer + __MIXED_PAGE_SIZE) == 0x32);
    for (index = 0; index < 2; index++) assert(ScResourceRelease(mappings[index].Handle) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK);
    __Drain();
    __FinishMixedFixture(&fixture);
}

static void
__TestDelayedMapping(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ unsigned int        backend)
{
    __MixedFixture_t fixture;
    SHMHandle_t buffer;
    struct OSDmaMappingResult mappings[2];
    SHMDeviceMapping_t* mapping;
    DMAIOVARequest_t request = { .Length = __MIXED_PAGE_SIZE, .Limit = __MIXED_IOVA_LAST };
    DMAIOVAAllocation_t* interval = NULL;
    OSDmaStopTicket_t ticket;
    uint64_t physical;
    uint64_t after;
    unsigned int peer = 1 - backend;

    __PrepareMixedFixture(&fixture, service, driver, 2, false, false);
    assert(SHMCreateDeviceBuffer(__MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    mappings[backend] = __MapMixed(&fixture.Domains[backend], buffer.ID, 0, DMAToDevice);
    mappings[peer] = __MapMixed(&fixture.Domains[peer], buffer.ID, 0, DMAToDevice);
    mapping = ((SHMRecord_t*)LookupHandle(mappings[backend].Handle, HandleTypeSHMAccess))->Object;
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &physical) == OS_EOK);
    __HoldMixedWalks(backend, true);
    assert(ScResourceRelease(mappings[backend].Handle) == OS_ETIMEOUT);
    assert(mapping->AddressMapping != NULL && mapping->Access.Buffer != NULL);
    assert(DMAIOVAAllocate(fixture.Domains[backend].Domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMAIOVAAllocationRange(interval)->First != mappings[backend].InlineSegment.Address);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    // Closing the source's CPU view and the independent mapping must not free
    // pages that the delayed unit can still reach through its cached entry.
    assert(SHMDetach(&buffer) == OS_EOK);
    assert(ScResourceRelease(mappings[peer].Handle) == OS_EOK);
    __Drain();
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &after) == OS_EOK);
    assert(after == physical && __FindMixedMemory(physical, __MIXED_PAGE_SIZE) != NULL);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Domains[backend].Attachment, &ticket) == OS_EOK);
    // Physical stop is simulated here: no new modeled device access is issued
    // after confirmation. Old table walks still hold the invalidation open.
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ETIMEOUT);
    assert(mapping->AddressMapping != NULL);
    __HoldMixedWalks(backend, false);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(mapping->State == SHMDeviceMappingStopped && mapping->AddressMapping == NULL);
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &after) == OS_EDEVFAULT);
    assert(DMAIOVAAllocate(fixture.Domains[backend].Domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMAIOVAAllocationRange(interval)->First == mappings[backend].InlineSegment.Address);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    g_current = driver;
    assert(ScResourceRelease(mappings[backend].Handle) == OS_EOK);
    __Drain();
    assert(__FindMixedMemory(physical, __MIXED_PAGE_SIZE) == NULL);
    __FinishMixedFixture(&fixture);
}

// A 512-byte restricted grant inside a pool buffer, mapped through one real
// backend's register model. The 512 bytes are not whole granules, so the
// kernel copies them through one private 4 KiB bounce granule. The model's
// translation covers that whole granule, read-only for DMAToDevice, and the
// published segment stays 512 bytes. While a removal is held back by an old
// table walk, the bounce unit is not given back to the pool. The pool release
// wrapper also checks that no cached translation still points into it.
static void
__TestMixedBounce(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ unsigned int        backend)
{
    __MixedFixture_t fixture;
    __MixedDomain_t* entry;
    SHMHandle_t buffer;
    SHMReference_t reference;
    SHMHandle_t view;
    struct OSDmaMapParameters parameters = { .Offset = 0, .Length = 512, .Direction = DMAToDevice };
    struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer, .Length = 512 };
    struct OSDmaMappingResult result;
    SHMDeviceMapping_t* mapping;
    const uint8_t* bounce;
    uint64_t unit;
    uint64_t physical;
    uint8_t* bytes;

    __PrepareMixedFixture(&fixture, service, driver, 2, false, false);
    entry = &fixture.Domains[backend];
    assert(SHMCreateDeviceBuffer(2 * __MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    bytes = buffer.Buffer;
    for (size_t i = 0; i < 2 * __MIXED_PAGE_SIZE; i++) {
        bytes[i] = (uint8_t)(i * 5 + 1);
    }
    assert(SHMResolve(buffer.ID, &reference) == OS_EOK);
    assert(SHMViewCreate(&reference, 100, 512, GrantTokenScopeRead, driver, &view) == OS_EOK);
    assert(ReleaseGlobalHandle(reference.GlobalHandleID) == OS_EINCOMPLETE);
    assert(ScDmaMapShm(entry->ContextHandle, view.ID, &parameters, &result) == OS_EOK);
    assert(result.SegmentCount == 1 && result.InlineSegment.Length == 512);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    assert(mapping->Stream.Bounce.Translated && mapping->Stream.Bounce.UnitCount == 1);
    unit = mapping->Stream.Bounce.Units[0].PhysicalBase;
    assert(__FindMixedMemory(unit, __MIXED_PAGE_SIZE) != NULL);

    // The whole granule is installed for reads, and nothing past it.
    assert(__MixedAccess(entry, result.InlineSegment.Address, false, &physical) == OS_EOK && physical == unit);
    assert(__MixedAccess(entry, result.InlineSegment.Address + __MIXED_PAGE_SIZE - 1, false, &physical) == OS_EOK);
    assert(physical == unit + __MIXED_PAGE_SIZE - 1);
    assert(__MixedAccess(entry, result.InlineSegment.Address, true, &physical) == OS_EDEVFAULT);
    assert(__MixedAccess(entry, result.InlineSegment.Address + __MIXED_PAGE_SIZE, false, &physical) == OS_EDEVFAULT);
    assert(ScShmAccessBegin(NULL, result.Handle, &begin) == OS_EOK);
    bounce = SMMUTestPhysical(unit, __MIXED_PAGE_SIZE);
    for (size_t i = 0; i < __MIXED_PAGE_SIZE; i++) {
        assert(bounce[i] == (i < 512 ? bytes[100 + i] : 0));
    }
    assert(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 512) == OS_EOK);

    // A held table walk delays the removal. The unit stays allocated.
    __HoldMixedWalks(backend, true);
    assert(ScResourceRelease(result.Handle) == OS_ETIMEOUT);
    assert(mapping->AddressMapping != NULL && mapping->Stream.Bounce.Units[0].Owner != NULL);
    assert(__FindMixedMemory(unit, __MIXED_PAGE_SIZE) != NULL);
    __HoldMixedWalks(backend, false);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __Drain();
    assert(__FindMixedMemory(unit, __MIXED_PAGE_SIZE) == NULL);
    assert(DestroyHandle(view.ID) == OS_EOK);
    assert(SHMDetach(&buffer) == OS_EOK);
    __Drain();
    __FinishMixedFixture(&fixture);
}

static void
__TestDelayedDisconnect(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ unsigned int        backend)
{
    __MixedFixture_t fixture;
    DMATranslatedDomain_t* domain;
    void* connection;
    struct DMAAddressSpace* space;
    unsigned int peer = 1 - backend;

    __PrepareMixedFixture(&fixture, service, driver, 2, false, false);
    __CloseMixedClients(&fixture);
    domain = fixture.Domains[backend].Domain;
    connection = domain->Connection;
    space = domain->AddressSpace;
    __HoldMixedWalks(backend, true);
    assert(DMATranslatedDomainDestroy(&fixture.Domains[backend].Domain) == OS_ETIMEOUT);
    assert(domain->Connection == connection && domain->ConnectionSpace == space);
    assert(domain->ConnectionState == DMAConnectionDisconnecting);
    assert(atomic_load(&space->References) == 2);
    __CheckMixedSID(&fixture.Domains[backend], OS_EEXISTS);
    assert(DMATranslatedDomainDestroy(&fixture.Domains[peer].Domain) == OS_EOK);
    __CheckMixedSID(&fixture.Domains[peer], OS_EOK);
    // The healthy unit can return its entire control arena while old walks
    // still pin the other unit's disconnect and hardware IDs.
    if (peer == __MixedV3) {
        assert(SMMUv3UnitDestroy(&g_mixedV3) == OS_EOK && g_mixedV3 == NULL);
    } else {
        assert(SMMU500UnitDestroy(&g_mixed500) == OS_EOK && g_mixed500 == NULL);
    }
    g_mixedUnits[peer] = NULL;
    __HoldMixedWalks(backend, false);
    assert(DMATranslatedDomainDestroy(&fixture.Domains[backend].Domain) == OS_EOK);
    __CheckMixedSID(&fixture.Domains[backend], OS_EOK);
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    DMAManagerRelease(&fixture.Manager);
    __Drain();
    __MixedFinish();
}

static void
__InjectMixedFault(
    _In_ __MixedDomain_t* entry,
    _In_ uint64_t        address,
    _In_ bool            unreliable)
{
    if (entry->Backend == __MixedV3) {
        DMAIOMMUFault_t faults[2];
        uint32_t count = 77;

        // A translation fault is a device fault since FC.2. Poll returns its
        // record and leaves the unit healthy. The DMATranslatedDomainFail call
        // below still fails the unit, so the rest of this case keeps checking
        // a failed unit with reliable completion. FC.4 splits this case.
        if (unreliable) g_smmuv3Model.Registers[SMMUV3_GERROR / 4] = 1;
        else SMMUv3ModelEvent(entry->SID, 0x10, address);
        assert(SMMUv3UnitPoll(g_mixedV3, faults, 2, &count) == (unreliable ? OS_ECANCELLED : OS_EOK));
        assert(count == (unreliable ? 0U : 1U));
        if (!unreliable) {
            assert(faults[0].Kind == DMAIOMMUFaultNoMapping && faults[0].HardwareID == entry->SID);
            assert(faults[0].DeviceAddress == address && (faults[0].Flags & DMA_IOMMU_FAULT_ADDRESS) != 0);
            assert(DMAIOMMUUnitCheck(g_mixedUnits[__MixedV3]) == OS_EOK);
        }
        assert(g_mixedV3->Common.CompletionUnreliable == unreliable);
    } else {
        SMMU500Space_t* space = entry->Domain->AddressSpace->Translated.BackendSpace;
        uint32_t bank = SMMU500ModelBank(space->Bank);
        DMAIOMMUFault_t faults[2];
        uint32_t count = 77;

        // A translation fault (TF) is a device fault since FC.3, and Poll
        // returns its record with a healthy unit. MULTI alone means faults
        // were lost with no recorded fault, which still fails the unit. The
        // DMATranslatedDomainFail call below fails the unit in both cases, as
        // for SMMUv3. FC.4 splits this case.
        SMMU500ModelContextFault(space->Bank, unreliable ? (1U << 31) : (1U << 1), entry->SID, address, false);
        assert(SMMU500UnitPoll(g_mixed500, faults, 2, &count) == (unreliable ? OS_ECANCELLED : OS_EOK));
        assert(count == (unreliable ? 0U : 1U));
        if (!unreliable) {
            assert(faults[0].Kind == DMAIOMMUFaultNoMapping && faults[0].HardwareID == entry->SID);
            assert(faults[0].DeviceAddress == address && (faults[0].Flags & DMA_IOMMU_FAULT_ADDRESS) != 0);
            assert(DMAIOMMUUnitCheck(g_mixedUnits[__Mixed500]) == OS_EOK);
        }
        assert(g_mixed500->Common.CompletionUnreliable == unreliable);
        assert(g_smmu500Model.Registers[(bank + SMMU500_CB_FSR) / 4] == 0);
    }
    // Poll has released the unit mutex. Recovery takes manager/group locks.
    assert(!g_mixedUnits[entry->Backend]->Lock.held);
    assert(DMATranslatedDomainFail(entry->Domain, OS_EDEVFAULT) == OS_EOK);
}

// Failed units are terminal at the generic boundary. Run in a child so the
// test can check pinned records without resetting health or clearing caches
// merely to satisfy the parent fixture's leak checks.
static void
__TestMixedFault(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ unsigned int        backend,
    _In_ bool                unreliable)
{
    __MixedFixture_t fixture;
    SHMHandle_t buffer;
    struct OSDmaMappingResult mappings[4];
    SHMDeviceMapping_t* deviceMappings[4];
    struct OSDmaMappingResult independent;
    OSDmaStopTicket_t ticket;
    uint64_t physical[4];
    uint64_t after;
    unsigned int index;
    unsigned int healthy = 3 - backend;
    DMAIOMMUUnit_t* unit;
    SMMUv3ModelRoute_t v3Routes[SMMUV3_MODEL_CACHE_ENTRIES];
    SMMUv3ModelTranslation_t v3Translations[SMMUV3_MODEL_CACHE_ENTRIES];
    SMMU500ModelRoute_t mmu500Routes[SMMU500_MODEL_CACHE_ENTRIES];
    SMMU500ModelTranslation_t mmu500Translations[SMMU500_MODEL_CACHE_ENTRIES];

    __PrepareMixedFixture(&fixture, service, driver, 4, true, false);
    assert(SHMCreateDeviceBuffer(2 * __MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    for (index = 0; index < 4; index++) {
        mappings[index] = __MapMixed(&fixture.Domains[index], buffer.ID, 0, DMAToDevice);
        deviceMappings[index] = ((SHMRecord_t*)LookupHandle(mappings[index].Handle, HandleTypeSHMAccess))->Object;
        assert(__MixedAccess(&fixture.Domains[index], mappings[index].InlineSegment.Address, false, &physical[index]) == OS_EOK);
    }
    memcpy(v3Routes, g_smmuv3Model.Routes, sizeof(v3Routes));
    memcpy(v3Translations, g_smmuv3Model.Translations, sizeof(v3Translations));
    memcpy(mmu500Routes, g_smmu500Model.Routes, sizeof(mmu500Routes));
    memcpy(mmu500Translations, g_smmu500Model.Translations, sizeof(mmu500Translations));
    __InjectMixedFault(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, unreliable);
    // Check before another modeled access can refill the caches from the still
    // valid tables. Poll must preserve Present, SID, VMID, IOVA and descriptor.
    assert(memcmp(v3Routes, g_smmuv3Model.Routes, sizeof(v3Routes)) == 0);
    assert(memcmp(v3Translations, g_smmuv3Model.Translations, sizeof(v3Translations)) == 0);
    assert(memcmp(mmu500Routes, g_smmu500Model.Routes, sizeof(mmu500Routes)) == 0);
    assert(memcmp(mmu500Translations, g_smmu500Model.Translations, sizeof(mmu500Translations)) == 0);
    for (index = 0; index < 4; index++) {
        __MixedDomain_t* entry = &fixture.Domains[index];
        bool blocked = index != healthy;

        assert(entry->Context->Binding->Group->Access.State == (blocked ? DMARecoveryBlocked : DMARecoveryLive));
        assert(__FindTestAttachment(fixture.Manager, entry->Attachment)->Event.Queued == blocked);
        assert(DMABindingCheckOperation(entry->Context->Binding) == (blocked ? OS_ECANCELLED : OS_EOK));
        // Acknowledging a diagnostic and blocking software admission do not
        // invalidate hardware. Cached routes and translations still work.
        assert(__MixedAccess(entry, mappings[index].InlineSegment.Address, false, &after) == OS_EOK);
        assert(after == physical[index]);
    }
    independent = __MapMixed(&fixture.Domains[healthy], buffer.ID, __MIXED_PAGE_SIZE, DMAToDevice);
    assert(ScResourceRelease(independent.Handle) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Domains[backend].Attachment, &ticket) == OS_EOK);
    // Simulate physical stop for the shared reset group. Its healthy backend
    // cleans up even though the failed backend makes the overall pass fail.
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    assert(deviceMappings[1 - backend]->State == SHMDeviceMappingStopped);
    assert(deviceMappings[1 - backend]->AddressMapping == NULL);
    assert(deviceMappings[backend]->AddressMapping != NULL);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);

    // The native backend distinguishes client faults from lost completion.
    // The generic gate currently rejects both, so probe this narrower native
    // contract directly, under its mutex, without freeing generic records.
    unit = g_mixedUnits[backend];
    MutexLock(&unit->Lock);
    assert(unit->Ops.DisconnectHardwareIDs(fixture.Domains[backend].Domain->AddressSpace->Translated.BackendSpace,
        fixture.Domains[backend].Domain->Connection, unit->Config.CompletionTimeoutNs) ==
        (unreliable ? OS_ECANCELLED : OS_EOK));
    MutexUnlock(&unit->Lock);
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &after) ==
        (unreliable ? OS_EOK : OS_EDEVFAULT));
    assert(deviceMappings[backend]->Access.Buffer != NULL);
    assert(__FindMixedMemory(physical[backend], __MIXED_PAGE_SIZE) != NULL);
    // Neither ordinary diagnostics nor a second cleanup pass restores health.
    assert(DMAIOMMUUnitCheck(unit) == OS_ECANCELLED);
}

static void
__TestMixedManagerLoss(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver,
    _In_ unsigned int        backend,
    _In_ bool                confirmed)
{
    __MixedFixture_t fixture;
    SHMHandle_t buffer;
    struct OSDmaMappingResult mappings[2];
    SHMDeviceMapping_t* mapping;
    OSDmaStopTicket_t ticket;
    DMAManager_t* replacement = NULL;
    uuid_t unused = UUID_INVALID;
    uint64_t physical;
    uint64_t after;
    unsigned int index;

    __PrepareMixedFixture(&fixture, service, driver, 2, true, false);
    assert(SHMCreateDeviceBuffer(__MIXED_PAGE_SIZE, __MIXED_PAGE_SIZE, 0, &buffer) == OS_EOK);
    for (index = 0; index < 2; index++) mappings[index] = __MapMixed(&fixture.Domains[index], buffer.ID, 0, DMAToDevice);
    mapping = ((SHMRecord_t*)LookupHandle(mappings[backend].Handle, HandleTypeSHMAccess))->Object;
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &physical) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Domains[backend].Attachment, &ticket) == OS_EOK);
    if (confirmed) {
        __HoldMixedWalks(backend, true);
        // A simulated physical stop preceded this call. Completion still waits
        // for old hardware walks, leaving a real retryable SHM mapping.
        assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ETIMEOUT);
    }
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    assert(fixture.Manager->State == DMAManagerClosed);
    assert(DMAManagerPrepareCreate(&replacement, &unused) == OS_EBUSY && replacement == NULL);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    for (index = 0; index < 2; index++) {
        DMARecoveryGroup_t* group = fixture.Domains[index].Context->Binding->Group;

        assert(group->Access.ManagerClosed);
        assert(group->Access.State == (confirmed ? DMARecoveryStopConfirmed : DMARecoveryBlocked));
        __CheckMixedSID(&fixture.Domains[index], OS_EEXISTS);
    }
    // Manager loss prevents new driver cleanup admission, even if a previous
    // pass recorded stop. Do not fabricate a manager or reset this gate.
    g_current = driver;
    assert(ScResourceRelease(mappings[backend].Handle) == OS_ECANCELLED);
    assert(SHMDetach(&buffer) == OS_EOK);
    __Drain();
    assert(mapping->AddressMapping != NULL && mapping->Access.Buffer != NULL);
    assert(__MixedAccess(&fixture.Domains[backend], mappings[backend].InlineSegment.Address, false, &after) == OS_EOK);
    assert(after == physical && __FindMixedMemory(physical, __MIXED_PAGE_SIZE) != NULL);
    assert(DMATranslatedDomainDestroy(&fixture.Domains[backend].Domain) == OS_EBUSY);
}

int
main(void)
{
    MSContextSecurity_t service;
    MSContextSecurity_t driver;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    __TestSharedBuffer(&service, &driver, DMAToDevice);
    __TestSharedBuffer(&service, &driver, DMAFromDevice);
    __TestSharedBuffer(&service, &driver, DMABidirectional);
    __TestEqualAddresses(&service, &driver);
    for (unsigned int backend = 0; backend < __MixedBackendCount; backend++) {
        __TestDelayedMapping(&service, &driver, backend);
        __TestDelayedDisconnect(&service, &driver, backend);
        __TestMixedBounce(&service, &driver, backend);
    }
    for (unsigned int scenario = 0; scenario < 8; scenario++) {
        int status;
        pid_t child = fork();

        assert(child >= 0);
        if (child == 0) {
            if (scenario < 4) __TestMixedFault(&service, &driver, scenario % 2, scenario >= 2);
            else __TestMixedManagerLoss(&service, &driver, scenario % 2, scenario >= 6);
            _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            // Keep the child's actual status. A parent assertion would turn
            // sanitizer crashes into apparent mutation-test successes.
            fprintf(stderr, "Mixed SMMU child %u failed (wait status %d)\n", scenario, status);
            return EXIT_FAILURE;
        }
    }
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(driver.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("Mixed SMMU: buffers, permissions, reservations, delayed cleanup, faults and manager loss passed");
    return 0;
}
