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

// Included by the bound-IOMMU fixture. Native root mappings let these cases
// inspect a hardware ID even when software admission has closed the domain.
struct __ConnectionFixture {
    DMAManager_t*          Manager;
    uuid_t                 ManagerHandle;
    DMAIOMMUSim_t*         Sim;
    DMATranslatedDomain_t* Domain;
};

static struct __ConnectionFixture
__CreateConnectionFixture(MSContextSecurity_t* service)
{
    struct __ConnectionFixture fixture = {0};
    uint32_t                    hardwareIDs[] = {0, 17};
    uuid_t                      devices[] = {1800, 1801};
    DMATranslatedDomainConfig_t config = __DomainConfig(300, 4096, hardwareIDs, 2, devices, 2);

    g_current = service;
    fixture.Manager = __CreateManager(&fixture.ManagerHandle);
    fixture.Sim = __CreateIOMMUSim(4096);
    assert(DMATranslatedDomainCreate(fixture.Manager, DMAIOMMUSimUnit(fixture.Sim),
        &config, &fixture.Domain) == OS_EOK);
    assert(fixture.Domain->ConnectionState == DMAConnectionPrepared);
    assert(fixture.Domain->ConnectionSpace == fixture.Domain->AddressSpace);
    assert(atomic_load(&fixture.Domain->AddressSpace->References) == 2);
    return fixture;
}

static void
__FinishConnectionFixture(struct __ConnectionFixture* fixture, MSContextSecurity_t* service)
{
    assert(DMATranslatedDomainDestroy(&fixture->Domain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&fixture->Sim) == OS_EOK);
    __FinishDomainManager(&fixture->Manager, fixture->ManagerHandle, service);
}

static struct DMAAddressMapping*
__MapConnectionRoot(DMATranslatedDomain_t* domain)
{
    struct DMAAddressMapping* mapping = NULL;
    SHMSG_t                    extent = {0x80000000, 4096};

    assert(DMAAddressMappingPrepare(domain->AddressSpace, &extent, 1, __IOVA_LAST,
        DMAToDevice, NULL, &mapping) == OS_EOK);
    assert(DMAAddressMappingInstall(mapping) == OS_EOK);
    return mapping;
}

static void
__ProbeConnection(DMAIOMMUSim_t* sim, uint32_t hardwareID, oserr_t expected)
{
    SHMSG_t  span = {1, 2};
    uint32_t count = 99;

    assert(DMAIOMMUSimAccessHardwareID(sim, hardwareID, __IOVA_FIRST, 1,
        DMAToDevice, &span, 1, &count) == expected);
    if (expected == OS_EOK) {
        assert(span.Address == 0x80000000 && span.Length == 1 && count == 1);
    } else {
        assert(span.Address == 1 && span.Length == 2 && count == 99);
    }
}

static void
__TestConnectionAdmission(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __ConnectionFixture fixture = __CreateConnectionFixture(service);
    struct DMAAddressMapping* mapping = __MapConnectionRoot(fixture.Domain);
    DMATranslatedAttachmentParameters_t member = {1800, 400, __IOVA_LAST};
    DMAAttachmentRegistration_t* registration = NULL;
    OSDmaAttachmentId_t attachment = {99};
    DMAIOMMUSimStats_t before;
    DMAIOMMUSimStats_t after;
    OSDmaBindParameters_t request = {.Principal = driver->GlobalHandleID, .RequestId = 1};
    DMABindingRegistration_t* binding = NULL;
    OSDmaBindingResult_t result;

    __ProbeConnection(fixture.Sim, 0, OS_ENOENT);
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    assert(before.Connections == 1 && before.ReservedHardwareIDs == 2);
    assert(before.InstalledHardwareIDs == 0 && before.CachedHardwareIDs == 0);
    assert(DMAManagerPrepareTranslatedAttachment(fixture.Manager, fixture.Domain,
        &member, &registration, &attachment) == OS_EBUSY);
    assert(registration == NULL && attachment.Value == 99);
    assert(DMATranslatedDomainConnect(NULL) == OS_EINVALPARAMS);
    // Everything needed by connection and cleanup was allocated during Create.
    g_failAllocation = 0;
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EOK);
    g_failAllocation = -1;
    __ProbeConnection(fixture.Sim, 0, OS_EOK);
    __ProbeConnection(fixture.Sim, 17, OS_EOK);
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    assert(before.InstalledHardwareIDs == 2 && before.CachedHardwareIDs == 2);
    request.Attachment = __RegisterDomainMember(fixture.Manager, fixture.Domain, 1800, 400);
    __RegisterDomainMember(fixture.Manager, fixture.Domain, 1801, 400);
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EOK);
    DMAIOMMUSimSnapshot(fixture.Sim, &after);
    assert(after.SubmittedCommands == before.SubmittedCommands);
    assert(DMAManagerPrepareBinding(fixture.Manager, &request, &binding, &result) == OS_EOK);
    // Fault injection checks the final publication gate independently of the
    // preparation gate. A disconnected domain must never publish a binding.
    MutexLock(&fixture.Manager->Lock);
    fixture.Domain->ConnectionState = DMAConnectionDisconnecting;
    MutexUnlock(&fixture.Manager->Lock);
    assert(DMAManagerPublishBinding(&binding) == OS_EBUSY);
    assert(binding != NULL && fixture.Domain->OwnerSecurityID == 0);
    MutexLock(&fixture.Manager->Lock);
    fixture.Domain->ConnectionState = DMAConnectionConnected;
    MutexUnlock(&fixture.Manager->Lock);
    DMAManagerCancelBinding(&binding);
    __Drain();
    __RemoveAllAttachments(fixture.Manager);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    __FinishConnectionFixture(&fixture, service);
}

