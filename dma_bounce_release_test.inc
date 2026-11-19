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
// Bounce storage release failures, for installed mappings (retry) and for
// mappings that failed before installation (quarantine).
#include <sys/wait.h>
#include <unistd.h>

// Pool release failures. While g_failUnitReleases is nonzero, a release of the
// unit at g_failUnitBase fails and leaves the record unchanged, as the pool
// does. A base of 0 matches any unit. No test pool starts at address 0.
// g_unitReleaseCalls counts every release call, failed or not. A suite that
// wraps the pool release itself defines DMA_TEST_EXTERNAL_POOL_RELEASE and
// does not run these cases.
static paddr_t      g_failUnitBase;
static unsigned int g_failUnitReleases;
static unsigned int g_unitReleaseCalls;

#ifndef DMA_TEST_EXTERNAL_POOL_RELEASE
oserr_t __real_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation);
oserr_t __wrap_SystemMemoryReleaseAllocation(struct SystemMemoryAllocation* allocation)
{
    g_unitReleaseCalls++;
    if (g_failUnitReleases != 0 && allocation->Owner != NULL &&
        (g_failUnitBase == 0 || allocation->PhysicalBase == g_failUnitBase)) {
        g_failUnitReleases--;
        return OS_EUNKNOWN;
    }
    return __real_SystemMemoryReleaseAllocation(allocation);
}
#endif

