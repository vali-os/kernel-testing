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
 * Host test for the MMC and eMMC setup. The real
 * modules/storage/sd/sequence.c, registers.c, card.c (for SdCardSetupCancel),
 * mmc_registers.c and mmc.c run against a scripted fake host, built the same
 * way as in sd_card_test.c:
 *
 * - Submit records the request and returns. The test answers it later and
 *   calls Complete from outside Submit, as the host contract requires.
 * - Schedule and CancelWork use a fake clock that moves only when the test
 *   runs the scheduled work item.
 * - SetBus and GetCardState record their arguments.
 * - DMABufferData and DMABufferSegments work on plain memory.
 *
 * Setup tests answer requests with a small model of an eMMC device: its OCR,
 * CID, CSD and a 512-byte EXT_CSD that CMD6 changes. Every test checks the
 * exact list of bus settings, delays and commands (with their arguments,
 * response formats, error masks, timeouts and data descriptions), not only
 * the final outcome.
 *
 * Expected values are written out as numbers here instead of being taken
 * from the production headers, so a changed constant fails a test.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddk/dma.h>
#include <ddk/utils.h>
#include "sd/mmc.h"

#define MAX_EVENTS 1024

// The values the setup must use, written out independently.
// SD_R1_ERROR_MASK without bit 3, with SWITCH_ERROR (bit 7).
#define R1_MASK            0xFD390080U
#define COMMAND_TIMEOUT_US 20000U
#define BUSY_TIMEOUT_US    500000U
#define EXT_CSD_TIMEOUT_US 1000000U
#define OCR_RETRY_US       10000U
#define CONTROL_ADDRESS    0x2000ULL
// The address the host gives the device, in bits 31:16 of the argument.
#define ADDRESS_ARGUMENT   0x00010000U
#define HOST_VOLTAGES      0x00300000U
// CMD1 argument: the host voltages and the sector access mode (bit 30).
#define OP_COND_ARGUMENT   0x40300000U
// GENERIC_CMD6_TIME of the default device: 25 units of 10 ms.
#define SWITCH_US          250000U

// EXT_CSD byte indices, written out independently.
#define EXT_CACHE_CTRL     33
#define EXT_PART_CONFIG    179
#define EXT_BUS_WIDTH      183
#define EXT_HS_TIMING      185
#define EXT_REV            192
#define EXT_STRUCTURE      194
#define EXT_CARD_TYPE      196
#define EXT_PART_TIME      199
#define EXT_SEC_COUNT      212
#define EXT_GENERIC_TIME   248

/////////////////////////////////////////////////////////////////////////////
// Logging.

static int  g_errorLogs;
static int  g_warningLogs;
static char g_lastError[256];
// The last line below WARNING.
static char g_lastNotice[256];

void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    char    text[256];
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errorLogs++;
        memcpy(g_lastError, text, sizeof(text));
    } else if (level == OSSYSLOGLEVEL_WARNING) {
        g_warningLogs++;
    } else {
        // NOTICE lines share the TRACE level, so this is the last line of
        // either kind. The ready line is the last one a setup writes.
        memcpy(g_lastNotice, text, sizeof(text));
    }
    if (getenv("SD_TEST_VERBOSE") != NULL) {
        fprintf(stderr, "log: %s\n", text);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Fake DMA buffer.

struct DMABuffer {
    uint8_t           Memory[4096];
    struct DMASegment Segments[2];
    uint32_t          SegmentCount;
};

static struct DMABuffer g_controlBuffer;

void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    return (void*)buffer->Memory;
}

const struct DMASegment*
DMABufferSegments(
    _In_  const struct DMABuffer* buffer,
    _Out_ uint32_t*               countOut)
{
    *countOut = buffer->SegmentCount;
    return buffer->Segments;
}

/////////////////////////////////////////////////////////////////////////////
// Fake host.

enum EventKind {
    EventBus,
    EventDelay,
    EventCommand,
    EventStop,
    EventCancelWork,
    EventCardState
};

struct Event {
    enum EventKind  Kind;
    SdBusSettings_t Bus;
    uint32_t        DelayUs;
    SdRequest_t     Request;
    uint64_t        SegmentAddress;
    uint64_t        SegmentLength;
};

static SdHost_t     g_host;
static struct Event g_events[MAX_EVENTS];
static int          g_eventCount;
static SdRequest_t* g_pending;
static SdWork_t*    g_scheduled;
static uint64_t     g_dueUs;
static uint64_t     g_nowUs;
static uint32_t     g_baseClockHz;
static int          g_setBusCount;
static int          g_setBusFailAt;
static oserr_t      g_setBusFailStatus;
static uint32_t     g_cardState;

static struct Event*
__Record(
    _In_ enum EventKind kind)
{
    struct Event* event;

    assert(g_eventCount < MAX_EVENTS);
    event = &g_events[g_eventCount++];
    memset(event, 0, sizeof(*event));
    event->Kind = kind;
    return event;
}