static void
__TestHardwareIDReservations(MSContextSecurity_t* service)
{
    struct __ConnectionFixture fixture = __CreateConnectionFixture(service);
    DMAIOMMUUnit_t* unit = DMAIOMMUSimUnit(fixture.Sim);
    DMAIOVAConfig_t config = {{__IOVA_FIRST, __IOVA_LAST}, 4096, NULL, 0};
    struct DMAAddressSpace* other = NULL;
    void* connection = NULL;
    uint32_t hardwareIDs[] = {77, 17};
    DMAIOMMUSimStats_t before;
    DMAIOMMUSimStats_t after;

    assert(DMAAddressSpaceCreateTranslated(unit, &config, &other) == OS_EOK);
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    // This space has no manager registry entry. The backend must enforce the
    // unit-wide conflict itself, including a conflict late in the supplied set.
    MutexLock(&unit->Lock);
    assert(unit->Ops.PrepareHardwareIDs(other->Translated.BackendSpace, hardwareIDs, 2, &connection) == OS_EBUSY);
    MutexUnlock(&unit->Lock);
    assert(connection == NULL);
    DMAIOMMUSimSnapshot(fixture.Sim, &after);
    assert(after.Connections == before.Connections && after.ReservedHardwareIDs == before.ReservedHardwareIDs);
    hardwareIDs[1] = 78;
    MutexLock(&unit->Lock);
    assert(unit->Ops.PrepareHardwareIDs(other->Translated.BackendSpace, hardwareIDs, 2, &connection) == OS_EOK);
    unit->Ops.DestroyHardwareIDs(other->Translated.BackendSpace, connection);
    MutexUnlock(&unit->Lock);
    assert(DMAAddressSpaceDestroy(&other) == OS_EOK);
    __FinishConnectionFixture(&fixture, service);
}

