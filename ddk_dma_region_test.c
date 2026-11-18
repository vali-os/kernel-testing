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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef int errno_t;
#include <ddk/dma_region.h>
#include <os/types/syscall.h>
#include <internal/_syscalls.h>

static unsigned int g_allocations, g_calls, g_segments, g_messages;
static int g_failAllocation = -1;
static oserr_t g_status, g_endStatus;
static unsigned int g_generation;
static bool g_session;
static void* __Allocate(size_t size)
{
    void* result;
    if (g_failAllocation == 0) return NULL;
    if (g_failAllocation > 0) g_failAllocation--;
    result = malloc(size);
    assert(result != NULL);
    g_allocations++;
    return result;
}
static void* __Calloc(size_t count, size_t size)
{
    void* result = __Allocate(count * size);
    if (result != NULL) memset(result, 0, count * size);
    return result;
}
static void __Free(void* pointer)
{
    if (pointer != NULL) { assert(g_allocations != 0); g_allocations--; free(pointer); }
}
#define malloc __Allocate
#define calloc __Calloc
#define free __Free
#include "../librt/libddk/dma/dma.c"
#include "../librt/libddk/dma/region.c"
#undef free
#undef calloc
#undef malloc

void __DmaDeferCleanup(struct __DmaResource* resource) { (void)resource; assert(0); }
void SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{ (void)level; (void)format; g_messages++; }

SCTYPE _syscall(SCTYPE number, SCTYPE a, SCTYPE b, SCTYPE c, SCTYPE d, SCTYPE e)
{
    g_calls++;
    assert(e == 0);
    if (g_status != OS_EOK) return g_status;
    switch (number) {
        case 84: {
            const struct OSDmaMapParameters* request = (void*)(uintptr_t)c;
            struct OSDmaMappingResult* result = (void*)(uintptr_t)d;
            assert(a == 11 && b == 22 && request->Flags == (DMA_MAP_REQUIRE_DIRECT | DMA_MAP_REGIONS));
            assert(request->Direction == DMABidirectional);
            *result = (struct OSDmaMappingResult) { .Handle = 33, .SegmentCount = 2 };
            return OS_EOK;
        }
        case 85: {
            struct DMASegment* result = (void*)(uintptr_t)b;
            assert(a == 33 && c == 2);
            result[0] = (struct DMASegment) { 0x8000, 256 };
            result[1] = (struct DMASegment) { 0xa000, 256 };
            *(uint32_t*)(uintptr_t)d = 2;
            g_segments++;
            return OS_EOK;
        }
        case 102: {
            OSDmaRegionSession_t* result = (void*)(uintptr_t)d;
            const OSBufferIdentity_t* identity = (void*)(uintptr_t)b;
            assert(a == 33 && c == 4 && !g_session);
            *result = (OSDmaRegionSession_t) { .Handle = 44, .Identity = *identity,
                .TokenCapacity = 4, .Offset = 128, .Length = 256 };
            g_session = true;
            g_generation = 0;
            return OS_EOK;
        }
        case 103:
            assert(a == 44 && g_session && b == 2 && c == g_generation + 1 && d == DMAFromDevice);
            g_generation++;
            return OS_EOK;
        case 104:
            assert(a == 44 && g_session && b == 2 && c == g_generation && d == 0);
            return g_endStatus;
        case 89:
            assert(b == 0 && c == 0 && d == 0);
            if (a == 44) { assert(g_session); g_session = false; }
            else { assert(a == 33 && !g_session); }
            return OS_EOK;
        default: assert(0); return OS_EUNKNOWN;
    }
}

int main(void)
{
    struct DMAContext context = { .Handle = 11 };
    OSHandle_t buffer = { .ID = 22, .Type = OSHANDLE_SHM };
    OSBufferIdentity_t identity = { 22, 1, 1 };
    struct DMAMapping* mapping = NULL;
    DMARegionSession_t* session = NULL;
    DMARegionUse_t use = { 0 }, duplicate = { 0 };
    const struct DMASegment* segments;
    uint32_t count;
    unsigned int calls, allocations, messages;
    oserr_t outcome = OS_EUNKNOWN;

    assert(DMAMapSHMRegions(&context, &buffer, 0, 512, DMABidirectional, NULL, &mapping) == OS_EOK);
    calls = g_calls;
    allocations = g_allocations;
    for (int point = 0; point < 2; point++) {
        g_failAllocation = point;
        assert(DMARegionSessionOpen(mapping, &identity, 4, &session) == OS_EOOM);
        assert(!session && g_calls == calls && g_allocations == allocations);
    }
    g_failAllocation = -1;
    g_status = OS_ECANCELLED;
    assert(DMARegionSessionOpen(mapping, &identity, 4, &session) == OS_ECANCELLED);
    assert(!session && g_allocations == allocations);
    g_status = OS_EOK;
    assert(DMARegionSessionOpen(mapping, &identity, 4, &session) == OS_EOK);
    segments = DMARegionSessionSegments(session, &count);
    assert(count == 2 && segments[0].Address == 0x8080 && segments[0].Length == 128);
    assert(segments[1].Address == 0xa000 && segments[1].Length == 128 && g_segments == 1);
    assert(DMARegionUseInitialize(session, 2, &use) == OS_EOK);
    assert(DMARegionUseInitialize(session, 2, &duplicate) == OS_EBUSY);
    assert(DMAUnmap(&mapping) == OS_EBUSY);
    calls = g_calls;
    allocations = g_allocations;
    messages = g_messages;
    g_failAllocation = 0;
    for (int i = 0; i < 100; i++) {
        g_status = OS_EBUSY;
        assert(DMATryBeginRegionUse(&use, DMAFromDevice) == OS_EBUSY);
        assert(!use.Active && use.Generation == (unsigned int)i);
        g_status = OS_EOK;
        assert(DMATryBeginRegionUse(&use, DMAFromDevice) == OS_EOK);
        assert(DMARegionSessionClose(&session) == OS_EBUSY);
        g_endStatus = OS_EBUSY;
        assert(DMAEndRegionUse(&use, &outcome) == OS_EBUSY && use.Active);
        g_endStatus = i % 2 ? OS_EDEVFAULT : OS_EOK;
        assert(DMAEndRegionUse(&use, &outcome) == OS_EOK && !use.Active && outcome == g_endStatus);
    }
    assert(g_calls == calls + 400 && g_allocations == allocations && g_messages == messages && g_segments == 1);
    g_failAllocation = -1;
    g_status = OS_EUNKNOWN;
    assert(DMARegionSessionClose(&session) == OS_EUNKNOWN && session != NULL);
    calls = g_calls;
    assert(DMATryBeginRegionUse(&use, DMAFromDevice) == OS_EBUSY && g_calls == calls);
    g_status = OS_EOK;
    assert(DMARegionSessionClose(&session) == OS_EOK && session == NULL);
    g_status = OS_EUNKNOWN;
    assert(DMAUnmap(&mapping) == OS_EUNKNOWN && mapping != NULL);
    calls = g_calls;
    assert(DMARegionSessionOpen(mapping, &identity, 4, &session) == OS_EBUSY && g_calls == calls);
    g_status = OS_EOK;
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL && g_allocations == 0);
    puts("DDK device regions: ABI arguments, cached slices, allocation failures, exact retries and syscall/message budgets passed");
    return 0;
}
