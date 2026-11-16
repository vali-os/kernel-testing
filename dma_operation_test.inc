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

static void
__TestOperationSetupFailures(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver)
{
    __BoundFixture_t          fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMDeviceMapping_t*       mapping;
    SHMDeviceOperation_t*     operation = NULL;
    DMARecoveryGroup_t*       group;
    SHMRecord_t         record = { .State = SHMRecordUpdating };
    unsigned int             contextReferences;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    group = fixture.Context->Binding->Group;
    contextReferences = atomic_load(&fixture.Context->References);
    assert(atomic_load(&mapping->Lifetime.References) == 1);
    assert(group->Gate.Operations == 0);

    // Fail before either reference is added. Cleanup must leave both counts intact.
    atomic_store(&fixture.Context->References, UINT_MAX);
    assert(SHMDeviceContextBeginOperation(
        &record, fixture.Context, SHMDeviceOperationSubmit, &operation) == OS_EOVERFLOW);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == UINT_MAX);
    assert(SHMDeviceMappingBeginOperation(
        &record, mapping, SHMDeviceOperationSubmit, &operation) == OS_EOVERFLOW);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == UINT_MAX);
    assert(atomic_load(&mapping->Lifetime.References) == 1);
    assert(group->Gate.Operations == 0);
    atomic_store(&fixture.Context->References, contextReferences);

    // A failed mapping reference must drop the context reference already added.
    atomic_store(&mapping->Lifetime.References, UINT_MAX);
    assert(SHMDeviceMappingBeginOperation(
        &record, mapping, SHMDeviceOperationSubmit, &operation) == OS_EOVERFLOW);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == contextReferences);
    assert(atomic_load(&mapping->Lifetime.References) == UINT_MAX);
    assert(group->Gate.Operations == 0);
    atomic_store(&mapping->Lifetime.References, 1);

    // A full group counter rejects admission after references have been added.
    group->Gate.Operations = UINT_MAX;
    assert(SHMDeviceContextBeginOperation(
        &record, fixture.Context, SHMDeviceOperationSubmit, &operation) == OS_EOVERFLOW);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == contextReferences);
    assert(group->Gate.Operations == UINT_MAX);
    assert(SHMDeviceMappingBeginOperation(
        &record, mapping, SHMDeviceOperationSubmit, &operation) == OS_EOVERFLOW);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == contextReferences);
    assert(atomic_load(&mapping->Lifetime.References) == 1);
    assert(group->Gate.Operations == UINT_MAX);
    group->Gate.Operations = 0;

    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}

static void
__TestOperationReferences(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver)
{
    __BoundFixture_t fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;
    SHMDeviceMapping_t* mapping;
    SHMDeviceOperation_t* operation = NULL;
    SHMRecord_t record = { .State = SHMRecordUpdating };
    unsigned int references;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    references = atomic_load(&fixture.Context->References);

    // Every admitted call holds a context reference and counts against group
    // stop. Mapping calls also hold the mapping through their final cleanup.
    assert(!SHMDeviceOperationIsValid(NULL, fixture.Context, NULL, SHMDeviceOperationSubmit));
    assert(SHMDeviceContextBeginOperation(
        &record, fixture.Context, SHMDeviceOperationSubmit, &operation) == OS_EOK);
    assert(SHMDeviceOperationIsValid(operation, fixture.Context, NULL, SHMDeviceOperationSubmit));
    assert(atomic_load(&fixture.Context->References) == references + 1);
    assert(fixture.Context->Binding->Group->Gate.Operations == 1);
    SHMDeviceOperationEnd(&operation);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == references);
    assert(fixture.Context->Binding->Group->Gate.Operations == 0);

    assert(SHMDeviceMappingBeginOperation(
        &record, mapping, SHMDeviceOperationSubmit, &operation) == OS_EOK);
    assert(SHMDeviceOperationIsValid(operation, fixture.Context, mapping, SHMDeviceOperationSubmit));
    assert(atomic_load(&fixture.Context->References) == references + 1);
    assert(atomic_load(&mapping->Lifetime.References) == 2);
    assert(fixture.Context->Binding->Group->Gate.Operations == 1);
    SHMDeviceOperationEnd(&operation);
    assert(operation == NULL && record.PendingOperation == NULL);
    assert(atomic_load(&fixture.Context->References) == references);
    assert(atomic_load(&mapping->Lifetime.References) == 1);
    assert(fixture.Context->Binding->Group->Gate.Operations == 0);
    SHMDeviceOperationEnd(&operation);
    assert(atomic_load(&mapping->Lifetime.References) == 1);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}