static oserr_t
__FakeSubmit(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    struct Event* event;

    // The contract: one request at a time, every request with a completion
    // and a command timeout, and a data timeout where data or busy follows.
    assert(host == &g_host);
    assert(g_pending == NULL);
    assert(request->Complete != NULL);
    assert(request->CommandTimeoutUs != 0);
    if (request->Direction != SdDataNone || request->ResponseType == SdResponseR1b) {
        assert(request->DataTimeoutUs != 0);
    }
    if (request->Direction != SdDataNone) {
        assert(request->Data.SegmentCount == 1);
        assert(request->Data.Length == (uint64_t)request->BlockSize * request->BlockCount);
        assert(request->Data.Segments[0].Length == request->Data.Length);
    }

    event = __Record(EventCommand);
    event->Request = *request;
    if (request->Data.Segments != NULL) {
        event->SegmentAddress = request->Data.Segments[0].Address;
        event->SegmentLength = request->Data.Segments[0].Length;
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
    struct Event* event;
    uint32_t      divider;

    assert(host == &g_host);
    assert(g_pending == NULL);
    event = __Record(EventBus);
    event->Bus = *settings;
    g_setBusCount++;
    if (g_setBusCount == g_setBusFailAt) {
        return g_setBusFailStatus;
    }
    if (settings->ClockHz == 0) {
        *clockHzOut = 0;
        return OS_EOK;
    }
    if (settings->ClockHz < host->Limits.MinimumClockHz || settings->ClockHz > host->Limits.MaximumClockHz) {
        return OS_ENOTSUPPORTED;
    }
    divider = (g_baseClockHz + settings->ClockHz - 1U) / settings->ClockHz;
    *clockHzOut = g_baseClockHz / divider;
    return OS_EOK;
}

static oserr_t
__FakeStop(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    assert(host == &g_host);
    assert(request == g_pending);
    __Record(EventStop);
    return OS_EOK;
}

static oserr_t
__FakeReset(
    _In_ struct SdHost* host)
{
    (void)host;
    assert(!"the card layer must not reset the host");
    return OS_EOK;
}

static oserr_t
__FakeGetCardState(
    _In_  struct SdHost* host,
    _Out_ uint32_t*      stateOut)
{
    assert(host == &g_host);
    __Record(EventCardState);
    *stateOut = g_cardState;
    return OS_EOK;
}

static oserr_t
__FakeSchedule(
    _In_ struct SdHost* host,
    _In_ struct SdWork* work,
    _In_ uint32_t       delayUs)
{
    struct Event* event;

    assert(host == &g_host);
    assert(work != NULL && work->Function != NULL);
    assert(g_scheduled == NULL);
    event = __Record(EventDelay);
    event->DelayUs = delayUs;
    g_scheduled = work;
    g_dueUs = g_nowUs + delayUs;
    return OS_EOK;
}

static oserr_t
__FakeCancelWork(
    _In_ struct SdHost* host,
    _In_ struct SdWork* work)
{
    assert(host == &g_host);
    __Record(EventCancelWork);
    if (work != g_scheduled) {
        return OS_ENOENT;
    }
    g_scheduled = NULL;
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

// A host with a 52 MHz base clock, wired for eight data lines, with high
// speed and a 3.2 to 3.4 V supply. 400 kHz, 26 MHz and 52 MHz are exact
// dividers of the base clock. It has no write protect switch.
static void
__ResetHost(void)
{
    memset(&g_host, 0, sizeof(g_host));
    memset(&g_controlBuffer, 0, sizeof(g_controlBuffer));
    g_host.Operations = &g_operations;
    g_host.Limits.MaximumClockHz = 52000000U;
    g_host.Limits.MinimumClockHz = 50000U;
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4 | SD_HOST_BUS_WIDTH_8;
    g_host.Limits.SignalVoltages = SD_HOST_SIGNAL_330;
    g_host.Limits.SupplyVoltages = HOST_VOLTAGES;
    g_host.Limits.MaximumBlockSize = 512;
    g_host.Limits.MaximumBlockCount = 65535;
    g_host.Limits.Flags = SD_HOST_FLAG_HIGH_SPEED;
    g_host.ControlBuffer = &g_controlBuffer;
    g_controlBuffer.Segments[0].Address = CONTROL_ADDRESS;
    g_controlBuffer.Segments[0].Length = sizeof(g_controlBuffer.Memory);
    g_controlBuffer.SegmentCount = 1;

    g_eventCount = 0;
    g_pending = NULL;
    g_scheduled = NULL;
    g_nowUs = 0;
    g_baseClockHz = 52000000U;
    g_setBusCount = 0;
    g_setBusFailAt = 0;
    g_cardState = SD_CARD_STATE_PRESENT;
    g_errorLogs = 0;
    g_warningLogs = 0;
    g_lastError[0] = '\0';
    g_lastNotice[0] = '\0';
}

static void
__Complete(
    _In_ const SdResult_t* result)
{
    SdRequest_t* request = g_pending;

    assert(request != NULL);
    g_pending = NULL;
    request->Result = *result;
    request->Complete(request);
}

static void
__RunScheduled(void)
{
    SdWork_t* work = g_scheduled;

    assert(work != NULL);
    g_scheduled = NULL;
    g_nowUs = g_dueUs;
    work->Function(work);
}

static SdResult_t
__Success(
    _In_ uint32_t response)
{
    SdResult_t result = { 0 };

    result.Status = OS_EOK;
    result.Response[0] = response;
    result.HardwareStopped = true;
    return result;
}

/////////////////////////////////////////////////////////////////////////////
// Expected events.

struct Expect {
    enum EventKind      Kind;
    uint8_t             Opcode;
    uint32_t            Argument;
    enum SdResponseType Type;
    // R1b busy timeout, or the data timeout of a read.
    uint32_t            DataTimeoutUs;
    uint32_t            DelayUs;
    uint32_t            ClockHz;
    enum SdBusPower     Power;
    enum SdBusWidth     Width;
    enum SdBusTiming    Timing;
};

static struct Expect g_expected[MAX_EVENTS];
static int           g_expectedCount;

static struct Expect*
__Expect(
    _In_ enum EventKind kind)
{
    struct Expect* expect;

    assert(g_expectedCount < MAX_EVENTS);
    expect = &g_expected[g_expectedCount++];
    memset(expect, 0, sizeof(*expect));
    expect->Kind = kind;
    return expect;
}

static void
__ExpectBus(
    _In_ uint32_t         clockHz,
    _In_ enum SdBusWidth  width,
    _In_ enum SdBusTiming timing,
    _In_ enum SdBusPower  power)
{
    struct Expect* expect = __Expect(EventBus);

    expect->ClockHz = clockHz;
    expect->Width = width;
    expect->Timing = timing;
    expect->Power = power;
}

static void
__ExpectDelay(
    _In_ uint32_t delayUs)
{
    __Expect(EventDelay)->DelayUs = delayUs;
}

static void
__ExpectCommand(
    _In_ uint8_t             opcode,
    _In_ uint32_t            argument,
    _In_ enum SdResponseType type)
{
    struct Expect* expect = __Expect(EventCommand);

    expect->Opcode = opcode;
    expect->Argument = argument;
    expect->Type = type;
    if (opcode == 7) {
        expect->DataTimeoutUs = BUSY_TIMEOUT_US;
    } else if (opcode == 8) {
        expect->DataTimeoutUs = EXT_CSD_TIMEOUT_US;
    }
}

// CMD6 SWITCH in write byte mode with the normal command set, R1b.
static void
__ExpectSwitch(
    _In_ uint8_t  index,
    _In_ uint8_t  value,
    _In_ uint32_t timeoutUs)
{
    struct Expect* expect = __Expect(EventCommand);

    expect->Opcode = 6;
    expect->Argument = 0x03000000U | ((uint32_t)index << 16) | ((uint32_t)value << 8) | 0x1U;
    expect->Type = SdResponseR1b;
    expect->DataTimeoutUs = timeoutUs;
}

static void
__ExpectStatus(void)
{
    __ExpectCommand(13, ADDRESS_ARGUMENT, SdResponseR1);
}

// The bus setup and CMD0. The SD setup that ran before has power cycled
// the device, so there is no power off and no wait: one SetBus with power
// on, 400 kHz, one data line and default timing, then CMD0 at once.
static void
__ExpectBusSetup(void)
{
    g_expectedCount = 0;
    __ExpectBus(400000, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn);
    __ExpectCommand(0, 0, SdResponseNone);
}

// Everything from CMD1 to the first EXT_CSD read for a device that is ready
// at once.
static void
__ExpectIdentification(
    _In_ bool blockLength)
{
    __ExpectCommand(1, OP_COND_ARGUMENT, SdResponseR3);
    __ExpectCommand(2, 0, SdResponseR2);
    __ExpectCommand(3, ADDRESS_ARGUMENT, SdResponseR1);
    __ExpectCommand(9, ADDRESS_ARGUMENT, SdResponseR2);
    __ExpectCommand(7, ADDRESS_ARGUMENT, SdResponseR1b);
    if (blockLength) {
        __ExpectCommand(16, 512, SdResponseR1);
    }
    __ExpectCommand(8, 0, SdResponseR1);
}

// The switch to wider lines: CMD6 of BUS_WIDTH, CMD13, SetBus, CMD8 again.
static void
__ExpectWidth(
    _In_ enum SdBusWidth width)
{
    __ExpectSwitch(EXT_BUS_WIDTH, width == SdBusWidth8 ? 2 : 1, SWITCH_US);
    __ExpectStatus();
    __ExpectBus(400000, width, SdBusTimingDefault, SdBusPowerOn);
    __ExpectCommand(8, 0, SdResponseR1);
}

// The switch to high speed: CMD6 of HS_TIMING, CMD13, SetBus.
static void
__ExpectHighSpeed(
    _In_ enum SdBusWidth width)
{
    __ExpectSwitch(EXT_HS_TIMING, 1, SWITCH_US);
    __ExpectStatus();
    __ExpectBus(400000, width, SdBusTimingHighSpeed, SdBusPowerOn);
}

// The final CMD13 and the data clock.
static void
__ExpectFinish(
    _In_ uint32_t         clockHz,
    _In_ enum SdBusWidth  width,
    _In_ enum SdBusTiming timing)
{
    __ExpectStatus();
    __ExpectBus(clockHz, width, timing, SdBusPowerOn);
}

static void
__CheckCommandDetails(
    _In_ const struct Event*  event,
    _In_ const struct Expect* expect)
{
    const SdRequest_t* request = &event->Request;
    bool               r1 = request->ResponseType == SdResponseR1 || request->ResponseType == SdResponseR1b;

    assert(request->CardErrorMask == (r1 ? R1_MASK : 0U));
    assert(request->CommandTimeoutUs == COMMAND_TIMEOUT_US);
    assert(request->StopPolicy == SdStopNone);
    if (request->Direction != SdDataNone) {
        // Only CMD8 SEND_EXT_CSD moves data: 512 bytes into the control
        // buffer.
        assert(request->Opcode == 8);
        assert(request->Direction == SdDataRead);
        assert(request->BlockSize == 512 && request->BlockCount == 1);
        assert(request->Data.Length == 512);
        assert(event->SegmentAddress == CONTROL_ADDRESS && event->SegmentLength == 512);
        assert(request->DataTimeoutUs == expect->DataTimeoutUs);
        return;
    }
    assert(request->Opcode != 8);
    if (request->DataTimeoutUs != expect->DataTimeoutUs) {
        fprintf(stderr, "CMD%u: data timeout %u, expected %u\n", request->Opcode, request->DataTimeoutUs,
                expect->DataTimeoutUs);
    }
    assert(request->DataTimeoutUs == expect->DataTimeoutUs);
    assert(request->BlockSize == 0 && request->BlockCount == 0);
    assert(request->Data.Segments == NULL && request->Data.Length == 0);
}

static void
__CheckEvents(void)
{
    int i;

    for (i = 0; i < g_expectedCount && i < g_eventCount; i++) {
        const struct Expect* expect = &g_expected[i];
        const struct Event*  event = &g_events[i];

        if (event->Kind != expect->Kind) {
            fprintf(stderr, "event %d: kind %d, expected %d\n", i, event->Kind, expect->Kind);
        }
        assert(event->Kind == expect->Kind);
        switch (expect->Kind) {
            case EventBus:
                if (event->Bus.ClockHz != expect->ClockHz || event->Bus.Width != expect->Width ||
                    event->Bus.Timing != expect->Timing) {
                    fprintf(stderr, "event %d: bus %u Hz, width %u, timing %u, expected %u Hz, width %u, timing %u\n",
                            i, event->Bus.ClockHz, event->Bus.Width, event->Bus.Timing,
                            expect->ClockHz, expect->Width, expect->Timing);
                }
                assert(event->Bus.ClockHz == expect->ClockHz);
                assert(event->Bus.Power == expect->Power);
                assert(event->Bus.Width == expect->Width);
                assert(event->Bus.Timing == expect->Timing);
                break;
            case EventDelay:
                assert(event->DelayUs == expect->DelayUs);
                break;
            case EventCommand:
                if (event->Request.Opcode != expect->Opcode || event->Request.Argument != expect->Argument) {
                    fprintf(stderr, "event %d: CMD%u 0x%08x, expected CMD%u 0x%08x\n", i,
                            event->Request.Opcode, event->Request.Argument, expect->Opcode, expect->Argument);
                }
                assert(event->Request.Opcode == expect->Opcode);
                assert(event->Request.Argument == expect->Argument);
                assert(event->Request.ResponseType == expect->Type);
                __CheckCommandDetails(event, expect);
                break;
            default:
                break;
        }
    }
    if (g_eventCount != g_expectedCount) {
        fprintf(stderr, "%d events, expected %d\n", g_eventCount, g_expectedCount);
    }
    assert(g_eventCount == g_expectedCount);
}

static int
__CountCommands(
    _In_ uint8_t opcode)
{
    int count = 0;
    int i;

    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventCommand && g_events[i].Request.Opcode == opcode) {
            count++;
        }
    }
    return count;
}

