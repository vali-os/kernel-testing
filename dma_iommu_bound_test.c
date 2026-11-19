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

// Reuse the real handle/grant/SHM fixture and fault hooks from the direct suite.
// Its test entry is renamed. This file supplies translated-domain scenarios.
#define main __DirectBoundRegressionMain
#include "dma_bound_test.c"
#undef main
#include "dma_iommu_sim.h"
#include "../kernel/memory/dma/private.h"
#include <sys/wait.h>
#include <unistd.h>

#define __IOVA_FIRST 0x10000ULL
#define __IOVA_LAST  0x1fffffULL

// This suite turns kernel warnings into calls here, so translated bounce cases
// can check what was reported and how often. Like errors, the history starts
// over when full, and tests clear it before the calls they check.
static char g_boundWarningHistory[16384];
void TestDmaWarning(const char* format, ...)
{
    char    message[512];
    va_list arguments;
    size_t  used = strlen(g_boundWarningHistory);
    int     length;

    va_start(arguments, format);
    length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(message));
    if (used + (size_t)length + 2 > sizeof(g_boundWarningHistory)) {
        used = 0;
    }
    snprintf(g_boundWarningHistory + used, sizeof(g_boundWarningHistory) - used, "%s\n", message);
}

// Fault injection runs inside the real unit callback boundary. Only the atomic
// latch is legal there. Recovery must run later, after the unit mutex is gone.
enum __CallbackFault {
    __FaultNone, __FaultCreate, __FaultCreateError, __FaultDestroy,
    __FaultPrepare, __FaultInstall, __FaultRemove, __FaultDestroyMapping,
    __FaultPrepareHardwareIDs, __FaultConnect, __FaultDisconnect, __FaultDestroyHardwareIDs,
    __FaultConnectPublication
};
static enum __CallbackFault g_callbackFault;
static DMAIOMMUBackendOps_t g_simOps;
static DMAIOMMUUnit_t*      g_faultUnit;
static bool                g_pauseInstall;
static bool                g_pauseConnect;
static pthread_barrier_t   g_callbackReady;
static pthread_barrier_t   g_callbackContinue;
static void (*g_spaceReferenceHook)(void);
static void (*g_spaceCreatedHook)(void);
static void (*g_connectionPreparedHook)(void);
static void (*g_connectionCheckHook)(void);
static void (*g_segmentsHook)(void);
static DMAManager_t* g_snapshotFailureManager;

static void
__Barrier(pthread_barrier_t* barrier)
{
    int status = pthread_barrier_wait(barrier);

    assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void
__LatchFailure(void)
{
    assert(g_faultUnit != NULL);
    DMAIOMMUUnitFail(g_faultUnit, OS_EDEVFAULT);
}

static void
__CallbackFailure(enum __CallbackFault point)
{
    if (g_callbackFault == point) {
        g_callbackFault = __FaultNone;
        __LatchFailure();
    }
}

static oserr_t
__FaultCreateSpace(void* context, void** space)
{
    oserr_t status;

    if (g_callbackFault == __FaultCreateError) {
        __CallbackFailure(__FaultCreateError);
        return OS_EOOM;
    }
    status = g_simOps.CreateSpace(context, space);
    if (status != OS_EOK) {
        return status;
    }
    __CallbackFailure(__FaultCreate);
    return OS_EOK;
}

static void
__FaultDestroySpace(void* context, void* space)
{
    g_simOps.DestroySpace(context, space);
    __CallbackFailure(__FaultDestroy);
}

static oserr_t
__FaultPrepareMapping(void* space, const DMAIOMMUMappingConfig_t* config, void** mapping)
{
    oserr_t status = g_simOps.PrepareMapping(space, config, mapping);

    if (status != OS_EOK) {
        return status;
    }
    __CallbackFailure(__FaultPrepare);
    return OS_EOK;
}

static oserr_t
__FaultInstallMapping(void* space, void* mapping, uint64_t timeout)
{
    oserr_t status = g_simOps.InstallMapping(space, mapping, timeout);

    if (status != OS_EOK) {
        return status;
    }
    if (g_pauseInstall) {
        __Barrier(&g_callbackReady);
        __Barrier(&g_callbackContinue);
    }
    __CallbackFailure(__FaultInstall);
    return OS_EOK;
}

static oserr_t
__FaultRemoveMapping(void* space, void* mapping, uint64_t timeout)
{
    oserr_t status = g_simOps.RemoveMapping(space, mapping, timeout);

    if (status != OS_EOK) {
        return status;
    }
    __CallbackFailure(__FaultRemove);
    return OS_EOK;
}

static void
__FaultDestroyMappingCallback(void* space, void* mapping)
{
    g_simOps.DestroyMapping(space, mapping);
    __CallbackFailure(__FaultDestroyMapping);
}

static oserr_t
__FaultPrepareHardwareIDsCallback(void* space, const uint32_t* hardwareIDs, uint32_t count, void** connection)
{
    oserr_t status = g_simOps.PrepareHardwareIDs(space, hardwareIDs, count, connection);

    if (status != OS_EOK) {
        return status;
    }
    __CallbackFailure(__FaultPrepareHardwareIDs);
    return OS_EOK;
}

static oserr_t
__FaultConnectHardwareIDs(void* space, void* connection, uint64_t timeout)
{
    oserr_t status = g_simOps.ConnectHardwareIDs(space, connection, timeout);

    if (status != OS_EOK) {
        return status;
    }
    if (g_pauseConnect) {
        __Barrier(&g_callbackReady);
        __Barrier(&g_callbackContinue);
    }
    if (g_callbackFault == __FaultConnectPublication) {
        // Latch failure after the unit-locked health read returned success.
        // The later manager-locked publication read must still reject it.
        g_callbackFault = __FaultNone;
        g_connectionCheckHook = __LatchFailure;
    }
    __CallbackFailure(__FaultConnect);
    return OS_EOK;
}

static oserr_t
__FaultDisconnectHardwareIDs(void* space, void* connection, uint64_t timeout)
{
    oserr_t status = g_simOps.DisconnectHardwareIDs(space, connection, timeout);

    if (status != OS_EOK) {
        return status;
    }
    __CallbackFailure(__FaultDisconnect);
    return OS_EOK;
}

static void
__FaultDestroyHardwareIDsCallback(void* space, void* connection)
{
    g_simOps.DestroyHardwareIDs(space, connection);
    __CallbackFailure(__FaultDestroyHardwareIDs);
}

oserr_t __real_DMAIOMMUUnitCreate(const DMAIOMMUUnitConfig_t*, const DMAIOMMUBackendOps_t*,
                                 void*, DMAIOMMUUnit_t**);
oserr_t
__wrap_DMAIOMMUUnitCreate(const DMAIOMMUUnitConfig_t* config, const DMAIOMMUBackendOps_t* ops,
                         void* context, DMAIOMMUUnit_t** unit)
{
    DMAIOMMUBackendOps_t wrapped = *ops;

    g_simOps = *ops;
    wrapped.CreateSpace = __FaultCreateSpace;
    wrapped.DestroySpace = __FaultDestroySpace;
    wrapped.PrepareMapping = __FaultPrepareMapping;
    wrapped.InstallMapping = __FaultInstallMapping;
    wrapped.RemoveMapping = __FaultRemoveMapping;
    wrapped.DestroyMapping = __FaultDestroyMappingCallback;
    wrapped.PrepareHardwareIDs = __FaultPrepareHardwareIDsCallback;
    wrapped.ConnectHardwareIDs = __FaultConnectHardwareIDs;
    wrapped.DisconnectHardwareIDs = __FaultDisconnectHardwareIDs;
    wrapped.DestroyHardwareIDs = __FaultDestroyHardwareIDsCallback;
    return __real_DMAIOMMUUnitCreate(config, &wrapped, context, unit);
}

oserr_t __real_DMAConnectionPrepare(DMATranslatedDomain_t*);
oserr_t
__wrap_DMAConnectionPrepare(DMATranslatedDomain_t* domain)
{
    oserr_t status = __real_DMAConnectionPrepare(domain);

    if (status == OS_EOK) {
        __RunHook(&g_connectionPreparedHook);
    }
    return status;
}

oserr_t __real_DMAAddressSpaceCheck(const struct DMAAddressSpace*);
oserr_t
__wrap_DMAAddressSpaceCheck(const struct DMAAddressSpace* space)
{
    oserr_t status = __real_DMAAddressSpaceCheck(space);

    if (status == OS_EOK) {
        __RunHook(&g_connectionCheckHook);
    }
    return status;
}

oserr_t __real_DMAAddressSpaceAcquire(struct DMAAddressSpace*);
oserr_t
__wrap_DMAAddressSpaceAcquire(struct DMAAddressSpace* space)
{
    oserr_t status = __real_DMAAddressSpaceAcquire(space);

    if (status == OS_EOK) {
        __RunHook(&g_spaceReferenceHook);
    }
    return status;
}

oserr_t __real_DMAAddressSpaceCreateTranslated(DMAIOMMUUnit_t*, const DMAIOVAConfig_t*, struct DMAAddressSpace**);
oserr_t
__wrap_DMAAddressSpaceCreateTranslated(DMAIOMMUUnit_t* unit, const DMAIOVAConfig_t* config,
                                       struct DMAAddressSpace** space)
{
    oserr_t status = __real_DMAAddressSpaceCreateTranslated(unit, config, space);

    if (status == OS_EOK) {
        __RunHook(&g_spaceCreatedHook);
    }
    return status;
}

// A nonzero g_segmentCountOverride reports that many segments once, so a
// test can make the published payload check fail after preparation.
static uint32_t g_segmentCountOverride;

const struct DMASegment* __real_DMAAddressMappingSegments(const struct DMAAddressMapping*, uint32_t*);
const struct DMASegment*
__wrap_DMAAddressMappingSegments(const struct DMAAddressMapping* mapping, uint32_t* count)
{
    const struct DMASegment* segments = __real_DMAAddressMappingSegments(mapping, count);

    __RunHook(&g_segmentsHook);
    if (g_segmentCountOverride != 0) {
        *count = g_segmentCountOverride;
        g_segmentCountOverride = 0;
    }
    return segments;
}

oserr_t __real_DMARecoveryGroupAcquire(DMARecoveryGroup_t*);
oserr_t
__wrap_DMARecoveryGroupAcquire(DMARecoveryGroup_t* group)
{
    if (g_snapshotFailureManager != NULL) {
        // The failure pass must block every affected group before its first
        // fallible snapshot reference, including direct and other-unit peers.
        foreach (node, &g_snapshotFailureManager->Registry.Attachments) {
            DMAAttachment_t* attachment = node->value;

            if (attachment->Configuration.Device != 1605) {
                assert(attachment->RecoveryGroup->Access.State == DMARecoveryBlocked);
                assert(attachment->Event.Queued);
            }
        }
        return OS_EOVERFLOW;
    }
    return __real_DMARecoveryGroupAcquire(group);
}

// Model the CPU view while running the real pool-backed SHM allocator. Fill
// fresh host storage with nonzero bytes first, so omitting MAPPING_CLEAN is
// observable. Ordinary source buffers still use the existing bound fixture.
static void*  g_sharedView;
static size_t g_sharedViewLength;

oserr_t __real_MemorySpaceMap(MemorySpace_t*, struct MemorySpaceMapOptions*, vaddr_t*);
oserr_t __real_MemorySpaceUnmap(MemorySpace_t*, vaddr_t, size_t);
void __real_kfree(void*);

// The physical RAM model of the translated bounce cases, defined in
// dma_translated_bounce_test.inc. Each returns false for views it does not own.
static bool __RamMapView(struct MemorySpaceMapOptions* options, vaddr_t* address, oserr_t* status);
static bool __RamUnmapView(vaddr_t address, size_t length, oserr_t* status);
static void __RamStart(void);
static void __RamStop(void);

oserr_t
__wrap_MemorySpaceMap(MemorySpace_t* space, struct MemorySpaceMapOptions* options, vaddr_t* address)
{
    oserr_t status;

    if (options->PlacementFlags & MAPPING_VIRTUAL_PROCESS) {
        assert(g_sharedView == NULL && g_buffer == NULL);
        g_buffer = LookupHandle(options->SHMTag, HandleTypeSHM);
        assert(g_buffer != NULL && g_buffer->Allocation.Owner != NULL);
        assert(options->Length == g_buffer->Allocation.Length);
        assert(options->Pages == g_buffer->Pages);
        assert(options->Flags & MAPPING_COMMIT);
        assert(options->Flags & MAPPING_PERSISTENT);
        g_sharedView = aligned_alloc(4096, options->Length);
        assert(g_sharedView != NULL);
        g_sharedViewLength = options->Length;
        memset(g_sharedView, 0xa5, g_sharedViewLength);
        if (options->Flags & MAPPING_CLEAN) {
            memset(g_sharedView, 0, g_sharedViewLength);
        }
        *address = (vaddr_t)g_sharedView;
        return OS_EOK;
    }
    if (__RamMapView(options, address, &status)) {
        return status;
    }
    return __real_MemorySpaceMap(space, options, address);
}

oserr_t
__wrap_MemorySpaceUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    oserr_t status;

    if (address == (vaddr_t)g_sharedView && g_sharedView != NULL) {
        assert(length == g_sharedViewLength);
        free(g_sharedView);
        g_sharedView = NULL;
        g_sharedViewLength = 0;
        return OS_EOK;
    }
    if (__RamUnmapView(address, length, &status)) {
        return status;
    }
    return __real_MemorySpaceUnmap(space, address, length);
}

