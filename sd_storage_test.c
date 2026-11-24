/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * Host test for SD sector I/O and the SD storage adapter. The real
 * modules/storage/sd/sequence.c, registers.c, sectors.c and storage.c run
 * against fakes:
 *
 * - A host whose Submit records the request. The test completes it later,
 *   from outside Submit, with a scripted result or with a small card model
 *   (data moves, CMD13 reports the transfer state). Schedule keeps a list of
 *   work items on a fake clock, in due order and among equal due times in
 *   the order they were scheduled, as host.h requires.
 * - DMAMapSHMChunk, DMAMappingSegments, DMABeginTransfer, DMAEndTransfer,
 *   DMAUnmap and SHMRelease, each with scripted results.
 * - StorageAttachTransfer (through testing/sd_storage_include), the IPC
 *   answer and message deferral, and the filed registration calls.
 *
 * Every call is written to one event list as a short line of text, such as
 * "CMD18 arg=100 ..." or "end 4096". Tests compare the exact lines, so they
 * check command arguments, timeouts and the order of the DMA calls. The
 * fakes also check invariants on every call: an answer is sent once per
 * request, never while its chunk is mapped or its buffer attached, and
 * cleanup calls are never made on a released object.
 *
 * Expected values are written out as numbers, so a changed constant in the
 * production code fails a test.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddk/dma.h>
#include <ddk/utils.h>
#include <gracht/link/vali.h>
#include <gracht/server.h>
#include <ddk/storage-transfer.h>
#include <os/shm.h>
#include <ctt_storage_service_server.h>
#include <sys_storage_service_client.h>
#include "sd/commands.h"
#include "sd/sectors.h"
#include "sd/storage.h"

#define MAX_EVENTS   8192
#define EVENT_LENGTH 160
#define MAX_SCRIPT   2048
#define MAX_MESSAGES 128
#define MAX_LOGS     256

// Values the production code must use, written out independently.
#define R1_MASK            0xFD390008U
#define COMMAND_TIMEOUT_US 20000U
#define CARD_ADDRESS       0xB368U
#define CLOCK_HZ           25000000U
#define DRIVER_ID          41U
#define DEVICE_ID          5U
#define ATTACH_OFFSET      4096U
#define FILE_SERVICE       7U

// The card status a ready card reports: transfer state and READY_FOR_DATA.
#define STATUS_TRANSFER ((4U << 9) | (1U << 8))
// The card status while it is still programming.
#define STATUS_PROGRAM  (7U << 9)

/////////////////////////////////////////////////////////////////////////////
// Event list and logging.

static char g_events[MAX_EVENTS][EVENT_LENGTH];
static int  g_eventCount;

static void
__Event(
    _In_ const char* format, ...)
{
    va_list arguments;

    assert(g_eventCount < MAX_EVENTS);
    va_start(arguments, format);
    vsnprintf(g_events[g_eventCount++], EVENT_LENGTH, format, arguments);
    va_end(arguments);
    if (getenv("SD_TEST_VERBOSE") != NULL) {
        fprintf(stderr, "event %d: %s\n", g_eventCount - 1, g_events[g_eventCount - 1]);
    }
}

static void
__DumpEvents(void)
{
    int i;

    for (i = 0; i < g_eventCount; i++) {
        fprintf(stderr, "  %3d: %s\n", i, g_events[i]);
    }
}

// Checks that the events from index start on are exactly the expected ones.
static void
__ExpectEventsFrom(
    _In_ int                start,
    _In_ const char* const* expected,
    _In_ int                count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (start + i >= g_eventCount || strcmp(g_events[start + i], expected[i]) != 0) {
            fprintf(stderr, "event %d: expected \"%s\", got \"%s\"\n", start + i, expected[i],
                    start + i < g_eventCount ? g_events[start + i] : "(none)");
            __DumpEvents();
            assert(!"event mismatch");
        }
    }
    if (start + count != g_eventCount) {
        fprintf(stderr, "expected %d events, got %d\n", start + count, g_eventCount);
        __DumpEvents();
        assert(!"event count mismatch");
    }
}

#define EXPECT_EVENTS(...) do { \
    static const char* const __expected[] = { __VA_ARGS__ }; \
    __ExpectEventsFrom(0, __expected, (int)(sizeof(__expected) / sizeof(__expected[0]))); \
} while (0)

static int
__CountEvents(
    _In_ const char* prefix)
{
    int count = 0;
    int i;

    for (i = 0; i < g_eventCount; i++) {
        if (strncmp(g_events[i], prefix, strlen(prefix)) == 0) {
            count++;
        }
    }
    return count;
}

// Index of the first event that starts with prefix, at or after start.
static int
__FindEvent(
    _In_ const char* prefix,
    _In_ int         start)
{
    int i;

    for (i = start; i < g_eventCount; i++) {
        if (strncmp(g_events[i], prefix, strlen(prefix)) == 0) {
            return i;
        }
    }
    return -1;
}

static char g_logs[MAX_LOGS][EVENT_LENGTH];
static int  g_logLevels[MAX_LOGS];
static int  g_logCount;

void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    char    text[EVENT_LENGTH];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (g_logCount < MAX_LOGS) {
        memcpy(g_logs[g_logCount], text, sizeof(text));
        g_logLevels[g_logCount] = (int)level;
        g_logCount++;
    }
    if (getenv("SD_TEST_VERBOSE") != NULL) {
        fprintf(stderr, "log %d: %s\n", (int)level, text);
    }
}

static int
__CountLogs(
    _In_ const char* text)
{
    int count = 0;
    int i;

    for (i = 0; i < g_logCount; i++) {
        if (strstr(g_logs[i], text) != NULL) {
            count++;
        }
    }
    return count;
}

/////////////////////////////////////////////////////////////////////////////
// Fake host.

static SdHost_t     g_host;
static SdCard_t     g_card;
static int          g_dmaContextStorage;
static SdRequest_t* g_pending;
static bool         g_holdPending;
static SdWork_t*    g_workList;
static uint64_t     g_nowUs;
static SdResult_t   g_script[MAX_SCRIPT];
static int          g_scriptCount;
static int          g_scriptNext;
static oserr_t      g_submitStatus;

static const char*
__StopName(
    _In_ enum SdStopPolicy policy)
{
    switch (policy) {
        case SdStopNone: return "none";
        case SdStopAutoCmd12: return "auto";
        default: return "cmd12";
    }
}

static oserr_t
__FakeSubmit(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    assert(host == &g_host);
    assert(g_pending == NULL);
    assert(request->Complete != NULL);
    assert(request->CommandTimeoutUs != 0);
    if (request->Direction == SdDataNone) {
        __Event("CMD%u arg=0x%x type=%u mask=0x%x cmd=%u", request->Opcode, request->Argument,
                request->ResponseType, request->CardErrorMask, request->CommandTimeoutUs);
    } else {
        assert(request->DataTimeoutUs != 0);
        assert(request->Data.Length == (uint64_t)request->BlockSize * request->BlockCount);
        __Event("CMD%u arg=0x%x type=%u mask=0x%x cmd=%u data=%u %s stop=%s blocks=%ux%u segments=%u",
                request->Opcode, request->Argument, request->ResponseType, request->CardErrorMask,
                request->CommandTimeoutUs, request->DataTimeoutUs,
                request->Direction == SdDataRead ? "read" : "write", __StopName(request->StopPolicy),
                request->BlockCount, request->BlockSize, request->Data.SegmentCount);
    }
    if (g_submitStatus != OS_EOK) {
        return g_submitStatus;
    }
    g_pending = request;
    return OS_EOK;
}

static oserr_t
__FakeSetBus(
    _In_  struct SdHost*         host,
    _In_  const SdBusSettings_t* settings,
    _Out_ uint32_t*              clockHzOut)
{
    (void)host;
    (void)settings;
    (void)clockHzOut;
    assert(!"sector I/O must not change the bus");
    return OS_ENOTSUPPORTED;
}

static oserr_t
__FakeStop(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    assert(host == &g_host);
    assert(request == g_pending);
    __Event("host stop");
    return OS_EOK;
}

static oserr_t
__FakeReset(
    _In_ struct SdHost* host)
{
    (void)host;
    assert(!"sector I/O must not reset the host");
    return OS_EOK;
}

static oserr_t
__FakeGetCardState(
    _In_  struct SdHost* host,
    _Out_ uint32_t*      stateOut)
{
    (void)host;
    (void)stateOut;
    assert(!"sector I/O must not read the card state");
    return OS_ENOTSUPPORTED;
}

static oserr_t
__FakeSchedule(
    _In_ struct SdHost* host,
    _In_ struct SdWork* work,
    _In_ uint32_t       delayUs)
{
    SdWork_t** link = &g_workList;

    assert(host == &g_host);
    assert(work != NULL && work->Function != NULL);
    __Event("schedule %u", delayUs);
    if (work->HostScheduled) {
        return OS_EBUSY;
    }

    // Insert after every item due at the same time or earlier, which keeps
    // items with equal due times in scheduling order.
    work->HostDueUs = g_nowUs + delayUs;
    while (*link != NULL && (*link)->HostDueUs <= work->HostDueUs) {
        link = &(*link)->HostNext;
    }
    work->HostNext = *link;
    *link = work;
    work->HostScheduled = true;
    return OS_EOK;
}

static oserr_t
__FakeCancelWork(
    _In_ struct SdHost* host,
    _In_ struct SdWork* work)
{
    SdWork_t** link = &g_workList;

    assert(host == &g_host);
    __Event("cancel work");
    while (*link != NULL && *link != work) {
        link = &(*link)->HostNext;
    }
    if (*link == NULL) {
        return OS_ENOENT;
    }
    *link = work->HostNext;
    work->HostScheduled = false;
    return OS_EOK;
}