/////////////////////////////////////////////////////////////////////////////
// Register vectors.

// Sets a field in a normalized R2 response, independently of SdResponseBits.
static void
__PutBits(
    _InOut_ uint32_t     words[4],
    _In_    unsigned int start,
    _In_    unsigned int width,
    _In_    uint32_t     value)
{
    unsigned int i;

    for (i = 0; i < width; i++) {
        unsigned int bit = start + i;
        uint32_t     mask = 1U << (bit % 32U);

        if ((value >> i) & 1U) {
            words[3 - bit / 32U] |= mask;
        } else {
            words[3 - bit / 32U] &= ~mask;
        }
    }
}

// CID: MID 0x15, CBX 1 (BGA), OID 0x01, PNM "8GTF4R", PRV 0x10, PSN
// 0xA1B2C3D4, month 6, year field 3.
static void
__BuildCid(
    _Out_ uint32_t words[4])
{
    const char*  name = "8GTF4R";
    unsigned int i;

    memset(words, 0, 4 * sizeof(uint32_t));
    __PutBits(words, 120, 8, 0x15);
    __PutBits(words, 112, 2, 1);
    __PutBits(words, 104, 8, 0x01);
    for (i = 0; i < 6; i++) {
        __PutBits(words, 96 - 8 * i, 8, (uint8_t)name[i]);
    }
    __PutBits(words, 48, 8, 0x10);
    __PutBits(words, 16, 32, 0xA1B2C3D4U);
    __PutBits(words, 12, 4, 6);
    __PutBits(words, 8, 4, 3);
}

// CSD: structure 3, SPEC_VERS 4, TAAC 0x27 (1.5 times 10 ms), NSAC 1,
// TRAN_SPEED 0x32, CCC 0x8F5, READ_BL_LEN 9, C_SIZE 4095, C_SIZE_MULT 7,
// R2W_FACTOR 2. As a byte addressed device that is 4096 * 512 blocks of
// 512 bytes, 1 GiB or 2,097,152 sectors.
static void
__BuildCsd(
    _Out_ uint32_t words[4])
{
    memset(words, 0, 4 * sizeof(uint32_t));
    __PutBits(words, 126, 2, 3);
    __PutBits(words, 122, 4, 4);
    __PutBits(words, 112, 8, 0x27);
    __PutBits(words, 104, 8, 0x01);
    __PutBits(words, 96, 8, 0x32);
    __PutBits(words, 84, 12, 0x8F5);
    __PutBits(words, 80, 4, 9);
    __PutBits(words, 62, 12, 4095);
    __PutBits(words, 47, 3, 7);
    __PutBits(words, 26, 3, 2);
}

// EXT_CSD of an eMMC 5.1 device: revision 8, structure 2, high speed at 26
// and 52 MHz, 15,269,888 sectors, GENERIC_CMD6_TIME 25 (250 ms),
// PARTITION_SWITCH_TIME 10 (100 ms), and some read only fields that the
// comparison after the width switch looks at.
static void
__BuildExtCsd(
    _Out_ uint8_t data[512])
{
    memset(data, 0, 512);
    data[EXT_REV] = 8;
    data[EXT_STRUCTURE] = 2;
    data[EXT_CARD_TYPE] = 0x57;
    data[EXT_PART_TIME] = 10;
    data[EXT_SEC_COUNT + 0] = 0x00;
    data[EXT_SEC_COUNT + 1] = 0x00;
    data[EXT_SEC_COUNT + 2] = 0xE9;
    data[EXT_SEC_COUNT + 3] = 0x00;
    data[EXT_GENERIC_TIME] = 25;
    data[160] = 0x07;
    data[224] = 0x01;
    data[229] = 0x11;
    data[232] = 0x02;
    data[249] = 0x00;
    data[250] = 0x04;
}

/////////////////////////////////////////////////////////////////////////////
// Device model.