// Run a case that leaves quarantined storage behind in a child process. The
// storage is lost on purpose, so the parent's final leak checks cannot run
// after it. A child assertion is printed by the child and fails the parent.
static void
__RunBoundChild(
    void (*test)(MSContextSecurity_t*, MSContextSecurity_t*, int),
    MSContextSecurity_t* service,
    MSContextSecurity_t* driver,
    int                  variant)
{
    int   status;
    pid_t child = fork();

    assert(child >= 0);
    if (child == 0) {
        test(service, driver, variant);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// Count how often pattern occurs in a log history. The translated bounce
// cases in dma_translated_bounce_test.inc use it too.
static size_t
__CountText(const char* text, const char* pattern)
{
    size_t count = 0;

    for (const char* at = strstr(text, pattern); at != NULL; at = strstr(at + 1, pattern)) {
        count++;
    }
    return count;
}

enum __BounceRetryPath {
    // Both attempts are driver unmap calls.
    __BounceRetryUnmap,
    // Both attempts are stop cleanup passes.
    __BounceRetryStop,
    // A driver unmap fails, and a stop cleanup pass finishes the release.
    __BounceRetryUnmapThenStop
};

// One release attempt, through a driver unmap or a stop cleanup pass. The
// first stop attempt prepares the stop. A retry completes the same ticket.
static oserr_t
__ReleaseBounceAttempt(
    __BoundFixture_t*         fixture,
    struct OSDmaMappingResult result,
    bool                      stop,
    bool*                     stopPrepared,
    OSDmaStopTicket_t*        ticket)
{
    oserr_t status;

    if (!stop) {
        g_current = fixture->Driver;
        return ScResourceRelease(result.Handle);
    }

    g_current = fixture->Service;
    if (!*stopPrepared) {
        assert(DMAManagerPrepareStop(fixture->Manager, fixture->Attachment.Value, ticket) == OS_EOK);
        *stopPrepared = true;
    }
    status = DMAManagerCompleteStop(fixture->Manager, ticket);
    g_current = fixture->Driver;
    return status;
}

// An installed direct bounce mapping fails to release its storage twice: first
// the bounce view stays mapped, then the pool refuses the unit. Each retry must
// resume where the last one stopped. The device addresses are removed once,
// the stop cache work runs once, and the unit goes back to the pool once.
static void
__TestBounceReleaseRetry(MSContextSecurity_t* service, MSContextSecurity_t* driver, int path)
{
    __BoundFixture_t          fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMapParameters parameters = { .Length = 256, .Direction = DMAFromDevice };
    struct OSDmaMappingResult result;
    OSDmaStopTicket_t         ticket;
    bool                      stopPrepared = false;
    SHMDeviceMapping_t*       mapping;
    SHMDeviceBounce_t*        bounce;
    paddr_t                   pages[] = { 0x9000 };
    unsigned int              addressReleases;
    unsigned int              unitReleases;
    int                       cacheCalls;
    bool                      firstStop = path == __BounceRetryStop;
    bool                      secondStop = path != __BounceRetryUnmap;

    // A cached pool makes the stop pass record its cache work.
    __UsePool(0x1000, true);
    __OpenBoundFixture(&fixture);
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    mapping = ((SHMRecord_t*)LookupHandle(result.Handle, HandleTypeSHMAccess))->Object;
    bounce = &mapping->Stream.Bounce;
    assert(bounce->UnitCount == 1 && bounce->Units[0].Owner != NULL && bounce->BounceMapping != 0);
    assert(bounce->BounceMappingLength == 0x1000 && bounce->MappedLength == 256);
    assert(mapping->ExtentCount == 1 && mapping->Extents[0].Address == bounce->Units[0].PhysicalBase);

    // First attempt: the bounce view cannot be removed. The device addresses
    // are gone, but the unit must stay out of the pool while the view exists.
    // The source view does not depend on the bounce view and is removed.
    addressReleases = g_addressReleaseCalls;
    unitReleases = g_unitReleaseCalls;
    g_cacheCallCount = 0;
    g_boundErrorHistory[0] = '\0';
    g_failBounceUnmaps = 1;
    assert(__ReleaseBounceAttempt(&fixture, result, firstStop, &stopPrepared, &ticket) == OS_EUNKNOWN);
    assert(g_failBounceUnmaps == 0 && g_addressReleaseCalls == addressReleases + 1);
    assert(g_unitReleaseCalls == unitReleases && !__PoolAllFree());
    assert(strstr(g_boundErrorHistory, "MemorySpaceUnmap bounce view") != NULL);
    assert(strstr(g_boundErrorHistory, "SHMDeviceReleaseBounce mapping") != NULL);
    assert(mapping->AddressMapping == NULL && bounce->BounceMapping != 0 && bounce->SourceMapping == 0);
    assert(bounce->Units != NULL && bounce->Units[0].Owner != NULL);
    assert(mapping->Access.Buffer == g_buffer && mapping->Extents != NULL);
    assert(mapping->State == (firstStop ? SHMDeviceMappingReleasingAfterStop : SHMDeviceMappingReleasing));
    assert(LookupHandle(result.Handle, HandleTypeSHMAccess) != NULL);
    assert(ScDmaGetSegments(result.Handle, NULL, 0, &(uint32_t){ 0 }) == (firstStop ? OS_ECANCELLED : OS_EBUSY));
    cacheCalls = g_cacheCallCount;
    assert(cacheCalls == (firstStop ? 1 : 0));

    // Second attempt: the view goes, but the pool refuses the unit.
    g_failUnitBase = bounce->Units[0].PhysicalBase;
    g_failUnitReleases = 1;
    g_boundErrorHistory[0] = '\0';
    assert(__ReleaseBounceAttempt(&fixture, result, secondStop, &stopPrepared, &ticket) == OS_EUNKNOWN);
    assert(g_failUnitReleases == 0 && g_unitReleaseCalls == unitReleases + 1);
    assert(strstr(g_boundErrorHistory, "SystemMemoryReleaseAllocation unit 0 of 1") != NULL);
    assert(bounce->BounceMapping == 0 && bounce->Units != NULL && bounce->Units[0].Owner != NULL);
    assert(mapping->AddressMapping == NULL && mapping->Access.Buffer == g_buffer && !__PoolAllFree());
    assert(g_addressReleaseCalls == addressReleases + 1 && g_cacheCallCount == cacheCalls);

    // Third attempt: only the unit is left to free. The buffer reference goes
    // last, after all bounce storage is back in the pool.
    assert(__ReleaseBounceAttempt(&fixture, result, secondStop, &stopPrepared, &ticket) == OS_EOK);
    assert(g_unitReleaseCalls == unitReleases + 2 && g_addressReleaseCalls == addressReleases + 1);
    assert(g_cacheCallCount == cacheCalls && __PoolAllFree() && g_liveViews == 0);
    if (secondStop) {
        assert(mapping->State == SHMDeviceMappingStopped && bounce->Units == NULL);
        assert(mapping->Access.Buffer == NULL && g_buffer->Access.Faults == 0);
        assert(ScResourceRelease(result.Handle) == OS_EOK);
    }
    assert(LookupHandle(result.Handle, HandleTypeSHMAccess) == NULL);
    g_failUnitBase = 0;
    __FinishBoundFixture(&fixture);
    __UsePool(0x100000, false);
}

enum __QuarantineFailure {
    __QuarantineBounceView,
    __QuarantineUnit,
    __QuarantineSourceView
};

// A direct bounce mapping fails before installation because the device cannot
// reach the pool, and its storage release fails too. The map call keeps its
// original error and publishes nothing. Units that could not be freed stay out
// of the pool, the binding stays live, and the next map on the context works.
static void
__TestBounceQuarantine(MSContextSecurity_t* service, MSContextSecurity_t* driver, int failure)
{
    __BoundFixture_t          fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMapParameters parameters = { .Length = 256, .Direction = DMAFromDevice };
    struct OSDmaMappingResult result = { .Handle = 0xdead };
    paddr_t                   pages[] = { 0x9000 };
    size_t                    freePages;
    int                       views;

    // The pool lies outside the device's only range.
    __UsePool(0x20000, false);
    __OpenBoundFixture(&fixture);
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    freePages = MemoryStackFreeBlockCount(&g_pool.Stack);
    views = g_liveViews;
    g_boundErrorHistory[0] = '\0';
    if (failure == __QuarantineBounceView) {
        g_failBounceUnmaps = 1;
    } else if (failure == __QuarantineUnit) {
        g_failUnitReleases = 1;
    } else {
        g_failSourceUnmaps = 1;
    }
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_ENOENT);
    assert(result.Handle == 0xdead && fixture.Context->Mappings == NULL);
    assert(g_failBounceUnmaps == 0 && g_failUnitReleases == 0 && g_failSourceUnmaps == 0);
    if (failure == __QuarantineSourceView) {
        // Only the source view is left behind. The unit goes back.
        assert(MemoryStackFreeBlockCount(&g_pool.Stack) == freePages && g_liveViews == views + 1);
        assert(strstr(g_boundErrorHistory, "MemorySpaceUnmap source view") != NULL);
        assert(strstr(g_boundErrorHistory, "0 bytes left allocated") != NULL);
    } else {
        assert(MemoryStackFreeBlockCount(&g_pool.Stack) == freePages - 1);
        assert(g_liveViews == views + (failure == __QuarantineBounceView ? 1 : 0));
        assert(strstr(g_boundErrorHistory, failure == __QuarantineBounceView ?
            "MemorySpaceUnmap bounce view" : "SystemMemoryReleaseAllocation unit 0 of 1") != NULL);
        assert(strstr(g_boundErrorHistory, "4096 bytes left allocated") != NULL);
    }
    assert(strstr(g_boundErrorHistory, "(original status 3)") != NULL);
    assert(fixture.Context->Binding->Group->Access.State == DMARecoveryLive);

    // The context stays usable. A reachable pool lets the same request bounce.
    // A quarantined bounce view stays mapped for good, and the host stub
    // tracks only one bounce view, so this child forgets it.
    if (failure == __QuarantineBounceView) {
        g_bounceMemory = NULL;
    }
    __UsePool(0x1000, false);
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    __FinishBoundFixture(&fixture);
}

// A direct bounce mapping that finds the pool full fails with the pool's
// OS_EOOM and leaves nothing behind. Direct pool shortages are not counted per
// context, so every shortage stays an error in the log, and not only the first one.
static void
__TestBouncePoolShortage(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t              fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMapParameters     parameters = { .Length = 256, .Direction = DMAFromDevice };
    struct OSDmaMappingResult     result = { .Handle = 0xdead };
    struct SystemMemoryAllocation held[8] = { 0 };
    paddr_t                       pages[] = { 0x9000 };

    __UsePool(0x1000, false);
    __OpenBoundFixture(&fixture);
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();
    for (size_t i = 0; i < 8; i++) {
        assert(SystemMemoryAllocateDevice(&g_machine.PhysicalMemory, 0x1000, 0, 0, &held[i]) == OS_EOK);
    }

    g_boundErrorHistory[0] = '\0';
    for (int attempt = 0; attempt < 2; attempt++) {
        assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOOM);
        assert(result.Handle == 0xdead && fixture.Context->Mappings == NULL && g_liveViews == 0);
    }
    assert(__CountText(g_boundErrorHistory, "SystemMemoryAllocateDevice unit 0 of 1, length 4096 failed") == 2);
    assert(fixture.Context->TranslatedBounce.PoolShortages == 0);

    // The syscall layer does not log a second error for a shortage. A map
    // call that fails for any other reason is still an error there. This
    // range starts past the end of the buffer.
    assert(strstr(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == NULL);
    parameters.Offset = 0x1000;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EINVALPARAMS);
    assert(__CountText(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == 1);
    parameters.Offset = 0;

    // The same request works once the pool has room again.
    for (size_t i = 0; i < 8; i++) {
        assert(SystemMemoryReleaseAllocation(&held[i]) == OS_EOK);
    }
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    assert(ScResourceRelease(result.Handle) == OS_EOK);
    assert(__PoolAllFree());
    __FinishBoundFixture(&fixture);
    __UsePool(0x100000, false);
}

// A direct-only map of pages the device cannot reach is refused with
// OS_ENOENT. Drivers send it first and then ask for a bounce mapping, so the
// refusal is not an error in the log. The first bounce mapping of the context
// is counted, so it is reported once. The same refusal for a map that may
// bounce is a real failure and stays an error.
static void
__TestDirectRefusalLogging(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    __BoundFixture_t          fixture = __PrepareBoundFixture(service, driver);
    struct OSDmaMapParameters parameters = {
        .Length = 256, .Direction = DMAFromDevice, .Flags = DMA_MAP_REQUIRE_DIRECT
    };
    struct OSDmaMappingResult result = { .Handle = 0xdead };
    paddr_t                   pages[] = { 0x9000 };

    // The device reaches only 0x1000-0x1fff. The pool lies there, the buffer
    // page does not.
    __UsePool(0x1000, false);
    __OpenBoundFixture(&fixture);
    __CreateBuffer(pages, 1, 0, 0x1000, SHM_COMMIT);
    __RegisterTestBuffer();

    g_boundErrorHistory[0] = '\0';
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_ENOENT);
    assert(result.Handle == 0xdead && fixture.Context->Mappings == NULL);
    assert(g_boundErrorHistory[0] == '\0');
    assert(fixture.Context->DirectBounceMappings == 0);

    // The bounce mapping that follows works and is counted once.
    parameters.Flags = 0;
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_EOK);
    assert(g_boundErrorHistory[0] == '\0');
    assert(fixture.Context->DirectBounceMappings == 1);
    assert(ScResourceRelease(result.Handle) == OS_EOK);

    // With the pool out of reach the bounce mapping fails too. Nothing else
    // can be tried, so the syscall layer logs it as an error.
    result.Handle = 0xdead;
    __UsePool(0x20000, false);
    assert(ScDmaMapShm(fixture.ContextHandle, g_buffer->ID, &parameters, &result) == OS_ENOENT);
    assert(result.Handle == 0xdead && fixture.Context->Mappings == NULL);
    assert(__CountText(g_boundErrorHistory, "ScDmaMapShm: SHMDeviceMap") == 1);
    assert(fixture.Context->DirectBounceMappings == 1);
    __FinishBoundFixture(&fixture);
    __UsePool(0x100000, false);
}

static void
__TestBounceReleaseFailures(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    for (int path = __BounceRetryUnmap; path <= __BounceRetryUnmapThenStop; path++) {
        __TestBounceReleaseRetry(service, driver, path);
    }
    for (int failure = __QuarantineBounceView; failure <= __QuarantineSourceView; failure++) {
        __RunBoundChild(__TestBounceQuarantine, service, driver, failure);
    }
    __TestBouncePoolShortage(service, driver);
    __TestDirectRefusalLogging(service, driver);
    puts("Bound DMA bounce release: retry after view and unit failures, quarantine before install, pool shortage passed");
}