static const SdHostOperations_t g_operations = {
    .Submit = __FakeSubmit,
    .SetBus = __FakeSetBus,
    .Stop = __FakeStop,
    .Reset = __FakeReset,
    .GetCardState = __FakeGetCardState,
    .Schedule = __FakeSchedule,
    .CancelWork = __FakeCancelWork
};

static SdResult_t
__Success(
    _In_ uint32_t response,
    _In_ uint64_t confirmedBytes)
{
    SdResult_t result = { 0 };

    result.Status = OS_EOK;
    result.StopStatus = OS_EOK;
    result.Response[0] = response;
    result.ConfirmedBytes = confirmedBytes;
    result.HardwareStopped = true;
    return result;
}

static SdResult_t
__Failure(
    _In_ oserr_t  status,
    _In_ uint32_t transportErrors,
    _In_ uint64_t confirmedBytes)
{
    SdResult_t result = { 0 };

    result.Status = status;
    result.StopStatus = OS_EOK;
    result.TransportErrors = transportErrors;
    result.ConfirmedBytes = confirmedBytes;
    result.HardwareStopped = true;
    return result;
}

static void
__PushResult(
    _In_ SdResult_t result)
{
    assert(g_scriptCount < MAX_SCRIPT);
    g_script[g_scriptCount++] = result;
}

// The card model used when the script is empty: data commands move all
// their bytes, CMD13 reports a ready card.
static SdResult_t
__ModelResult(
    _In_ const SdRequest_t* request)
{
    if (request->Direction != SdDataNone) {
        return __Success(STATUS_TRANSFER, request->Data.Length);
    }
    return __Success(STATUS_TRANSFER, 0);
}

static void
__CompletePending(void)
{
    SdRequest_t* request = g_pending;
    SdResult_t   result;

    assert(request != NULL);
    if (g_scriptNext < g_scriptCount) {
        result = g_script[g_scriptNext++];
    } else {
        result = __ModelResult(request);
    }
    g_pending = NULL;
    request->Result = result;
    request->Complete(request);
}

static void
__RunWorkItem(void)
{
    SdWork_t* work = g_workList;

    assert(work != NULL);
    g_workList = work->HostNext;
    work->HostScheduled = false;
    if (work->HostDueUs > g_nowUs) {
        g_nowUs = work->HostDueUs;
    }
    work->Function(work);
}