struct DeviceModel {
    // An empty slot answers nothing but CMD0.
    bool     Present;
    // CMD1 answers with READY clear this many times first. -1 is forever.
    int      BusyAnswers;
    // Access mode bits 30:29 of the OCR once ready.
    uint32_t AccessMode;
    uint32_t Cid[4];
    uint32_t Csd[4];
    uint8_t  ExtCsd[512];
    // EXT_CSD byte whose CMD6 the device refuses with SWITCH_ERROR, or -1.
    int      RefuseIndex;
    // After a CMD6 of this EXT_CSD byte the device stays in the programming
    // state, or -1.
    int      ProgramAfterIndex;
    // EXT_CSD byte that reads back changed once the bus is wider than one
    // line, or -1.
    int      CorruptWideByte;

    // Fault injection by 1-based command number.
    int      FaultAt;
    oserr_t  FaultStatus;
    uint32_t FaultTransport;
    int      CardErrorAt;
    uint32_t CardErrorBits;

    // State while running.
    int      CommandNumber;
    uint32_t State;
    bool     SwitchErrorPending;
    bool     ProgramPending;
};

static void
__ModelDevice(
    _Out_ struct DeviceModel* model)
{
    memset(model, 0, sizeof(*model));
    model->Present = true;
    model->AccessMode = 2U << 29;
    __BuildCid(model->Cid);
    __BuildCsd(model->Csd);
    __BuildExtCsd(model->ExtCsd);
    model->RefuseIndex = -1;
    model->ProgramAfterIndex = -1;
    model->CorruptWideByte = -1;
}

static uint32_t
__Status(
    _In_ const struct DeviceModel* model)
{
    return (model->State << 9) | MMC_R1_READY_FOR_DATA;
}

// CMD8: the EXT_CSD into the control buffer.
static SdResult_t
__AnswerExtCsd(
    _InOut_ struct DeviceModel* model,
    _In_    const SdRequest_t*  request)
{
    uint64_t   offset = request->Data.Segments[0].Address - CONTROL_ADDRESS;
    SdResult_t result;

    assert(request->Direction == SdDataRead);
    assert(offset + 512 <= sizeof(g_controlBuffer.Memory));
    memcpy(&g_controlBuffer.Memory[offset], model->ExtCsd, 512);
    if (model->CorruptWideByte >= 0 && model->ExtCsd[EXT_BUS_WIDTH] != 0) {
        g_controlBuffer.Memory[offset + (uint64_t)model->CorruptWideByte] ^= 0x5A;
    }
    result = __Success(__Status(model));
    result.ConfirmedBytes = 512;
    return result;
}

// CMD6: write one EXT_CSD byte, or refuse with SWITCH_ERROR in the next
// status.
static SdResult_t
__AnswerSwitch(
    _InOut_ struct DeviceModel* model,
    _In_    const SdRequest_t*  request)
{
    uint32_t index = (request->Argument >> 16) & 0xFFU;
    uint32_t value = (request->Argument >> 8) & 0xFFU;

    assert((request->Argument >> 24) == 0x3U);
    assert((request->Argument & 0x7U) == 0x1U);
    assert(index == EXT_CACHE_CTRL || index == EXT_PART_CONFIG || index == EXT_BUS_WIDTH ||
           index == EXT_HS_TIMING);
    if ((int)index == model->RefuseIndex) {
        model->SwitchErrorPending = true;
    } else {
        model->ExtCsd[index] = (uint8_t)value;
    }
    if ((int)index == model->ProgramAfterIndex) {
        model->ProgramPending = true;
    }
    return __Success(__Status(model));
}

static SdResult_t
__AnswerByOpcode(
    _InOut_ struct DeviceModel* model,
    _In_    const SdRequest_t*  request)
{
    SdResult_t result;
    uint32_t   status;

    switch (request->Opcode) {
        case 0:
            model->State = 0;
            return __Success(0);
        case 1:
            assert(model->State == 0 || model->State == 1);
            if (model->BusyAnswers != 0) {
                if (model->BusyAnswers > 0) {
                    model->BusyAnswers--;
                }
                return __Success(0x00FF8080U);
            }
            model->State = 1;
            return __Success(0x80FF8080U | model->AccessMode);
        case 2:
            result = __Success(0);
            memcpy(result.Response, model->Cid, sizeof(model->Cid));
            model->State = 2;
            return result;
        case 3:
            assert(request->Argument == ADDRESS_ARGUMENT);
            result = __Success(model->State << 9);
            model->State = 3;
            return result;
        case 9:
            assert(request->Argument == ADDRESS_ARGUMENT);
            result = __Success(0);
            memcpy(result.Response, model->Csd, sizeof(model->Csd));
            return result;
        case 7:
            result = __Success(model->State << 9);
            model->State = 4;
            return result;
        case 16:
            assert(request->Argument == 512);
            return __Success(__Status(model));
        case 8:
            return __AnswerExtCsd(model, request);
        case 6:
            return __AnswerSwitch(model, request);
        case 13:
            assert(request->Argument == ADDRESS_ARGUMENT);
            status = __Status(model);
            if (model->SwitchErrorPending) {
                status |= MMC_R1_SWITCH_ERROR;
                model->SwitchErrorPending = false;
            }
            if (model->ProgramPending) {
                status = 7U << 9;
                model->ProgramPending = false;
            }
            return __Success(status);
        default:
            fprintf(stderr, "unexpected CMD%u\n", request->Opcode);
            assert(!"unexpected command");
            return __Success(0);
    }
}

static SdResult_t
__Answer(
    _InOut_ struct DeviceModel* model,
    _In_    const SdRequest_t*  request)
{
    SdResult_t result = { 0 };

    model->CommandNumber++;
    if (model->CommandNumber == model->FaultAt) {
        result.Status = model->FaultStatus;
        result.TransportErrors = model->FaultTransport;
        result.HardwareStopped = true;
        return result;
    }
    if (!model->Present) {
        if (request->ResponseType == SdResponseNone) {
            return __Success(0);
        }
        result.Status = OS_ETIMEOUT;
        result.TransportErrors = SD_TRANSPORT_COMMAND_TIMEOUT;
        result.HardwareStopped = true;
        return result;
    }
    result = __AnswerByOpcode(model, request);
    if (model->CommandNumber == model->CardErrorAt) {
        result.Response[0] |= model->CardErrorBits;
    }
    if (request->ResponseType == SdResponseR1 || request->ResponseType == SdResponseR1b) {
        result.CardErrors = result.Response[0] & request->CardErrorMask;
    }
    return result;
}

static void
__RunDevice(
    _InOut_ struct DeviceModel* model)
{
    int guard;

    for (guard = 0; guard < 10000; guard++) {
        if (g_pending != NULL) {
            SdResult_t result = __Answer(model, g_pending);
            __Complete(&result);
            continue;
        }
        if (g_scheduled != NULL) {
            __RunScheduled();
            continue;
        }
        return;
    }
    assert(!"the sequence did not end");
}

/////////////////////////////////////////////////////////////////////////////
// Setup helpers.

// Setup options with every value zero: the normal setup.
static const SdCardSetupOptions_t g_noOptions = { 0 };

static SdCard_t               g_card;
static int                    g_setupDoneCount;
static enum SdCardSetupResult g_setupResult;
static oserr_t                g_setupStatus;
static int                    g_setupContext;

static void
__SetupDone(
    _In_ SdCard_t*              card,
    _In_ enum SdCardSetupResult result,
    _In_ oserr_t                status,
    _In_ void*                  context)
{
    assert(card == &g_card);
    assert(context == &g_setupContext);
    assert(g_setupDoneCount == 0);
    g_setupDoneCount++;
    g_setupResult = result;
    g_setupStatus = status;
}

static void
__StartSetup(
    _In_ const SdCardSetupOptions_t* options)
{
    g_setupDoneCount = 0;
    g_setupResult = (enum SdCardSetupResult)-1;
    g_setupStatus = OS_EUNKNOWN;
    assert(SdMmcSetupWithOptions(&g_card, &g_host, options, __SetupDone, &g_setupContext) == OS_EOK);
}