void
__wrap_kfree(void* memory)
{
    if (memory == g_buffer) {
        g_buffer = NULL;
    }
    __real_kfree(memory);
}

MemorySpace_t*
__wrap_GetCurrentMemorySpace(void)
{
    // Shared pool allocations have a CPU view, but no SHMView mapping records.
    static MemorySpace_t space = { .GroupGlobalHandleID = UUID_INVALID };

    return &space;
}

oserr_t MSUnmap(MemorySpace_t* space, vaddr_t address, size_t length)
{
    (void)space;
    (void)address;
    (void)length;
    // This fixture creates DMA grants, not restricted CPU mapping records.
    assert(false);
    return OS_EUNKNOWN;
}

void FreePhysicalMemory(int count, const uintptr_t* pages)
{
    (void)count;
    (void)pages;
    // Real SHM allocations in this suite must return to their recorded pool.
    assert(false);
}

static DMAIOMMUSim_t*
__CreateLimitedIOMMUSim(uint64_t granule, uint64_t physicalLimit)
{
    DMAIOMMUSim_t* sim = NULL;
    DMAIOMMUSimConfig_t config = {
        .Unit = { .Granularity = granule, .AddressLimit = __IOVA_LAST,
                  .PhysicalLimit = physicalLimit, .CompletionTimeoutNs = 100 },
        .QueueCapacity = 128
    };

    assert(DMAIOMMUSimCreate(&config, &sim) == OS_EOK);
    return sim;
}

static DMAIOMMUSim_t*
__CreateIOMMUSim(uint64_t granule)
{
    return __CreateLimitedIOMMUSim(granule, UINT64_MAX);
}

// IOVA bounds must be whole granules, so granules above 64 KiB start the
// space at their own size.
static DMATranslatedDomainConfig_t
__DomainConfig(uint64_t key, uint64_t granule, const uint32_t* hardwareIDs,
               uint32_t hardwareIDCount, const uuid_t* devices, uint32_t deviceCount)
{
    return (DMATranslatedDomainConfig_t) {
        .Host = 1, .RequesterGroup = key,
        .CachePolicy = SHMDeviceCacheCoherent, .AddressLimit = __IOVA_LAST,
        .HardwareIDs = hardwareIDs, .HardwareIDCount = hardwareIDCount,
        .Devices = devices, .DeviceCount = deviceCount,
        .AddressSpace = { .Bounds = { granule > __IOVA_FIRST ? granule : __IOVA_FIRST, __IOVA_LAST },
                          .Granularity = granule }
    };
}

static uint64_t
__RegisterDomainMember(DMAManager_t* manager, DMATranslatedDomain_t* domain,
                       uuid_t device, uint64_t reset)
{
    DMAAttachmentRegistration_t*        registration = NULL;
    OSDmaAttachmentId_t                 id;
    DMATranslatedAttachmentParameters_t parameters = { device, reset, __IOVA_LAST };

    assert(DMAManagerPrepareTranslatedAttachment(manager, domain, &parameters, &registration, &id) == OS_EOK);
    assert(DMAManagerPublishAttachment(&registration) == OS_EOK && registration == NULL);
    return id.Value;
}

static void
__AckDomainEvents(DMAManager_t* manager)
{
    DMAAttachmentEventBatch_t batch;

    do {
        assert(DMAManagerSnapshotEvents(manager, DMA_MANAGER_MAX_EVENTS, &batch) == OS_EOK);
        assert(DMAManagerFinishEvents(manager, &batch, true) == OS_EOK);
    } while (batch.Count != 0);
}

static void
__FinishDomainManager(DMAManager_t** manager, uuid_t handle, MSContextSecurity_t* service)
{
    g_current = service;
    __AckDomainEvents(*manager);
    __RemoveAllAttachments(*manager);
    assert((*manager)->DomainCount == 0);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(manager);
    __Drain();
}

static void
__TestCheckedSpaceLifetime(void)
{
    struct SHMDeviceRange   ranges = { 0x1000, 0x2000, 0x1000 };
    struct DMAAddressSpace* space = NULL;
    struct DMAAddressSpace* extra;

    assert(DMAAddressSpaceAcquire(NULL) == OS_EINVALPARAMS);
    assert(DMAAddressSpaceDestroy(NULL) == OS_EOK);
    assert(DMAAddressSpaceCreateDirect(&ranges, 1, &space) == OS_EOK);
    assert(DMAAddressSpaceGetGranularity(space) == 0);
    atomic_store(&space->References, 0);
    assert(DMAAddressSpaceAcquire(space) == OS_ECANCELLED);
    atomic_store(&space->References, UINT_MAX);
    assert(DMAAddressSpaceAcquire(space) == OS_EOVERFLOW);
    atomic_store(&space->References, 1);
    assert(DMAAddressSpaceAcquire(space) == OS_EOK);
    extra = space;
    assert(DMAAddressSpaceDestroy(&space) == OS_EBUSY && space == extra);
    DMAAddressSpaceRelease(&extra);
    assert(DMAAddressSpaceDestroy(&space) == OS_EOK && space == NULL);
}

static void
__TestDomainRegistration(MSContextSecurity_t* service)
{
    uint32_t                     hardwareIDs[] = { 17, 0 };
    uuid_t                       devices[] = { 1401, 1400 };
    uint32_t                     otherHardwareID = 23;
    uuid_t                       otherDevice = 1402;
    DMATranslatedDomainConfig_t  config = __DomainConfig(100, 4096, hardwareIDs, 2, devices, 2);
    DMATranslatedDomainConfig_t  other;
    DMATranslatedDomain_t*       domain = NULL;
    DMATranslatedDomain_t*       duplicate = NULL;
    DMAIOMMUSim_t*               sim = __CreateIOMMUSim(4096);
    DMAIOMMUSim_t*               otherSim = __CreateIOMMUSim(4096);
    DMAManager_t*                manager;
    uuid_t                       managerHandle;
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentId_t          attachment = { 99 };
    OSDmaAttachmentParameters_t  direct = __Parameters(devices[0], 101, 100);
    struct DMAAddressSpace*      extra;
    DMAIOVAAllocation_t*         interval = NULL;
    DMAIOVARequest_t             request = { .Length = 4096, .Limit = __IOVA_LAST };
    SHMSG_t                      span = { 1, 2 };
    uint32_t                     count = 99;

    g_current = service;
    manager = __CreateManager(&managerHandle);
    other = config;
    other.HardwareIDCount = 0;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &domain) == OS_EINVALPARAMS);
    other = config;
    other.DeviceCount = UINT32_MAX;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &domain) == OS_ENOTSUPPORTED);
    hardwareIDs[0] = 0;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EINVALPARAMS);
    hardwareIDs[0] = 17;
    devices[0] = devices[1];
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EINVALPARAMS);
    devices[0] = 1401;
    other = config;
    other.AddressLimit = UINT64_MAX;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &domain) == OS_EINVALPARAMS);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(domain) == OS_EOK);
    assert(domain->HardwareIDs[0] == 0 && domain->HardwareIDs[1] == 17);
    assert(domain->Devices[0] == 1400 && domain->Devices[1] == 1401);
    hardwareIDs[0] = 31;
    devices[0] = 1500;
    assert(domain->HardwareIDs[1] == 17 && domain->Devices[1] == 1401);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EBUSY);
    assert(DMAManagerPrepareAttachment(manager, &direct, &registration, &attachment) == OS_EBUSY);
    direct.Device = 1402;
    direct.RequesterGroup = 100;
    assert(DMAManagerPrepareAttachment(manager, &direct, &registration, &attachment) == OS_EBUSY);
    assert(registration == NULL && attachment.Value == 99);

    other = __DomainConfig(102, 4096, &otherHardwareID, 1, &otherDevice, 1);
    other.RequesterGroup = 100;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &duplicate) == OS_EBUSY);
    other.RequesterGroup = 102;
    otherDevice = 1400;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(otherSim), &other, &duplicate) == OS_EBUSY);
    otherDevice = 1402;
    otherHardwareID = 17;
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &duplicate) == OS_EBUSY);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(otherSim), &other, &duplicate) == OS_EOK);
    assert(DMATranslatedDomainConnect(duplicate) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&duplicate) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&otherSim) == OS_EOK);

    // Checked destruction must preserve identities for both extra references
    // and an allocator consistency failure. Completed disconnect removes the route.
    assert(DMAAddressSpaceAcquire(domain->AddressSpace) == OS_EOK);
    extra = domain->AddressSpace;
    assert(DMATranslatedDomainDestroy(&domain) == OS_EBUSY && domain != NULL);
    assert(domain->State == DMATranslatedDomainClosing && manager->DomainCount == 1);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &other, &duplicate) == OS_EBUSY);
    assert(DMAIOMMUSimAccessHardwareID(sim, 17, __IOVA_FIRST, 1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    assert(span.Address == 1 && span.Length == 2 && count == 99);
    assert(DMAIOMMUSimAccessHardwareID(sim, 999, __IOVA_FIRST, 1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    assert(DMAManagerPrepareAttachment(manager, &direct, &registration, &attachment) == OS_EBUSY);
    DMAAddressSpaceRelease(&extra);
    assert(DMAIOVAAllocate(domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EBUSY && domain != NULL);
    assert(atomic_load(&domain->AddressSpace->References) == 1);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EOK && domain == NULL);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __FinishDomainManager(&manager, managerHandle, service);
    puts("Translated domains: canonical copied sets, conflicts, closing reservations and checked destruction passed");
}

static void
__TestClosedManagerDomainLifetime(MSContextSecurity_t* service)
{
    uint32_t                     hardwareID = 0;
    uuid_t                       device = 1400;
    DMATranslatedDomainConfig_t   config = __DomainConfig(100, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t*        domain = NULL;
    DMAIOMMUSim_t*               sim = __CreateIOMMUSim(4096);
    DMAManager_t*                manager;
    DMAManager_t*                replacement = NULL;
    uuid_t                       handle;
    uuid_t                       unused;

    g_current = service;
    manager = __CreateManager(&handle);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(domain) == OS_EOK);
    assert(DestroyHandle(handle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
    assert(domain->Manager->Registry.Count == 0 && domain->Manager->DomainCount == 1);
    assert(DMAManagerPrepareCreate(&replacement, &unused) == OS_EBUSY && replacement == NULL);
    // The domain reference alone keeps the closed manager and registry alive.
    // Removing that final domain allows a new manager to register.
    assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
    __Drain();
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    manager = __CreateManager(&handle);
    __FinishDomainManager(&manager, handle, service);
}

static void
__TestDomainMembership(MSContextSecurity_t* service, MSContextSecurity_t* driver, MSContextSecurity_t* otherDriver)
{
    uint32_t                            hardwareIDs[] = { 0, 17 };
    uuid_t                              devices[] = { 1400, 1401 };
    DMATranslatedDomainConfig_t         config = __DomainConfig(100, 4096, hardwareIDs, 2, devices, 2);
    DMATranslatedDomain_t*              domain = NULL;
    DMAIOMMUSim_t*                      sim = __CreateIOMMUSim(4096);
    DMAManager_t*                       manager;
    uuid_t                              managerHandle;
    uint64_t                            first;
    uint64_t                            second;
    OSDmaBindParameters_t               request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    DMABindingRegistration_t*           registration = NULL;
    OSDmaBindingResult_t                result = { 0 };
    OSDmaBindingResult_t                firstBinding;
    OSDmaBindingResult_t                secondBinding;
    DMATranslatedAttachmentParameters_t parameters = { 1401, 200, __IOVA_LAST };
    DMAAttachmentRegistration_t*        member = NULL;
    OSDmaAttachmentId_t                 memberID;

    g_current = service;
    manager = __CreateManager(&managerHandle);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(domain) == OS_EOK);
    first = __RegisterDomainMember(manager, domain, 1400, 200);
    request.Attachment = first;
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_EBUSY);
    assert(domain->Members == 1 && domain->OwnerSecurityID == 0);
    assert(DMAManagerPrepareTranslatedAttachment(manager, domain, &parameters, &member, &memberID) == OS_EOK);
    assert(domain->Members == 1);
    DMAManagerCancelAttachment(&member);
    assert(domain->Members == 1);
    second = __RegisterDomainMember(manager, domain, 1401, 200);
    assert(__RegisterDomainMember(manager, domain, 1401, 200) == second && domain->Members == 2);
    parameters.ResetGroup++;
    assert(DMAManagerPrepareTranslatedAttachment(manager, domain, &parameters, &member, &memberID) == OS_EBUSY);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EBUSY && domain->State == DMATranslatedDomainReady);
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_EOK);
    DMAManagerCancelBinding(&registration);
    __Drain();
    assert(domain->OwnerSecurityID == 0);
    firstBinding = __BindAttachment(manager, &request);
    assert(domain->OwnerSecurityID == driver->GlobalHandleID);
    request.Attachment = second;
    request.Principal = otherDriver->GlobalHandleID;
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_EPERMISSIONS);
    request.Principal = driver->GlobalHandleID;
    secondBinding = __BindAttachment(manager, &request);
    assert(__FindTestAttachment(manager, first)->RecoveryGroup == __FindTestAttachment(manager, second)->RecoveryGroup);
    assert(DMAManagerRemoveAttachment(manager, second) == OS_EBUSY);
    assert(DestroyHandleInContext(driver, firstBinding.Handle) == OS_EOK);
    assert(DestroyHandleInContext(driver, secondBinding.Handle) == OS_EOK);
    __Drain();
    __AckDomainEvents(manager);
    request.Principal = otherDriver->GlobalHandleID;
    request.RequestId++;
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_EPERMISSIONS);
    assert(DMAManagerRemoveAttachment(manager, second) == OS_EOK && domain->Members == 1);
    request.Principal = driver->GlobalHandleID;
    request.Attachment = first;
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_EBUSY);
    assert(DMAManagerRemoveAttachment(manager, first) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __FinishDomainManager(&manager, managerHandle, service);
    puts("Translated bindings: complete membership, principal lifetime and busy removal passed");
}