// Runs the worker until nothing is pending: completes requests (unless
// g_holdPending is set) and runs due work items, moving the fake clock.
static void
__Run(void)
{
    int steps = 0;

    for (;;) {
        assert(steps++ < 1000000);
        if (g_pending != NULL && !g_holdPending) {
            __CompletePending();
            continue;
        }
        if (g_workList != NULL) {
            __RunWorkItem();
            continue;
        }
        return;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Fake DMA, attachments and IPC.

struct DMAMapping {
    bool              Live;
    bool              Active;
    size_t            Length;
    struct DMASegment Segments[8];
    uint32_t          SegmentCount;
};

struct __MapScript {
    oserr_t  Status;
    // Bytes to map. Zero maps min(length, DirectLength).
    size_t   Length;
    uint32_t Segments;
};

static struct DMAMapping  g_mappings[4096];
static int                g_mappingCount;
static int                g_liveMappings;
static struct __MapScript g_mapScript[MAX_SCRIPT];
static int                g_mapScriptCount;
static int                g_mapScriptNext;
static oserr_t            g_beginScript[MAX_SCRIPT];
static int                g_beginScriptCount;
static int                g_beginScriptNext;
static oserr_t            g_endScript[16];
static int                g_endScriptCount;
static int                g_endScriptNext;
static oserr_t            g_unmapScript[16];
static int                g_unmapScriptCount;
static int                g_unmapScriptNext;
static oserr_t            g_releaseScript[16];
static int                g_releaseScriptCount;
static int                g_releaseScriptNext;
static oserr_t            g_attachStatus;

// The IPC message ids. The test sets g_currentId before each transfer.
static int                     g_currentId;
static bool                    g_attached[MAX_MESSAGES];
static int                     g_answers[MAX_MESSAGES];
static oserr_t                 g_answerStatus[MAX_MESSAGES];
static size_t                  g_answerSectors[MAX_MESSAGES];
static struct gracht_message*  g_messages[MAX_MESSAGES];
static bool                    g_allowAnswerWithMemory;
static struct sys_shm_transfer g_transfer;

static oserr_t
__NextStatus(
    _In_    const oserr_t* script,
    _In_    int            count,
    _InOut_ int*           next)
{
    if (*next < count) {
        return script[(*next)++];
    }
    return OS_EOK;
}

oserr_t
DMAMapSHMChunk(
    _In_    struct DMAContext*               context,
    _In_    const OSHandle_t*                buffer,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    const struct DMAMapRequirements* requirements,
    _In_    const struct DMAChunkLimits*     limits,
    _InOut_ struct DMAMapping**              mappingOut,
    _Out_   size_t*                          mappedLengthOut)
{
    struct __MapScript script = { OS_EOK, 0, 1 };
    struct DMAMapping* mapping;
    uint64_t           address = 0x10000000ULL + offset;
    uint32_t           i;

    assert(context == (struct DMAContext*)&g_dmaContextStorage);
    assert(*mappingOut == NULL);
    assert(buffer->ID != UUID_INVALID && g_attached[buffer->ID - 1000U]);
    __Event("map off=%zu len=%zu dir=%s align=%llu/%llu limits=%zu/%zu/%zu", offset, length,
            direction == DMAFromDevice ? "from" : (direction == DMAToDevice ? "to" : "both"),
            (unsigned long long)requirements->AddressAlignment, (unsigned long long)requirements->LengthAlignment,
            limits->DirectLength, limits->BounceLength, limits->BlockLength);

    if (g_mapScriptNext < g_mapScriptCount) {
        script = g_mapScript[g_mapScriptNext++];
    }
    if (script.Status != OS_EOK) {
        return script.Status;
    }
    if (script.Length == 0) {
        script.Length = length < limits->DirectLength ? length : limits->DirectLength;
    }
    if (script.Segments == 0) {
        script.Segments = 1;
    }

    assert(g_mappingCount < (int)(sizeof(g_mappings) / sizeof(g_mappings[0])));
    mapping = &g_mappings[g_mappingCount++];
    memset(mapping, 0, sizeof(*mapping));
    mapping->Live = true;
    mapping->Length = script.Length;
    mapping->SegmentCount = script.Segments;
    // Equal pieces of a multiple of four bytes, the rest in the last one.
    for (i = 0; i < script.Segments; i++) {
        mapping->Segments[i].Address = address + i * ((script.Length / script.Segments) & ~(size_t)3U);
        mapping->Segments[i].Length = (script.Length / script.Segments) & ~(size_t)3U;
    }
    mapping->Segments[script.Segments - 1U].Length =
        script.Length - (script.Segments - 1U) * ((script.Length / script.Segments) & ~(size_t)3U);
    g_liveMappings++;
    *mappingOut = mapping;
    *mappedLengthOut = script.Length;
    return OS_EOK;
}

const struct DMASegment*
DMAMappingSegments(
    _In_  const struct DMAMapping* mapping,
    _Out_ uint32_t*                countOut)
{
    assert(mapping->Live);
    *countOut = mapping->SegmentCount;
    return mapping->Segments;
}

oserr_t
DMABeginTransfer(
    _In_ struct DMAMapping*   mapping,
    _In_ size_t               offset,
    _In_ size_t               length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    oserr_t status;

    assert(mapping->Live && !mapping->Active);
    __Event("begin off=%zu len=%zu %s", offset, length,
            mode == SHMWaitNonBlocking ? "nonblocking" : (deadline == NULL ? "wait forever" : "wait"));
    status = __NextStatus(g_beginScript, g_beginScriptCount, &g_beginScriptNext);
    if (status == OS_EOK) {
        mapping->Active = true;
    }
    return status;
}

oserr_t
DMAEndTransfer(
    _In_ struct DMAMapping* mapping,
    _In_ size_t             completedLength)
{
    oserr_t status;

    assert(mapping->Live && mapping->Active);
    assert(completedLength <= mapping->Length);
    __Event("end %zu", completedLength);
    status = __NextStatus(g_endScript, g_endScriptCount, &g_endScriptNext);
    if (status == OS_EOK) {
        mapping->Active = false;
    }
    return status;
}

oserr_t
DMAUnmap(
    _InOut_ struct DMAMapping** mapping)
{
    oserr_t status;

    assert(*mapping != NULL && (*mapping)->Live && !(*mapping)->Active);
    __Event("unmap");
    status = __NextStatus(g_unmapScript, g_unmapScriptCount, &g_unmapScriptNext);
    if (status == OS_EOK) {
        (*mapping)->Live = false;
        *mapping = NULL;
        g_liveMappings--;
    }
    return status;
}

oserr_t
StorageAttachTransfer(
    _In_  const struct sys_shm_transfer* description,
    _In_  size_t                         sectorSize,
    _In_  size_t                         count,
    _In_  bool                           write,
    _Out_ OSHandle_t*                    buffer,
    _Out_ size_t*                        offsetOut)
{
    assert(description == &g_transfer);
    assert(buffer->ID == UUID_INVALID || buffer->ID == 0);
    __Event("attach id=%d size=%zu count=%zu %s", g_currentId, sectorSize, count, write ? "write" : "read");
    if (g_attachStatus != OS_EOK) {
        return g_attachStatus;
    }
    assert(!g_attached[g_currentId]);
    g_attached[g_currentId] = true;
    buffer->ID = 1000U + (uuid_t)g_currentId;
    buffer->Type = OSHANDLE_SHM;
    *offsetOut = ATTACH_OFFSET;
    return OS_EOK;
}

oserr_t
SHMRelease(
    _In_ OSHandle_t* handle)
{
    int     id = (int)(handle->ID - 1000U);
    oserr_t status;

    assert(handle->ID != UUID_INVALID && g_attached[id]);
    // The attachment is kept until every chunk is unmapped.
    if (!g_allowAnswerWithMemory) {
        assert(g_liveMappings == 0);
    }
    __Event("release id=%d", id);
    status = __NextStatus(g_releaseScript, g_releaseScriptCount, &g_releaseScriptNext);
    if (status == OS_EOK) {
        g_attached[id] = false;
        handle->ID = UUID_INVALID;
    }
    return status;
}

void
gracht_server_defer_message(
    struct gracht_message* in,
    struct gracht_message* out)
{
    memcpy(out, in, sizeof(struct gracht_message) + in->size);
}

int
ctt_storage_transfer_response(
    struct gracht_message* message,
    oserr_t                result,
    size_t                 sectorsTransferred)
{
    int id;

    assert(message->size == sizeof(int));
    memcpy(&id, message->payload, sizeof(id));
    assert(id > 0 && id < MAX_MESSAGES);
    __Event("answer id=%d status=%u sectors=%zu", id, result, sectorsTransferred);
    // One answer per request, and only once the memory is handed back.
    assert(g_answers[id] == 0);
    if (!g_allowAnswerWithMemory) {
        assert(!g_attached[id]);
        assert(g_liveMappings == 0);
    }
    g_answers[id]++;
    g_answerStatus[id] = result;
    g_answerSectors[id] = sectorsTransferred;
    return 0;
}

static struct gracht_message*
__Message(
    _In_ int id)
{
    struct gracht_message* message;

    assert(id > 0 && id < MAX_MESSAGES && g_messages[id] == NULL);
    message = calloc(1, sizeof(struct gracht_message) + sizeof(int));
    assert(message != NULL);
    message->size = sizeof(int);
    memcpy(message->payload, &id, sizeof(id));
    g_messages[id] = message;
    return message;
}

/////////////////////////////////////////////////////////////////////////////
// Fake filed client.

static int     g_clientStorage;
static int     g_sendStatus;
static int     g_statusScript[MAX_SCRIPT];
static int     g_statusScriptCount;
static int     g_statusScriptNext;
static int     g_statusDefault;
static oserr_t g_filedResult;

gracht_client_t*
GetGrachtClient(void)
{
    return (gracht_client_t*)&g_clientStorage;
}

uuid_t
OSServiceGetFile(void)
{
    return FILE_SERVICE;
}

int
sys_storage_register(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    const uuid_t                   driverId,
    const uuid_t                   deviceId,
    const enum sys_storage_flags   flags)
{
    struct vali_link_message* message = (struct vali_link_message*)context;

    assert(client == GetGrachtClient());
    assert(message->address.Data.GlobalHandleID == FILE_SERVICE);
    __Event("register driver=%u device=%u flags=%u", driverId, deviceId, (unsigned int)flags);
    return g_sendStatus;
}

int
sys_storage_unregister(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    const uuid_t                   driverId,
    const uuid_t                   deviceId,
    const uint8_t                  forced)
{
    struct vali_link_message* message = (struct vali_link_message*)context;

    assert(client == GetGrachtClient());
    assert(message->address.Data.GlobalHandleID == FILE_SERVICE);
    __Event("unregister driver=%u device=%u forced=%u", driverId, deviceId, forced);
    return g_sendStatus;
}

int
gracht_client_get_status(
    gracht_client_t*               client,
    struct gracht_message_context* context)
{
    (void)context;
    assert(client == GetGrachtClient());
    __Event("status");
    if (g_statusScriptNext < g_statusScriptCount) {
        return g_statusScript[g_statusScriptNext++];
    }
    return g_statusDefault;
}

int
sys_storage_register_result(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    oserr_t*                       resultOut)
{
    (void)context;
    assert(client == GetGrachtClient());
    __Event("register result");
    *resultOut = g_filedResult;
    return 0;
}

int
sys_storage_unregister_result(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    oserr_t*                       resultOut)
{
    (void)context;
    assert(client == GetGrachtClient());
    __Event("unregister result");
    *resultOut = g_filedResult;
    return 0;
}

int
gracht_client_abandon(
    gracht_client_t*               client,
    struct gracht_message_context* context)
{
    (void)context;
    assert(client == GetGrachtClient());
    __Event("abandon");
    return 0;
}

/////////////////////////////////////////////////////////////////////////////
// Test setup.

// A high capacity card of 2^22 sectors (2 GiB) on one data line at 25 MHz.
static void
__Reset(void)
{
    int i;

    memset(&g_host, 0, sizeof(g_host));
    g_host.Operations = &g_operations;
    g_host.Limits.MaximumClockHz = 50000000U;
    g_host.Limits.MinimumClockHz = 100000U;
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1;
    g_host.Limits.MaximumBlockSize = 512U;
    g_host.Limits.MaximumBlockCount = 65535U;
    g_host.Limits.Flags = SD_HOST_FLAG_REMOVABLE;
    g_host.DMAContext = (struct DMAContext*)&g_dmaContextStorage;

    memset(&g_card, 0, sizeof(g_card));
    g_card.Host = &g_host;
    g_card.State = SdCardStateReady;
    g_card.MediaGeneration = 3U;
    g_card.RelativeAddress = CARD_ADDRESS;
    g_card.SectorCount = 1ULL << 22;
    g_card.Addressing = SdCardAddressingSectors;
    g_card.Width = SdBusWidth1;
    g_card.Timing = SdBusTimingDefault;
    g_card.ClockHz = CLOCK_HZ;
    g_card.CardData.Structure = SD_CSD_VERSION_2;
    g_card.Identity.ManufacturerId = 0x03U;
    memcpy(g_card.Identity.ProductName, "SU08G", 6);
    g_card.Identity.SerialNumber = 0x1A2B3C4DU;

    g_eventCount = 0;
    g_logCount = 0;
    g_pending = NULL;
    g_holdPending = false;
    g_workList = NULL;
    g_nowUs = 0;
    g_scriptCount = 0;
    g_scriptNext = 0;
    g_submitStatus = OS_EOK;

    g_mappingCount = 0;
    g_liveMappings = 0;
    g_mapScriptCount = 0;
    g_mapScriptNext = 0;
    g_beginScriptCount = 0;
    g_beginScriptNext = 0;
    g_endScriptCount = 0;
    g_endScriptNext = 0;
    g_unmapScriptCount = 0;
    g_unmapScriptNext = 0;
    g_releaseScriptCount = 0;
    g_releaseScriptNext = 0;
    g_attachStatus = OS_EOK;
    g_allowAnswerWithMemory = false;
    for (i = 0; i < MAX_MESSAGES; i++) {
        free(g_messages[i]);
        g_messages[i] = NULL;
        g_attached[i] = false;
        g_answers[i] = 0;
        g_answerStatus[i] = OS_EOK;
        g_answerSectors[i] = 0;
    }
    g_currentId = 0;

    g_sendStatus = 0;
    g_statusScriptCount = 0;
    g_statusScriptNext = 0;
    g_statusDefault = GRACHT_MESSAGE_COMPLETED;
    g_filedResult = OS_EOK;
}

// A standard capacity card (byte addressed) with CSD timing: TAAC 0x15 is
// 1.2 * 100 us = 120 us, NSAC 10 is 1000 clocks = 40 us at 25 MHz. The read
// limit is 100 * 160 us = 16 ms, the write limit 8 times that, 128 ms.
static void
__MakeStandardCapacity(void)
{
    g_card.Addressing = SdCardAddressingBytes;
    g_card.SectorCount = 1ULL << 22;
    g_card.CardData.Structure = SD_CSD_VERSION_1;
    g_card.CardData.ReadAccessTime = 0x15U;
    g_card.CardData.ReadAccessClocks = 10U;
    g_card.CardData.WriteSpeedFactor = 3U;
}

// An eMMC device: MMC kind, CSD timing TAAC 0x27 (1.5 * 10 ms = 15 ms) and
// NSAC 1 (100 clocks, 4 us at 25 MHz), R2W_FACTOR 2. The read limit is
// 10 * 15004 us = 150040 us, the write limit 4 times that, 600160 us.
static void
__MakeMmc(
    _In_ enum SdCardAddressing addressing)
{
    g_card.Kind = SdCardKindMmc;
    g_card.Addressing = addressing;
    memset(&g_card.CardData, 0, sizeof(g_card.CardData));
    g_card.MmcCardData.Structure = 3U;
    g_card.MmcCardData.SpecVersion = 4U;
    g_card.MmcCardData.ReadAccessTime = 0x27U;
    g_card.MmcCardData.ReadAccessClocks = 1U;
    g_card.MmcCardData.WriteSpeedFactor = 2U;
    g_card.MmcIdentity.ManufacturerId = 0x15U;
    memcpy(g_card.MmcIdentity.ProductName, "8GTF4R", 7);
    g_card.MmcIdentity.SerialNumber = 0xA1B2C3D4U;
}

/////////////////////////////////////////////////////////////////////////////
// Sector I/O tests.

static SdSectorIo_t      g_io;
static int               g_ioDone;
static oserr_t           g_ioStatus;
static uint32_t          g_ioConfirmed;
static struct DMASegment g_segments[2];

static void
__IoDone(
    _In_ SdSectorIo_t* io,
    _In_ oserr_t       status,
    _In_ uint32_t      sectorsConfirmed,
    _In_ void*         context)
{
    assert(io == &g_io);
    assert(context == &g_io);
    g_ioDone++;
    g_ioStatus = status;
    g_ioConfirmed = sectorsConfirmed;
    __Event("io done status=%u sectors=%u", status, sectorsConfirmed);
}

static oserr_t
__StartIo(
    _In_ enum SdDataDirection direction,
    _In_ uint64_t             firstSector,
    _In_ uint32_t             sectorCount)
{
    memset(&g_io, 0, sizeof(g_io));
    g_ioDone = 0;
    g_segments[0].Address = 0x20000000ULL;
    g_segments[0].Length = (uint64_t)sectorCount * 512U;
    return SdSectorIoStart(&g_io, &g_card, direction, firstSector, sectorCount,
                           g_segments, 1, __IoDone, &g_io);
}

static void
TestMmcSectors(void)
{
    // A sector addressed eMMC device: the MMC card error mask, and limits
    // from its CSD with a multiplier of 10. Read: 150040 us + 2 * 164 us +
    // 20 ms. Write: 600160 us + 328 us + 20 ms.
    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    assert(__StartIo(SdDataRead, 100, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataWrite, 101, 1) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD17 arg=0x64 type=1 mask=0xfd390080 cmd=20000 data=170368 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1",
        "CMD24 arg=0x65 type=1 mask=0xfd390080 cmd=20000 data=620488 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390080 cmd=20000",
        "io done status=0 sectors=1"
    );
    assert(g_io.ProgramBudgetUs == 600160U);

    // A short TAAC (100 ns): a sector addressed device still gets the high
    // capacity SD limits, 100 ms and 500 ms.
    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    g_card.MmcCardData.ReadAccessTime = 0x0AU;
    g_card.MmcCardData.ReadAccessClocks = 0U;
    assert(__StartIo(SdDataRead, 100, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataWrite, 100, 1) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD17 arg=0x64 type=1 mask=0xfd390080 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1",
        "CMD24 arg=0x64 type=1 mask=0xfd390080 cmd=20000 data=520328 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390080 cmd=20000",
        "io done status=0 sectors=1"
    );

    // A byte addressed device keeps the CSD limits: 10 us to read and 40 us
    // to write one block, and byte offsets as arguments.
    __Reset();
    __MakeMmc(SdCardAddressingBytes);
    g_card.MmcCardData.ReadAccessTime = 0x0AU;
    g_card.MmcCardData.ReadAccessClocks = 0U;
    assert(__StartIo(SdDataRead, 3, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataWrite, 3, 1) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD17 arg=0x600 type=1 mask=0xfd390080 cmd=20000 data=20338 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1",
        "CMD24 arg=0x600 type=1 mask=0xfd390080 cmd=20000 data=20368 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390080 cmd=20000",
        "io done status=0 sectors=1"
    );

    // The largest R2W_FACTOR is 5, reserved values count as 5.
    __Reset();
    __MakeMmc(SdCardAddressingBytes);
    g_card.MmcCardData.WriteSpeedFactor = 7U;
    assert(__StartIo(SdDataWrite, 3, 1) == OS_EOK);
    __Run();
    assert(g_io.ProgramBudgetUs == 150040U * 32U);
    printf("eMMC sector I/O passed\n");
}

static void
TestReadSingleSector(void)
{
    __Reset();
    assert(__StartIo(SdDataRead, 100, 1) == OS_EOK);
    __Run();
    // Data timeout: 100 ms access + 2 * 164 us on the bus + 20 ms margin.
    EXPECT_EVENTS(
        "CMD17 arg=0x64 type=1 mask=0xfd390008 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1"
    );
    assert(g_ioDone == 1 && g_ioStatus == OS_EOK && g_ioConfirmed == 1);
}

static void
TestReadMultipleSectorsCmd12(void)
{
    __Reset();
    assert(__StartIo(SdDataRead, 0x12345, 8) == OS_EOK);
    __Run();
    // 8 * 100 ms + 2 * 1311 us + 20 ms.
    EXPECT_EVENTS(
        "CMD18 arg=0x12345 type=1 mask=0xfd390008 cmd=20000 data=822622 read stop=cmd12 blocks=8x512 segments=1",
        "io done status=0 sectors=8"
    );
}

static void
TestAutoCmd12Host(void)
{
    __Reset();
    g_host.Limits.Flags |= SD_HOST_FLAG_AUTO_CMD12_REQUIRED;
    assert(__StartIo(SdDataRead, 7, 2) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataWrite, 9, 2) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataRead, 11, 1) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD18 arg=0x7 type=1 mask=0xfd390008 cmd=20000 data=220656 read stop=auto blocks=2x512 segments=1",
        "io done status=0 sectors=2",
        "CMD25 arg=0x9 type=1 mask=0xfd390008 cmd=20000 data=1020656 write stop=auto blocks=2x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "io done status=0 sectors=2",
        "CMD17 arg=0xb type=1 mask=0xfd390008 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1"
    );
}

