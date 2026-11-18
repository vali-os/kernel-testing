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

#include "../kernel/api/dma_region.c"

static void
__TestTranslatedRegions(MSContextSecurity_t* service, MSContextSecurity_t* driver)
{
    struct __TranslatedFixture fixture = __PrepareTranslatedFixture(service, driver, 4096, 2);
    paddr_t pages[] = { 0x10000000, 0x10002000 };
    struct SHMRegionRange ranges[] = { { 0, 128 }, { 128, 4096 }, { 4224, 3968 } };
    struct OSDmaMappingResult mappings[2];
    OSDmaRegionSession_t sessions[2];
    OSBufferIdentity_t identity;
    DMAIOMMUSimStats_t before, after;
    OSDmaStopTicket_t ticket;
    oserr_t status;

    __CreateBuffer(pages, 2, 0, 8192, SHM_COMMIT | SHM_REGIONS);
    __RegisterTestBuffer();
    assert(SHMRegionRegister(g_buffer->ID, ranges, 3) == OS_EOK);
    identity = (OSBufferIdentity_t) { g_buffer->ID, 1, 1 };
    for (unsigned int i = 0; i < 2; i++) {
        SHMRecord_t* record;
        SHMDeviceRegionSession_t* session;

        mappings[i] = __MapTranslated(&fixture, i, g_buffer->ID, 0, 8192,
            DMABidirectional, DMA_MAP_REQUIRE_DIRECT | DMA_MAP_REGIONS);
        assert(ScDmaRegionSessionOpen(mappings[i].Handle, &identity, 2, &sessions[i]) == OS_EOK);
        assert(sessions[i].Offset == 128 && sessions[i].Length == 4096);
        record = LookupHandle(sessions[i].Handle, HandleTypeSHMAccess);
        session = record->Object;
        assert(session->Region->FirstExtent == 0 && session->Region->ExtentOffset == 128);
        assert(session->Mapping->ExtentCount == 2);
        assert(session->Mapping->Extents[0].Address == pages[0]);
        assert(session->Mapping->Extents[1].Address == pages[1]);
        assert(mappings[i].InlineSegment.Address != pages[0]);
    }
    DMAIOMMUSimSnapshot(fixture.Sim, &before);
    assert(ScDmaRegionBeginUse(sessions[0].Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionBeginUse(sessions[1].Handle, 0, 1, DMAToDevice) == OS_EOK);
    assert(ScDmaRegionBeginUse(sessions[0].Handle, 1, 1, DMAFromDevice) == OS_EBUSY);
    __CheckHardwareID(&fixture, 17, mappings[0].InlineSegment.Address + 128, 128, DMAToDevice, pages[0] + 128);
    DMAIOMMUSimSnapshot(fixture.Sim, &after);
    assert(before.Mappings == after.Mappings && before.SubmittedCommands == after.SubmittedCommands);
    assert(ScDmaRegionEndUse(sessions[0].Handle, 0, 1) == OS_EOK);
    assert(ScDmaRegionBeginUse(sessions[0].Handle, 0, 2, DMAToDevice) == OS_EOK);
    g_current = service;
    assert(DMAManagerPrepareStop(fixture.Manager, fixture.Attachments[0], &ticket) == OS_EOK);
    status = DMAManagerCompleteStop(fixture.Manager, &ticket);
    assert(status == OS_EBUSY || status == OS_EOK);
    if (status == OS_EBUSY) {
        assert(DMAManagerCompleteStop(fixture.Manager, &ticket) == OS_EOK);
    }
    g_current = driver;
    assert(ScDmaRegionEndUse(sessions[0].Handle, 0, 2) == OS_EDEVFAULT);
    assert(ScDmaRegionEndUse(sessions[0].Handle, 0, 2) == OS_EDEVFAULT);
    assert(ScDmaRegionEndUse(sessions[1].Handle, 0, 1) == OS_EDEVFAULT);
    for (unsigned int i = 0; i < 2; i++) {
        assert(ScResourceRelease(sessions[i].Handle) == OS_EOK);
        assert(ScResourceRelease(mappings[i].Handle) == OS_EOK);
    }
    assert(DestroyHandle(g_buffer->ID) == OS_EOK);
    __Drain();
    assert(g_buffer == NULL);
    __FinishTranslatedFixture(&fixture);
    puts("Translated regions: physical cache extents, shared readers, stable IOVAs and stopped receipts passed");
}