static void
__TestFailedResetPeer(MSContextSecurity_t* service, MSContextSecurity_t* driver, bool translatedPeer)
{
    uint32_t                    hardwareID = 0;
    uuid_t                      device = 1400;
    DMATranslatedDomainConfig_t config = __DomainConfig(100, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t*      failedDomain = NULL;
    DMATranslatedDomain_t*      healthyDomain = NULL;
    DMAIOMMUSim_t*              sim = __CreateIOMMUSim(4096);
    DMAIOMMUSim_t*              healthySim = __CreateIOMMUSim(4096);
    DMAManager_t*               manager;
    uuid_t                      managerHandle;
    uint64_t                    failed;
    uint64_t                    peer;
    OSDmaAttachmentParameters_t direct = __Parameters(1401, 101, 200);
    OSDmaBindParameters_t       request = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    DMABindingRegistration_t*   registration = NULL;
    OSDmaBindingResult_t        result;
    OSDmaStopTicket_t           ticket;
    DMARecoveryGroup_t*         group;

    g_current = service;
    manager = __CreateManager(&managerHandle);
    assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &failedDomain) == OS_EOK);
    assert(DMATranslatedDomainConnect(failedDomain) == OS_EOK);
    failed = __RegisterDomainMember(manager, failedDomain, device, 200);
    if (translatedPeer) {
        device = 1401;
        config.RequesterGroup = 101;
        assert(DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(healthySim), &config, &healthyDomain) == OS_EOK);
        assert(DMATranslatedDomainConnect(healthyDomain) == OS_EOK);
        peer = __RegisterDomainMember(manager, healthyDomain, device, 200);
    } else {
        peer = __RegisterAttachment(manager, &direct);
    }
    group = __FindTestAttachment(manager, failed)->RecoveryGroup;
    assert(group == __FindTestAttachment(manager, peer)->RecoveryGroup);
    assert(DMATranslatedDomainFail(failedDomain, OS_EDEVFAULT) == OS_EOK);
    assert(DMAManagerPrepareStop(manager, failed, &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(manager, &ticket) == OS_EOK);
    __AckDomainEvents(manager);
    assert(group->Access.State == DMARecoveryStopCleaned);
    request.Attachment = peer;
    assert(DMAManagerPrepareBinding(manager, &request, &registration, &result) == OS_ECANCELLED);
    assert(group->Access.State == DMARecoveryStopCleaned && group->Stop.Round == ticket.Epoch);
    assert(registration == NULL);
    assert(DMAManagerRemoveAttachment(manager, peer) == OS_EOK);
    assert(DMAManagerRemoveAttachment(manager, failed) == OS_EOK);
    // Software stop cannot prove hardware ID disconnection on a failed unit.
    // This case runs in a child and verifies that the uncertain root stays live.
    assert(DMATranslatedDomainDestroy(&failedDomain) == OS_ECANCELLED);
    assert(failedDomain->Connection != NULL && failedDomain->ConnectionSpace != NULL);
    assert(DMATranslatedDomainDestroy(&healthyDomain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EBUSY);
    assert(DMAIOMMUSimDestroy(&healthySim) == OS_EOK);
    assert(DestroyHandle(managerHandle) == OS_EOK);
    DMAManagerRelease(&manager);
    __Drain();
}

// Up to two logical devices share this domain. Their contexts and syscall
// handles remain independent, while their address-space references coincide.
struct __TranslatedFixture {
    MSContextSecurity_t*   Service;
    MSContextSecurity_t*   Driver;
    DMAManager_t*          Manager;
    uuid_t                 ManagerHandle;
    DMAIOMMUSim_t*         Sim;
    DMATranslatedDomain_t* Domain;
    uint32_t               Count;
    uint64_t               Attachments[2];
    OSDmaBindingResult_t   Bindings[2];
    uuid_t                 ContextHandles[2];
    SHMDeviceContext_t*    Contexts[2];
};

static SHMDeviceContext_t*
__OpenLimitedTranslatedContext(OSDmaBindingResult_t binding, uint64_t addressLimit, uuid_t* handle)
{
    OSDmaBoundOpenParameters_t parameters = { binding.Generation, addressLimit };
    SHMRecord_t*               record;

    assert(ScDmaOpenBound(binding.Handle, &parameters, handle) == OS_EOK);
    record = LookupHandle(*handle, HandleTypeSHMAccess);
    assert(record != NULL);
    return record->Object;
}

static SHMDeviceContext_t*
__OpenTranslatedContext(OSDmaBindingResult_t binding, uuid_t* handle)
{
    return __OpenLimitedTranslatedContext(binding, __IOVA_LAST, handle);
}

// Optional unit, domain and context settings for translated bounce cases.
// IOVAFromZero starts the IOVA space at address zero. A ContextLimit of zero
// opens the contexts with the domain's own address limit.
struct __TranslatedOptions {
    uint64_t                  PhysicalLimit;
    enum SHMDeviceCachePolicy CachePolicy;
    bool                      IOVAFromZero;
    uint64_t                  ContextLimit;
};

static struct __TranslatedFixture
__PrepareTranslatedFixtureWith(MSContextSecurity_t* service, MSContextSecurity_t* driver,
                               uint64_t granule, uint32_t count, const struct __TranslatedOptions* options)
{
    struct __TranslatedFixture  fixture = { .Service = service, .Driver = driver, .Count = count };
    uint32_t                    hardwareIDs[] = { 0, 17 };
    uuid_t                      devices[] = { 1400, 1401 };
    DMATranslatedDomainConfig_t config = __DomainConfig(100, granule, hardwareIDs, count, devices, count);

    assert(count == 1 || count == 2);
    config.CachePolicy = options->CachePolicy;
    if (options->IOVAFromZero) {
        config.AddressSpace.Bounds.First = 0;
    }
    g_current = service;
    fixture.Manager = __CreateManager(&fixture.ManagerHandle);
    fixture.Sim = __CreateLimitedIOMMUSim(granule, options->PhysicalLimit);
    assert(DMATranslatedDomainCreate(fixture.Manager, DMAIOMMUSimUnit(fixture.Sim), &config, &fixture.Domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EOK);
    for (uint32_t i = 0; i < count; i++) {
        fixture.Attachments[i] = __RegisterDomainMember(fixture.Manager, fixture.Domain, devices[i], 200);
    }
    for (uint32_t i = 0; i < count; i++) {
        OSDmaBindParameters_t request = { .Attachment = fixture.Attachments[i],
            .Principal = driver->GlobalHandleID, .RequestId = 1 };

        fixture.Bindings[i] = __BindAttachment(fixture.Manager, &request);
    }
    g_current = driver;
    for (uint32_t i = 0; i < count; i++) {
        fixture.Contexts[i] = __OpenLimitedTranslatedContext(fixture.Bindings[i],
            options->ContextLimit != 0 ? options->ContextLimit : __IOVA_LAST, &fixture.ContextHandles[i]);
        assert(fixture.Contexts[i]->AddressSpace == fixture.Domain->AddressSpace);
    }
    assert(atomic_load(&fixture.Domain->AddressSpace->References) == 2 + count);
    return fixture;
}

static struct __TranslatedFixture
__PrepareTranslatedFixture(MSContextSecurity_t* service, MSContextSecurity_t* driver,
                           uint64_t granule, uint32_t count)
{
    const struct __TranslatedOptions options = { .PhysicalLimit = UINT64_MAX, .CachePolicy = SHMDeviceCacheCoherent };

    return __PrepareTranslatedFixtureWith(service, driver, granule, count, &options);
}

static void
__FinishTranslatedFixture(struct __TranslatedFixture* fixture)
{
    g_current = fixture->Driver;
    for (uint32_t i = 0; i < fixture->Count; i++) {
        assert(ScResourceRelease(fixture->ContextHandles[i]) == OS_EOK);
        assert(DestroyHandle(fixture->Bindings[i].Handle) == OS_EOK);
    }
    __Drain();
    g_current = fixture->Service;
    __AckDomainEvents(fixture->Manager);
    __RemoveAllAttachments(fixture->Manager);
    assert(DMATranslatedDomainDestroy(&fixture->Domain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&fixture->Sim) == OS_EOK);
    __FinishDomainManager(&fixture->Manager, fixture->ManagerHandle, fixture->Service);
}

static struct OSDmaMappingResult
__MapTranslated(struct __TranslatedFixture* fixture, uint32_t index, uuid_t source,
                size_t offset, size_t length, enum DMADirection direction, unsigned int flags)
{
    struct OSDmaMapParameters parameters = { .Offset = offset, .Length = length,
        .Direction = direction, .Flags = flags };
    struct OSDmaMappingResult result;

    assert(ScDmaMapShm(fixture->ContextHandles[index], source, &parameters, &result) == OS_EOK);
    assert(result.SegmentCount == 1 && result.InlineSegment.Length == length);
    return result;
}

static void
__CheckHardwareID(struct __TranslatedFixture* fixture, uint32_t hardwareID, uint64_t address,
                 uint64_t length, enum DMADirection direction, paddr_t physical)
{
    SHMSG_t  span;
    uint32_t count;

    assert(DMAIOMMUSimAccessHardwareID(fixture->Sim, hardwareID,
        address, length, direction, &span, 1, &count) == OS_EOK);
    assert(count == 1 && span.Address == physical && span.Length == length);
}

static void
__TestSharedDomainAddresses(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture  fixture = __PrepareTranslatedFixture(service, driver, 4096, 2);
    paddr_t                     pages[] = { 0x100000000ULL, 0x100001000ULL };
    struct OSDmaMappingResult   first;
    struct OSDmaMappingResult   second;
    struct OSDmaMappingResult   independent;
    uint32_t                    hardwareID = 33;
    uuid_t                      device = 1402;
    DMATranslatedDomainConfig_t config = __DomainConfig(101, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t*      domain = NULL;
    uint64_t                    attachment;
    OSDmaBindingResult_t        binding;
    uuid_t                      contextHandle;
    SHMDeviceContext_t*         context;
    struct OSDmaMapParameters parameters = { .Offset = 4096, .Length = 4096,
        .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT };
    SHMSG_t  span = { 0 };
    uint32_t count = 99;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    __RegisterTestBuffer();
    first = __MapTranslated(&fixture, 0, g_buffer->ID, 0, 4096, DMAToDevice, 0);
    second = __MapTranslated(&fixture, 1, g_buffer->ID, 4096, 4096, DMAFromDevice, DMA_MAP_REQUIRE_DIRECT);
    assert(first.InlineSegment.Address != second.InlineSegment.Address);
    assert(first.InlineSegment.Address <= __IOVA_LAST && pages[0] > __IOVA_LAST);
    __CheckHardwareID(&fixture, 0, first.InlineSegment.Address, 4096, DMAToDevice, pages[0]);
    __CheckHardwareID(&fixture, 17, first.InlineSegment.Address, 4096, DMAToDevice, pages[0]);
    __CheckHardwareID(&fixture, 0, second.InlineSegment.Address, 4096, DMAFromDevice, pages[1]);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 17, first.InlineSegment.Address,
        1, DMAFromDevice, &span, 1, &count) == OS_EPERMISSIONS && count == 99);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 999, first.InlineSegment.Address,
        1, DMAToDevice, &span, 1, &count) == OS_ENOENT);

    // A different domain on the same unit starts its own allocator at the same
    // IOVA, but the hardware ID selects different backing and permissions.
    g_current = service;
    assert(DMATranslatedDomainCreate(fixture.Manager, DMAIOMMUSimUnit(fixture.Sim), &config, &domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(domain) == OS_EOK);
    attachment = __RegisterDomainMember(fixture.Manager, domain, device, 201);
    binding = __BindAttachment(fixture.Manager, &(OSDmaBindParameters_t) {
        .Attachment = attachment, .Principal = driver->GlobalHandleID, .RequestId = 1 });
    g_current = driver;
    context = __OpenTranslatedContext(binding, &contextHandle);
    assert(context->AddressSpace != fixture.Contexts[0]->AddressSpace);
    assert(ScDmaMapShm(contextHandle, g_buffer->ID, &parameters, &independent) == OS_EOK);
    assert(independent.InlineSegment.Address == first.InlineSegment.Address);
    __CheckHardwareID(&fixture, hardwareID, independent.InlineSegment.Address, 4096, DMAFromDevice, pages[1]);
    assert(ScResourceRelease(independent.Handle) == OS_EOK);
    assert(ScResourceRelease(contextHandle) == OS_EOK);
    assert(DestroyHandle(binding.Handle) == OS_EOK);
    __Drain();
    g_current = service;
    __AckDomainEvents(fixture.Manager);
    assert(DMAManagerRemoveAttachment(fixture.Manager, attachment) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
    g_current = driver;
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(g_buffer == NULL);
    __FinishTranslatedFixture(&fixture);
    puts("Translated SHM: shared IOVA space, separate domains, high physical RAM and hardware ID permissions passed");
}

static void
__TestTranslatedGrantBounds(MSContextSecurity_t* service, MSContextSecurity_t* driver, uint64_t granule)
{
    struct __TranslatedFixture fixture = __PrepareTranslatedFixture(service, driver, granule, 1);
    paddr_t                    pages[32];
    size_t                     pageCount = granule / 4096 + 1;
    SHMReference_t             source;
    SHMHandle_t                view;
    struct OSDmaMappingResult  mapping;
    struct OSDmaMappingResult  unchanged;
    struct OSDmaMapParameters  parameters = { .Length = granule, .Direction = DMAToDevice };
    DMAIOMMUSimStats_t         stats;
    SHMSG_t                    span;
    uint32_t                   count;
    uuid_t                     original;

    // The source buffer itself starts 128 bytes into its page list. The view
    // offset compensates for that, authorizing exactly one aligned granule.
    // Bounce storage for the partial ranges below comes from the RAM pool.
    __RamStart();
    for (size_t i = 0; i < pageCount; i++) {
        pages[i] = 0x10000000 + i * 4096;
    }
    __CreateBuffer(pages, (int)pageCount, 128, granule + 4096 - 128, SHM_COMMIT);
    __RegisterTestBuffer();
    original = g_buffer->ID;
    assert(SHMResolve(original, &source) == OS_EOK);
    // Use the next granule boundary within a two-granule backing when needed.
    // A 64 KiB case starts at physical 0x10010000 instead of rounding backward.
    for (size_t i = 0; i < pageCount; i++) {
        g_buffer->Pages[i] = 0x10010000 - 4096 + i * 4096;
    }
    assert(SHMViewCreate(&source, 4096 - 128, granule, GrantTokenScopeRead, driver, &view) == OS_EOK);
    assert(ReleaseGlobalHandle(source.GlobalHandleID) == OS_EINCOMPLETE);
    mapping = __MapTranslated(&fixture, 0, view.ID, 0, granule, DMAToDevice, 0);
    __CheckHardwareID(&fixture, 0, mapping.InlineSegment.Address, granule, DMAToDevice, 0x10010000);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, mapping.InlineSegment.Address - 1,
        1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 0, mapping.InlineSegment.Address + granule,
        1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    unchanged = mapping;
    parameters.Direction = DMAFromDevice;
    assert(ScDmaMapShm(fixture.ContextHandles[0], view.ID, &parameters, &unchanged) == OS_EPERMISSIONS);
    assert(memcmp(&mapping, &unchanged, sizeof(mapping)) == 0);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    assert(DestroyHandle(view.ID) == OS_EOK);
    __Drain();

    assert(SHMResolve(original, &source) == OS_EOK);
    assert(SHMViewCreate(&source, 4096 - 128 + 1, granule - 1,
        GrantTokenScopeRead, driver, &view) == OS_EOK);
    assert(ReleaseGlobalHandle(source.GlobalHandleID) == OS_EINCOMPLETE);
    // A partial range is not whole granules. Direct-only and persistent
    // mappings refuse it and change nothing. A bounce mapping copies it
    // through private storage and publishes exactly the requested length.
    parameters = (struct OSDmaMapParameters) { .Length = granule - 1, .Direction = DMAToDevice,
        .Flags = DMA_MAP_REQUIRE_DIRECT };
    memset(&unchanged, 0xa5, sizeof(unchanged));
    mapping = unchanged;
    assert(ScDmaMapShm(fixture.ContextHandles[0], view.ID, &parameters, &unchanged) == OS_ENOTSUPPORTED);
    assert(memcmp(&mapping, &unchanged, sizeof(mapping)) == 0);
    parameters.Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;
    assert(ScDmaMapShm(fixture.ContextHandles[0], view.ID, &parameters, &unchanged) == OS_ENOTSUPPORTED);
    assert(memcmp(&mapping, &unchanged, sizeof(mapping)) == 0);
    parameters.Flags = 0;
    assert(ScDmaMapShm(fixture.ContextHandles[0], view.ID, &parameters, &unchanged) == OS_EOK);
    assert(unchanged.SegmentCount == 1 && unchanged.InlineSegment.Length == granule - 1);
    assert(ScResourceRelease(unchanged.Handle) == OS_EOK);
    assert(DestroyHandle(view.ID) == OS_EOK);
    __Drain();
    // An aligned start with a partial final granule is split the same way.
    parameters = (struct OSDmaMapParameters) { .Offset = 4096 - 128,
        .Length = granule - 1, .Direction = DMAToDevice, .Flags = DMA_MAP_REQUIRE_DIRECT };
    unchanged = mapping;
    assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_ENOTSUPPORTED);
    parameters.Flags = 0;
    assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_EOK);
    assert(unchanged.SegmentCount == 1 && unchanged.InlineSegment.Length == granule - 1);
    assert(ScResourceRelease(unchanged.Handle) == OS_EOK);
    unchanged = mapping;
    parameters.Length = granule;
    g_buffer->Pages[1] = 0;
    assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_EINCOMPLETE);
    g_buffer->Pages[1] = 0x10010000;
    if (granule > 4096) {
        // CPU pages can be individually committed without forming the whole
        // contiguous physical granule required by the translation table.
        // Only a bounce mapping can copy them into one private granule.
        g_buffer->Pages[2] += 4096;
        parameters.Flags = DMA_MAP_REQUIRE_DIRECT;
        assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_ENOTSUPPORTED);
        parameters.Flags = DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT;
        assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_ENOTSUPPORTED);
        parameters.Flags = 0;
        assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_EOK);
        assert(unchanged.SegmentCount == 1 && unchanged.InlineSegment.Length == granule);
        assert(ScResourceRelease(unchanged.Handle) == OS_EOK);
        unchanged = mapping;
        g_buffer->Pages[2] -= 4096;
    }
    g_buffer->Flags |= SHM_REGIONS;
    assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_ENOTSUPPORTED);
    g_buffer->Flags &= ~SHM_REGIONS;
    // Exact physical layout is not enough when no IOVA satisfies the device's
    // placement limit. Bounce permission must not route this into direct DMA.
    parameters.Requirements.AddressAlignment = 1ULL << 32;
    assert(ScDmaMapShm(fixture.ContextHandles[0], original, &parameters, &unchanged) == OS_ENOENT);
    assert(memcmp(&mapping, &unchanged, sizeof(mapping)) == 0);
    DMAIOMMUSimSnapshot(fixture.Sim, &stats);
    assert(stats.Mappings == 0 && stats.PendingCommands == 0);
    assert(g_bounceMemory == NULL && g_liveViews == 0);
    assert(DestroyHandle(original) == OS_EOK);
    __Drain();
    __FinishTranslatedFixture(&fixture);
    __RamStop();
}