static void
TestWriteByteAddressed(void)
{
    __Reset();
    __MakeStandardCapacity();
    assert(__StartIo(SdDataWrite, 3, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataWrite, 5, 4) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataRead, 5, 2) == OS_EOK);
    __Run();
    // Byte offsets 3 * 512 and 5 * 512. Write: 128 ms + 2 * 164 us + 20 ms.
    // Four writes: 4 * 128 ms + 2 * 656 us + 20 ms. Two reads: 2 * 16 ms +
    // 2 * 328 us + 20 ms.
    EXPECT_EVENTS(
        "CMD24 arg=0x600 type=1 mask=0xfd390008 cmd=20000 data=148328 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "io done status=0 sectors=1",
        "CMD25 arg=0xa00 type=1 mask=0xfd390008 cmd=20000 data=533312 write stop=cmd12 blocks=4x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "io done status=0 sectors=4",
        "CMD18 arg=0xa00 type=1 mask=0xfd390008 cmd=20000 data=52656 read stop=cmd12 blocks=2x512 segments=1",
        "io done status=0 sectors=2"
    );
}

static void
TestLargeRunTimeoutCap(void)
{
    __Reset();
    // 2048 sectors: the summed access time is capped at 10 s, plus twice
    // the 335545 us that 1 MiB takes on one line, plus 20 ms.
    assert(__StartIo(SdDataRead, 0, 2048) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD18 arg=0x0 type=1 mask=0xfd390008 cmd=20000 data=10691090 read stop=cmd12 blocks=2048x512 segments=1",
        "io done status=0 sectors=2048"
    );
}

static void
TestRangeRejected(void)
{
    __Reset();
    // The last sector is 2^22 - 1. Nothing may reach the card.
    assert(__StartIo(SdDataRead, (1ULL << 22) - 1U, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataRead, (1ULL << 22) - 1U, 2) == OS_EINVALPARAMS);
    assert(__StartIo(SdDataRead, 1ULL << 22, 1) == OS_EINVALPARAMS);
    assert(__StartIo(SdDataRead, UINT64_MAX, 2) == OS_EINVALPARAMS);
    assert(__StartIo(SdDataRead, 0, 0) == OS_EINVALPARAMS);
    assert(__StartIo(SdDataNone, 0, 1) == OS_EINVALPARAMS);
    g_host.Limits.MaximumBlockCount = 16U;
    assert(__StartIo(SdDataRead, 0, 17) == OS_EINVALPARAMS);
    // Segments that do not add up to the run, or are misaligned.
    memset(&g_io, 0, sizeof(g_io));
    g_segments[0].Address = 0x20000000ULL;
    g_segments[0].Length = 512U;
    assert(SdSectorIoStart(&g_io, &g_card, SdDataRead, 0, 2, g_segments, 1, __IoDone, &g_io) == OS_EINVALPARAMS);
    g_segments[0].Address = 0x20000002ULL;
    assert(SdSectorIoStart(&g_io, &g_card, SdDataRead, 0, 1, g_segments, 1, __IoDone, &g_io) == OS_EINVALPARAMS);
    // A card that is not ready.
    g_card.State = SdCardStateFailed;
    assert(__StartIo(SdDataRead, 0, 1) == OS_EINVALPARAMS);
    EXPECT_EVENTS(
        "CMD17 arg=0x3fffff type=1 mask=0xfd390008 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1"
    );
}

static void
TestByteAddressLimit(void)
{
    __Reset();
    __MakeStandardCapacity();
    // A byte addressed card that reports more than 4 GiB: the last sector
    // whose byte offset fits in 32 bits works, the next one is refused.
    g_card.SectorCount = 1ULL << 24;
    assert(__StartIo(SdDataRead, 0x7FFFFFU, 1) == OS_EOK);
    __Run();
    assert(__StartIo(SdDataRead, 0x800000U, 1) == OS_EOVERFLOW);
    EXPECT_EVENTS(
        "CMD17 arg=0xfffffe00 type=1 mask=0xfd390008 cmd=20000 data=36328 read stop=none blocks=1x512 segments=1",
        "io done status=0 sectors=1"
    );
}

static void
TestWriteProtectedSectors(void)
{
    __Reset();
    g_card.WriteProtected = true;
    assert(__StartIo(SdDataWrite, 0, 1) == OS_EPERMISSIONS);
    assert(g_eventCount == 0);
}