static void
__TestConnectionAllocationFailures(MSContextSecurity_t* service)
{
    DMAIOMMUSim_t* sim = __CreateIOMMUSim(4096);
    DMAManager_t* manager;
    uuid_t handle;
    uint32_t hardwareID = 0;
    uuid_t device = 1800;
    DMATranslatedDomainConfig_t config = __DomainConfig(300, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t* domain = NULL;
    DMAIOMMUSimStats_t stats;
    size_t allocations;
    bool completed = false;

    g_current = service;
    manager = __CreateManager(&handle);
    allocations = g_allocations;
    for (int step = 0; step < 32; step++) {
        oserr_t status;

        g_failAllocation = step;
        status = DMATranslatedDomainCreate(manager, DMAIOMMUSimUnit(sim), &config, &domain);
        g_failAllocation = -1;
        if (status == OS_EOK) {
            completed = true;
            g_failAllocation = 0;
            assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
            g_failAllocation = -1;
            break;
        }
        assert(status == OS_EOOM && domain == NULL && manager->DomainCount == 0);
        DMAIOMMUSimSnapshot(sim, &stats);
        assert(stats.Spaces == 0 && stats.Connections == 0 && stats.ReservedHardwareIDs == 0);
        assert(g_allocations == allocations);
    }
    assert(completed && g_allocations == allocations);
    assert(DMAIOMMUSimDestroy(&sim) == OS_EOK);
    __FinishDomainManager(&manager, handle, service);
}

static void
__CheckHardwareIDReuse(DMAIOMMUSim_t* sim, oserr_t expected)
{
    DMAIOMMUUnit_t* unit = DMAIOMMUSimUnit(sim);
    DMAIOVAConfig_t config = {{__IOVA_FIRST, __IOVA_LAST}, 4096, NULL, 0};
    struct DMAAddressSpace* space = NULL;
    void* connection = NULL;
    uint32_t hardwareID = 0;

    assert(DMAAddressSpaceCreateTranslated(unit, &config, &space) == OS_EOK);
    MutexLock(&unit->Lock);
    assert(unit->Ops.PrepareHardwareIDs(space->Translated.BackendSpace,
        &hardwareID, 1, &connection) == expected);
    if (expected == OS_EOK) {
        unit->Ops.DestroyHardwareIDs(space->Translated.BackendSpace, connection);
    } else {
        assert(connection == NULL);
    }
    MutexUnlock(&unit->Lock);
    assert(DMAAddressSpaceDestroy(&space) == OS_EOK);
}

static void
__TestConnectionProgress(MSContextSecurity_t* service, bool disconnect, unsigned int point)
{
    struct __ConnectionFixture fixture = __CreateConnectionFixture(service);
    struct DMAAddressMapping* mapping = __MapConnectionRoot(fixture.Domain);
    const enum DMAIOMMUSimFailurePoint points[] = {
        DMAIOMMUSimFailureConnectEntry, DMAIOMMUSimFailureSubmit, DMAIOMMUSimFailureComplete
    };
    DMAIOMMUSimControl_t control = {.FailurePoint = points[point],
        .FailureStatus = OS_EUNKNOWN, .AfterSteps = point == 0 ? 1 : 0};
    DMAIOMMUSimStats_t before;
    DMAIOMMUSimStats_t after;
    void* saved = fixture.Domain->Connection;

    if (disconnect) {
        assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EOK);
        __ProbeConnection(fixture.Sim, 0, OS_EOK);
        __ProbeConnection(fixture.Sim, 17, OS_EOK);
        if (point == 0) control.FailurePoint = DMAIOMMUSimFailureDisconnectEntry;
    }
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    if (disconnect) {
        assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EUNKNOWN);
    } else {
        assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EUNKNOWN);
        assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EBUSY);
    }
    assert(fixture.Domain->Connection == saved && fixture.Domain->ConnectionSpace != NULL);
    __ProbeConnection(fixture.Sim, 0, OS_EOK);
    __ProbeConnection(fixture.Sim, 17, !disconnect && point == 0 ? OS_ENOENT : OS_EOK);
    // A held disconnect must keep the stale cached route and its root alive.
    control = (DMAIOMMUSimControl_t){.HoldCompletions = true};
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_ETIMEOUT);
    assert(fixture.Domain->State == DMATranslatedDomainClosing);
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_EBUSY);
    __ProbeConnection(fixture.Sim, 0, OS_EOK);
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    assert(before.Connections == 1 && before.ReservedHardwareIDs == 2 && before.PendingCommands != 0);
    __CheckHardwareIDReuse(fixture.Sim, OS_EBUSY);
    control.HoldCompletions = false;
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EBUSY);
    assert(fixture.Domain->Connection == NULL && fixture.Domain->ConnectionSpace == NULL);
    assert(atomic_load(&fixture.Domain->AddressSpace->References) == 2);
    __ProbeConnection(fixture.Sim, 0, OS_ENOENT);
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    assert(before.Connections == 0 && before.ReservedHardwareIDs == 0 && before.CachedHardwareIDs == 0);
    // A hardware ID can select a new root after proven disconnect, even while
    // the old root remains allocated for its independent mapping reference.
    __CheckHardwareIDReuse(fixture.Sim, OS_EOK);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EBUSY);
    DMAIOMMUSimSnapshot(fixture.Sim, &after);
    assert(after.SubmittedCommands == before.SubmittedCommands);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    __FinishConnectionFixture(&fixture, service);
}

static void
__TestConnectionDeadline(MSContextSecurity_t* service)
{
    struct __ConnectionFixture fixture = __CreateConnectionFixture(service);
    DMAIOMMUSimControl_t control = {.CompletionDelayNs = 150};
    DMAIOMMUSimStats_t stats;

    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_ETIMEOUT);
    control.CompletionDelayNs = 60;
    assert(DMAIOMMUSimSetControl(fixture.Sim, &control) == OS_EOK);
    // Fifty of the hundred nanoseconds go to the pending connect command.
    // The new disconnect command needs sixty, so this call cannot complete it.
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_ETIMEOUT);
    DMAIOMMUSimSnapshot(fixture.Sim, &stats);
    assert(stats.SubmittedCommands == 2 && stats.CompletedCommands == 1 && stats.PendingCommands == 1);
    __FinishConnectionFixture(&fixture, service);
}

struct __ConnectionRace {
    DMATranslatedDomain_t* Domain;
    oserr_t                Status;
};

static void*
__ConnectWhileClosing(void* argument)
{
    struct __ConnectionRace* race = argument;

    race->Status = DMATranslatedDomainConnect(race->Domain);
    return NULL;
}