static void
__TestTranslatedSharedAllocation(MSContextSecurity_t* service, MSContextSecurity_t* driver, uint64_t granule)
{
    struct __TranslatedFixture fixture = __PrepareTranslatedFixture(service, driver, granule, 1);
    struct DMARequirements requirements = { .Length = granule + 1, .Alignment = granule,
        .Boundary = granule * 4, .Contiguous = true };
    struct OSDmaAllocationResult allocation;
    struct OSDmaAllocationResult unchanged;
    size_t                       length = (size_t)(granule * 2);
    size_t                       poolBytes = (size_t)(granule * 4);
    paddr_t                      physical;
    uint8_t*                     bytes;

    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, 4096, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, 0x900000 + granule * 3,
        poolBytes, false, g_poolStorage, sizeof(g_poolStorage)) == OS_EOK);
    assert(ScDmaAllocate(fixture.ContextHandles[0], &requirements, &allocation) == OS_EOK);
    assert(allocation.Buffer.Length == length && allocation.Buffer.Capacity == length);
    assert(allocation.Mapping.SegmentCount == 1 && allocation.Mapping.InlineSegment.Length == length);
    assert(allocation.Mapping.InlineSegment.Address % requirements.Boundary == 0);
    physical = g_buffer->Allocation.PhysicalBase;
    assert(physical % requirements.Boundary != allocation.Mapping.InlineSegment.Address % requirements.Boundary);
    assert(physical % granule == 0);
    bytes = allocation.Buffer.Buffer;
    for (size_t i = 0; i < length; i++) {
        assert(bytes[i] == 0);
    }
    __CheckHardwareID(&fixture, 0, allocation.Mapping.InlineSegment.Address, length, DMABidirectional, physical);
    assert(ScResourceRelease(allocation.Mapping.Handle) == OS_EOK);
    assert(SHMDetach(&allocation.Buffer) == OS_EOK);
    __Drain();
    assert(g_buffer == NULL && g_sharedView == NULL && __PoolAllFree());
    memset(&unchanged, 0xa5, sizeof(unchanged));
    allocation = unchanged;
    requirements.Boundary = granule;
    assert(ScDmaAllocate(fixture.ContextHandles[0], &requirements, &unchanged) == OS_EINVALPARAMS);
    assert(memcmp(&allocation, &unchanged, sizeof(allocation)) == 0);
    // Requested bytes fit, but rounding to the translation granule does not.
    requirements.Length = granule / 2;
    requirements.Boundary = granule / 2;
    assert(ScDmaAllocate(fixture.ContextHandles[0], &requirements, &unchanged) == OS_ENOTSUPPORTED);
    assert(memcmp(&allocation, &unchanged, sizeof(allocation)) == 0);
    requirements.Length = SIZE_MAX;
    requirements.Boundary = 0;
    assert(ScDmaAllocate(fixture.ContextHandles[0], &requirements, &unchanged) == OS_EOVERFLOW);
    requirements = (struct DMARequirements) { .Length = poolBytes + granule, .Contiguous = true };
    assert(ScDmaAllocate(fixture.ContextHandles[0], &requirements, &unchanged) == OS_EOOM);
    assert(memcmp(&allocation, &unchanged, sizeof(allocation)) == 0);
    __FinishTranslatedFixture(&fixture);
}