static void
TestProgramWait(void)
{
    __Reset();
    // Busy twice, then ready: 1 ms between the CMD13 attempts.
    __PushResult(__Success(STATUS_TRANSFER, 1024));
    __PushResult(__Success(STATUS_PROGRAM, 0));
    __PushResult(__Success(STATUS_PROGRAM, 0));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    assert(__StartIo(SdDataWrite, 40, 2) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD25 arg=0x28 type=1 mask=0xfd390008 cmd=20000 data=1020656 write stop=cmd12 blocks=2x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "schedule 1000",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "schedule 1000",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "io done status=0 sectors=2"
    );
    assert(g_nowUs == 2000U);
}

static void
TestProgramDeadline(void)
{
    int i;

    __Reset();
    // A card that never leaves the programming state. The budget is the
    // 500 ms write limit of a high capacity card, counted in 1 ms delays:
    // the first CMD13 and 500 more, then a timeout.
    __PushResult(__Success(STATUS_TRANSFER, 512));
    for (i = 0; i < 501; i++) {
        __PushResult(__Success(STATUS_PROGRAM, 0));
    }
    assert(__StartIo(SdDataWrite, 1, 1) == OS_EOK);
    __Run();
    assert(__CountEvents("CMD13") == 501);
    assert(__CountEvents("schedule 1000") == 500);
    assert(g_nowUs == 500000U);
    assert(g_ioDone == 1 && g_ioStatus == OS_ETIMEOUT && g_ioConfirmed == 0);
}

static void
TestProgramErrorBit(void)
{
    SdResult_t status = __Success(STATUS_TRANSFER | SD_R1_ERROR, 0);

    __Reset();
    status.CardErrors = SD_R1_ERROR;
    __PushResult(__Success(STATUS_TRANSFER, 512));
    __PushResult(status);
    assert(__StartIo(SdDataWrite, 1, 1) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD24 arg=0x1 type=1 mask=0xfd390008 cmd=20000 data=520328 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "io done status=24 sectors=0"
    );
    assert(g_ioStatus == OS_EDEVFAULT);
}

static void
TestReadRetry(void)
{
    __Reset();
    // A CRC error, the card is back in the transfer state, the read works.
    __PushResult(__Failure(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, 0));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    assert(__StartIo(SdDataRead, 64, 4) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD18 arg=0x40 type=1 mask=0xfd390008 cmd=20000 data=421312 read stop=cmd12 blocks=4x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "CMD18 arg=0x40 type=1 mask=0xfd390008 cmd=20000 data=421312 read stop=cmd12 blocks=4x512 segments=1",
        "io done status=0 sectors=4"
    );
}

static void
TestReadRetryBudget(void)
{
    __Reset();
    // Three timeouts: the read is sent once and again twice. The last
    // attempt confirmed the first two sectors.
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 512));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 0));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 1024 + 100));
    assert(__StartIo(SdDataRead, 64, 4) == OS_EOK);
    __Run();
    assert(__CountEvents("CMD18") == 3);
    assert(__CountEvents("CMD13") == 2);
    assert(g_ioDone == 1 && g_ioStatus == OS_ETIMEOUT && g_ioConfirmed == 2);

    // A card that is not back in the transfer state ends the retries.
    __Reset();
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 0));
    __PushResult(__Success(5U << 9, 0));
    assert(__StartIo(SdDataRead, 64, 4) == OS_EOK);
    __Run();
    assert(__CountEvents("CMD18") == 1);
    assert(g_ioStatus == OS_ETIMEOUT && g_ioConfirmed == 0);

    // A card error in the read response is never retried and counts nothing.
    __Reset();
    {
        SdResult_t result = __Success(STATUS_TRANSFER | SD_R1_OUT_OF_RANGE, 2048);
        result.CardErrors = SD_R1_OUT_OF_RANGE;
        __PushResult(result);
    }
    assert(__StartIo(SdDataRead, 64, 4) == OS_EOK);
    __Run();
    assert(__CountEvents("CMD18") == 1 && __CountEvents("CMD13") == 0);
    assert(g_ioStatus == OS_EDEVFAULT && g_ioConfirmed == 0);
}

static void
TestWriteNeverRepeated(void)
{
    __Reset();
    // The host even reports a confirmed prefix: a failed write still counts nothing,
    // and it is not sent again.
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 1024));
    assert(__StartIo(SdDataWrite, 64, 4) == OS_EOK);
    __Run();
    EXPECT_EVENTS(
        "CMD25 arg=0x40 type=1 mask=0xfd390008 cmd=20000 data=2021312 write stop=cmd12 blocks=4x512 segments=1",
        "io done status=6 sectors=0"
    );
    assert(g_ioStatus == OS_ETIMEOUT);
}

static void
TestHardwareRunning(void)
{
    SdResult_t result = __Failure(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE, 1024);

    __Reset();
    result.HardwareStopped = false;
    __PushResult(result);
    assert(__StartIo(SdDataRead, 64, 4) == OS_EOK);
    __Run();
    assert(__CountEvents("CMD18") == 1 && __CountEvents("CMD13") == 0);
    assert(g_ioDone == 1 && g_ioStatus == SD_SEQUENCE_HARDWARE_RUNNING && g_ioConfirmed == 0);
}

static void
TestReadAccessDecode(void)
{
    // Factor 1.0 in 1 ns units, factor 1.2 in 100 us units, 8.0 in 10 ms
    // units, and the reserved factor.
    assert(SdCsdReadAccessNs(0x08U) == 1U);
    assert(SdCsdReadAccessNs(0x15U) == 120000U);
    assert(SdCsdReadAccessNs(0x7FU) == 80000000U);
    assert(SdCsdReadAccessNs(0x07U) == 0U);
}

/////////////////////////////////////////////////////////////////////////////
// Storage adapter tests.

static SDStorage_t* g_adapter;

static void
__CreateAdapter(
    _In_ bool appendGeneration)
{
    SDStorageParameters_t parameters = {
        .DriverId = DRIVER_ID,
        .DeviceId = DEVICE_ID,
        .MediaGeneration = g_card.MediaGeneration,
        .AppendGeneration = appendGeneration
    };

    g_adapter = NULL;
    assert(SDStorageCreate(&g_card, &parameters, &g_adapter) == OS_EOK);
    assert(g_adapter != NULL);
}

static void
__DestroyAdapter(void)
{
    SDStorageStop(g_adapter, OS_ECANCELLED);
    __Run();
    assert(SDStorageDestroy(g_adapter) == OS_EOK);
    g_adapter = NULL;
}

static void
__Transfer(
    _In_ int                         id,
    _In_ enum sys_transfer_direction direction,
    _In_ uint64_t                    firstSector,
    _In_ size_t                      sectorCount)
{
    g_currentId = id;
    SDStorageTransfer(g_adapter, __Message(id), direction, firstSector, sectorCount, &g_transfer);
}

static void
TestStorageReadOneChunk(void)
{
    __Reset();
    __CreateAdapter(false);
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 8);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=8 read",
        "schedule 0",
        "map off=4096 len=4096 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=4096 nonblocking",
        "CMD18 arg=0x64 type=1 mask=0xfd390008 cmd=20000 data=822622 read stop=cmd12 blocks=8x512 segments=1",
        "end 4096",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=8"
    );
    __DestroyAdapter();
}

static void
TestStorageWrite(void)
{
    __Reset();
    __CreateAdapter(false);
    __Transfer(1, SYS_TRANSFER_DIRECTION_WRITE, 7, 1);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=1 write",
        "schedule 0",
        "map off=4096 len=512 dir=to align=4/4 limits=1048576/131072/512",
        "begin off=0 len=512 nonblocking",
        "CMD24 arg=0x7 type=1 mask=0xfd390008 cmd=20000 data=520328 write stop=none blocks=1x512 segments=1",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "end 512",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=1"
    );
    __DestroyAdapter();
}

static void
TestStorageLargeRequest(void)
{
    __Reset();
    __CreateAdapter(false);
    // 4096 sectors (2 MiB): two chunks of the 1 MiB direct limit, with the
    // attachment kept until the second chunk is unmapped.
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 0, 4096);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=4096 read",
        "schedule 0",
        "map off=4096 len=1048576 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=1048576 nonblocking",
        "CMD18 arg=0x0 type=1 mask=0xfd390008 cmd=20000 data=10691090 read stop=cmd12 blocks=2048x512 segments=1",
        "end 1048576",
        "unmap",
        "map off=1052672 len=1048576 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=1048576 nonblocking",
        "CMD18 arg=0x800 type=1 mask=0xfd390008 cmd=20000 data=10691090 read stop=cmd12 blocks=2048x512 segments=1",
        "end 1048576",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=4096"
    );
    __DestroyAdapter();
}

static void
TestStorageShortMapping(void)
{
    __Reset();
    __CreateAdapter(false);
    // 512 sectors. The pages bounce, so each map returns 128 KiB, in three
    // segments. The command covers exactly the mapped length.
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 131072, 3 };
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 131072, 3 };
    __Transfer(1, SYS_TRANSFER_DIRECTION_WRITE, 1000, 512);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=512 write",
        "schedule 0",
        "map off=4096 len=262144 dir=to align=4/4 limits=1048576/131072/512",
        "begin off=0 len=131072 nonblocking",
        "CMD25 arg=0x3e8 type=1 mask=0xfd390008 cmd=20000 data=10103888 write stop=cmd12 blocks=256x512 segments=3",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "end 131072",
        "unmap",
        "map off=135168 len=131072 dir=to align=4/4 limits=1048576/131072/512",
        "begin off=0 len=131072 nonblocking",
        "CMD25 arg=0x4e8 type=1 mask=0xfd390008 cmd=20000 data=10103888 write stop=cmd12 blocks=256x512 segments=3",
        "CMD13 arg=0xb3680000 type=1 mask=0xfd390008 cmd=20000",
        "end 131072",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=512"
    );
    __DestroyAdapter();
}

