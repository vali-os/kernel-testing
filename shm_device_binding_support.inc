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

// Low-level memory suites control the binding boundary while running the real
// SHM context and operation code. dma_bound_test covers real grants and groups.
// Each context gets an independent group. Tests serialize preparation, revocation
// and stop. Atomic counts also support concurrent operations on separate mappings.

#include "../kernel/memory/shm/device_private.h"
#include <shm/record.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum __TestBindingState {
    __TestBindingPreparing,
    __TestBindingLive,
    __TestBindingBlocked,
    __TestBindingStopped
};

struct DMABinding {
    SHMDeviceContext_t* Context;
    atomic_uint         Operations;
    atomic_int          State;
};

static OSDmaAttachmentParameters_t g_testBindingRoute;
static atomic_uint g_testBindings;

oserr_t
DMABindingAttachContext(
    _In_    uuid_t                             handle,
    _In_    const OSDmaBoundOpenParameters_t*   parameters,
    _In_    SHMDeviceContext_t*                 context,
    _InOut_ DMABinding_t**                      bindingOut,
    _Out_   OSDmaAttachmentParameters_t*        configurationOut)
{
    DMABinding_t* binding;

    assert(handle == 1 && parameters->Generation == 1 && *bindingOut == NULL);
    binding = calloc(1, sizeof(*binding));
    assert(binding != NULL);
    binding->Context = context;
    atomic_init(&binding->Operations, 1);
    atomic_init(&binding->State, __TestBindingPreparing);
    atomic_fetch_add(&g_testBindings, 1);
    *configurationOut = g_testBindingRoute;
    if (parameters->AddressLimit < configurationOut->AddressLimit) {
        configurationOut->AddressLimit = parameters->AddressLimit;
    }
    *bindingOut = binding;
    return OS_EOK;
}

oserr_t
DMABindingPublishContext(
    _In_ DMABinding_t* binding)
{
    assert(atomic_load(&binding->State) == __TestBindingPreparing);
    assert(atomic_fetch_sub(&binding->Operations, 1) == 1);
    atomic_store(&binding->State, __TestBindingLive);
    return OS_EOK;
}

void
DMABindingDetachContext(
    _InOut_ DMABinding_t**    slot,
    _In_    SHMDeviceContext_t* context)
{
    DMABinding_t* binding = *slot;

    if (binding == NULL) {
        return;
    }
    assert(binding->Context == context && context->Mappings == NULL);
    assert(atomic_load(&binding->Operations) ==
        (atomic_load(&binding->State) == __TestBindingPreparing ? 1U : 0U));
    assert(atomic_fetch_sub(&g_testBindings, 1) != 0);
    free(binding);
    *slot = NULL;
}

oserr_t
DMABindingCheckOperation(
    _In_ DMABinding_t* binding)
{
    assert(binding != NULL);
    return atomic_load(&binding->State) == __TestBindingLive ? OS_EOK : OS_ECANCELLED;
}

oserr_t
DMABindingBeginOperation(
    _In_  DMABinding_t* binding,
    _In_  bool          release,
    _Out_ bool*         stoppedOut)
{
    assert(binding != NULL);
    if (!release && atomic_load(&binding->State) != __TestBindingLive) {
        return OS_ECANCELLED;
    }
    *stoppedOut = atomic_load(&binding->State) == __TestBindingStopped;
    assert(atomic_fetch_add(&binding->Operations, 1) != UINT_MAX);
    return OS_EOK;
}

void
DMABindingEndOperation(
    _In_ DMABinding_t* binding)
{
    assert(atomic_fetch_sub(&binding->Operations, 1) != 0);
}

void
DMABindingRevoke(
    _In_ DMABinding_t* binding)
{
    assert(binding != NULL);
    if (atomic_load(&binding->State) == __TestBindingStopped) {
        return;
    }
    atomic_store(&binding->State, __TestBindingBlocked);
    assert(SHMDeviceContextRevoke(binding->Context) == OS_EOK);
}

static inline oserr_t
__TestConfirmStop(
    _In_ SHMDeviceContext_t* context)
{
    DMABinding_t* binding = context->Binding;

    assert(atomic_load(&binding->State) == __TestBindingBlocked ||
           atomic_load(&binding->State) == __TestBindingStopped);
    assert(atomic_load(&binding->Operations) == 0);
    atomic_store(&binding->State, __TestBindingStopped);
    return SHMDeviceContextCompleteStop(context);
}

static inline oserr_t
__PrepareTestContext(
    _In_    const struct SHMDeviceRange* ranges,
    _In_    uint32_t                     count,
    _In_    enum SHMDeviceCachePolicy    policy,
    _In_    uint64_t                     limit,
    _InOut_ SHMDeviceContext_t**         contextOut)
{
    OSDmaBoundOpenParameters_t parameters = { .Generation = 1, .AddressLimit = limit };
    oserr_t              status;

    assert(count <= DMA_MAX_RANGES && contextOut != NULL && *contextOut == NULL);
    g_testBindingRoute = (OSDmaAttachmentParameters_t) {
        .Version = DMA_ATTACHMENT_VERSION, .Device = 1, .Host = 1,
        .RequesterGroup = 1, .ResetGroup = 1, .AddressLimit = UINT64_MAX,
        .CachePolicy = policy, .RangeCount = count
    };
    memcpy(g_testBindingRoute.Ranges, ranges, count * sizeof(*ranges));
    status = SHMDeviceContextPrepareBound(1, &parameters, contextOut);
    if (status != OS_EOK) {
        return status;
    }
    assert(SHMDeviceContextPublishBound(*contextOut) == OS_EOK);
    return OS_EOK;
}