// Removal timeout leaves the mapping, source reference and IOVA reserved. A
// context handle can disappear in that interval. The mapping still protects it.
static void
__TestDelayedStopAndContextLifetime(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture = __PrepareTranslatedFixture(service, driver, 4096, 2);
    paddr_t                    pages[] = { 0x10000000, 0x10001000 };
    struct OSDmaMappingResult  first;
    struct OSDmaMappingResult  second;
    SHMDeviceMapping_t*        mapping;
    DMAIOMMUSimControl_t       control = { .HoldCompletions = true };
    DMAIOMMUSimStats_t         stats;
    DMAIOVAAllocation_t*       interval = NULL;
    DMAIOVARequest_t           request = { .Length = 4096, .Limit = __IOVA_LAST };
    OSDmaStopTicket_t          ticket;
    DMAAttachmentEventBatch_t  events;
    SHMSG_t                    span;
    uint32_t                   count;
    uuid_t                     source;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    __RegisterTestBuffer();
    source = g_buffer->ID;
    first = __MapTranslated(&fixture, 0, source, 0, 4096, DMAToDevice, 0);
    second = __MapTranslated(&fixture, 1, source, 4096, 4096, DMAFromDevice, 0);
    mapping = ((SHMRecord_t*)LookupHandle(first.Handle, HandleTypeSHMAccess))->Object;
    __CheckHardwareID(&fixture, 17, first.InlineSegment.Address, 4096, DMAToDevice, pages[0]);
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(ScResourceRelease(first.Handle) == OS_ETIMEOUT);
    DMAIOMMUSimSnapshot(fixture.Sim, &stats);
    assert(stats.Mappings == 2 && stats.PendingCommands == 1);
    assert(mapping->AddressMapping != NULL && mapping->Access.Buffer == g_buffer);
    assert(DMAIOVAAllocate(fixture.Domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMAIOVAAllocationRange(interval)->First != first.InlineSegment.Address);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    __CheckHardwareID(&fixture, 0, first.InlineSegment.Address, 4096, DMAToDevice, pages[0]);
    assert(DestroyHandle(source) == OS_EOK);
    assert(DestroyHandle(fixture.ContextHandles[0]) == OS_EOK);
    __Drain();
    assert(g_buffer != NULL && fixture.Contexts[0]->Mappings == mapping);
    assert(fixture.Contexts[0]->Binding->Group->Access.State == DMARecoveryBlocked);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EBUSY);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[1], &ticket) == OS_EOK);
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &events) == OS_EOK && events.Count == 2);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ETIMEOUT);
    assert(mapping->AddressMapping != NULL && g_buffer != NULL);
    control.HoldCompletions = false;
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(DMAIOMMUSimAdvance(fixture.Sim, 100) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    assert(mapping->State == SHMDeviceMappingStopped && mapping->AddressMapping == NULL);
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 17,
        first.InlineSegment.Address, 1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    assert(DMAIOVAAllocate(fixture.Domain->AddressSpace->Translated.Allocator, &request, &interval) == OS_EOK);
    assert(DMAIOVAAllocationRange(interval)->First == first.InlineSegment.Address);
    assert(DMAIOVAAllocationRelease(&interval) == OS_EOK);
    g_current = driver;
    assert(ScResourceRelease(first.Handle) == OS_EOK);
    assert(ScResourceRelease(second.Handle) == OS_EOK);
    assert(ScResourceRelease(fixture.ContextHandles[1]) == OS_EOK);
    for (unsigned int i = 0; i < 2; i++) {
        assert(DestroyHandle(fixture.Bindings[i].Handle) == OS_EOK);
    }
    __Drain();
    assert(g_buffer == NULL && atomic_load(&fixture.Domain->AddressSpace->References) == 2);
    g_current = service;
    __AckDomainEvents(fixture.Manager);
    __RemoveAllAttachments(fixture.Manager);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&fixture.Sim) == OS_EOK);
    __FinishDomainManager(&fixture.Manager, fixture.ManagerHandle, service);
}

// Four translated domains on two units, with six devices in three recovery
// groups. Devices 0/1 share a domain, 2 shares their unit, 3 is a direct reset
// peer of 2, 4 is an other-unit reset peer of 0/1, and 5 is independent.
struct __FailureFixture {
    DMAManager_t*          Manager;
    uuid_t                 ManagerHandle;
    DMAIOMMUSim_t*         Sims[2];
    DMATranslatedDomain_t* Domains[4];
    uint64_t               Attachments[6];
    OSDmaBindingResult_t   Bindings[6];
    uuid_t                 ContextHandles[6];
    SHMDeviceContext_t*    Contexts[6];
};

static struct __FailureFixture
__PrepareFailureFixture(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __FailureFixture     fixture = { 0 };
    uint32_t                    hardwareIDs[] = { 0, 17, 33, 44, 55 };
    uuid_t                      devices[] = { 1600, 1601, 1602, 1604, 1605 };
    const uint32_t              first[] = { 0, 2, 3, 4 };
    const uint32_t              counts[] = { 2, 1, 1, 1 };
    const uint32_t              slots[] = { 0, 2, 4, 5 };
    const uint64_t              resets[] = { 200, 300, 200, 500 };
    OSDmaAttachmentParameters_t direct = __Parameters(1603, 104, 300);

    g_current = service;
    fixture.Manager = __CreateManager(&fixture.ManagerHandle);
    fixture.Sims[0] = __CreateIOMMUSim(4096);
    fixture.Sims[1] = __CreateIOMMUSim(4096);
    for (unsigned int i = 0; i < 4; i++) {
        DMATranslatedDomainConfig_t config = __DomainConfig(100 + i, 4096,
            hardwareIDs + first[i], counts[i], devices + first[i], counts[i]);

        assert(DMATranslatedDomainCreate(fixture.Manager, DMAIOMMUSimUnit(fixture.Sims[i / 2]),
            &config, &fixture.Domains[i]) == OS_EOK);
        assert(DMATranslatedDomainConnect(fixture.Domains[i]) == OS_EOK);
        for (uint32_t j = 0; j < counts[i]; j++) {
            fixture.Attachments[slots[i] + j] = __RegisterDomainMember(fixture.Manager,
                fixture.Domains[i], devices[first[i] + j], resets[i]);
        }
    }
    fixture.Attachments[3] = __RegisterAttachment(fixture.Manager, &direct);
    for (unsigned int i = 0; i < 6; i++) {
        fixture.Bindings[i] = __BindAttachment(fixture.Manager, &(OSDmaBindParameters_t) {
            .Attachment = fixture.Attachments[i], .Principal = driver->GlobalHandleID, .RequestId = 1 });
    }
    g_current = driver;
    for (unsigned int i = 0; i < 6; i++) {
        fixture.Contexts[i] = __OpenTranslatedContext(fixture.Bindings[i], &fixture.ContextHandles[i]);
    }
    assert(fixture.Contexts[0]->Binding->Group == fixture.Contexts[1]->Binding->Group);
    assert(fixture.Contexts[0]->Binding->Group == fixture.Contexts[4]->Binding->Group);
    assert(fixture.Contexts[2]->Binding->Group == fixture.Contexts[3]->Binding->Group);
    assert(fixture.Contexts[5]->Binding->Group != fixture.Contexts[0]->Binding->Group);
    g_faultUnit = DMAIOMMUSimUnit(fixture.Sims[0]);
    return fixture;
}

static void
__CheckFailureGroups(struct __FailureFixture* fixture, bool confirmed)
{
    for (unsigned int i = 0; i < 5; i++) {
        DMARecoveryGroup_t* group = fixture->Contexts[i]->Binding->Group;
        DMAAttachment_t*    attachment = __FindTestAttachment(fixture->Manager, fixture->Attachments[i]);

        assert(group->Access.State == DMARecoveryBlocked || (confirmed && group->Access.State == DMARecoveryStopConfirmed));
        assert(attachment->Event.Queued);
        assert(fixture->Contexts[i]->Binding->State == DMABindingClosed);
    }
    assert(fixture->Contexts[5]->Binding->Group->Access.State == DMARecoveryLive);
    assert(!__FindTestAttachment(fixture->Manager, fixture->Attachments[5])->Event.Queued);
}