static void
TestStorageFailures(void)
{
    SdResult_t cardError = __Success(STATUS_TRANSFER | SD_R1_ADDRESS_ERROR, 4096);

    // A card error in the first chunk: nothing counts, the transfer ends
    // with zero bytes.
    __Reset();
    __CreateAdapter(false);
    cardError.CardErrors = SD_R1_ADDRESS_ERROR;
    __PushResult(cardError);
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 8);
    __Run();
    assert(__FindEvent("end 0", 0) >= 0);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EDEVFAULT && g_answerSectors[1] == 0);
    __DestroyAdapter();

    // A write that fails in the second chunk: the first chunk counts, the
    // second counts nothing, and no third chunk is mapped.
    __Reset();
    __CreateAdapter(false);
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    __PushResult(__Success(STATUS_TRANSFER, 65536));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    __PushResult(__Failure(OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, 0));
    __Transfer(2, SYS_TRANSFER_DIRECTION_WRITE, 0, 512);
    __Run();
    assert(__CountEvents("CMD25") == 2);
    assert(__CountEvents("map") == 2);
    assert(__FindEvent("end 65536", 0) >= 0 && __FindEvent("end 0", 0) > __FindEvent("end 65536", 0));
    assert(g_answers[2] == 1 && g_answerStatus[2] == OS_EDEVFAULT && g_answerSectors[2] == 128);
    __DestroyAdapter();

    // A read that keeps timing out in the second chunk, with two sectors the
    // host confirmed in the last attempt: those count, and only those bytes
    // are handed back to the client.
    __Reset();
    __CreateAdapter(false);
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    __PushResult(__Success(STATUS_TRANSFER, 65536));
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 0));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 0));
    __PushResult(__Success(STATUS_TRANSFER, 0));
    __PushResult(__Failure(OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, 1024));
    __Transfer(3, SYS_TRANSFER_DIRECTION_READ, 0, 512);
    __Run();
    assert(__CountEvents("CMD18") == 4);
    assert(__FindEvent("end 1024", 0) >= 0);
    assert(g_answers[3] == 1 && g_answerStatus[3] == OS_ETIMEOUT && g_answerSectors[3] == 130);
    __DestroyAdapter();
}

static void
TestStoragePoolShortage(void)
{
    int first;
    int i;

    // Short three times, then the mapping works.
    __Reset();
    __CreateAdapter(false);
    for (i = 0; i < 3; i++) {
        g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOOM, 0, 1 };
    }
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=1 read",
        "schedule 0",
        "map off=4096 len=512 dir=from align=4/4 limits=1048576/131072/512",
        "schedule 10000",
        "map off=4096 len=512 dir=from align=4/4 limits=1048576/131072/512",
        "schedule 10000",
        "map off=4096 len=512 dir=from align=4/4 limits=1048576/131072/512",
        "schedule 10000",
        "map off=4096 len=512 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=512 nonblocking",
        "CMD17 arg=0x64 type=1 mask=0xfd390008 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "end 512",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=1"
    );
    assert(g_nowUs == 30000U);
    assert(__CountLogs("device pool short") == 1);
    assert(__CountLogs("device pool wait ended after 30 ms") == 1);
    __DestroyAdapter();

    // Short for good: the first attempt and 100 more, the last one 1 second
    // after the first, then the request fails with the mapping error.
    __Reset();
    __CreateAdapter(false);
    for (i = 0; i < 200; i++) {
        g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EBUFFER, 0, 1 };
    }
    __Transfer(1, SYS_TRANSFER_DIRECTION_WRITE, 100, 16);
    first = g_eventCount;
    __Run();
    (void)first;
    assert(__CountEvents("map") == 101);
    assert(__CountEvents("schedule 10000") == 100);
    assert(__CountEvents("begin") == 0);
    assert(g_nowUs == 1000000U);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EBUFFER && g_answerSectors[1] == 0);
    assert(__CountLogs("device pool short") == 1);
    assert(__CountLogs("still short after 1000 ms") == 1);
    __DestroyAdapter();

    // Any other mapping error fails at once.
    __Reset();
    __CreateAdapter(false);
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EPERMISSIONS, 0, 1 };
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    assert(__CountEvents("map") == 1 && __CountEvents("schedule 10000") == 0);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EPERMISSIONS);
    __DestroyAdapter();
}

static void
TestStorageBegin(void)
{
    int i;

    // Two conflicts, then the transfer begins.
    __Reset();
    __CreateAdapter(false);
    g_beginScript[g_beginScriptCount++] = OS_EBUSY;
    g_beginScript[g_beginScriptCount++] = OS_EBUSY;
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=1 read",
        "schedule 0",
        "map off=4096 len=512 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=512 nonblocking",
        "schedule 1000",
        "begin off=0 len=512 nonblocking",
        "schedule 1000",
        "begin off=0 len=512 nonblocking",
        "CMD17 arg=0x64 type=1 mask=0xfd390008 cmd=20000 data=120328 read stop=none blocks=1x512 segments=1",
        "end 512",
        "unmap",
        "release id=1",
        "answer id=1 status=0 sectors=1"
    );
    __DestroyAdapter();

    // A conflict that lasts: 1 second of 1 ms retries, then OS_ETIMEOUT. The
    // transfer never began, so it is only unmapped.
    __Reset();
    __CreateAdapter(false);
    for (i = 0; i < 1100; i++) {
        g_beginScript[g_beginScriptCount++] = OS_EBUSY;
    }
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    assert(__CountEvents("begin") == 1001);
    assert(g_nowUs == 1000000U);
    assert(__CountEvents("end") == 0 && __CountEvents("unmap") == 1);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_ETIMEOUT);
    __DestroyAdapter();

    // Any other begin error fails at once.
    __Reset();
    __CreateAdapter(false);
    g_beginScript[g_beginScriptCount++] = OS_EDEVFAULT;
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    assert(__CountEvents("begin") == 1 && __CountEvents("end") == 0 && __CountEvents("unmap") == 1);
    assert(__CountEvents("CMD") == 0);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EDEVFAULT);
    __DestroyAdapter();

    // The host refuses the chunk (for example a table too small): an
    // internal fault. The transfer ends with zero bytes and is unmapped.
    __Reset();
    __CreateAdapter(false);
    g_submitStatus = OS_EINVALPARAMS;
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 1);
    __Run();
    assert(__FindEvent("end 0", 0) >= 0 && __CountEvents("unmap") == 1);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EINVALPARAMS);
    assert(__CountLogs("internal fault") == 1);
    g_submitStatus = OS_EOK;
    __DestroyAdapter();
}

static void
TestStorageCleanupRetry(void)
{
    __Reset();
    __CreateAdapter(false);
    // DMAEndTransfer fails twice, DMAUnmap once and SHMRelease once. Each is
    // tried again after 10 ms until it works, and never after that.
    g_endScript[g_endScriptCount++] = OS_EUNKNOWN;
    g_endScript[g_endScriptCount++] = OS_EUNKNOWN;
    g_unmapScript[g_unmapScriptCount++] = OS_EBUSY;
    g_releaseScript[g_releaseScriptCount++] = OS_EUNKNOWN;
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 100, 2);
    __Run();
    EXPECT_EVENTS(
        "attach id=1 size=512 count=2 read",
        "schedule 0",
        "map off=4096 len=1024 dir=from align=4/4 limits=1048576/131072/512",
        "begin off=0 len=1024 nonblocking",
        "CMD18 arg=0x64 type=1 mask=0xfd390008 cmd=20000 data=220656 read stop=cmd12 blocks=2x512 segments=1",
        "end 1024",
        "schedule 10000",
        "end 1024",
        "schedule 10000",
        "end 1024",
        "unmap",
        "schedule 10000",
        "unmap",
        "release id=1",
        "schedule 10000",
        "release id=1",
        "answer id=1 status=0 sectors=2"
    );
    assert(g_nowUs == 40000U);
    __DestroyAdapter();
}

static void
TestStorageHardwareRunning(void)
{
    SdResult_t result = __Failure(OS_ETIMEOUT, SD_TRANSPORT_DEADLINE, 0);

    __Reset();
    __CreateAdapter(false);
    result.HardwareStopped = false;
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    __PushResult(__Success(STATUS_TRANSFER, 65536));
    __PushResult(result);
    g_allowAnswerWithMemory = true;
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 0, 512);
    __Transfer(2, SYS_TRANSFER_DIRECTION_READ, 0, 1);
    __Run();
    // The second chunk keeps its mapping and the attachment. The request is
    // answered once with the first chunk's sectors, the queued one fails.
    assert(__CountEvents("end") == 1 && __CountEvents("unmap") == 1);
    assert(g_liveMappings == 1 && g_attached[1]);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EDEVFAULT && g_answerSectors[1] == 128);
    assert(g_answers[2] == 1 && g_answerStatus[2] == OS_EDEVFAULT && g_answerSectors[2] == 0);
    assert(!g_attached[2]);

    // A new request is refused at once without attaching its buffer.
    __Transfer(3, SYS_TRANSFER_DIRECTION_READ, 0, 1);
    assert(g_answers[3] == 1 && g_answerStatus[3] == OS_EDEVFAULT);
    assert(__CountEvents("attach id=3") == 0);
    assert(SDStorageDestroy(g_adapter) == OS_EBUSY);
    __Run();
    assert(g_liveMappings == 1 && g_answers[1] == 1);
}