static inline SHMDeviceContext_t*
__CreateContext(
    _In_ const struct SHMDeviceRange* ranges,
    _In_ uint32_t                     count,
    _In_ enum SHMDeviceCachePolicy    policy,
    _In_ uint64_t                     limit)
{
    SHMDeviceContext_t* context = NULL;

    assert(__PrepareTestContext(ranges, count, policy, limit, &context) == OS_EOK);
    return context;
}

// These helpers serialize each call through a real Updating record. They keep
// its token alive through the operation and release it even when the call fails.
// Async continuation lifetime is tested with real records in dma_bound_test.
static inline oserr_t
__TestMap(
    _In_     SHMDeviceContext_t*              context,
    _In_     uuid_t                           shm,
    _In_     size_t                           offset,
    _In_     size_t                           length,
    _In_     enum DMADirection                direction,
    _In_     enum SHMDeviceMappingKind        kind,
    _In_Opt_ const struct DMAMapRequirements* requirements,
    _InOut_  SHMDeviceMapping_t**             mappingOut)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = context };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (context == NULL) {
        return SHMDeviceMap(NULL, context, shm, offset, length, direction, kind, requirements, mappingOut);
    }
    status = SHMDeviceContextBeginOperation(&record, context, SHMDeviceOperationSubmit, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceMap(operation, context, shm, offset, length, direction, kind, requirements, mappingOut);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline oserr_t
__TestAllocate(
    _In_    SHMDeviceContext_t*        context,
    _In_    const struct DMARequirements* requirements,
    _Out_   SHMHandle_t*               bufferOut,
    _InOut_ SHMDeviceMapping_t**        mappingOut)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = context };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (context == NULL) {
        return SHMDeviceAllocate(NULL, context, requirements, bufferOut, mappingOut);
    }
    status = SHMDeviceContextBeginOperation(&record, context, SHMDeviceOperationSubmit, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceAllocate(operation, context, requirements, bufferOut, mappingOut);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline oserr_t
__TestUnmap(
    _InOut_ SHMDeviceMapping_t** mapping)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (mapping == NULL || *mapping == NULL) {
        return SHMDeviceUnmap(NULL, mapping);
    }
    record.Object = *mapping;
    status = SHMDeviceMappingBeginOperation(&record, *mapping, SHMDeviceOperationRelease, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceUnmap(operation, mapping);
    // The operation holds the mapping's final reference after a successful unmap.
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline const struct DMASegment*
__TestMappingSegments(
    _In_  SHMDeviceMapping_t* mapping,
    _Out_ uint32_t*           countOut)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = mapping };
    SHMDeviceOperation_t* operation = NULL;
    const struct DMASegment* segments;
    oserr_t              status;

    if (mapping == NULL) {
        return SHMDeviceMappingSegments(NULL, NULL, countOut);
    }
    status = SHMDeviceMappingBeginOperation(&record, mapping, SHMDeviceOperationSubmit, &operation);
    if (status != OS_EOK) {
        if (countOut != NULL) {
            *countOut = 0;
        }
        return NULL;
    }
    segments = SHMDeviceMappingSegments(operation, mapping, countOut);
    SHMDeviceOperationEnd(&operation);
    // These fixtures never run stop cleanup concurrently with segment assertions.
    return segments;
}

static inline oserr_t
__TestSyncForDevice(
    _In_     SHMDeviceMapping_t*         mapping,
    _In_     size_t                      offset,
    _In_     size_t                      length,
    _In_Opt_ const struct SHMAccessWait* wait)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = mapping };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (mapping == NULL) {
        return SHMDeviceSyncForDevice(NULL, mapping, offset, length, wait);
    }
    status = SHMDeviceMappingBeginOperation(&record, mapping, SHMDeviceOperationSubmit, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceSyncForDevice(operation, mapping, offset, length, wait);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline oserr_t
__TestSyncForCpu(
    _In_ SHMDeviceMapping_t* mapping,
    _In_ size_t              length)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = mapping };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (mapping == NULL) {
        return SHMDeviceSyncForCpu(NULL, mapping, length);
    }
    status = SHMDeviceMappingBeginOperation(&record, mapping, SHMDeviceOperationComplete, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceSyncForCpu(operation, mapping, length);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline oserr_t
__TestBeginUse(
    _In_     SHMDeviceMapping_t*         mapping,
    _In_Opt_ const struct SHMAccessWait* wait)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = mapping };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (mapping == NULL) {
        return SHMDeviceBeginUse(NULL, mapping, wait);
    }
    status = SHMDeviceMappingBeginOperation(&record, mapping, SHMDeviceOperationSubmit, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceBeginUse(operation, mapping, wait);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}

static inline oserr_t
__TestEndUse(
    _In_ SHMDeviceMapping_t* mapping)
{
    SHMRecord_t          record = { .State = SHMRecordUpdating, .Object = mapping };
    SHMDeviceOperation_t* operation = NULL;
    oserr_t              status;

    if (mapping == NULL) {
        return SHMDeviceEndUse(NULL, mapping);
    }
    status = SHMDeviceMappingBeginOperation(&record, mapping, SHMDeviceOperationComplete, &operation);
    if (status != OS_EOK) {
        return status;
    }
    status = SHMDeviceEndUse(operation, mapping);
    assert(status != OS_EFORKED);
    SHMDeviceOperationEnd(&operation);
    assert(record.PendingOperation == NULL);
    return status;
}