static void
__TestFailurePropagation(MSContextSecurity_t* service, MSContextSecurity_t* driver, unsigned int trigger)
{
    struct __FailureFixture   fixture = __PrepareFailureFixture(service, driver);
    paddr_t                   pages[] = { 0x10000000, 0x10001000 };
    struct OSDmaMapParameters parameters = { .Length = 4096, .Direction = DMAFromDevice };
    struct OSDmaMappingResult mapping;
    struct OSDmaMappingResult peer;
    SHMRecord_t*              record;
    SHMDeviceOperation_t*     operation = NULL;
    struct SHMAccess          cpu;
    struct SHMAccessBeginParameters begin = { .Kind = DMAAccessTransfer,
        .Mode = SHMWaitUntilAvailable, .Length = 4096 };
    DMAAttachmentEventBatch_t events;
    OSDmaStopTicket_t         ticket;
    SHMSG_t                   span;
    uint32_t                  count = 99;
    DMAManager_t*             replacement = NULL;
    uuid_t                    unused;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    __RegisterTestBuffer();
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &mapping) == OS_EOK);
    assert(ScDmaMapShm(fixture.ContextHandles[2], g_buffer->ID, &parameters, &peer) == OS_EOK);
    record = LookupHandle(mapping.Handle, HandleTypeSHMAccess);
    SHMAccessInitialize(&cpu, g_buffer, 0, 4096);
    assert(SHMAccessBegin(&cpu, 0, 4096, SHMAccessHolderCpu, true, NULL) == OS_EOK);
    begin.Deadline = *OS_DEADLINE_INFINITE;
    atomic_store(&g_rangeWaitError, OS_EFORKED);
    assert(ScShmAccessBegin(NULL, peer.Handle, &begin) == OS_EFORKED);
    assert(g_buffer->Access.Waiting == 1);
    if (trigger == 0) {
        // No cancellation references can be taken. All gates and events must
        // still be set. A later event read retries the skipped cancellation.
        g_snapshotFailureManager = fixture.Manager;
        assert(DMATranslatedDomainFail(fixture.Domains[0], OS_EDEVFAULT) == OS_EOVERFLOW);
        __CheckFailureGroups(&fixture, false);
        assert(g_buffer->Access.Waiting == 1);
        g_snapshotFailureManager = NULL;
    } else {
        bool peerStopped = false;

        assert(SHMRecordBegin(mapping.Handle, &g_mappingOps, &record) == OS_EOK);
        assert(SHMDeviceMappingBeginOperation(record, record->Object, SHMDeviceOperationSubmit, &operation) == OS_EOK);
        if (trigger == 1) {
            assert(DMABindingBeginOperation(fixture.Contexts[3]->Binding, false, &peerStopped) == OS_EOK);
        }
        __LatchFailure();
        // IRQ latches must be discovered even when the next operation belongs
        // to a direct reset peer. No operation on the failed unit is needed.
        assert(g_buffer->Access.Waiting == 1);
        if (trigger == 1) {
            DMABindingEndOperation(fixture.Contexts[3]->Binding);
            __CheckFailureGroups(&fixture, false);
            assert(g_buffer->Access.Waiting == 0);
            SHMDeviceOperationEnd(&operation);
            SHMRecordEnd(record);
        } else {
            assert(SHMDeviceOperationCancel(record) == OS_EBUSY);
            assert(record->PendingOperation == operation && operation->Phase == SHMDeviceOperationRunning);
            assert(fixture.Contexts[0]->Binding->Group->Access.Operations == 1);
        }
        __CheckFailureGroups(&fixture, false);
        assert(g_buffer->Access.Waiting == 0);
    }
    g_current = service;
    assert(DMAManagerSnapshotEvents(fixture.Manager, 32, &events) == OS_EOK && events.Count == 5);
    assert(g_buffer->Access.Waiting == 0 && g_buffer->Access.Faults == 2);
    __CheckFailureGroups(&fixture, false);
    // The failed unit's old translations remain observable by hardware IDs.
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sims[0], 17,
        mapping.InlineSegment.Address, 1, DMAFromDevice, &span, 1, &count) == OS_EOK);
    assert(span.Address == pages[0] && count == 1);
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == (trigger == 2 ? OS_EBUSY : OS_ECANCELLED));
    assert(((SHMDeviceMapping_t*)record->Object)->AddressMapping != NULL);
    // The independent domain can still allocate and release on the other unit.
    g_current = driver;
    parameters.Offset = 4096;
    assert(ScDmaMapShm(fixture.ContextHandles[5], g_buffer->ID, &parameters, &peer) == OS_EOK);
    assert(ScResourceRelease(peer.Handle) == OS_EOK);
    g_current = service;
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    assert(fixture.Manager->State == DMAManagerClosed && fixture.Manager->DomainCount == 4);
    assert(DMAManagerPrepareCreate(&replacement, &unused) == OS_EBUSY && replacement == NULL);
    assert(DMATranslatedDomainDestroy(&fixture.Domains[0]) == OS_EBUSY);
    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_ECANCELLED);
    // This child models terminal retention. It must not reset unit health or
    // manufacture successful invalidation just to satisfy leak cleanup.
}

// Order the setup cases before mapping cases so the two fixture paths stay explicit.
enum __PublicationPoint {
    __PublishCreateCallback,
    __PublishCreateError,
    __PublishDestroyCallback,
    __PublishAttachment,
    __PublishBinding,
    __PublishContextReference,
    __PublishContextHandle,
    __PublishPrepareCallback,
    __PublishInstallCallback,
    __PublishReleaseRemove,
    __PublishReleaseDestroy,
    __PublishStopRemove,
    __PublishStopDestroy,
    __PublishSegments,
    __PublishMappingHandle,
    __PublishMappingLink,
    __PublishDomain,
    __PublicationPointCount
};

// Each case starts with other live domains already registered on the unit.
// This catches propagation paths that only revoke the triggering context.
static void
__TestPublicationFault(MSContextSecurity_t* service, MSContextSecurity_t* driver, enum __PublicationPoint point)
{
    struct __FailureFixture             fixture = __PrepareFailureFixture(service, driver);
    uint32_t                            hardwareID = 77;
    uuid_t                              device = 1700;
    DMATranslatedDomainConfig_t         config = __DomainConfig(110, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t*              domain = NULL;
    DMAAttachmentRegistration_t*        attachmentRegistration = NULL;
    DMABindingRegistration_t*           bindingRegistration = NULL;
    OSDmaAttachmentId_t                 attachment;
    DMATranslatedAttachmentParameters_t member = { device, 700, __IOVA_LAST };
    OSDmaBindingResult_t                binding;
    OSDmaBindParameters_t               bind = { .Principal = driver->GlobalHandleID, .RequestId = 1 };
    OSDmaBoundOpenParameters_t          open;
    SHMDeviceContext_t*                 context = NULL;
    uuid_t                              contextHandle = UUID_INVALID;
    paddr_t                             page = 0x10000000;
    struct OSDmaMapParameters           parameters = { .Length = 4096, .Direction = DMAToDevice };
    struct OSDmaMappingResult           mapping;
    OSDmaStopTicket_t                   ticket;
    uint32_t                            count = 99;
    oserr_t                             status;

    if (point <= __PublishContextHandle || point == __PublishDomain) {
        g_current = service;
        if (point <= __PublishCreateError || point == __PublishDomain) {
            size_t             allocations = g_allocations;
            unsigned int       managerReferences = __LookupGlobalSafe(fixture.Manager->ID)->References;
            DMAIOMMUSimStats_t before;
            DMAIOMMUSimStats_t after;

            DMAIOMMUSimSnapshot(fixture.Sims[0], &before);
            if (point == __PublishDomain) {
                // Native creation succeeded. Fail before the registry's final
                // health read, which runs under the publication lock.
                g_connectionPreparedHook = __LatchFailure;
            } else {
                g_callbackFault = point == __PublishCreateCallback ? __FaultCreate : __FaultCreateError;
            }
            assert(DMATranslatedDomainCreate(fixture.Manager, g_faultUnit, &config, &domain) ==
                (point == __PublishCreateError ? OS_EOOM : OS_ECANCELLED));
            assert(domain == NULL && fixture.Manager->DomainCount == 4);
            // Failed creation had no published hardware ID or mapping. Its new
            // space/domain and manager reference must unwind even though the
            // unit's pre-existing domains remain registered after failure.
            DMAIOMMUSimSnapshot(fixture.Sims[0], &after);
            assert(after.Spaces == before.Spaces && after.TablePages == before.TablePages);
            assert(g_faultUnit->Spaces == before.Spaces && g_allocations == allocations);
            assert(__LookupGlobalSafe(fixture.Manager->ID)->References == managerReferences);
        } else {
            assert(DMATranslatedDomainCreate(fixture.Manager, g_faultUnit, &config, &domain) == OS_EOK);
            assert(DMATranslatedDomainConnect(domain) == OS_EOK);
            if (point == __PublishDestroyCallback) {
                g_callbackFault = __FaultDestroy;
                assert(DMATranslatedDomainDestroy(&domain) == OS_EOK && domain == NULL);
            } else {
                assert(DMAManagerPrepareTranslatedAttachment(fixture.Manager, domain, &member,
                    &attachmentRegistration, &attachment) == OS_EOK);
                if (point == __PublishAttachment) {
                    __LatchFailure();
                    assert(DMAManagerPublishAttachment(&attachmentRegistration) == OS_ECANCELLED);
                    assert(domain->Members == 0);
                    __CheckFailureGroups(&fixture, false);
                    DMAManagerCancelAttachment(&attachmentRegistration);
                } else {
                    assert(DMAManagerPublishAttachment(&attachmentRegistration) == OS_EOK);
                    bind.Attachment = attachment.Value;
                    assert(DMAManagerPrepareBinding(fixture.Manager, &bind, &bindingRegistration, &binding) == OS_EOK);
                    if (point == __PublishBinding) {
                        __LatchFailure();
                        assert(DMAManagerPublishBinding(&bindingRegistration) == OS_ECANCELLED);
                        assert(domain->OwnerSecurityID == 0);
                        __CheckFailureGroups(&fixture, false);
                        DMAManagerCancelBinding(&bindingRegistration);
                    } else {
                        assert(DMAManagerPublishBinding(&bindingRegistration) == OS_EOK);
                        open = (OSDmaBoundOpenParameters_t) { binding.Generation, __IOVA_LAST };
                        g_current = driver;
                        if (point == __PublishContextReference) {
                            g_spaceReferenceHook = __LatchFailure;
                            assert(SHMDeviceContextPrepareBound(binding.Handle, &open, &context) == OS_ECANCELLED);
                            assert(context == NULL);
                        } else {
                            g_boundCopyHook = __LatchFailure;
                            assert(ScDmaOpenBound(binding.Handle, &open, &contextHandle) == OS_ECANCELLED);
                            assert(LookupHandle(contextHandle, HandleTypeSHMAccess) == NULL);
                        }
                        __Drain();
                        assert(atomic_load(&domain->AddressSpace->References) == 2);
                        assert(__FindTestAttachment(fixture.Manager, attachment.Value)->Binding.Current->Context.Pointer == NULL);
                        assert(__FindTestAttachment(fixture.Manager, attachment.Value)->RecoveryGroup->Access.Operations == 0);
                    }
                }
            }
        }
    } else {
        __CreateBuffer(&page, 1, 0, 4096, SHM_COMMIT);
        __RegisterTestBuffer();
        if (point == __PublishPrepareCallback || point == __PublishInstallCallback || point == __PublishMappingHandle || point == __PublishMappingLink) {
            if (point == __PublishPrepareCallback || point == __PublishInstallCallback) {
                g_callbackFault = point == __PublishPrepareCallback ? __FaultPrepare : __FaultInstall;
            } else if (point == __PublishMappingHandle) {
                g_boundCopyHook = __LatchFailure;
            } else {
                g_beforeLinkHook = __LatchFailure;
            }
            assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &mapping) == OS_ECANCELLED);
            if (point == __PublishPrepareCallback) {
                // No translation was installed. The software-only mapping can
                // unwind after failure instead of pinning unused source pages.
                assert(fixture.Contexts[0]->Mappings == NULL);
                assert(atomic_load(&fixture.Domains[0]->AddressSpace->References) == 4);
            } else {
                assert(fixture.Contexts[0]->Mappings != NULL);
                assert(fixture.Contexts[0]->Mappings->AddressMapping != NULL);
                assert(g_buffer->Access.Faults == 1);
                if (point == __PublishMappingLink) {
                    // Linking exposes the access entry to cancellation, so
                    // even Prepared software cleanup waits for confirmed stop.
                    assert(fixture.Contexts[0]->Mappings->AddressMapping->State == DMAAddressMappingPrepared);
                    g_current = service;
                    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
                    assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
                    assert(fixture.Contexts[0]->Mappings == NULL);
                }
            }
        } else {
            assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &mapping) == OS_EOK);
            if (point == __PublishSegments) {
                g_segmentsHook = __LatchFailure;
                assert(ScDmaGetSegments(mapping.Handle, NULL, 0, &count) == OS_EBUSY && count == 99);
            } else {
                g_callbackFault = point == __PublishReleaseRemove || point == __PublishStopRemove ? __FaultRemove : __FaultDestroyMapping;
                if (point == __PublishReleaseRemove || point == __PublishReleaseDestroy) {
                    status = ScResourceRelease(mapping.Handle);
                } else {
                    g_current = service;
                    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
                    status = DMAManagerCompleteStop(fixture.Manager, &ticket);
                }
                assert(status == (point == __PublishReleaseRemove || point == __PublishStopRemove ? OS_ECANCELLED : OS_EOK));
            }
        }
    }
    assert(g_callbackFault == __FaultNone);
    // A successful software destructor can complete its group despite failing
    // the unit. Every other affected live group must still be blocked.
    if (point == __PublishStopDestroy || point == __PublishMappingLink) {
        assert(fixture.Contexts[0]->Binding->Group->Access.State == DMARecoveryStopCleaned);
        assert(fixture.Contexts[2]->Binding->Group->Access.State == DMARecoveryBlocked);
        assert(__FindTestAttachment(fixture.Manager, fixture.Attachments[2])->Event.Queued);
    } else {
        __CheckFailureGroups(&fixture, point == __PublishStopRemove);
    }
}