static void
__SetupWithOptions(
    _InOut_ struct DeviceModel*         model,
    _In_    const SdCardSetupOptions_t* options)
{
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup(options);
    __RunDevice(model);
    assert(g_setupDoneCount == 1);
    assert(g_pending == NULL && g_scheduled == NULL);
}

static void
__Setup(
    _InOut_ struct DeviceModel* model)
{
    __SetupWithOptions(model, &g_noOptions);
}

static void
__ExpectReady(void)
{
    if (g_setupResult != SdCardSetupReady) {
        fprintf(stderr, "setup result %d (%u), last error \"%s\"\n", (int)g_setupResult, g_setupStatus, g_lastError);
    }
    assert(g_setupResult == SdCardSetupReady && g_setupStatus == OS_EOK);
    assert(g_card.State == SdCardStateReady);
    assert(g_card.Kind == SdCardKindMmc);
    assert(g_errorLogs == 0);
}

static void
__ExpectSetupResult(
    _In_ enum SdCardSetupResult result,
    _In_ oserr_t                status,
    _In_ const char*            stepName)
{
    if (g_setupResult != result || g_setupStatus != status) {
        fprintf(stderr, "setup result %d (%u), expected %d (%u), after \"%s\"\n", (int)g_setupResult,
                g_setupStatus, (int)result, status, g_lastError);
    }
    assert(g_setupResult == result);
    assert(g_setupStatus == status);
    assert(g_card.State == SdCardStateFailed);
    assert(g_errorLogs == 1);
    if (strstr(g_lastError, stepName) == NULL) {
        fprintf(stderr, "error log \"%s\" does not name \"%s\"\n", g_lastError, stepName);
    }
    assert(strstr(g_lastError, stepName) != NULL);
}

// The full event list of a sector addressed device on the default host:
// eight lines, high speed at 52 MHz.
static void
__ExpectFullSetup(void)
{
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(52000000, SdBusWidth8, SdBusTimingHighSpeed);
}

/////////////////////////////////////////////////////////////////////////////
// Register tests.

static void
TestRegisters(void)
{
    uint32_t    words[4];
    uint8_t     extCsd[512];
    uint8_t     other[512];
    MmcCid_t    cid;
    MmcCsd_t    csd;
    MmcExtCsd_t decoded;

    __BuildCid(words);
    assert(MmcCidDecode(words, &cid) == OS_EOK);
    assert(cid.ManufacturerId == 0x15 && cid.DeviceType == 1 && cid.OemId == 0x01);
    assert(strcmp(cid.ProductName, "8GTF4R") == 0);
    assert(cid.ProductRevision == 0x10 && cid.SerialNumber == 0xA1B2C3D4U);
    assert(cid.ManufactureMonth == 6 && cid.ManufactureYear == 2000);
    // From EXT_CSD revision 5 on, the years before 2010 move on by 16.
    MmcCidYearForRevision(&cid, 4);
    assert(cid.ManufactureYear == 2000);
    MmcCidYearForRevision(&cid, 5);
    assert(cid.ManufactureYear == 2016);
    MmcCidYearForRevision(&cid, 8);
    assert(cid.ManufactureYear == 2016);
    assert(MmcCidDecode(NULL, &cid) == OS_EINVALPARAMS);

    __BuildCsd(words);
    assert(MmcCsdDecode(words, &csd) == OS_EOK);
    assert(csd.Structure == 3 && csd.SpecVersion == 4);
    assert(csd.ReadAccessTime == 0x27 && csd.ReadAccessClocks == 1 && csd.TransferSpeed == 0x32);
    assert(csd.CommandClasses == 0x8F5 && csd.ReadBlockLengthLog2 == 9);
    assert(csd.DeviceSize == 4095 && csd.DeviceSizeMultiplier == 7 && csd.WriteSpeedFactor == 2);
    assert(csd.SectorCount == 2097152ULL);
    assert(!csd.PermanentWriteProtect && !csd.TemporaryWriteProtect);
    // 2048-byte blocks give four times as many sectors.
    __PutBits(words, 80, 4, 11);
    assert(MmcCsdDecode(words, &csd) == OS_EOK && csd.SectorCount == 8388608ULL);
    // A block length below 512 bytes leaves no capacity.
    __PutBits(words, 80, 4, 8);
    assert(MmcCsdDecode(words, &csd) == OS_EOK && csd.SectorCount == 0);
    // Write protect bits.
    __BuildCsd(words);
    __PutBits(words, 13, 1, 1);
    assert(MmcCsdDecode(words, &csd) == OS_EOK && csd.PermanentWriteProtect);
    // CSD structure version 1.0 is rejected, and the output stays as it was.
    memset(&csd, 0xA5, sizeof(csd));
    __PutBits(words, 126, 2, 0);
    assert(MmcCsdDecode(words, &csd) == OS_ENOTSUPPORTED);
    assert(csd.Structure == 0xA5);

    __BuildExtCsd(extCsd);
    extCsd[EXT_CACHE_CTRL] = 1;
    extCsd[EXT_PART_CONFIG] = 0x49;
    extCsd[EXT_HS_TIMING] = 1;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_EOK);
    assert(decoded.Revision == 8 && decoded.Structure == 2 && decoded.CardType == 0x57);
    assert(decoded.SectorCount == 15269888U);
    assert(decoded.GenericSwitchTimeMs == 250 && decoded.PartitionSwitchTimeMs == 100);
    assert(decoded.CacheSizeKiB == 1024U);
    assert(decoded.CacheControl == 1 && decoded.PartitionConfig == 0x49 && decoded.HsTiming == 1);
    // Fields are read only from the revision that defines them.
    extCsd[EXT_REV] = 5;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_EOK);
    assert(decoded.GenericSwitchTimeMs == 0 && decoded.CacheSizeKiB == 0 && decoded.PartitionSwitchTimeMs == 100);
    extCsd[EXT_REV] = 2;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_EOK);
    assert(decoded.PartitionSwitchTimeMs == 0 && decoded.SectorCount == 15269888U);
    extCsd[EXT_REV] = 1;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_EOK && decoded.SectorCount == 0);
    // Every revision is accepted, a revision newer than 5.1 too.
    extCsd[EXT_REV] = 9;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_EOK && decoded.Revision == 9);
    // A structure version above 2 is not.
    memset(&decoded, 0xA5, sizeof(decoded));
    extCsd[EXT_STRUCTURE] = 3;
    assert(MmcExtCsdDecode(extCsd, &decoded) == OS_ENOTSUPPORTED);
    assert(decoded.Revision == 0xA5);
    assert(MmcExtCsdDecode(NULL, &decoded) == OS_EINVALPARAMS);

    // Only the read only fields are compared.
    __BuildExtCsd(extCsd);
    memcpy(other, extCsd, sizeof(other));
    other[EXT_BUS_WIDTH] = 2;
    other[EXT_HS_TIMING] = 1;
    other[246] = 0x01;
    assert(MmcExtCsdSameProperties(extCsd, other));
    other[EXT_SEC_COUNT] ^= 0x01;
    assert(!MmcExtCsdSameProperties(extCsd, other));
    memcpy(other, extCsd, sizeof(other));
    other[253] = 0x33;
    assert(!MmcExtCsdSameProperties(extCsd, other));
    memcpy(other, extCsd, sizeof(other));
    other[160] = 0x00;
    assert(!MmcExtCsdSameProperties(extCsd, other));
    printf("MMC register decoding passed\n");
}