static void
__TestCloseDuringConnect(MSContextSecurity_t* service)
{
    struct __ConnectionFixture fixture = __CreateConnectionFixture(service);
    struct DMAAddressMapping* mapping = __MapConnectionRoot(fixture.Domain);
    struct __ConnectionRace race = {.Domain = fixture.Domain};
    pthread_t worker;

    assert(pthread_barrier_init(&g_callbackReady, NULL, 2) == 0);
    assert(pthread_barrier_init(&g_callbackContinue, NULL, 2) == 0);
    g_pauseConnect = true;
    assert(pthread_create(&worker, NULL, __ConnectWhileClosing, &race) == 0);
    __Barrier(&g_callbackReady);
    // These locks must be available while the backend callback is paused.
    assert(pthread_mutex_trylock(&fixture.Manager->UpdateLock.Native) == 0);
    assert(pthread_mutex_unlock(&fixture.Manager->UpdateLock.Native) == 0);
    assert(DestroyHandle(fixture.ManagerHandle) == OS_EOK);
    __ProbeConnection(fixture.Sim, 0, OS_EOK);
    __Barrier(&g_callbackContinue);
    assert(pthread_join(worker, NULL) == 0);
    g_pauseConnect = false;
    assert(race.Status == OS_ECANCELLED);
    assert(fixture.Domain->ConnectionState == DMAConnectionConnecting);
    assert(DMATranslatedDomainConnect(fixture.Domain) == OS_ECANCELLED);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EBUSY);
    __ProbeConnection(fixture.Sim, 0, OS_ENOENT);
    assert(DMAAddressMappingRelease(&mapping) == OS_EOK);
    assert(DMATranslatedDomainDestroy(&fixture.Domain) == OS_EOK);
    assert(DMAIOMMUSimDestroy(&fixture.Sim) == OS_EOK);
    DMAManagerRelease(&fixture.Manager);
    __Drain();
    assert(pthread_barrier_destroy(&g_callbackReady) == 0);
    assert(pthread_barrier_destroy(&g_callbackContinue) == 0);
}

static void
__TestConnectionCallbackFailure(MSContextSecurity_t* service, MSContextSecurity_t* driver,
                                enum __CallbackFault point)
{
    struct __FailureFixture fixture = __PrepareFailureFixture(service, driver);
    uint32_t hardwareID = 77;
    uuid_t device = 1800;
    DMATranslatedDomainConfig_t config = __DomainConfig(300, 4096, &hardwareID, 1, &device, 1);
    DMATranslatedDomain_t* domain = NULL;
    DMAIOMMUSimStats_t before;
    DMAIOMMUSimStats_t after;

    g_current = service;
    DMAIOMMUSimSnapshot(fixture.Sims[0], &before);
    if (point == __FaultPrepareHardwareIDs) {
        g_callbackFault = point;
        assert(DMATranslatedDomainCreate(fixture.Manager, g_faultUnit, &config, &domain) == OS_ECANCELLED);
        assert(domain == NULL);
        DMAIOMMUSimSnapshot(fixture.Sims[0], &after);
        assert(after.Connections == before.Connections && after.Spaces == before.Spaces);
    } else {
        assert(DMATranslatedDomainCreate(fixture.Manager, g_faultUnit, &config, &domain) == OS_EOK);
        if (point == __FaultConnect || point == __FaultConnectPublication) {
            g_callbackFault = point;
            assert(DMATranslatedDomainConnect(domain) == OS_ECANCELLED);
            __CheckFailureGroups(&fixture, false);
            assert(DMATranslatedDomainDestroy(&domain) == OS_ECANCELLED);
        } else if (point == __FaultDisconnect) {
            assert(DMATranslatedDomainConnect(domain) == OS_EOK);
            g_callbackFault = point;
            assert(DMATranslatedDomainDestroy(&domain) == OS_ECANCELLED);
        } else {
            assert(point == __FaultDestroyHardwareIDs);
            g_callbackFault = point;
            assert(DMATranslatedDomainDestroy(&domain) == OS_EOK);
        }
        if (domain != NULL) {
            assert(domain->Connection != NULL && domain->ConnectionSpace == domain->AddressSpace);
            assert(atomic_load(&domain->AddressSpace->References) == 2);
        }
    }
    __CheckFailureGroups(&fixture, false);
}

static void
__TestConnections(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __TestConnectionAdmission(service, driver);
    __TestHardwareIDReservations(service);
    __TestConnectionAllocationFailures(service);
    for (unsigned int point = 0; point < 3; point++) {
        __TestConnectionProgress(service, false, point);
        __TestConnectionProgress(service, true, point);
    }
    __TestConnectionDeadline(service);
    __TestCloseDuringConnect(service);
    puts("DMA connections: reservations, admission, partial progress, stale routes, retries and close race passed");
}