struct __InstallRace {
    struct __TranslatedFixture* Fixture;
    oserr_t                     Status;
};

static void*
__MapDuringInstallFault(void* argument)
{
    struct __InstallRace*     race = argument;
    struct OSDmaMapParameters parameters = { .Length = 4096, .Direction = DMAToDevice };
    struct OSDmaMappingResult mapping;

    race->Status = ScDmaMapShm(race->Fixture->ContextHandles[0], g_buffer->ID, &parameters, &mapping);
    return NULL;
}

static void
__TestInstallHealthRace(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture = __PrepareTranslatedFixture(service, driver, 4096, 2);
    struct __InstallRace       race = { .Fixture = &fixture };
    paddr_t                    page = 0x10000000;
    pthread_t                  worker;
    SHMSG_t                    span;
    uint32_t                   count;

    __CreateBuffer(&page, 1, 0, 4096, SHM_COMMIT);
    __RegisterTestBuffer();
    assert(pthread_barrier_init(&g_callbackReady, NULL, 2) == 0);
    assert(pthread_barrier_init(&g_callbackContinue, NULL, 2) == 0);
    g_pauseInstall = true;
    g_faultUnit = DMAIOMMUSimUnit(fixture.Sim);
    assert(pthread_create(&worker, NULL, __MapDuringInstallFault, &race) == 0);
    __Barrier(&g_callbackReady);
    // Tables are installed, but the callback has not returned to its caller's
    // final health check. Latching failure must not pretend they disappeared.
    __LatchFailure();
    assert(DMAIOMMUSimAccessHardwareID(fixture.Sim, 17,
        __IOVA_FIRST, 1, DMAToDevice, &span, 1, &count) == OS_EOK);
    __Barrier(&g_callbackContinue);
    assert(pthread_join(worker, NULL) == 0);
    assert(race.Status == OS_ECANCELLED);
    assert(fixture.Contexts[0]->Binding->Group->Access.State == DMARecoveryBlocked);
    assert(g_buffer->Access.Faults == 1 && fixture.Contexts[0]->Mappings->AddressMapping != NULL);
    assert(pthread_barrier_destroy(&g_callbackReady) == 0);
    assert(pthread_barrier_destroy(&g_callbackContinue) == 0);
}

#include "dma_connection_test.inc"
#include "dma_region_iommu_test.inc"
#include "dma_translated_bounce_test.inc"

// Builds a fault record by hand, the way a backend Poll returns one. The code
// is the SMMUv3 F_TRANSLATION event number, which only appears in the log.
static DMAIOMMUFault_t
__DeviceFault(enum DMAIOMMUFaultKind kind, uint32_t hardwareID, uint64_t address)
{
    return (DMAIOMMUFault_t) { .Kind = kind, .Flags = DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                               .HardwareID = hardwareID, .Code = 0x10, .DeviceAddress = address };
}

// Checks that exactly the failure fixture attachments in queuedMask have an
// event waiting. Each one must carry reasons, stopRound and the generation of
// its current binding.
static void
__CheckFaultEvents(struct __FailureFixture* fixture, unsigned int queuedMask, uint32_t reasons, uint64_t stopRound)
{
    for (unsigned int i = 0; i < 6; i++) {
        DMAAttachment_t* attachment = __FindTestAttachment(fixture->Manager, fixture->Attachments[i]);

        if ((queuedMask & (1U << i)) == 0) {
            assert(!attachment->Event.Queued);
            continue;
        }
        assert(attachment->Event.Queued && attachment->Event.Value.Reasons == reasons);
        assert(attachment->Event.Value.Generation == attachment->Binding.Generation);
        assert(attachment->Event.Value.StopEpoch == stopRound);
    }
}

// A device fault is reported only. Every group stays Live, every binding stays
// open and the unit stays healthy.
static void
__CheckFaultChangedNothing(struct __FailureFixture* fixture, DMAIOMMUUnit_t* unit)
{
    assert(DMAIOMMUUnitCheck(unit) == OS_EOK);
    for (unsigned int i = 0; i < 6; i++) {
        assert(fixture->Contexts[i]->Binding->Group->Access.State == DMARecoveryLive);
        assert(fixture->Contexts[i]->Binding->State == DMABindingLive);
    }
}

// Requests that make no domain count a fault. None of them may queue an event
// or change a fault count. The fixture already has two fault events queued.
static void
__TestFaultWithoutDomain(struct __FailureFixture* fixture, DMAIOMMUUnit_t* unit)
{
    DMAIOMMUFault_t fault = __DeviceFault(DMAIOMMUFaultNoMapping, 99, 0x1f0000);

    // Hardware ID 99 is reserved nowhere. Hardware ID 44 belongs to domain 2,
    // which sits on the other unit.
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_ENOENT);
    fault.HardwareID = 44;
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_ENOENT);
    assert(fixture->Domains[2]->FaultCount == 0);

    // NoRoute names an ID that has no address space, even when a domain on
    // this unit holds the same number.
    fault = __DeviceFault(DMAIOMMUFaultNoRoute, 17, 0x1f0000);
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_ENOENT);

    g_boundErrorHistory[0] = '\0';
    fault = (DMAIOMMUFault_t) { .Kind = DMAIOMMUFaultReportsLost, .Code = 0x1 };
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(strstr(g_boundErrorHistory, "dropped an unknown number of fault reports (code 0x1)") != NULL);

    assert(DMAManagerReportFault(NULL, unit, &fault) == OS_EINVALPARAMS);
    assert(DMAManagerReportFault(fixture->Manager, NULL, &fault) == OS_EINVALPARAMS);
    assert(DMAManagerReportFault(fixture->Manager, unit, NULL) == OS_EINVALPARAMS);
    fault.Kind = (enum DMAIOMMUFaultKind)99;
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EINVALPARAMS);

    assert(fixture->Domains[0]->FaultCount == 1 && fixture->Domains[1]->FaultCount == 0);
    assert(fixture->Manager->PendingEvents.count == 2);
    __CheckFaultEvents(fixture, 0x3, DMA_ATTACHMENT_DEVICE_FAULT, 0);
}

// Domain 3 holds only device 5, which sits alone in its recovery group. Faults
// 1, 2, 4 and 8 are logged and queued. The others are only counted. While a
// fault event waits, the device cannot get a new binding.
static void
__TestFaultReportLimit(struct __FailureFixture* fixture, MSContextSecurity_t* service,
                       MSContextSecurity_t* driver)
{
    DMAIOMMUUnit_t*           unit = DMAIOMMUSimUnit(fixture->Sims[1]);
    DMAIOMMUFault_t           fault = __DeviceFault(DMAIOMMUFaultPermission, 55, 0x20000);
    DMAAttachment_t*          attachment = __FindTestAttachment(fixture->Manager, fixture->Attachments[5]);
    OSDmaBindParameters_t     request = { .Attachment = fixture->Attachments[5],
                                          .Principal = driver->GlobalHandleID, .RequestId = 2 };
    DMABindingRegistration_t* registration = NULL;
    OSDmaBindingResult_t      result;
    char                      expected[96];

    g_current = driver;
    assert(ScResourceRelease(fixture->ContextHandles[5]) == OS_EOK);
    assert(DestroyHandle(fixture->Bindings[5].Handle) == OS_EOK);
    __Drain();
    g_current = service;
    __AckDomainEvents(fixture->Manager);

    for (uint64_t faultNumber = 1; faultNumber <= 8; faultNumber++) {
        bool reported = (faultNumber & (faultNumber - 1)) == 0;

        g_boundWarningHistory[0] = '\0';
        assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
        assert(fixture->Domains[3]->FaultCount == faultNumber);
        assert(attachment->Event.Queued == reported);
        snprintf(expected, sizeof(expected), "(code 0x10, fault %llu of this domain)",
                 (unsigned long long)faultNumber);
        assert(__CountText(g_boundWarningHistory, "hardware ID 55 permission fault") == (reported ? 1 : 0));
        assert((strstr(g_boundWarningHistory, expected) != NULL) == reported);
        if (faultNumber == 1) {
            // The device has no binding. Its waiting fault event holds back a
            // new one until the device manager has read the event.
            assert(attachment->Binding.Current == NULL);
            assert(attachment->Event.Value.Generation == fixture->Bindings[5].Generation);
            assert(DMAManagerPrepareBinding(fixture->Manager, &request, &registration, &result) == OS_EBUSY);
            assert(registration == NULL);
        }
        __AckDomainEvents(fixture->Manager);
        if (faultNumber == 1) {
            fixture->Bindings[5] = __BindAttachment(fixture->Manager, &request);
        }
    }

    // The count stops at UINT64_MAX. That value is not a power of two, so
    // nothing more is reported.
    fixture->Domains[3]->FaultCount = UINT64_MAX - 1;
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(fixture->Domains[3]->FaultCount == UINT64_MAX && !attachment->Event.Queued);
    fixture->Domains[3]->FaultCount = 0;
}

// A fault on a group that a stop already Blocked joins the stop event, using
// the stop round, and changes nothing about the stop itself.
static void
__TestFaultOnBlockedGroup(struct __FailureFixture* fixture)
{
    DMAIOMMUUnit_t*           unit = DMAIOMMUSimUnit(fixture->Sims[1]);
    DMAIOMMUFault_t           fault = __DeviceFault(DMAIOMMUFaultAddressRange, 55, 0x300000);
    DMAAttachment_t*          attachment = __FindTestAttachment(fixture->Manager, fixture->Attachments[5]);
    DMARecoveryGroup_t*       group = attachment->RecoveryGroup;
    DMAAttachmentEventBatch_t events;
    OSDmaStopTicket_t         ticket;

    assert(DMAManagerPrepareStop(fixture->Manager, fixture->Attachments[5], &ticket) == OS_EOK);
    assert(group->Access.State == DMARecoveryBlocked && group->Stop.Round == ticket.Epoch);
    assert(attachment->Event.Value.Reasons == DMA_ATTACHMENT_STOP_REQUIRED);

    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(attachment->Event.Value.Reasons == (DMA_ATTACHMENT_STOP_REQUIRED | DMA_ATTACHMENT_DEVICE_FAULT));
    assert(attachment->Event.Value.StopEpoch == ticket.Epoch);
    assert(attachment->Event.Value.Generation == fixture->Bindings[5].Generation);
    assert(group->Access.State == DMARecoveryBlocked && group->Stop.Round == ticket.Epoch);
    assert(group->Stop.Phase == DMARecoveryStopIdle && group->Access.Operations == 0);
    assert(DMAManagerSnapshotEvents(fixture->Manager, 32, &events) == OS_EOK && events.Count == 1);
    assert(DMAManagerFinishEvents(fixture->Manager, &events, true) == OS_EOK);

    // With the stop event read, a second fault queues a new event of its own
    // in the same stop round.
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(attachment->Event.Queued && attachment->Event.Value.Reasons == DMA_ATTACHMENT_DEVICE_FAULT);
    assert(attachment->Event.Value.StopEpoch == ticket.Epoch);
    assert(group->Access.State == DMARecoveryBlocked && group->Stop.Round == ticket.Epoch);
    assert(DMAManagerCompleteStop(fixture->Manager, &ticket) == OS_EOK);
    assert(group->Access.State == DMARecoveryStopCleaned);
    __AckDomainEvents(fixture->Manager);
}