static void
TestCommandHelpers(void)
{
    assert(MMCSwitchArgument(183, 2) == 0x03B70201U);
    assert(MMCRelativeAddressArgument(1) == 0x00010000U);
    assert(MMCOpCondArgument(0xFFFFFFFFU) == 0x40FF8000U);
    assert(MMC_R1_ERROR_MASK == R1_MASK);
    printf("MMC command helpers passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Setup tests.

static void
TestNoDevice(void)
{
    struct DeviceModel model;

    // No answer to the first CMD1: the slot holds no MMC device either.
    __ResetHost();
    __ModelDevice(&model);
    model.Present = false;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectCommand(1, OP_COND_ARGUMENT, SdResponseR3);
    __CheckEvents();
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);
    assert(g_card.State == SdCardStateNoCard && g_card.Kind == SdCardKindMmc);
    assert(g_errorLogs == 0 && g_warningLogs == 0);
    printf("MMC setup of an empty slot passed\n");
}

static void
TestSectorDevice(void)
{
    struct DeviceModel   model;
    SdCardSetupOptions_t quiet = { 0 };
    uint32_t             generation;
    size_t               length;

    __ResetHost();
    __ModelDevice(&model);
    memset(&g_card, 0, sizeof(g_card));
    g_card.MediaGeneration = 6;
    // A card the SD setup left behind keeps nothing but the generation.
    g_card.Identity.SerialNumber = 0x1234;
    generation = g_card.MediaGeneration;
    __StartSetup(&g_noOptions);
    __RunDevice(&model);
    __ExpectFullSetup();
    __CheckEvents();
    __ExpectReady();
    assert(g_card.MediaGeneration == generation + 1);
    assert(g_card.Identity.SerialNumber == 0);
    assert(g_card.RelativeAddress == 1);
    assert(g_card.Addressing == SdCardAddressingSectors);
    assert(g_card.SectorCount == 15269888ULL);
    assert(g_card.Width == SdBusWidth8 && g_card.Timing == SdBusTimingHighSpeed);
    assert(g_card.ClockHz == 52000000U);
    assert(!g_card.WriteProtected);
    assert(strcmp(g_card.MmcIdentity.ProductName, "8GTF4R") == 0);
    assert(g_card.MmcIdentity.ManufactureYear == 2016);
    assert(g_card.MmcExtendedData.Revision == 8);
    assert(model.ExtCsd[EXT_BUS_WIDTH] == 2 && model.ExtCsd[EXT_HS_TIMING] == 1);
    quiet.Quiet = true;

    // The ready line names everything and fits the log.
    length = strlen(g_lastNotice);
    assert(length < 127);
    assert(strcmp(g_lastNotice, "MMC: device ready, manufacturer 0x15, product \"8GTF4R\", 7456 MiB, sector "
                                "addressing, 8 data lines, high speed, 52000000 Hz") == 0);

    // The quiet option leaves the line out.
    __ResetHost();
    __ModelDevice(&model);
    __SetupWithOptions(&model, &quiet);
    __ExpectReady();
    assert(strstr(g_lastNotice, "device ready") == NULL);
    printf("MMC setup of a sector addressed device passed\n");
}

static void
TestByteDevice(void)
{
    struct DeviceModel model;

    // A byte addressed device gets CMD16 and takes its capacity from the
    // CSD, while SEC_COUNT is ignored.
    __ResetHost();
    __ModelDevice(&model);
    model.AccessMode = 0;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(true);
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(52000000, SdBusWidth8, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();
    assert(g_card.Addressing == SdCardAddressingBytes);
    assert(g_card.SectorCount == 2097152ULL);
    assert(strstr(g_lastNotice, "1024 MiB, byte addressing") != NULL);

    // A byte addressed device whose CSD gives no capacity fails.
    __ResetHost();
    __ModelDevice(&model);
    model.AccessMode = 0;
    __PutBits(model.Csd, 80, 4, 8);
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EPROTOCOL, "CMD9");

    // A reserved access mode fails.
    __ResetHost();
    __ModelDevice(&model);
    model.AccessMode = 1U << 29;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EPROTOCOL, "CMD1");

    // Write protect bits of the CSD.
    __ResetHost();
    __ModelDevice(&model);
    __PutBits(model.Csd, 12, 1, 1);
    __Setup(&model);
    __ExpectReady();
    assert(g_card.WriteProtected);
    printf("MMC setup of a byte addressed device passed\n");
}

static void
TestPowerUp(void)
{
    struct DeviceModel model;
    int                i;

    // Busy for three answers, then ready: CMD1 again every 10 ms.
    __ResetHost();
    __ModelDevice(&model);
    model.BusyAnswers = 3;
    __Setup(&model);
    __ExpectBusSetup();
    for (i = 0; i < 3; i++) {
        __ExpectCommand(1, OP_COND_ARGUMENT, SdResponseR3);
        __ExpectDelay(OCR_RETRY_US);
    }
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(52000000, SdBusWidth8, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();

    // Busy for good: the first CMD1 and 100 more over 1 s, then a timeout.
    __ResetHost();
    __ModelDevice(&model);
    model.BusyAnswers = -1;
    __Setup(&model);
    assert(__CountCommands(1) == 101);
    __ExpectSetupResult(SdCardSetupFailed, OS_ETIMEOUT, "CMD1");
    assert(g_nowUs == 100U * OCR_RETRY_US);

    // An answer, then silence: a failure, not an empty slot.
    __ResetHost();
    __ModelDevice(&model);
    model.BusyAnswers = 1;
    model.FaultAt = 3;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_TIMEOUT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_ETIMEOUT, "CMD1");

    // A CRC error on the first CMD1 is no empty slot either.
    __ResetHost();
    __ModelDevice(&model);
    model.FaultAt = 2;
    model.FaultStatus = OS_EDEVFAULT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_CRC;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EDEVFAULT, "CMD1");

    // A host without a supply window fails before CMD1.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.SupplyVoltages = 0x80U;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_ENOTSUPPORTED, "CMD1");
    printf("MMC power up passed\n");
}

static void
TestUnsupportedDevices(void)
{
    struct DeviceModel model;

    // System specification 3.x: no EXT_CSD, not supported.
    __ResetHost();
    __ModelDevice(&model);
    __PutBits(model.Csd, 122, 4, 3);
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_ENOTSUPPORTED, "CMD9");
    assert(g_warningLogs == 1);
    assert(__CountCommands(8) == 0 && __CountCommands(7) == 0);

    // An EXT_CSD structure the decoder does not know.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_STRUCTURE] = 3;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_ENOTSUPPORTED, "CMD8");
    assert(__CountCommands(6) == 0);

    // A revision newer than the decoder knows is accepted (forward
    // compatible layout).
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_REV] = 9;
    __Setup(&model);
    __ExpectReady();

    // A sector addressed device without SEC_COUNT: revision 1, or zero.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_REV] = 1;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EPROTOCOL, "CMD8");
    __ResetHost();
    __ModelDevice(&model);
    memset(&model.ExtCsd[EXT_SEC_COUNT], 0, 4);
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EPROTOCOL, "CMD8");
    printf("MMC unsupported devices passed\n");
}

