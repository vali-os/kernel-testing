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

static __BoundFixture_t* g_recoveryFixture;
static OSDmaStopTicket_t g_recoveryTicket;
static unsigned int     g_cancelPasses;
static unsigned int     g_cleanupChecks;

static void
__RequestAnotherCancelPass(void)
{
    DMARecoveryGroup_t* group = g_recoveryFixture->Context->Binding->Group;

    g_cancelPasses++;
    assert(g_cancelPasses <= 2);
    if (g_cancelPasses == 1) {
        // Several requests during one pass need only one repeat.
        DMARecoveryCancel(group);
        DMARecoveryCancel(group);
        assert(DMAManagerCompleteStop(g_recoveryFixture->Manager, &g_recoveryTicket) == OS_EBUSY);
        g_afterRevokeHook = __RequestAnotherCancelPass;
    }
}

static void
__CheckCleanupExclusion(void)
{
    bool stopped = false;

    g_cleanupChecks++;
    // Manager and group locks are dropped.
    // A second stop pass or a driver release must return OS_EBUSY while the
    // first pass is still freeing device addresses.
    assert(DMAManagerCompleteStop(g_recoveryFixture->Manager, &g_recoveryTicket) == OS_EBUSY);
    assert(DMABindingBeginOperation(g_recoveryFixture->Context->Binding, true, &stopped) == OS_EBUSY);
}

static void
__TestRecoveryPassExclusion(
    _In_ MSContextSecurity_t* service,
    _In_ MSContextSecurity_t* driver)
{
    __BoundFixture_t          fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMappingResult result;

    __OpenBoundFixture(&fixture);
    result = __MapBoundFixture(&fixture, DMA_MAP_REQUIRE_DIRECT);
    g_recoveryFixture = &fixture;
    g_cancelPasses = 0;
    g_cleanupChecks = 0;
    g_afterRevokeHook = __RequestAnotherCancelPass;
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachment.Value, &g_recoveryTicket) == OS_EOK);
    assert(g_cancelPasses == 2 && g_afterRevokeHook == NULL);

    g_releaseHook = __CheckCleanupExclusion;
    g_failAddressRelease = true;
    assert(DMAManagerCompleteStop(fixture.Manager, &g_recoveryTicket) == OS_EUNKNOWN);
    assert(g_cleanupChecks == 1 && g_releaseHook == NULL);

    // Allow another attempt after cleanup fails.
    g_failAddressRelease = false;
    g_releaseHook = __CheckCleanupExclusion;
    assert(DMAManagerCompleteStop(fixture.Manager, &g_recoveryTicket) == OS_EOK);
    assert(g_cleanupChecks == 2 && g_releaseHook == NULL);
    assert(g_cancelPasses == 2);
    g_current = driver;
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
    g_recoveryFixture = NULL;
}