static void
TestStorageQueue(void)
{
    int i;

    __Reset();
    __CreateAdapter(false);
    // Three requests are served in arrival order, one at a time.
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 10, 1);
    __Transfer(2, SYS_TRANSFER_DIRECTION_WRITE, 20, 1);
    __Transfer(3, SYS_TRANSFER_DIRECTION_READ, 30, 1);
    __Run();
    assert(__FindEvent("CMD17 arg=0xa", 0) < __FindEvent("CMD24 arg=0x14", 0));
    assert(__FindEvent("CMD24 arg=0x14", 0) < __FindEvent("CMD17 arg=0x1e", 0));
    assert(__FindEvent("answer id=1", 0) < __FindEvent("map", __FindEvent("CMD17 arg=0xa", 0)));
    assert(__FindEvent("answer id=1", 0) < __FindEvent("answer id=2", 0));
    assert(__FindEvent("answer id=2", 0) < __FindEvent("answer id=3", 0));
    __DestroyAdapter();

    // 64 requests fit. The 65th is answered OS_EBUSY before its buffer is
    // attached. Once the queue drains, there is room again.
    __Reset();
    __CreateAdapter(false);
    for (i = 1; i <= 64; i++) {
        __Transfer(i, SYS_TRANSFER_DIRECTION_READ, (uint64_t)i, 1);
    }
    assert(__CountEvents("attach") == 64);
    __Transfer(65, SYS_TRANSFER_DIRECTION_READ, 65, 1);
    assert(__CountEvents("attach") == 64);
    assert(g_answers[65] == 1 && g_answerStatus[65] == OS_EBUSY && g_answerSectors[65] == 0);
    __Run();
    for (i = 1; i <= 64; i++) {
        assert(g_answers[i] == 1 && g_answerStatus[i] == OS_EOK && g_answerSectors[i] == 1);
    }
    __Transfer(66, SYS_TRANSFER_DIRECTION_READ, 66, 1);
    __Run();
    assert(g_answers[66] == 1 && g_answerStatus[66] == OS_EOK);
    __DestroyAdapter();
}

static void
TestStorageStop(void)
{
    int i;

    // Five requests. The first runs, then the adapter stops: the first
    // finishes its only chunk, the queued four fail with the cause.
    __Reset();
    __CreateAdapter(false);
    for (i = 1; i <= 5; i++) {
        __Transfer(i, SYS_TRANSFER_DIRECTION_READ, (uint64_t)i, 1);
    }
    g_holdPending = true;
    __Run();
    assert(g_pending != NULL && g_pending->Argument == 1U);
    SDStorageStop(g_adapter, OS_ENOENT);
    assert(SDStorageDestroy(g_adapter) == OS_EBUSY);
    g_holdPending = false;
    __Run();
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_EOK && g_answerSectors[1] == 1);
    for (i = 2; i <= 5; i++) {
        assert(g_answers[i] == 1 && g_answerStatus[i] == OS_ENOENT && g_answerSectors[i] == 0);
        assert(!g_attached[i]);
    }
    assert(__CountEvents("CMD") == 1);
    // A request after the stop is refused at once, and a second stop keeps
    // the first cause.
    SDStorageStop(g_adapter, OS_ECANCELLED);
    __Transfer(6, SYS_TRANSFER_DIRECTION_READ, 6, 1);
    assert(g_answers[6] == 1 && g_answerStatus[6] == OS_ENOENT);
    assert(__CountEvents("attach id=6") == 0);
    assert(SDStorageDestroy(g_adapter) == OS_EOK);

    // A stop while the first of two chunks runs: the second is never mapped.
    __Reset();
    __CreateAdapter(false);
    g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOK, 65536, 1 };
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 0, 256);
    g_holdPending = true;
    __Run();
    SDStorageStop(g_adapter, OS_ENOENT);
    g_holdPending = false;
    __Run();
    assert(__CountEvents("map") == 1);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_ENOENT && g_answerSectors[1] == 128);
    assert(SDStorageDestroy(g_adapter) == OS_EOK);

    // A stop during the device pool wait ends the wait with the cause.
    __Reset();
    __CreateAdapter(false);
    for (i = 0; i < 200; i++) {
        g_mapScript[g_mapScriptCount++] = (struct __MapScript){ OS_EOOM, 0, 1 };
    }
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 0, 1);
    // Arrival and the first two map attempts.
    __RunWorkItem();
    __RunWorkItem();
    SDStorageStop(g_adapter, OS_ENOENT);
    assert(__CountEvents("map") == 2);
    assert(__FindEvent("cancel work", 0) >= 0);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_ENOENT);
    assert(__CountLogs("wait ended by a stop") == 1);
    __Run();
    assert(__CountEvents("map") == 2);
    assert(SDStorageDestroy(g_adapter) == OS_EOK);

    // A stop during a buffer conflict unmaps the chunk.
    __Reset();
    __CreateAdapter(false);
    for (i = 0; i < 20; i++) {
        g_beginScript[g_beginScriptCount++] = OS_EBUSY;
    }
    __Transfer(1, SYS_TRANSFER_DIRECTION_READ, 0, 1);
    __RunWorkItem();
    SDStorageStop(g_adapter, OS_ECANCELLED);
    assert(__CountEvents("unmap") == 1 && __CountEvents("end") == 0);
    assert(g_answers[1] == 1 && g_answerStatus[1] == OS_ECANCELLED);
    assert(SDStorageDestroy(g_adapter) == OS_EOK);
}

static void
TestStorageAdmission(void)
{
    __Reset();
    __CreateAdapter(false);
    // Each refusal is answered at once without attaching the buffer.
    __Transfer(1, (enum sys_transfer_direction)7, 0, 1);
    __Transfer(2, SYS_TRANSFER_DIRECTION_READ, 0, 0);
    __Transfer(3, SYS_TRANSFER_DIRECTION_READ, (1ULL << 22) - 1U, 2);
    __Transfer(4, SYS_TRANSFER_DIRECTION_READ, UINT64_MAX, 1);
    __Transfer(5, SYS_TRANSFER_DIRECTION_READ, 0, (size_t)(UINT32_MAX / 512U) + 1U);
    __Transfer(6, SYS_TRANSFER_DIRECTION_READ, (1ULL << 22) - 1U, 1);
    assert(__CountEvents("attach") == 1);
    assert(g_answerStatus[1] == OS_EINVALPARAMS && g_answerStatus[2] == OS_EINVALPARAMS);
    assert(g_answerStatus[3] == OS_EINVALPARAMS && g_answerStatus[4] == OS_EINVALPARAMS);
    assert(g_answerStatus[5] == OS_EINVALPARAMS);
    __Run();
    assert(g_answers[6] == 1 && g_answerStatus[6] == OS_EOK);

    // An attach failure is answered as it is, and gives the place back.
    g_attachStatus = OS_EPERMISSIONS;
    __Transfer(7, SYS_TRANSFER_DIRECTION_WRITE, 0, 1);
    assert(g_answers[7] == 1 && g_answerStatus[7] == OS_EPERMISSIONS);
    g_attachStatus = OS_EOK;
    __DestroyAdapter();

    // A write protected card refuses writes and serves reads.
    __Reset();
    g_card.WriteProtected = true;
    __CreateAdapter(false);
    __Transfer(1, SYS_TRANSFER_DIRECTION_WRITE, 0, 1);
    __Transfer(2, SYS_TRANSFER_DIRECTION_READ, 0, 1);
    __Run();
    assert(g_answerStatus[1] == OS_EPERMISSIONS && __CountEvents("attach id=1") == 0);
    assert(g_answers[2] == 1 && g_answerStatus[2] == OS_EOK);
    __DestroyAdapter();
}

static void
__ExpectSerial(
    _In_ const char* product,
    _In_ uint8_t     manufacturer,
    _In_ uint32_t    serialNumber,
    _In_ bool        appendGeneration,
    _In_ const char* expected)
{
    char    serial[32];
    SdCid_t identity = { 0 };

    identity.ManufacturerId = manufacturer;
    memcpy(identity.ProductName, product, 5);
    identity.SerialNumber = serialNumber;
    assert(SDStorageFormatSerial(&identity, 4000000000U, appendGeneration, serial, sizeof(serial)) == OS_EOK);
    if (strcmp(serial, expected) != 0) {
        fprintf(stderr, "serial: expected \"%s\", got \"%s\"\n", expected, serial);
        assert(!"serial mismatch");
    }
}