static void
TestWidths(void)
{
    struct DeviceModel   model;
    SdCardSetupOptions_t options = { 0 };

    // Four lines when the host is wired for four.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth4);
    __ExpectHighSpeed(SdBusWidth4);
    __ExpectFinish(52000000, SdBusWidth4, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();
    assert(model.ExtCsd[EXT_BUS_WIDTH] == 1);

    // One line when the host is wired for one: no switch, no second read.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectHighSpeed(SdBusWidth1);
    __ExpectFinish(52000000, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();
    assert(__CountCommands(8) == 1);

    // The limit of four lines on an eight-line host.
    __ResetHost();
    __ModelDevice(&model);
    options.WidthLimit = SdCardWidthLimitFourLines;
    __SetupWithOptions(&model, &options);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth4);
    __ExpectHighSpeed(SdBusWidth4);
    __ExpectFinish(52000000, SdBusWidth4, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();

    // The limit of one line.
    __ResetHost();
    __ModelDevice(&model);
    options.WidthLimit = SdCardWidthLimitOneLine;
    __SetupWithOptions(&model, &options);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectHighSpeed(SdBusWidth1);
    __ExpectFinish(52000000, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();
    printf("MMC bus widths passed\n");
}

static void
TestWidthFailures(void)
{
    struct DeviceModel model;

    // The device refuses BUS_WIDTH: SWITCH_ERROR in the CMD13 after it.
    __ResetHost();
    __ModelDevice(&model);
    model.RefuseIndex = EXT_BUS_WIDTH;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EDEVFAULT, "CMD13 after BUS_WIDTH");
    assert(g_setBusCount == 1);

    // The device stays busy programming after the switch.
    __ResetHost();
    __ModelDevice(&model);
    model.ProgramAfterIndex = EXT_BUS_WIDTH;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "CMD13 after BUS_WIDTH");

    // The EXT_CSD read on eight lines differs in SEC_COUNT.
    __ResetHost();
    __ModelDevice(&model);
    model.CorruptWideByte = EXT_SEC_COUNT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "CMD8 SEND_EXT_CSD on the new width");
    assert(g_warningLogs == 1);
    assert(__CountCommands(8) == 2 && g_card.Width == SdBusWidth8);

    // A field that may differ (BKOPS_STATUS) does not fail the check.
    __ResetHost();
    __ModelDevice(&model);
    model.CorruptWideByte = 246;
    __Setup(&model);
    __ExpectReady();

    // A data CRC error on the second read.
    __ResetHost();
    __ModelDevice(&model);
    model.FaultAt = 10;
    model.FaultStatus = OS_EDEVFAULT;
    model.FaultTransport = SD_TRANSPORT_DATA_CRC;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EDEVFAULT, "CMD8 SEND_EXT_CSD on the new width");

    // A timeout of the switch itself.
    __ResetHost();
    __ModelDevice(&model);
    model.FaultAt = 8;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_BUSY_TIMEOUT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_ETIMEOUT, "CMD6 SWITCH BUS_WIDTH");

    // The host fails to follow: a host failure, not a width failure.
    __ResetHost();
    __ModelDevice(&model);
    g_setBusFailAt = 2;
    g_setBusFailStatus = OS_ETIMEOUT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_ETIMEOUT, "host bus width");
    printf("MMC bus width failures passed\n");
}

static void
TestHighSpeed(void)
{
    struct DeviceModel   model;
    SdCardSetupOptions_t options = { 0 };

    // A device that offers high speed at 26 MHz only.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_CARD_TYPE] = 0x01;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(26000000, SdBusWidth8, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();

    // A device without high speed: default timing at 26 MHz.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_CARD_TYPE] = 0x00;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectFinish(26000000, SdBusWidth8, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady();
    assert(model.ExtCsd[EXT_HS_TIMING] == 0);

    // A host without high speed.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.Flags = 0;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectFinish(26000000, SdBusWidth8, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady();

    // The timing limit.
    __ResetHost();
    __ModelDevice(&model);
    options.TimingLimit = SdCardTimingLimitDefaultSpeed;
    __SetupWithOptions(&model, &options);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectFinish(26000000, SdBusWidth8, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady();

    // The device refuses HS_TIMING.
    __ResetHost();
    __ModelDevice(&model);
    model.RefuseIndex = EXT_HS_TIMING;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_EDEVFAULT, "CMD13 after HS_TIMING");

    // The final CMD13 fails after high speed: counted against high speed.
    __ResetHost();
    __ModelDevice(&model);
    model.FaultAt = 13;
    model.FaultStatus = OS_EDEVFAULT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_CRC;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_EDEVFAULT, "CMD13 SEND_STATUS");

    // The host fails to follow the timing.
    __ResetHost();
    __ModelDevice(&model);
    g_setBusFailAt = 3;
    g_setBusFailStatus = OS_EDEVFAULT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_EDEVFAULT, "host timing");
    printf("MMC high speed passed\n");
}

static void
TestClockLimits(void)
{
    struct DeviceModel   model;
    SdCardSetupOptions_t options = { 0 };

    // A data clock limit from the options.
    __ResetHost();
    __ModelDevice(&model);
    options.DataClockLimitHz = 10000000U;
    __SetupWithOptions(&model, &options);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(10000000, SdBusWidth8, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();

    // A limit below the setup clock is raised to it.
    __ResetHost();
    __ModelDevice(&model);
    options.DataClockLimitHz = 100000U;
    __SetupWithOptions(&model, &options);
    __ExpectReady();
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 400000U);

    // The host maximum caps the clock.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.MaximumClockHz = 40000000U;
    __Setup(&model);
    __ExpectReady();
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 40000000U);
    printf("MMC clock limits passed\n");
}

static void
TestPartitionAndCache(void)
{
    struct DeviceModel model;

    // PARTITION_ACCESS 1 (boot partition 1) with boot bits 0x48: switched to
    // the user area, boot bits kept. The time is at least 300 ms. Then the
    // cache, which was on, is turned off with at least 1.6 s.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_PART_CONFIG] = 0x49;
    model.ExtCsd[EXT_CACHE_CTRL] = 0x01;
    __Setup(&model);
    __ExpectBusSetup();
    __ExpectIdentification(false);
    __ExpectSwitch(EXT_PART_CONFIG, 0x48, 300000);
    __ExpectStatus();
    __ExpectSwitch(EXT_CACHE_CTRL, 0x00, 1600000);
    __ExpectStatus();
    __ExpectWidth(SdBusWidth8);
    __ExpectHighSpeed(SdBusWidth8);
    __ExpectFinish(52000000, SdBusWidth8, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady();
    assert(model.ExtCsd[EXT_PART_CONFIG] == 0x48 && model.ExtCsd[EXT_CACHE_CTRL] == 0);
    assert(g_card.MmcExtendedData.PartitionConfig == 0x48 && g_card.MmcExtendedData.CacheControl == 0);

    // A long PARTITION_SWITCH_TIME is used as it is.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_PART_CONFIG] = 0x02;
    model.ExtCsd[EXT_PART_TIME] = 90;
    __Setup(&model);
    __ExpectReady();
    assert(g_events[8].Request.Opcode == 6 && g_events[8].Request.DataTimeoutUs == 900000U);

    // A refused partition switch fails setup in the basic part.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_PART_CONFIG] = 0x01;
    model.RefuseIndex = EXT_PART_CONFIG;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EDEVFAULT, "CMD13 after PARTITION_CONFIG");

    // A refused cache switch too.
    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_CACHE_CTRL] = 0x01;
    model.RefuseIndex = EXT_CACHE_CTRL;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EDEVFAULT, "CMD13 after CACHE_CTRL");
    printf("MMC partition and cache passed\n");
}

// The BUS_WIDTH switch timeout for a given EXT_CSD revision and
// GENERIC_CMD6_TIME.
static uint32_t
__WidthSwitchTimeout(
    _In_ uint8_t revision,
    _In_ uint8_t genericTime)
{
    struct DeviceModel model;
    int                i;

    __ResetHost();
    __ModelDevice(&model);
    model.ExtCsd[EXT_REV] = revision;
    model.ExtCsd[EXT_GENERIC_TIME] = genericTime;
    __Setup(&model);
    __ExpectReady();
    for (i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventCommand && g_events[i].Request.Opcode == 6) {
            return g_events[i].Request.DataTimeoutUs;
        }
    }
    assert(!"no CMD6");
    return 0;
}