// The device manager may still stop a device after a fault. The unit stays
// healthy, so the stop removes the mappings, and the group goes Live again
// with a new binding that can map and reach memory.
static void
__TestStopAfterDeviceFault(struct __FailureFixture* fixture, MSContextSecurity_t* service,
                           MSContextSecurity_t* driver, struct OSDmaMappingResult* mappings)
{
    DMAIOMMUUnit_t*            unit = DMAIOMMUSimUnit(fixture->Sims[0]);
    DMAIOMMUFault_t            fault = __DeviceFault(DMAIOMMUFaultNoMapping, 0, 0x1f0000);
    DMARecoveryGroup_t*        group = fixture->Contexts[0]->Binding->Group;
    OSDmaBindParameters_t      request = { .Attachment = fixture->Attachments[0],
                                           .Principal = driver->GlobalHandleID, .RequestId = 2 };
    struct OSDmaMapParameters  parameters = { .Length = 4096, .Direction = DMAToDevice };
    struct OSDmaMappingResult  mapping;
    OSDmaStopTicket_t          ticket;
    SHMSG_t                    span;
    uint32_t                   count;
    const unsigned int         members[] = { 0, 1, 4 };

    // This is the second fault of domain 0, so it is reported. The stop then
    // moves the waiting fault events into its own round.
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(fixture->Domains[0]->FaultCount == 2);
    __CheckFaultEvents(fixture, 0x3, DMA_ATTACHMENT_DEVICE_FAULT, 0);
    assert(DMAManagerPrepareStop(fixture->Manager, fixture->Attachments[0], &ticket) == OS_EOK);
    for (unsigned int i = 0; i < 3; i++) {
        DMAAttachment_t* attachment = __FindTestAttachment(fixture->Manager, fixture->Attachments[members[i]]);
        uint32_t         reasons = DMA_ATTACHMENT_STOP_REQUIRED;

        if (members[i] != 4) {
            reasons |= DMA_ATTACHMENT_DEVICE_FAULT;
        }
        assert(attachment->Event.Queued && attachment->Event.Value.Reasons == reasons);
        assert(attachment->Event.Value.StopEpoch == ticket.Epoch);
    }
    assert(DMAManagerCompleteStop(fixture->Manager, &ticket) == OS_EOK);
    assert(group->Access.State == DMARecoveryStopCleaned);
    for (unsigned int i = 0; i < 2; i++) {
        assert(DMAIOMMUSimAccessHardwareID(fixture->Sims[0], 17, mappings[i].InlineSegment.Address,
            1, DMAToDevice, &span, 1, &count) == OS_ENOENT);
    }

    g_current = driver;
    for (unsigned int i = 0; i < 2; i++) {
        assert(ScResourceRelease(mappings[i].Handle) == OS_EOK);
    }
    for (unsigned int i = 0; i < 3; i++) {
        assert(ScResourceRelease(fixture->ContextHandles[members[i]]) == OS_EOK);
        assert(DestroyHandle(fixture->Bindings[members[i]].Handle) == OS_EOK);
    }
    __Drain();
    g_current = service;
    __AckDomainEvents(fixture->Manager);

    fixture->Bindings[0] = __BindAttachment(fixture->Manager, &request);
    assert(group->Access.State == DMARecoveryLive && group->Stop.Round == ticket.Epoch + 1);
    g_current = driver;
    fixture->Contexts[0] = __OpenTranslatedContext(fixture->Bindings[0], &fixture->ContextHandles[0]);
    assert(ScDmaMapShm(fixture->ContextHandles[0], g_buffer->ID, &parameters, &mapping) == OS_EOK);
    assert(DMAIOMMUSimAccessHardwareID(fixture->Sims[0], 17, mapping.InlineSegment.Address,
        4096, DMAToDevice, &span, 1, &count) == OS_EOK);
    assert(count == 1 && span.Address == 0x10000000);
    assert(ScResourceRelease(mapping.Handle) == OS_EOK);
    assert(ScResourceRelease(fixture->ContextHandles[0]) == OS_EOK);
    assert(DestroyHandle(fixture->Bindings[0].Handle) == OS_EOK);
    __Drain();
    g_current = service;
}

// A domain whose devices are not attached yet still counts its faults, but
// has nobody to send an event to.
static void
__TestFaultWithoutAttachments(struct __FailureFixture* fixture, DMAIOMMUUnit_t* unit)
{
    uint32_t                    hardwareID = 77;
    uuid_t                      device = 1700;
    DMATranslatedDomainConfig_t config = __DomainConfig(110, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t*      domain = NULL;
    DMAIOMMUFault_t             fault = __DeviceFault(DMAIOMMUFaultOther, 77, 0x40000);

    __AckDomainEvents(fixture->Manager);
    assert(DMATranslatedDomainCreate(fixture->Manager, unit, &config, &domain) == OS_EOK);
    assert(DMATranslatedDomainConnect(domain) == OS_EOK);
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_EOK);
    assert(domain->FaultCount == 1 && fixture->Manager->PendingEvents.count == 0);
    assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
    assert(DMAManagerReportFault(fixture->Manager, unit, &fault) == OS_ENOENT);
}

// Reports device faults through DMAManagerReportFault on the failure fixture.
// Domain 0 (devices 0 and 1) faults. Device 2 shares its unit, and device 4 is
// a reset peer in another domain. Neither may receive the event.
static void
__TestDeviceFaultReport(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __FailureFixture   fixture = __PrepareFailureFixture(service, driver);
    DMAIOMMUUnit_t*           unit = DMAIOMMUSimUnit(fixture.Sims[0]);
    DMAIOMMUFault_t           fault = __DeviceFault(DMAIOMMUFaultNoMapping, 17, 0x1f0000);
    paddr_t                   pages[] = { 0x10000000, 0x10001000 };
    struct OSDmaMapParameters parameters = { .Length = 4096, .Direction = DMAToDevice };
    struct OSDmaMappingResult mappings[2];
    struct OSDmaMappingResult peer;
    SHMSG_t                   span;
    uint32_t                  count;
    bool                      stopped = false;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT);
    __RegisterTestBuffer();
    assert(ScDmaMapShm(fixture.ContextHandles[0], g_buffer->ID, &parameters, &mappings[0]) == OS_EOK);

    g_current = service;
    g_boundWarningHistory[0] = '\0';
    assert(DMAManagerReportFault(fixture.Manager, unit, &fault) == OS_EOK);
    assert(strstr(g_boundWarningHistory, "hardware ID 17 no mapping fault at device address 0x1f0000 "
                                         "(code 0x10, fault 1 of this domain)") != NULL);
    assert(fixture.Domains[0]->FaultCount == 1 && fixture.Domains[1]->FaultCount == 0);
    __CheckFaultChangedNothing(&fixture, unit);
    __CheckFaultEvents(&fixture, 0x3, DMA_ATTACHMENT_DEVICE_FAULT, 0);
    for (unsigned int i = 0; i < 2; i++) {
        DMAAttachment_t* attachment = __FindTestAttachment(fixture.Manager, fixture.Attachments[i]);

        assert(attachment->Event.Value.Generation == fixture.Bindings[i].Generation);
    }

    // The faulting domain keeps working. Its old mapping stays installed, and
    // a new mapping and an operation through it succeed after the report.
    // Domain 1 on the same unit is unaffected.
    g_current = driver;
    parameters.Offset = 4096;
    assert(ScDmaMapShm(fixture.ContextHandles[1], g_buffer->ID, &parameters, &mappings[1]) == OS_EOK);
    for (unsigned int i = 0; i < 2; i++) {
        assert(DMAIOMMUSimAccessHardwareID(fixture.Sims[0], i == 0 ? 0 : 17, mappings[i].InlineSegment.Address,
            4096, DMAToDevice, &span, 1, &count) == OS_EOK);
        assert(count == 1 && span.Address == pages[i]);
    }
    assert(DMABindingBeginOperation(fixture.Contexts[0]->Binding, false, &stopped) == OS_EOK && !stopped);
    DMABindingEndOperation(fixture.Contexts[0]->Binding);
    assert(ScDmaMapShm(fixture.ContextHandles[2], g_buffer->ID, &parameters, &peer) == OS_EOK);
    assert(ScResourceRelease(peer.Handle) == OS_EOK);
    __CheckFaultChangedNothing(&fixture, unit);

    g_current = service;
    __TestFaultWithoutDomain(&fixture, unit);
    __AckDomainEvents(fixture.Manager);
    __TestFaultReportLimit(&fixture, service, driver);
    __TestFaultOnBlockedGroup(&fixture);
    __TestStopAfterDeviceFault(&fixture, service, driver, mappings);
    __TestFaultWithoutAttachments(&fixture, unit);

    // Devices 2, 3 and 5 still hold their bindings. Device 5 has no context
    // after the report limit case replaced its binding.
    g_current = driver;
    for (unsigned int i = 2; i < 6; i++) {
        if (i != 4) {
            if (i != 5) {
                assert(ScResourceRelease(fixture.ContextHandles[i]) == OS_EOK);
            }
            assert(DestroyHandle(fixture.Bindings[i].Handle) == OS_EOK);
        }
    }
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(g_buffer == NULL);
    g_current = service;
    __AckDomainEvents(fixture.Manager);
    __RemoveAllAttachments(fixture.Manager);
    for (unsigned int i = 0; i < 4; i++) {
        assert(DMATranslatedDomainDestroy(&fixture.Domains[i]) == OS_EOK);
    }

    // A closed manager reports nothing.
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    assert(DMAManagerReportFault(fixture.Manager, unit, &fault) == OS_ECANCELLED);
    for (unsigned int i = 0; i < 2; i++) {
        assert(DMAIOMMUSimDestroy(&fixture.Sims[i]) == OS_EOK);
    }
    DMAManagerRelease(&fixture.Manager);
    __Drain();
    g_faultUnit = NULL;
    puts("Device faults: report only, domain events, report limit, blocked groups, busy binding and stop after fault passed");
}

int
main(void)
{
    MSContextSecurity_t service;
    MSContextSecurity_t driver;
    MSContextSecurity_t otherDriver;

    assert(InitializeHandles() == OS_EOK);
    assert(HandleSetsInitialize() == OS_EOK);
    __NewGroup(&service);
    __NewGroup(&driver);
    __NewGroup(&otherDriver);
    __TestTranslatedRegions(&service, &driver);
    __TestConnections(&service, &driver);
    __TestCheckedSpaceLifetime();
    __TestDomainRegistration(&service);
    __TestClosedManagerDomainLifetime(&service);
    __TestDomainMembership(&service, &driver, &otherDriver);
    __TestSharedDomainAddresses(&service, &driver);
    __TestTranslatedGrantBounds(&service, &driver, 4096);
    __TestTranslatedGrantBounds(&service, &driver, 65536);
    __TestTranslatedSharedAllocation(&service, &driver, 4096);
    __TestTranslatedSharedAllocation(&service, &driver, 65536);
    __TestTranslatedBounce(&service, &driver);
    __TestDelayedStopAndContextLifetime(&service, &driver);
    __TestDeviceFaultReport(&service, &driver);
    for (unsigned int trigger = 0; trigger < 3 + __PublicationPointCount + 8; trigger++) {
        int   status;
        pid_t child = fork();

        assert(child >= 0);
        if (child == 0) {
            if (trigger < 3) {
                __TestFailurePropagation(&service, &driver, trigger);
            } else if (trigger < 3 + __PublicationPointCount) {
                __TestPublicationFault(&service, &driver, (enum __PublicationPoint)(trigger - 3));
            } else if (trigger == 3 + __PublicationPointCount) {
                __TestInstallHealthRace(&service, &driver);
            } else if (trigger < 3 + __PublicationPointCount + 3) {
                __TestFailedResetPeer(&service, &driver, trigger == 3 + __PublicationPointCount + 2);
            } else {
                __TestConnectionCallbackFailure(&service, &driver,
                    (enum __CallbackFault)(__FaultPrepareHardwareIDs + trigger - (3 + __PublicationPointCount + 3)));
            }
            _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            // Preserve the child's real diagnostic. Manufacturing a parent
            // assertion here would disguise sanitizer faults as test assertions.
            fprintf(stderr, "Translated DMA child %u failed (wait status %d)\n", trigger, status);
            return EXIT_FAILURE;
        }
    }
    g_current = NULL;
    DestroyHandleGrantsTable(service.HandleTable);
    DestroyHandleGrantsTable(driver.HandleTable);
    DestroyHandleGrantsTable(otherDriver.HandleTable);
    ReleaseGlobalHandle(service.GlobalHandleID);
    ReleaseGlobalHandle(driver.GlobalHandleID);
    ReleaseGlobalHandle(otherDriver.GlobalHandleID);
    __Drain();
    assert(g_handles.element_count == 0 && g_allocations == 0);
    puts("Translated DMA integration: grants, allocation, publication faults, stop, lifetime and failure races passed");
    return 0;
}