static void
TestStorageStatAndSerial(void)
{
    StorageDescriptor_t descriptor;
    char                serial[8];
    SdCid_t             identity = { 0 };

    __Reset();
    g_card.WriteProtected = true;
    __CreateAdapter(false);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(descriptor.DeviceID == DEVICE_ID && descriptor.DriverID == DRIVER_ID);
    assert(descriptor.Flags == 0x2U);
    assert(descriptor.SectorSize == 512U && descriptor.SectorCount == (1ULL << 22));
    assert(descriptor.LUNCount == 1U);
    assert(strcmp(descriptor.Model, "SD card SU08G") == 0);
    assert(strcmp(descriptor.Serial, "SD03-SU08G-1A2B3C4D") == 0);
    __DestroyAdapter();

    // A duplicate gets the media generation appended.
    __Reset();
    __CreateAdapter(true);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(descriptor.Flags == 0U);
    assert(strcmp(descriptor.Serial, "SD03-SU08G-1A2B3C4D-G3") == 0);
    __DestroyAdapter();

    __ExpectSerial("SU08G", 0x03U, 0x1A2B3C4DU, false, "SD03-SU08G-1A2B3C4D");
    // Empty fields: zero bytes or spaces only.
    __ExpectSerial("\0\0\0\0\0", 0x00U, 0, false, "SD00-NONAME-00000000");
    __ExpectSerial("     ", 0x00U, 0, false, "SD00-NONAME-00000000");
    // Path-unsafe bytes are replaced, trailing spaces dropped.
    __ExpectSerial("a/. \xff", 0xFEU, 0xFFFFFFFFU, false, "SDFE-a____-FFFFFFFF");
    __ExpectSerial("ab/  ", 0x1BU, 0x00000001U, false, "SD1B-ab_-00000001");
    __ExpectSerial("x\0y\0\0", 0x1BU, 0x00000001U, false, "SD1B-x_y-00000001");
    // The longest serial still fits in 32 bytes.
    __ExpectSerial("ABCDE", 0xFFU, 0xFFFFFFFFU, true, "SDFF-ABCDE-FFFFFFFF-G4000000000");

    // The model replaces unprintable bytes.
    __Reset();
    memcpy(g_card.Identity.ProductName, "S\x01 x\0", 6);
    __CreateAdapter(false);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(strcmp(descriptor.Model, "SD card S? x") == 0);
    assert(strcmp(descriptor.Serial, "SD03-S__x-1A2B3C4D") == 0);
    __DestroyAdapter();

    // A buffer too small for the serial.
    assert(SDStorageFormatSerial(&identity, 0, false, serial, sizeof(serial)) == OS_EBUFFER);
}

static void
__ExpectMmcSerial(
    _In_ const char* product,
    _In_ uint8_t     manufacturer,
    _In_ uint32_t    serialNumber,
    _In_ bool        appendGeneration,
    _In_ const char* expected)
{
    char     serial[32];
    MmcCid_t identity = { 0 };

    identity.ManufacturerId = manufacturer;
    memcpy(identity.ProductName, product, 6);
    identity.SerialNumber = serialNumber;
    assert(SDStorageFormatMmcSerial(&identity, 4000000000U, appendGeneration, serial, sizeof(serial)) == OS_EOK);
    if (strcmp(serial, expected) != 0) {
        fprintf(stderr, "serial: expected \"%s\", got \"%s\"\n", expected, serial);
        assert(!"serial mismatch");
    }
}

static void
TestMmcDescriptor(void)
{
    StorageDescriptor_t descriptor;
    char                serial[8];
    MmcCid_t            identity = { 0 };

    // Model and serial come from the MMC CID. The SD CID is ignored.
    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    __CreateAdapter(false);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(strcmp(descriptor.Model, "eMMC 8GTF4R") == 0);
    assert(strcmp(descriptor.Serial, "M15-8GTF4R-A1B2C3D4") == 0);
    __DestroyAdapter();

    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    __CreateAdapter(true);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(strcmp(descriptor.Serial, "M15-8GTF4R-A1B2C3D4-G3") == 0);
    __DestroyAdapter();

    // Unprintable bytes in the model, path-unsafe ones in the serial.
    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    memcpy(g_card.MmcIdentity.ProductName, "e\x01/MC ", 7);
    __CreateAdapter(false);
    assert(SDStorageDescribe(g_adapter, &descriptor) == OS_EOK);
    assert(strcmp(descriptor.Model, "eMMC e?/MC") == 0);
    assert(strcmp(descriptor.Serial, "M15-e__MC-A1B2C3D4") == 0);
    __DestroyAdapter();

    __ExpectMmcSerial("\0\0\0\0\0\0", 0x00U, 0, false, "M00-NONAME-00000000");
    // The longest serial still fits in 32 bytes.
    __ExpectMmcSerial("ABCDEF", 0xFFU, 0xFFFFFFFFU, true, "MFF-ABCDEF-FFFFFFFF-G4000000000");
    assert(SDStorageFormatMmcSerial(&identity, 0, false, serial, sizeof(serial)) == OS_EBUFFER);
    assert(SDStorageFormatMmcSerial(NULL, 0, false, serial, sizeof(serial)) == OS_EINVALPARAMS);
    printf("eMMC model and serial passed\n");
}

static int     g_registrationDone;
static oserr_t g_registrationResult;

static void
__RegistrationDone(
    _In_ SDStorage_t* adapter,
    _In_ oserr_t      result,
    _In_ void*        context)
{
    assert(adapter == g_adapter);
    assert(context == &g_registrationDone);
    g_registrationDone++;
    g_registrationResult = result;
    __Event("registration done %u", result);
}

static void
TestStorageRegistration(void)
{
    int i;

    // filed answers after two looks: the result is read once and reported.
    __Reset();
    __CreateAdapter(false);
    g_registrationDone = 0;
    g_statusScript[g_statusScriptCount++] = GRACHT_MESSAGE_INPROGRESS;
    g_statusScript[g_statusScriptCount++] = GRACHT_MESSAGE_INPROGRESS;
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EBUSY);
    __Run();
    EXPECT_EVENTS(
        "register driver=41 device=5 flags=1",
        "schedule 10000",
        "status",
        "schedule 10000",
        "status",
        "schedule 10000",
        "status",
        "register result",
        "registration done 0"
    );
    assert(g_registrationDone == 1);

    // A rejection is passed on as filed sent it.
    g_eventCount = 0;
    g_filedResult = OS_EEXISTS;
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(g_registrationDone == 2 && g_registrationResult == OS_EEXISTS);

    // No answer within 5 seconds: the request is abandoned.
    g_eventCount = 0;
    g_statusDefault = GRACHT_MESSAGE_INPROGRESS;
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(__CountEvents("status") == 501 && __CountEvents("abandon") == 1);
    assert(__CountEvents("register result") == 0);
    assert(g_registrationDone == 3 && g_registrationResult == OS_ETIMEOUT);

    // A transport error is abandoned and reported as OS_EPROTOCOL.
    g_eventCount = 0;
    g_statusDefault = GRACHT_MESSAGE_ERROR;
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(__CountEvents("abandon") == 1);
    assert(g_registrationDone == 4 && g_registrationResult == OS_EPROTOCOL);

    // A request that cannot be sent reports at once and calls nothing.
    g_eventCount = 0;
    g_sendStatus = -1;
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EUNKNOWN);
    __Run();
    assert(g_registrationDone == 4 && g_eventCount == 1);
    g_sendStatus = 0;

    // Unregister passes OS_ENOENT on, and the forced flag.
    g_eventCount = 0;
    g_statusDefault = GRACHT_MESSAGE_COMPLETED;
    g_filedResult = OS_ENOENT;
    assert(SDStorageUnregister(g_adapter, true, __RegistrationDone, &g_registrationDone) == OS_EOK);
    assert(SDStorageDestroy(g_adapter) == OS_EBUSY);
    __Run();
    EXPECT_EVENTS(
        "unregister driver=41 device=5 forced=1",
        "schedule 10000",
        "status",
        "unregister result",
        "registration done 3"
    );
    assert(g_registrationDone == 5 && g_registrationResult == OS_ENOENT);
    for (i = 0; i < 1; i++) {
        __DestroyAdapter();
    }
}

static void
TestRemovableFlag(void)
{
    // The flag follows the host, not the kind of card: a removable host
    // registers an eMMC device as removable, a fixed host an SD card as
    // fixed.
    __Reset();
    __MakeMmc(SdCardAddressingSectors);
    __CreateAdapter(false);
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(__CountEvents("register driver=41 device=5 flags=1") == 1);
    __DestroyAdapter();

    __Reset();
    g_host.Limits.Flags = 0;
    __CreateAdapter(false);
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(__CountEvents("register driver=41 device=5 flags=0") == 1);
    __DestroyAdapter();

    __Reset();
    g_host.Limits.Flags = 0;
    __MakeMmc(SdCardAddressingSectors);
    __CreateAdapter(false);
    assert(SDStorageRegister(g_adapter, __RegistrationDone, &g_registrationDone) == OS_EOK);
    __Run();
    assert(__CountEvents("register driver=41 device=5 flags=0") == 1);
    __DestroyAdapter();
    printf("Removable flag by host passed\n");
}

int
main(void)
{
    TestReadAccessDecode();
    TestReadSingleSector();
    TestReadMultipleSectorsCmd12();
    TestAutoCmd12Host();
    TestWriteByteAddressed();
    TestLargeRunTimeoutCap();
    TestRangeRejected();
    TestByteAddressLimit();
    TestWriteProtectedSectors();
    TestProgramWait();
    TestProgramDeadline();
    TestProgramErrorBit();
    TestReadRetry();
    TestReadRetryBudget();
    TestWriteNeverRepeated();
    TestHardwareRunning();

    TestStorageReadOneChunk();
    TestStorageWrite();
    TestStorageLargeRequest();
    TestStorageShortMapping();
    TestStorageFailures();
    TestStoragePoolShortage();
    TestStorageBegin();
    TestStorageCleanupRetry();
    TestStorageHardwareRunning();
    TestStorageQueue();
    TestStorageStop();
    TestStorageAdmission();
    TestStorageStatAndSerial();
    TestStorageRegistration();
    TestMmcSectors();
    TestMmcDescriptor();
    TestRemovableFlag();
    __Reset();
    printf("SD sector I/O and storage adapter tests passed\n");
    return 0;
}