static void
TestSwitchTimeouts(void)
{
    assert(__WidthSwitchTimeout(8, 25) == 250000U);
    // No GENERIC_CMD6_TIME: 500 ms, with revision 6 and zero, and before
    // revision 6.
    assert(__WidthSwitchTimeout(8, 0) == 500000U);
    assert(__WidthSwitchTimeout(5, 25) == 500000U);
    // Bounds: 10 ms is raised to 100 ms, 2550 ms stays.
    assert(__WidthSwitchTimeout(8, 1) == 100000U);
    assert(__WidthSwitchTimeout(8, 255) == 2550000U);
    printf("MMC switch timeouts passed\n");
}

static void
TestErrorMask(void)
{
    struct DeviceModel model;

    // Bit 3 (AKE_SEQ_ERROR on SD) is reserved on MMC and no error.
    __ResetHost();
    __ModelDevice(&model);
    model.CardErrorAt = 6;
    model.CardErrorBits = 1U << 3;
    __Setup(&model);
    __ExpectReady();

    // SWITCH_ERROR in the final status is an error.
    __ResetHost();
    __ModelDevice(&model);
    model.CardErrorAt = 13;
    model.CardErrorBits = MMC_R1_SWITCH_ERROR;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_EDEVFAULT, "CMD13 SEND_STATUS");

    // A card error on CMD3.
    __ResetHost();
    __ModelDevice(&model);
    model.CardErrorAt = 4;
    model.CardErrorBits = MMC_R1_ERROR;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EDEVFAULT, "CMD3");
    printf("MMC card error mask passed\n");
}

static void
TestWriteProtectSwitch(void)
{
    struct DeviceModel model;

    // A host with a write protect switch: the slot's switch counts too.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.Flags |= SD_HOST_FLAG_WRITE_PROTECT;
    g_cardState = SD_CARD_STATE_PRESENT | SD_CARD_STATE_WRITE_PROTECTED;
    __Setup(&model);
    __ExpectReady();
    assert(g_card.WriteProtected);

    // A card that is gone by then: no card.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.Flags |= SD_HOST_FLAG_WRITE_PROTECT;
    g_cardState = 0;
    __Setup(&model);
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);
    printf("MMC write protection passed\n");
}

static void
TestCancel(void)
{
    struct DeviceModel model;
    SdResult_t         cancelled = { 0 };

    // During the wait between CMD1 attempts.
    __ResetHost();
    __ModelDevice(&model);
    model.BusyAnswers = -1;
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup(&g_noOptions);
    while (__CountCommands(1) == 0) {
        if (g_pending != NULL) {
            SdResult_t result = __Answer(&model, g_pending);
            __Complete(&result);
        } else {
            __RunScheduled();
        }
    }
    __Complete(&(SdResult_t){ .Status = OS_EOK, .Response = { 0x00FF8080U }, .HardwareStopped = true });
    assert(g_scheduled != NULL);
    assert(SdCardSetupCancel(&g_card) == OS_EOK);
    assert(g_setupDoneCount == 1);
    assert(g_setupResult == SdCardSetupFailed && g_setupStatus == OS_ECANCELLED);
    assert(g_scheduled == NULL);

    // While the EXT_CSD read is with the host: Stop, then the completion.
    __ResetHost();
    __ModelDevice(&model);
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup(&g_noOptions);
    while (__CountCommands(8) == 0) {
        if (g_pending != NULL) {
            SdResult_t result = __Answer(&model, g_pending);
            __Complete(&result);
        } else {
            __RunScheduled();
        }
    }
    assert(g_pending != NULL && g_pending->Opcode == 8);
    assert(SdCardSetupCancel(&g_card) == OS_EOK);
    assert(g_events[g_eventCount - 1].Kind == EventStop);
    assert(g_setupDoneCount == 0);
    cancelled.Status = OS_ECANCELLED;
    cancelled.HardwareStopped = true;
    __Complete(&cancelled);
    assert(g_setupDoneCount == 1);
    assert(g_setupResult == SdCardSetupFailed && g_setupStatus == OS_ECANCELLED);
    assert(SdCardSetupCancel(&g_card) == OS_ENOENT);

    // A second setup while one runs is refused.
    __ResetHost();
    __ModelDevice(&model);
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup(&g_noOptions);
    assert(SdMmcSetupWithOptions(&g_card, &g_host, &g_noOptions, __SetupDone, &g_setupContext) == OS_EBUSY);
    assert(SdMmcSetupWithOptions(NULL, &g_host, &g_noOptions, __SetupDone, NULL) == OS_EINVALPARAMS);
    assert(SdMmcSetupWithOptions(&g_card, &g_host, NULL, __SetupDone, NULL) == OS_EINVALPARAMS);
    __RunDevice(&model);
    __ExpectReady();
    printf("MMC setup cancellation passed\n");
}

static void
TestBusSetup(void)
{
    struct DeviceModel model;
    int                i;

    // A full setup sets the bus four times and never removes power or
    // waits: setup state, eight lines, high speed and data clock.
    __ResetHost();
    __ModelDevice(&model);
    __Setup(&model);
    __ExpectReady();
    assert(g_setBusCount == 4);
    for (i = 0; i < g_eventCount; i++) {
        assert(g_events[i].Kind != EventDelay);
        if (g_events[i].Kind == EventBus) {
            assert(g_events[i].Bus.Power == SdBusPowerOn);
        }
    }

    // The bus setup comes before CMD0 and takes the clock the host chose:
    // a 1 MHz base clock divided by 3, 333,333 Hz, the fastest rate at or
    // below 400 kHz. The width switch asks for that clock.
    __ResetHost();
    __ModelDevice(&model);
    g_baseClockHz = 1000000U;
    g_host.Limits.MaximumClockHz = 1000000U;
    g_host.Limits.MinimumClockHz = 100000U;
    __Setup(&model);
    __ExpectReady();
    assert(g_events[0].Kind == EventBus && g_events[0].Bus.ClockHz == 400000U);
    assert(g_events[1].Kind == EventCommand && g_events[1].Request.Opcode == 0);
    for (i = 1; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventBus) {
            break;
        }
    }
    assert(g_events[i].Bus.Width == SdBusWidth8 && g_events[i].Bus.ClockHz == 333333U);
    assert(g_card.ClockHz == 1000000U);
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 1000000U);

    // A SetBus failure in the bus setup is a host failure, before any
    // command.
    __ResetHost();
    __ModelDevice(&model);
    g_setBusFailAt = 1;
    g_setBusFailStatus = OS_EDEVFAULT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_EDEVFAULT, "bus setup");
    assert(__CountCommands(0) == 0 && g_eventCount == 1);

    // A host that cannot run at 400 kHz or below.
    __ResetHost();
    __ModelDevice(&model);
    g_host.Limits.MinimumClockHz = 500000U;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_ENOTSUPPORTED, "bus setup");
    assert(g_eventCount == 0);
    printf("MMC bus setup passed\n");
}

static void
TestControlBuffer(void)
{
    struct DeviceModel model;

    // No control buffer: the EXT_CSD read cannot start.
    __ResetHost();
    __ModelDevice(&model);
    g_host.ControlBuffer = NULL;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_ENOTSUPPORTED, "CMD8");

    // One that is too small for 512 bytes.
    __ResetHost();
    __ModelDevice(&model);
    g_controlBuffer.Segments[0].Length = 256;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHostFailed, OS_EINVALPARAMS, "CMD8");
    printf("MMC control buffer checks passed\n");
}

int
main(void)
{
    TestRegisters();
    TestCommandHelpers();
    TestNoDevice();
    TestSectorDevice();
    TestByteDevice();
    TestPowerUp();
    TestUnsupportedDevices();
    TestWidths();
    TestWidthFailures();
    TestHighSpeed();
    TestClockLimits();
    TestPartitionAndCache();
    TestSwitchTimeouts();
    TestErrorMask();
    TestWriteProtectSwitch();
    TestCancel();
    TestControlBuffer();
    TestBusSetup();
    printf("MMC setup tests passed\n");
    return 0;
}
