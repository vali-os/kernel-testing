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
 * Host test for the SD card layer. The real modules/storage/sd/sequence.c,
 * registers.c and card.c run against a scripted fake host:
 *
 * - Submit records the request and returns. The test answers it later and
 *   calls Complete from outside Submit, as the host contract requires.
 * - Schedule and CancelWork use a fake clock that moves only when the test
 *   runs the scheduled work item.
 * - SetBus and GetCardState record their arguments.
 * - DMABufferData and DMABufferSegments work on plain memory.
 *
 * Card setup tests answer requests with a small model of an SD card. Every
 * test checks the exact list of bus settings, delays and commands (with
 * their arguments, response formats, error masks, timeouts and data
 * descriptions), not only the final outcome.
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
#include "sd/card.h"
#include "sd/commands.h"
#include "sd/registers.h"
#include "sd/sequence.h"

#define MAX_EVENTS 1024

// The values the card layer must use, written out independently.
#define R1_MASK            0xFD390008U
#define COMMAND_TIMEOUT_US 20000U
#define BUSY_TIMEOUT_US    500000U
#define READ_TIMEOUT_US    150000U
#define POWER_OFF_US       10000U
// 8 clock cycles at 400 kHz after CMD6 switches the timing.
#define SWITCH_WAIT_US     20U
// CMD6 arguments: function 1 (high speed) of group 1, every other group kept.
#define SWITCH_CHECK_ARG   0x00FFFFF1U
#define SWITCH_SET_ARG     0x80FFFFF1U
// Supply ramp-up (35 ms) plus the 1 ms wait after it.
#define POWER_ON_BASE_US   36000U
#define OCR_RETRY_US       10000U
#define CONTROL_ADDRESS    0x2000ULL
#define CARD_ADDRESS       0xB368U
#define HOST_VOLTAGES      0x00300000U

/////////////////////////////////////////////////////////////////////////////
// Logging.

static int  g_errorLogs;
static int  g_traceLogs;
static char g_lastError[256];

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
    } else if (level == OSSYSLOGLEVEL_TRACE) {
        g_traceLogs++;
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
    // Copy of the first data segment, which may not outlive the request.
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
static int          g_submitCount;
static int          g_submitFailAt;
static oserr_t      g_submitFailStatus;
static int          g_setBusCount;
static int          g_setBusFailAt;
static oserr_t      g_setBusFailStatus;
static oserr_t      g_scheduleStatus;
static oserr_t      g_cardStateStatus;
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
        assert(request->Data.Segments[0].Address % 4U == 0);
    }

    event = __Record(EventCommand);
    event->Request = *request;
    if (request->Data.Segments != NULL) {
        event->SegmentAddress = request->Data.Segments[0].Address;
        event->SegmentLength = request->Data.Segments[0].Length;
    }
    g_submitCount++;
    if (g_submitCount == g_submitFailAt) {
        return g_submitFailStatus;
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
    // The fastest rate from an integer divider of the base clock that does
    // not exceed the request.
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
    if (g_cardStateStatus == OS_EOK) {
        *stateOut = g_cardState;
    }
    return g_cardStateStatus;
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
    if (g_scheduleStatus != OS_EOK) {
        return g_scheduleStatus;
    }
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

// A PCI host with a 100 MHz base clock, a write protect switch and 3.2 to
// 3.4 V supply. It is wired for one data line and has no high speed, so the
// basic setup tests see the setup without the four-line and high speed
// steps. Those tests turn the features on (__EnableFeatures).
static void
__ResetHost(void)
{
    memset(&g_host, 0, sizeof(g_host));
    memset(&g_controlBuffer, 0, sizeof(g_controlBuffer));
    g_host.Operations = &g_operations;
    g_host.Limits.MaximumClockHz = 50000000U;
    g_host.Limits.MinimumClockHz = 50000U;
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1;
    g_host.Limits.SignalVoltages = SD_HOST_SIGNAL_330;
    g_host.Limits.SupplyVoltages = HOST_VOLTAGES;
    g_host.Limits.MaximumBlockSize = 512;
    g_host.Limits.MaximumBlockCount = 65535;
    g_host.Limits.Flags = SD_HOST_FLAG_REMOVABLE | SD_HOST_FLAG_WRITE_PROTECT;
    g_host.ControlBuffer = &g_controlBuffer;
    g_controlBuffer.Segments[0].Address = CONTROL_ADDRESS;
    g_controlBuffer.Segments[0].Length = sizeof(g_controlBuffer.Memory);
    g_controlBuffer.SegmentCount = 1;

    g_eventCount = 0;
    g_pending = NULL;
    g_scheduled = NULL;
    g_nowUs = 0;
    g_baseClockHz = 100000000U;
    g_submitCount = 0;
    g_submitFailAt = 0;
    g_setBusCount = 0;
    g_setBusFailAt = 0;
    g_scheduleStatus = OS_EOK;
    g_cardStateStatus = OS_EOK;
    g_cardState = SD_CARD_STATE_PRESENT;
    g_errorLogs = 0;
    g_traceLogs = 0;
    g_lastError[0] = '\0';
}

// Delivers the result of the pending request from outside Submit.
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
__ExpectBusWith(
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

// Bus settings with one data line and default timing.
static void
__ExpectBus(
    _In_ uint32_t        clockHz,
    _In_ enum SdBusPower power)
{
    __ExpectBusWith(clockHz, SdBusWidth1, SdBusTimingDefault, power);
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
}

// Power off, discharge, power on at the setup clock, power-up wait.
static void
__ExpectPowerCycle(
    _In_ uint32_t setupClockHz,
    _In_ uint32_t powerUpDelayUs)
{
    g_expectedCount = 0;
    __ExpectBus(0, SdBusPowerOff);
    __ExpectDelay(POWER_OFF_US);
    __ExpectBus(setupClockHz, SdBusPowerOn);
    __ExpectDelay(powerUpDelayUs);
}

// Every detail of a recorded command that follows from its opcode and
// response format.
static void
__CheckCommandDetails(
    _In_ const struct Event* event)
{
    const SdRequest_t* request = &event->Request;
    bool               r1 = request->ResponseType == SdResponseR1 || request->ResponseType == SdResponseR1b;

    assert(request->CardErrorMask == (r1 ? R1_MASK : 0U));
    assert((request->CardErrorMask & (SD_R1_ILLEGAL_COMMAND | SD_R1_COM_CRC_ERROR)) == 0);
    assert(request->CommandTimeoutUs == COMMAND_TIMEOUT_US);
    assert(request->StopPolicy == SdStopNone);
    // ACMD51 reads the 8-byte SCR. CMD6 and ACMD13 read 64 bytes of status
    // (ACMD6 and CMD13 share their opcodes but move no data).
    if (request->Opcode == 51 || request->Direction != SdDataNone) {
        uint32_t size = request->Opcode == 51 ? 8U : 64U;

        assert(request->Opcode == 51 || request->Opcode == 6 || request->Opcode == 13);
        assert(request->ResponseType == SdResponseR1);
        assert(request->DataTimeoutUs == READ_TIMEOUT_US);
        assert(request->Direction == SdDataRead);
        assert(request->BlockSize == size && request->BlockCount == 1);
        assert(request->Data.SegmentCount == 1 && request->Data.Length == size);
        assert(event->SegmentAddress == CONTROL_ADDRESS && event->SegmentLength == size);
        return;
    }
    assert(request->DataTimeoutUs == (request->ResponseType == SdResponseR1b ? BUSY_TIMEOUT_US : 0U));
    assert(request->Direction == SdDataNone);
    assert(request->BlockSize == 0 && request->BlockCount == 0);
    assert(request->Data.Segments == NULL && request->Data.SegmentCount == 0 && request->Data.Length == 0);
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
                __CheckCommandDetails(event);
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

// CID: MID 0x03, OID "SD", PNM "SU04G", PRV 8.0, PSN 0x12345678, made in
// April 2013.
static const uint32_t g_cid[4] = { 0x03534453U, 0x55303447U, 0x80123456U, 0x7800D400U };

// CSD version 1, 2 GiB: C_SIZE 4095, C_SIZE_MULT 7, READ_BL_LEN 10 (1024-byte
// blocks). 4096 * 512 * 1024 bytes = 4,194,304 sectors.
static const uint32_t g_csd2GiB[4] = { 0x00260032U, 0x5F5A83FFU, 0xC003FF80U, 0x12800000U };

// CSD version 2, 32 GiB: C_SIZE 0xFFFF. 65536 * 1024 = 67,108,864 sectors.
static const uint32_t g_csd32GiB[4] = { 0x400E0032U, 0x5B590000U, 0xFFFF7F80U, 0x0A400000U };

// SCR: structure 0, SD_SPEC 2, security 3, one and four data lines, SD_SPEC3
// and SD_SPEC4 set, CMD20 and CMD23 supported.
static const uint8_t g_scr[8] = { 0x02, 0x35, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00 };

static void
__CsdVersion2(
    _Out_ uint32_t words[4],
    _In_  uint32_t deviceSize)
{
    memcpy(words, g_csd32GiB, sizeof(g_csd32GiB));
    __PutBits(words, 48, 22, deviceSize);
}

/////////////////////////////////////////////////////////////////////////////
// Card model.

enum CardKind {
    CardEmpty,
    CardLegacy,
    CardVersion2
};

struct CardModel {
    enum CardKind Kind;
    // Answers CCS once HCS was sent. Without HCS it never becomes ready.
    bool          HighCapacity;
    // ACMD41 answers with READY clear before the card is ready. -1 is forever.
    int           BusyAnswers;
    bool          WrongEcho;
    // CMD55 answers without APP_CMD, like a device that is no SD card.
    bool          NoAppCommand;
    // ACMD41 gets no answer.
    bool          SilentOpCond;
    // Reports CCS even without HCS in the argument, which no real card does.
    bool          CcsWithoutHcs;
    int           ZeroAddressAnswers;
    uint32_t      Cid[4];
    uint32_t      Csd[4];
    uint8_t       Scr[8];
    uint32_t      FinalState;

    // Fault injection by 1-based command number.
    int           FaultAt;
    oserr_t       FaultStatus;
    uint32_t      FaultTransport;
    bool          FaultHardwareRunning;
    int           CardErrorAt;
    uint32_t      CardErrorBits;

    // Four data lines and high speed.
    // ACMD6 gets no answer, like a card that does not know it.
    bool          SilentBusWidth;
    // DAT_BUS_WIDTH the SD status reports. -1 reports what ACMD6 set.
    int           SdStatusWidth;
    // Group 1 support bits in the switch status. Bit 1 is high speed.
    uint16_t      Group1Support;
    // The switch status names the asked function even when it is not
    // supported, instead of the error value.
    bool          EchoSelection;
    // Switch mode reports this function instead of the one that was asked.
    // -1 reports the normal answer.
    int           SwitchSelection;
    // Data structure version of the switch status.
    uint8_t       SwitchVersion;
    // Switch mode answers with an unknown data structure version.
    bool          CorruptSwitchMode;

    // Card state while running.
    int           CommandNumber;
    uint32_t      State;
    bool          AppCommand;
    bool          IllegalPending;
    // Bus width field of the last ACMD6, as in its argument.
    uint32_t      BusWidthArgument;
    // Function of group 1 (access mode) the card uses.
    uint32_t      AccessMode;
};

static void
__ModelVersion2(
    _Out_ struct CardModel* model)
{
    memset(model, 0, sizeof(*model));
    model->Kind = CardVersion2;
    model->HighCapacity = true;
    memcpy(model->Cid, g_cid, sizeof(g_cid));
    memcpy(model->Csd, g_csd32GiB, sizeof(g_csd32GiB));
    memcpy(model->Scr, g_scr, sizeof(g_scr));
    model->FinalState = SD_STATE_TRANSFER;
    model->SdStatusWidth = -1;
    model->Group1Support = 0x8003U;
    model->SwitchSelection = -1;
}

static void
__ModelLegacy(
    _Out_ struct CardModel* model)
{
    __ModelVersion2(model);
    model->Kind = CardLegacy;
    model->HighCapacity = false;
    memcpy(model->Csd, g_csd2GiB, sizeof(g_csd2GiB));
}

static void
__Silent(
    _InOut_ struct CardModel* model,
    _Out_   SdResult_t*       result)
{
    result->Status = OS_ETIMEOUT;
    result->TransportErrors = SD_TRANSPORT_COMMAND_TIMEOUT;
    model->IllegalPending = true;
}

// Card status for an R1 answer: the current state, and ILLEGAL_COMMAND when
// the previous command got no answer.
static uint32_t
__CardStatus(
    _InOut_ struct CardModel* model)
{
    uint32_t status = model->State << 9;

    if (model->IllegalPending) {
        status |= SD_R1_ILLEGAL_COMMAND;
        model->IllegalPending = false;
    }
    return status;
}

static void
__AnswerOpCond(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request,
    _Out_   SdResult_t*        result)
{
    bool hcs = (request->Argument & SD_OCR_HCS) != 0;
    bool busy = model->BusyAnswers != 0 || (model->HighCapacity && !hcs && !model->CcsWithoutHcs);

    *result = __Success(0x00FF8000U);
    if (busy) {
        if (model->BusyAnswers > 0) {
            model->BusyAnswers--;
        }
        return;
    }
    result->Response[0] |= SD_OCR_READY;
    if (model->HighCapacity) {
        result->Response[0] |= SD_OCR_CCS;
    }
    model->State = SD_STATE_READY;
}

static void
__AnswerAddress(
    _InOut_ struct CardModel* model,
    _Out_   SdResult_t*       result)
{
    uint32_t address = CARD_ADDRESS;

    if (model->ZeroAddressAnswers > 0) {
        model->ZeroAddressAnswers--;
        address = 0;
    }
    *result = __Success((address << 16) | (model->State << 9));
    model->State = SD_STATE_STANDBY;
}

static void
__AnswerScr(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request,
    _Out_   SdResult_t*        result)
{
    uint64_t offset = request->Data.Segments[0].Address - CONTROL_ADDRESS;

    assert(offset + 8 <= sizeof(g_controlBuffer.Memory));
    memcpy(&g_controlBuffer.Memory[offset], model->Scr, sizeof(model->Scr));
    *result = __Success(__CardStatus(model) | SD_R1_READY_FOR_DATA | SD_R1_APP_CMD);
    result->ConfirmedBytes = 8;
}

// Copies a 64-byte status block to the request's place in the control buffer.
static void
__PutStatusBlock(
    _In_ const SdRequest_t* request,
    _In_ const uint8_t      block[64])
{
    uint64_t offset = request->Data.Segments[0].Address - CONTROL_ADDRESS;

    assert(offset + 64 <= sizeof(g_controlBuffer.Memory));
    memcpy(&g_controlBuffer.Memory[offset], block, 64);
}

// ACMD13: the SD status. DAT_BUS_WIDTH sits in bits 511:510.
static void
__AnswerSdStatus(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request,
    _Out_   SdResult_t*        result)
{
    uint8_t  block[64] = { 0 };
    uint32_t width = model->SdStatusWidth >= 0 ? (uint32_t)model->SdStatusWidth : model->BusWidthArgument;

    block[0] = (uint8_t)(width << 6);
    __PutStatusBlock(request, block);
    *result = __Success(__CardStatus(model) | SD_R1_READY_FOR_DATA | SD_R1_APP_CMD);
    result->ConfirmedBytes = 64;
}

// CMD6: the switch status. The maximum current is in bytes 0 and 1, the
// group 1 support bits in bytes 12 and 13, the selected function of group 1
// in the low half of byte 16 and the data structure version in byte 17.
static void
__AnswerSwitch(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request,
    _Out_   SdResult_t*        result)
{
    uint8_t  block[64] = { 0 };
    uint32_t function = request->Argument & 0xFU;
    bool     switchMode = (request->Argument & 0x80000000U) != 0;

    assert((request->Argument & 0x7FFFFFF0U) == 0x00FFFFF0U);
    if (!model->EchoSelection && (model->Group1Support & (1U << function)) == 0) {
        function = 0xFU;
    }
    if (switchMode && model->SwitchSelection >= 0) {
        function = (uint32_t)model->SwitchSelection;
    }
    if (switchMode && function != 0xFU) {
        model->AccessMode = function;
    }
    block[1] = 100;
    block[12] = (uint8_t)(model->Group1Support >> 8);
    block[13] = (uint8_t)model->Group1Support;
    block[16] = (uint8_t)(0xF0U | function);
    block[17] = switchMode && model->CorruptSwitchMode ? 7U : model->SwitchVersion;
    __PutStatusBlock(request, block);
    *result = __Success(__CardStatus(model) | SD_R1_READY_FOR_DATA);
    result->ConfirmedBytes = 64;
}

static void
__AnswerByOpcode(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request,
    _In_    bool               appCommand,
    _Out_   SdResult_t*        result)
{
    switch (request->Opcode) {
        case 0:
            model->State = SD_STATE_IDLE;
            model->IllegalPending = false;
            *result = __Success(0);
            return;
        case 8:
            if (model->Kind != CardVersion2) {
                __Silent(model, result);
                return;
            }
            *result = __Success((request->Argument ^ (model->WrongEcho ? 0x55U : 0U)) & 0xFFFU);
            return;
        case 55:
            if (model->Kind == CardEmpty) {
                __Silent(model, result);
                return;
            }
            *result = __Success(__CardStatus(model) | (model->NoAppCommand ? 0U : SD_R1_APP_CMD));
            model->AppCommand = !model->NoAppCommand;
            return;
        case 41:
            if (!appCommand || model->SilentOpCond) {
                __Silent(model, result);
                return;
            }
            __AnswerOpCond(model, request, result);
            return;
        case 2:
            *result = __Success(0);
            memcpy(result->Response, model->Cid, sizeof(model->Cid));
            model->State = SD_STATE_IDENT;
            return;
        case 3:
            __AnswerAddress(model, result);
            return;
        case 9:
            assert(request->Argument == (uint32_t)CARD_ADDRESS << 16);
            *result = __Success(0);
            memcpy(result->Response, model->Csd, sizeof(model->Csd));
            return;
        case 7:
            *result = __Success(__CardStatus(model));
            model->State = SD_STATE_TRANSFER;
            return;
        case 16:
            *result = __Success(__CardStatus(model) | SD_R1_READY_FOR_DATA);
            return;
        case 51:
            if (!appCommand) {
                __Silent(model, result);
                return;
            }
            __AnswerScr(model, request, result);
            return;
        case 13:
            if (appCommand) {
                __AnswerSdStatus(model, request, result);
                return;
            }
            *result = __Success((model->FinalState << 9) | SD_R1_READY_FOR_DATA);
            return;
        case 6:
            if (!appCommand) {
                __AnswerSwitch(model, request, result);
                return;
            }
            if (model->SilentBusWidth) {
                __Silent(model, result);
                return;
            }
            assert(request->Argument == 0 || request->Argument == 2);
            model->BusWidthArgument = request->Argument;
            *result = __Success(__CardStatus(model) | SD_R1_APP_CMD);
            return;
        default:
            assert(!"unexpected command");
    }
}

// Answers the pending request the way the model card would, including the
// host's own card error check for R1 and R1b.
static SdResult_t
__Answer(
    _InOut_ struct CardModel*  model,
    _In_    const SdRequest_t* request)
{
    SdResult_t result = { 0 };
    bool       appCommand = model->AppCommand;

    model->AppCommand = false;
    model->CommandNumber++;
    if (model->CommandNumber == model->FaultAt) {
        result.Status = model->FaultStatus;
        result.TransportErrors = model->FaultTransport;
        result.HardwareStopped = !model->FaultHardwareRunning;
        return result;
    }

    __AnswerByOpcode(model, request, appCommand, &result);
    if (model->CommandNumber == model->CardErrorAt) {
        result.Response[0] |= model->CardErrorBits;
        // A card that reports an error for CMD55 does not take the next
        // command as an application command.
        if (request->Opcode == 55) {
            model->AppCommand = false;
        }
    }
    if (request->ResponseType == SdResponseR1 || request->ResponseType == SdResponseR1b) {
        result.CardErrors = result.Response[0] & request->CardErrorMask;
    }
    result.HardwareStopped = true;
    return result;
}

static void
__RunCard(
    _InOut_ struct CardModel* model)
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
// Card setup helpers.

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
__StartSetup(void)
{
    g_setupDoneCount = 0;
    g_setupResult = (enum SdCardSetupResult)-1;
    g_setupStatus = OS_EUNKNOWN;
    assert(SdCardSetup(&g_card, &g_host, __SetupDone, &g_setupContext) == OS_EOK);
}

static void
__Setup(
    _InOut_ struct CardModel* model)
{
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup();
    __RunCard(model);
    assert(g_setupDoneCount == 1);
    assert(g_pending == NULL && g_scheduled == NULL);
}

static void
__SetupWithOptions(
    _InOut_ struct CardModel*           model,
    _In_    const SdCardSetupOptions_t* options)
{
    memset(&g_card, 0, sizeof(g_card));
    g_setupDoneCount = 0;
    g_setupResult = (enum SdCardSetupResult)-1;
    g_setupStatus = OS_EUNKNOWN;
    assert(SdCardSetupWithOptions(&g_card, &g_host, options, __SetupDone, &g_setupContext) == OS_EOK);
    __RunCard(model);
    assert(g_setupDoneCount == 1);
    assert(g_pending == NULL && g_scheduled == NULL);
}

static void
__ExpectSetupResult(
    _In_ enum SdCardSetupResult result,
    _In_ oserr_t                status,
    _In_ const char*            stepName)
{
    if (g_setupResult != result) {
        fprintf(stderr, "setup result %d, expected %d, after \"%s\"\n", (int)g_setupResult, (int)result, g_lastError);
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

// A command reached the card and failed.
static void
__ExpectSetupFailed(
    _In_ oserr_t     status,
    _In_ const char* stepName)
{
    __ExpectSetupResult(SdCardSetupFailed, status, stepName);
}

// The step failed in the host before its command reached the card.
static void
__ExpectSetupHostFailed(
    _In_ oserr_t     status,
    _In_ const char* stepName)
{
    __ExpectSetupResult(SdCardSetupHostFailed, status, stepName);
}

// The commands of a version 2 setup from CMD0 to ACMD41 when the card is
// ready at once, with the given ACMD41 argument.
static void
__ExpectVersion2Start(
    _In_ uint32_t opCondArgument)
{
    __ExpectPowerCycle(400000, POWER_ON_BASE_US + 185);
    __ExpectCommand(0, 0, SdResponseNone);
    __ExpectCommand(8, 0x1AA, SdResponseR7);
    __ExpectCommand(55, 0, SdResponseR1);
    __ExpectCommand(41, opCondArgument, SdResponseR3);
}

// Everything after ACMD41 for a card at CARD_ADDRESS.
static void
__ExpectIdentification(
    _In_ bool     blockLength,
    _In_ uint32_t finalClockHz)
{
    uint32_t address = (uint32_t)CARD_ADDRESS << 16;

    __ExpectCommand(2, 0, SdResponseR2);
    __ExpectCommand(3, 0, SdResponseR6);
    __ExpectCommand(9, address, SdResponseR2);
    __ExpectCommand(7, address, SdResponseR1b);
    if (blockLength) {
        __ExpectCommand(16, 512, SdResponseR1);
    }
    __ExpectCommand(55, address, SdResponseR1);
    __ExpectCommand(51, 0, SdResponseR1);
    __ExpectCommand(13, address, SdResponseR1);
    __Expect(EventCardState);
    __ExpectBus(finalClockHz, SdBusPowerOn);
}

/////////////////////////////////////////////////////////////////////////////
// Register tests.

static void
TestCid(void)
{
    SdCid_t cid;

    assert(SdCidDecode(g_cid, &cid) == OS_EOK);
    assert(cid.ManufacturerId == 0x03);
    assert(strcmp(cid.OemId, "SD") == 0);
    assert(strcmp(cid.ProductName, "SU04G") == 0);
    assert(cid.ProductRevision == 0x80);
    assert(cid.SerialNumber == 0x12345678U);
    assert(cid.ManufactureYear == 2013);
    assert(cid.ManufactureMonth == 4);
    assert(SdCidDecode(NULL, &cid) == OS_EINVALPARAMS);
    assert(SdCidDecode(g_cid, NULL) == OS_EINVALPARAMS);
    printf("CID decoding passed\n");
}

static void
TestCsdVersion1(void)
{
    uint32_t words[4];
    SdCsd_t  csd;

    // The literal vector matches the fields written by the test encoder.
    memset(words, 0, sizeof(words));
    memcpy(words, g_csd2GiB, sizeof(words));
    __PutBits(words, 62, 12, 4095);
    __PutBits(words, 47, 3, 7);
    __PutBits(words, 80, 4, 10);
    assert(memcmp(words, g_csd2GiB, sizeof(words)) == 0);

    assert(SdCsdDecode(g_csd2GiB, &csd) == OS_EOK);
    assert(csd.Structure == SD_CSD_VERSION_1);
    assert(csd.TransferSpeed == 0x32);
    assert(csd.CommandClasses == 0x5F5);
    assert(csd.ReadBlockLengthLog2 == 10);
    assert(csd.DeviceSize == 4095);
    assert(csd.DeviceSizeMultiplier == 7);
    assert(csd.SectorCount == 4194304ULL);
    assert(!csd.PermanentWriteProtect && !csd.TemporaryWriteProtect);

    // 4 GiB with 2048-byte blocks, the largest version 1 card.
    __PutBits(words, 80, 4, 11);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(csd.SectorCount == 8388608ULL);

    // 512-byte blocks: C_SIZE 1000, C_SIZE_MULT 3. 1001 * 32 blocks.
    __PutBits(words, 80, 4, 9);
    __PutBits(words, 62, 12, 1000);
    __PutBits(words, 47, 3, 3);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(csd.SectorCount == 1001ULL * 32ULL);

    // Block lengths below 512 bytes and the reserved values are rejected.
    memset(&csd, 0xA5, sizeof(csd));
    __PutBits(words, 80, 4, 8);
    assert(SdCsdDecode(words, &csd) == OS_EPROTOCOL);
    __PutBits(words, 80, 4, 12);
    assert(SdCsdDecode(words, &csd) == OS_EPROTOCOL);
    assert(csd.Structure == 0xA5);

    // Write protect bits.
    memcpy(words, g_csd2GiB, sizeof(words));
    __PutBits(words, 13, 1, 1);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(csd.PermanentWriteProtect && !csd.TemporaryWriteProtect);
    __PutBits(words, 13, 1, 0);
    __PutBits(words, 12, 1, 1);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(!csd.PermanentWriteProtect && csd.TemporaryWriteProtect);
    printf("CSD version 1 decoding passed\n");
}

static void
TestCsdVersion2(void)
{
    uint32_t words[4];
    SdCsd_t  csd;

    __CsdVersion2(words, 0xFFFF);
    assert(memcmp(words, g_csd32GiB, sizeof(words)) == 0);
    assert(SdCsdDecode(g_csd32GiB, &csd) == OS_EOK);
    assert(csd.Structure == SD_CSD_VERSION_2);
    assert(csd.DeviceSize == 0xFFFF);
    assert(csd.ReadBlockLengthLog2 == 9);
    assert(csd.SectorCount == 67108864ULL);

    // 64 GiB.
    __CsdVersion2(words, 0x1FFFF);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(csd.SectorCount == 134217728ULL);

    // The largest C_SIZE gives 2^32 sectors, one more than 32-bit arithmetic
    // can hold.
    __CsdVersion2(words, 0x3FFFFF);
    assert(SdCsdDecode(words, &csd) == OS_EOK);
    assert(csd.SectorCount == 4294967296ULL);

    // Version 2 always has 512-byte read blocks.
    __CsdVersion2(words, 0xFFFF);
    __PutBits(words, 80, 4, 10);
    assert(SdCsdDecode(words, &csd) == OS_EPROTOCOL);

    // Version 3 (SDUC) and the reserved value are not supported, and the
    // output stays unchanged.
    memset(&csd, 0xA5, sizeof(csd));
    __CsdVersion2(words, 0xFFFF);
    __PutBits(words, 126, 2, 2);
    assert(SdCsdDecode(words, &csd) == OS_ENOTSUPPORTED);
    __PutBits(words, 126, 2, 3);
    assert(SdCsdDecode(words, &csd) == OS_ENOTSUPPORTED);
    assert(csd.Structure == 0xA5);
    assert(SdCsdDecode(NULL, &csd) == OS_EINVALPARAMS);
    assert(SdCsdDecode(words, NULL) == OS_EINVALPARAMS);
    printf("CSD version 2 decoding passed\n");
}

static void
TestScr(void)
{
    uint8_t data[8];
    SdScr_t scr;

    assert(SdScrDecode(g_scr, &scr) == OS_EOK);
    assert(scr.Structure == 0);
    assert(scr.SpecVersion == 2);
    assert(scr.SpecVersion3 && scr.SpecVersion4);
    assert(scr.SpecVersionX == 0);
    assert(!scr.DataAfterErase);
    assert(scr.Security == 3);
    assert(scr.BusWidths == (SD_SCR_BUS_WIDTH_1 | SD_SCR_BUS_WIDTH_4));
    assert(scr.CommandSupport == (SD_SCR_COMMAND_SPEED_CLASS | SD_SCR_COMMAND_SET_BLOCK_COUNT));

    // SD_SPECX spans bytes 2 and 3: bits 41:38 = 0b0110.
    memcpy(data, g_scr, sizeof(data));
    data[2] = 0x81;
    data[3] = 0x80;
    assert(SdScrDecode(data, &scr) == OS_EOK);
    assert(scr.SpecVersionX == 6);
    assert(!scr.SpecVersion4);

    // An unknown structure version and a missing one-line bus are rejected.
    memset(&scr, 0xA5, sizeof(scr));
    memcpy(data, g_scr, sizeof(data));
    data[0] = 0x12;
    assert(SdScrDecode(data, &scr) == OS_ENOTSUPPORTED);
    memcpy(data, g_scr, sizeof(data));
    data[1] = 0x34;
    assert(SdScrDecode(data, &scr) == OS_EPROTOCOL);
    assert(scr.Structure == 0xA5);
    assert(SdScrDecode(NULL, &scr) == OS_EINVALPARAMS);
    printf("SCR decoding passed\n");
}

static void
TestSdStatus(void)
{
    uint8_t    data[64];
    SdStatus_t status;

    // Four lines (0b10 in bits 511:510), secured mode (bit 509) and card
    // type 1 (bits 495:480, bytes 2 and 3).
    memset(data, 0, sizeof(data));
    data[0] = 0xA0;
    data[3] = 0x01;
    assert(SdStatusDecode(data, &status) == OS_EOK);
    assert(status.BusWidth == SD_STATUS_BUS_WIDTH_4);
    assert(status.SecuredMode);
    assert(status.CardType == 1);

    // One line, nothing else set. The other bits of byte 0 do not leak in.
    data[0] = 0x1F;
    data[3] = 0x00;
    assert(SdStatusDecode(data, &status) == OS_EOK);
    assert(status.BusWidth == SD_STATUS_BUS_WIDTH_1);
    assert(!status.SecuredMode && status.CardType == 0);

    // The reserved widths 1 and 3 are rejected, and the output stays as it was.
    memset(&status, 0xA5, sizeof(status));
    data[0] = 0x40;
    assert(SdStatusDecode(data, &status) == OS_EPROTOCOL);
    data[0] = 0xC0;
    assert(SdStatusDecode(data, &status) == OS_EPROTOCOL);
    assert(status.BusWidth == 0xA5);
    assert(SdStatusDecode(NULL, &status) == OS_EINVALPARAMS);
    assert(SdStatusDecode(data, NULL) == OS_EINVALPARAMS);
    printf("SD status decoding passed\n");
}

static void
TestSwitchStatus(void)
{
    uint8_t          data[64];
    SdSwitchStatus_t status;

    // 200 mA (bits 511:496), group 1 supports functions 0, 1 and 15 (bits
    // 415:400), function 1 selected (bits 379:376), group 2 bits set around
    // it, data structure version 1 (bits 375:368).
    memset(data, 0, sizeof(data));
    data[0] = 0x00;
    data[1] = 0xC8;
    data[12] = 0x80;
    data[13] = 0x03;
    data[16] = 0xF1;
    data[17] = 0x01;
    assert(SdSwitchStatusDecode(data, &status) == OS_EOK);
    assert(status.MaximumCurrentMa == 200);
    assert(status.Group1Support == 0x8003);
    assert(status.Group1Selection == SD_SWITCH_ACCESS_HIGH_SPEED);
    assert(status.StructureVersion == SD_SWITCH_STATUS_VERSION_1);

    // The error value, and version 0.
    data[16] = 0x0F;
    data[17] = 0x00;
    assert(SdSwitchStatusDecode(data, &status) == OS_EOK);
    assert(status.Group1Selection == SD_SWITCH_FUNCTION_ERROR);
    assert(status.StructureVersion == SD_SWITCH_STATUS_VERSION_0);

    // Unknown versions and a group 1 without default speed are rejected,
    // and the output stays as it was.
    memset(&status, 0xA5, sizeof(status));
    data[17] = 0x02;
    assert(SdSwitchStatusDecode(data, &status) == OS_ENOTSUPPORTED);
    data[17] = 0x00;
    data[13] = 0x02;
    assert(SdSwitchStatusDecode(data, &status) == OS_EPROTOCOL);
    assert(status.Group1Selection == 0xA5);
    assert(SdSwitchStatusDecode(NULL, &status) == OS_EINVALPARAMS);
    assert(SdSwitchStatusDecode(data, NULL) == OS_EINVALPARAMS);
    printf("Switch status decoding passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Sequence runner tests.

#define SCRIPT_STEPS    4
#define SCRIPT_VERDICTS 8

struct ScriptStep {
    SdStepStart_t   Start;
    SdStepVerdict_t Verdicts[SCRIPT_VERDICTS];
    int             VerdictCount;
    int             StartCalls;
    int             FinishCalls;
};

static struct ScriptStep g_script[SCRIPT_STEPS];
static SdSequence_t      g_sequence;
static int               g_runnerContext;
static int               g_runnerDoneCount;
static oserr_t           g_runnerStatus;
// Order in which the steps started, as step numbers.
static int               g_startOrder[64];
static int               g_startOrderCount;

static SdStepStart_t
__ScriptStart(
    _In_ int          index,
    _In_ void*        context,
    _In_ SdRequest_t* request)
{
    struct ScriptStep* step = &g_script[index];

    assert(context == &g_runnerContext);
    assert(request == &g_sequence.Request);
    // The runner hands every step a zeroed request.
    assert(request->Opcode == 0 && request->Argument == 0 && request->Complete == NULL);
    step->StartCalls++;
    assert(g_startOrderCount < 64);
    g_startOrder[g_startOrderCount++] = index;
    if (step->Start.Kind == SdStepStartKindSubmit) {
        request->Opcode = (uint8_t)(10 + index);
        request->ResponseType = SdResponseR1;
        request->CommandTimeoutUs = 1000;
    }
    return step->Start;
}

static SdStepVerdict_t
__ScriptFinish(
    _In_ int               index,
    _In_ void*             context,
    _In_ const SdResult_t* result)
{
    struct ScriptStep* step = &g_script[index];
    int                verdict = step->FinishCalls;

    assert(context == &g_runnerContext);
    assert(result == &g_sequence.Request.Result);
    assert(step->VerdictCount > 0);
    if (verdict >= step->VerdictCount) {
        verdict = step->VerdictCount - 1;
    }
    step->FinishCalls++;
    return step->Verdicts[verdict];
}

#define SCRIPT_STEP_FUNCTIONS(index)                                                     \
    static SdStepStart_t __Step##index##Start(void* context, SdRequest_t* request)        \
    {                                                                                     \
        return __ScriptStart(index, context, request);                                    \
    }                                                                                     \
    static SdStepVerdict_t __Step##index##Finish(void* context, const SdResult_t* result) \
    {                                                                                     \
        return __ScriptFinish(index, context, result);                                    \
    }

SCRIPT_STEP_FUNCTIONS(0)
SCRIPT_STEP_FUNCTIONS(1)
SCRIPT_STEP_FUNCTIONS(2)
SCRIPT_STEP_FUNCTIONS(3)

static const SdStep_t g_scriptSteps[SCRIPT_STEPS] = {
    { "first",  __Step0Start, __Step0Finish },
    { "second", __Step1Start, __Step1Finish },
    { "third",  __Step2Start, __Step2Finish },
    { "fourth", __Step3Start, __Step3Finish },
};

static void
__RunnerDone(
    _In_ void*   context,
    _In_ oserr_t status)
{
    assert(context == &g_runnerContext);
    assert(g_runnerDoneCount == 0);
    assert(g_sequence.State == SdSequenceStateIdle);
    g_runnerDoneCount++;
    g_runnerStatus = status;
}

// Every step submits and goes on to the next one, unless a test changes it.
static void
__ResetScript(void)
{
    int i;

    __ResetHost();
    memset(g_script, 0, sizeof(g_script));
    for (i = 0; i < SCRIPT_STEPS; i++) {
        g_script[i].Start = SdStartSubmit();
        g_script[i].Verdicts[0] = SdStepNext();
        g_script[i].VerdictCount = 1;
    }
    memset(&g_sequence, 0, sizeof(g_sequence));
    g_runnerDoneCount = 0;
    g_runnerStatus = OS_EUNKNOWN;
    g_startOrderCount = 0;
}

static void
__StartScript(void)
{
    assert(SdSequenceStart(
        &g_sequence, &g_host, "test", g_scriptSteps, SCRIPT_STEPS, __RunnerDone, &g_runnerContext) == OS_EOK);
}

// Answers every request with success and runs every delay until the
// sequence ends.
static void
__RunScript(void)
{
    int guard;

    for (guard = 0; guard < 1000 && g_runnerDoneCount == 0; guard++) {
        if (g_pending != NULL) {
            SdResult_t result = __Success(0);
            __Complete(&result);
        } else if (g_scheduled != NULL) {
            __RunScheduled();
        } else {
            break;
        }
    }
    assert(g_runnerDoneCount == 1);
    assert(g_pending == NULL && g_scheduled == NULL);
}

static void
__CheckStartOrder(
    _In_ const int* order,
    _In_ int        count)
{
    int i;

    assert(g_startOrderCount == count);
    for (i = 0; i < count; i++) {
        assert(g_startOrder[i] == order[i]);
    }
}

static void
TestRunnerNext(void)
{
    static const int order[] = { 0, 1, 2, 3 };
    int              i;

    __ResetScript();
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    __CheckStartOrder(order, 4);
    assert(g_eventCount == 4);
    for (i = 0; i < 4; i++) {
        assert(g_events[i].Kind == EventCommand && g_events[i].Request.Opcode == 10 + i);
        assert(g_script[i].FinishCalls == 1);
    }
    assert(g_errorLogs == 0);

    // A finished sequence can start again.
    g_runnerDoneCount = 0;
    g_startOrderCount = 0;
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK && g_startOrderCount == 4);
    printf("Runner next passed\n");
}

static void
TestRunnerGoTo(void)
{
    static const int orderNow[] = { 0, 1, 0, 1, 2, 3 };
    static const int orderLater[] = { 0, 1, 1, 2, 3 };

    // Without a delay the target runs at once, and nothing is scheduled.
    __ResetScript();
    g_script[1].Verdicts[0] = SdStepGoTo(0, 0);
    g_script[1].Verdicts[1] = SdStepNext();
    g_script[1].VerdictCount = 2;
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    __CheckStartOrder(orderNow, 6);
    assert(g_eventCount == 6);

    // With a delay the runner schedules it and runs the target afterwards.
    __ResetScript();
    g_script[1].Verdicts[0] = SdStepGoTo(1, 500);
    g_script[1].Verdicts[1] = SdStepNext();
    g_script[1].VerdictCount = 2;
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    __CheckStartOrder(orderLater, 5);
    assert(g_events[2].Kind == EventDelay && g_events[2].DelayUs == 500);
    assert(g_events[3].Kind == EventCommand && g_events[3].Request.Opcode == 11);
    assert(g_nowUs == 500);

    // A target outside the table fails the step.
    __ResetScript();
    g_script[0].Verdicts[0] = SdStepGoTo(SCRIPT_STEPS, 0);
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EINVALPARAMS);
    assert(g_errorLogs == 1 && strstr(g_lastError, "first") != NULL);
    printf("Runner go to passed\n");
}

static void
TestRunnerStartKinds(void)
{
    static const int order[] = { 0, 1, 2, 3 };

    // Done and skip go on at once, a delay goes on after the delay. Only the
    // last step sends a command, and only it gets a Finish call.
    __ResetScript();
    g_script[0].Start = SdStartDone();
    g_script[1].Start = SdStartSkip();
    g_script[2].Start = SdStartDelay(300);
    __StartScript();
    assert(g_runnerDoneCount == 0);
    assert(g_eventCount == 1 && g_events[0].Kind == EventDelay && g_events[0].DelayUs == 300);
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    __CheckStartOrder(order, 4);
    assert(g_eventCount == 2 && g_events[1].Kind == EventCommand && g_events[1].Request.Opcode == 13);
    assert(g_script[0].FinishCalls == 0 && g_script[1].FinishCalls == 0 && g_script[2].FinishCalls == 0);
    assert(g_script[3].FinishCalls == 1);

    // Steps that send nothing can end the sequence before done returns.
    __ResetScript();
    g_script[0].Start = SdStartDone();
    g_script[1].Start = SdStartDone();
    g_script[2].Start = SdStartSkip();
    g_script[3].Start = SdStartDone();
    __StartScript();
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_EOK);
    assert(g_eventCount == 0);

    // A delay on the last step ends the sequence once the delay is over.
    __ResetScript();
    g_script[3].Start = SdStartDelay(40);
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    assert(g_events[g_eventCount - 1].Kind == EventDelay && g_nowUs == 40);
    printf("Runner start kinds passed\n");
}

static void
TestRunnerFailures(void)
{
    // A failed Finish stops the sequence, logs the step name and reports the error.
    __ResetScript();
    g_script[1].Verdicts[0] = SdStepFail(OS_EDEVFAULT);
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EDEVFAULT);
    assert(g_script[2].StartCalls == 0 && g_script[3].StartCalls == 0);
    assert(g_errorLogs == 1);
    assert(strstr(g_lastError, "test") != NULL && strstr(g_lastError, "second") != NULL);
    // The line carries the command and its result, because the host engine
    // logs a command the card did not answer only as a trace.
    assert(strstr(g_lastError, "CMD11 (argument 0x00000000) ended with 0, transport 0x0000") != NULL);
    assert(g_sequence.Failure == SdSequenceFailureCommand);

    // A failed Start does the same, before done returns.
    __ResetScript();
    g_script[0].Start = SdStartFail(OS_ENOTSUPPORTED);
    __StartScript();
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_ENOTSUPPORTED);
    assert(g_eventCount == 0 && g_errorLogs == 1 && strstr(g_lastError, "first") != NULL);
    assert(g_sequence.Failure == SdSequenceFailureHost);

    // A failure without an error code still reports an error.
    __ResetScript();
    g_script[0].Verdicts[0] = SdStepFail(OS_EOK);
    __StartScript();
    __RunScript();
    assert(g_runnerStatus != OS_EOK);

    // End stops early with the given outcome and logs nothing.
    __ResetScript();
    g_script[0].Verdicts[0] = SdStepEnd(OS_ENOENT);
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_ENOENT && g_errorLogs == 0 && g_script[1].StartCalls == 0);
    assert(g_sequence.Failure == SdSequenceFailureNone);

    // Finish ends successfully and skips the remaining steps.
    __ResetScript();
    g_script[1].Verdicts[0] = SdStepFinish();
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EOK && g_script[2].StartCalls == 0);

    // A rejected submission fails the step with the host's error.
    __ResetScript();
    g_submitFailAt = 2;
    g_submitFailStatus = OS_EBUSY;
    __StartScript();
    __RunScript();
    assert(g_runnerStatus == OS_EBUSY && g_errorLogs == 1 && strstr(g_lastError, "second") != NULL);
    assert(g_script[1].FinishCalls == 0);
    assert(g_sequence.Failure == SdSequenceFailureHost);

    // A failed Schedule call fails the step.
    __ResetScript();
    g_script[0].Start = SdStartDelay(10);
    g_scheduleStatus = OS_EINVALPARAMS;
    __StartScript();
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_EINVALPARAMS);
    assert(g_sequence.Failure == SdSequenceFailureHost);
    printf("Runner failures passed\n");
}

static void
TestRunnerHardwareRunning(void)
{
    SdResult_t result;

    // Without proof that the hardware stopped, Finish is not called and the
    // sequence ends with its own error.
    __ResetScript();
    __StartScript();
    result = __Success(0);
    result.HardwareStopped = false;
    __Complete(&result);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == SD_SEQUENCE_HARDWARE_RUNNING);
    assert(g_script[0].FinishCalls == 0 && g_script[1].StartCalls == 0);
    assert(g_errorLogs == 1 && strstr(g_lastError, "first") != NULL);

    // The same while a cancellation is on its way.
    __ResetScript();
    __StartScript();
    assert(SdSequenceCancel(&g_sequence) == OS_EOK);
    result = __Success(0);
    result.Status = OS_ECANCELLED;
    result.HardwareStopped = false;
    __Complete(&result);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == SD_SEQUENCE_HARDWARE_RUNNING);
    printf("Runner hardware not stopped passed\n");
}

static void
TestRunnerCancel(void)
{
    SdResult_t result;

    // A request in flight is stopped, and done follows its completion.
    __ResetScript();
    __StartScript();
    assert(SdSequenceCancel(&g_sequence) == OS_EOK);
    assert(g_runnerDoneCount == 0);
    assert(g_eventCount == 2 && g_events[1].Kind == EventStop);
    assert(SdSequenceCancel(&g_sequence) == OS_EOK);
    assert(g_eventCount == 2);
    result = __Success(0);
    result.Status = OS_ECANCELLED;
    __Complete(&result);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_ECANCELLED);
    assert(g_script[0].FinishCalls == 0 && g_script[1].StartCalls == 0);
    assert(g_errorLogs == 0);
    assert(SdSequenceCancel(&g_sequence) == OS_ENOENT);

    // A request that finished before the stop took effect still ends the
    // sequence as cancelled.
    __ResetScript();
    __StartScript();
    assert(SdSequenceCancel(&g_sequence) == OS_EOK);
    result = __Success(0);
    __Complete(&result);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_ECANCELLED);
    assert(g_script[0].FinishCalls == 0);

    // A pending delay is removed, and done is called at once.
    __ResetScript();
    g_script[0].Start = SdStartDelay(1000);
    __StartScript();
    assert(g_scheduled != NULL);
    assert(SdSequenceCancel(&g_sequence) == OS_EOK);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_ECANCELLED);
    assert(g_scheduled == NULL);
    assert(g_eventCount == 2 && g_events[1].Kind == EventCancelWork);
    assert(g_script[1].StartCalls == 0);
    assert(SdSequenceCancel(&g_sequence) == OS_ENOENT);

    // Cancel on a sequence that never ran.
    __ResetScript();
    assert(SdSequenceCancel(&g_sequence) == OS_ENOENT);
    printf("Runner cancellation passed\n");
}

static void
TestRunnerArguments(void)
{
    SdStep_t broken[2];

    __ResetScript();
    memcpy(broken, g_scriptSteps, sizeof(broken));
    broken[1].Start = NULL;
    assert(SdSequenceStart(&g_sequence, &g_host, "test", broken, 2, __RunnerDone, &g_runnerContext) == OS_EINVALPARAMS);
    broken[1] = g_scriptSteps[1];
    broken[0].Name = NULL;
    assert(SdSequenceStart(&g_sequence, &g_host, "test", broken, 2, __RunnerDone, &g_runnerContext) == OS_EINVALPARAMS);
    assert(SdSequenceStart(&g_sequence, &g_host, "test", g_scriptSteps, 0, __RunnerDone, &g_runnerContext) == OS_EINVALPARAMS);
    assert(SdSequenceStart(&g_sequence, &g_host, "test", g_scriptSteps, 4, NULL, &g_runnerContext) == OS_EINVALPARAMS);
    assert(g_runnerDoneCount == 0 && g_eventCount == 0);

    // A step that submits without a Finish function fails.
    broken[0] = g_scriptSteps[0];
    broken[0].Finish = NULL;
    assert(SdSequenceStart(&g_sequence, &g_host, "test", broken, 1, __RunnerDone, &g_runnerContext) == OS_EOK);
    assert(g_runnerDoneCount == 1 && g_runnerStatus == OS_EINVALPARAMS);

    // A running sequence cannot start again.
    __ResetScript();
    __StartScript();
    assert(SdSequenceStart(&g_sequence, &g_host, "test", g_scriptSteps, 4, __RunnerDone, &g_runnerContext) == OS_EBUSY);
    __RunScript();
    assert(g_runnerStatus == OS_EOK);
    printf("Runner arguments passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Card setup tests.

static void
TestSetupVersion2(void)
{
    struct CardModel model;

    __ResetHost();
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectVersion2Start(0x40300000U);
    __ExpectIdentification(false, 25000000U);
    __CheckEvents();

    assert(g_setupResult == SdCardSetupReady && g_setupStatus == OS_EOK);
    assert(g_card.State == SdCardStateReady);
    assert(g_card.Host == &g_host);
    assert(g_card.MediaGeneration == 1);
    assert(g_card.RelativeAddress == CARD_ADDRESS);
    assert(g_card.SectorCount == 67108864ULL);
    assert(g_card.Addressing == SdCardAddressingSectors);
    assert(g_card.Width == SdBusWidth1 && g_card.Timing == SdBusTimingDefault);
    assert(g_card.ClockHz == 25000000U);
    assert(!g_card.WriteProtected);
    assert(strcmp(g_card.Identity.ProductName, "SU04G") == 0);
    assert(g_card.Identity.SerialNumber == 0x12345678U);
    assert(g_card.CardData.Structure == SD_CSD_VERSION_2);
    assert(g_card.Configuration.BusWidths == (SD_SCR_BUS_WIDTH_1 | SD_SCR_BUS_WIDTH_4));
    // One line when setup starts and one with the card, nothing else.
    assert(g_errorLogs == 0 && g_traceLogs == 2);

    // The next setup is a new medium, and a running setup cannot restart.
    __ModelVersion2(&model);
    __StartSetup();
    assert(g_card.MediaGeneration == 2);
    assert(SdCardSetup(&g_card, &g_host, __SetupDone, &g_setupContext) == OS_EBUSY);
    __RunCard(&model);
    assert(g_setupResult == SdCardSetupReady);
    printf("Setup of a version 2 high capacity card passed\n");
}

static void
TestSetupLegacy(void)
{
    struct CardModel model;

    // No answer to CMD8: no HCS in ACMD41, byte addressing and CMD16. The
    // CMD55 after CMD8 reports ILLEGAL_COMMAND, which is no error.
    __ResetHost();
    __ModelLegacy(&model);
    __Setup(&model);
    __ExpectPowerCycle(400000, POWER_ON_BASE_US + 185);
    __ExpectCommand(0, 0, SdResponseNone);
    __ExpectCommand(8, 0x1AA, SdResponseR7);
    __ExpectCommand(55, 0, SdResponseR1);
    __ExpectCommand(41, 0x00300000U, SdResponseR3);
    __ExpectIdentification(true, 25000000U);
    __CheckEvents();

    assert(g_setupResult == SdCardSetupReady);
    assert(g_card.Addressing == SdCardAddressingBytes);
    assert(g_card.SectorCount == 4194304ULL);
    assert(g_card.CardData.Structure == SD_CSD_VERSION_1);
    assert(g_errorLogs == 0);
    printf("Setup of a legacy byte addressed card passed\n");
}

static void
TestSetupInterfaceCondition(void)
{
    struct CardModel model;

    // A wrong echo fails setup right after CMD8.
    __ResetHost();
    __ModelVersion2(&model);
    model.WrongEcho = true;
    __Setup(&model);
    __ExpectPowerCycle(400000, POWER_ON_BASE_US + 185);
    __ExpectCommand(0, 0, SdResponseNone);
    __ExpectCommand(8, 0x1AA, SdResponseR7);
    __CheckEvents();
    __ExpectSetupFailed(OS_EPROTOCOL, "CMD8");

    // A CRC error is no legacy card.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 2;
    model.FaultStatus = OS_EDEVFAULT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_CRC;
    __Setup(&model);
    __CheckEvents();
    __ExpectSetupFailed(OS_EDEVFAULT, "CMD8");

    // Neither is a timeout of the host's own deadline, or a timeout together
    // with another error.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 2;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_DEADLINE;
    __Setup(&model);
    __ExpectSetupFailed(OS_ETIMEOUT, "CMD8");
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 2;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_TIMEOUT | SD_TRANSPORT_COMMAND_CRC;
    __Setup(&model);
    __ExpectSetupFailed(OS_ETIMEOUT, "CMD8");
    printf("Setup CMD8 outcomes passed\n");
}

static void
TestSetupOperatingCondition(void)
{
    struct CardModel model;
    int              i;

    // Ready after three busy answers: CMD55 before every ACMD41, and a 10 ms
    // delay before every repeat.
    __ResetHost();
    __ModelVersion2(&model);
    model.BusyAnswers = 3;
    __Setup(&model);
    __ExpectVersion2Start(0x40300000U);
    for (i = 0; i < 3; i++) {
        __ExpectDelay(OCR_RETRY_US);
        __ExpectCommand(55, 0, SdResponseR1);
        __ExpectCommand(41, 0x40300000U, SdResponseR3);
    }
    __ExpectIdentification(false, 25000000U);
    __CheckEvents();
    assert(g_setupResult == SdCardSetupReady);

    // A card that never gets ready: one attempt plus 100 repeats, each 10 ms
    // apart, which uses up the one second budget.
    __ResetHost();
    __ModelVersion2(&model);
    model.BusyAnswers = -1;
    __Setup(&model);
    __ExpectVersion2Start(0x40300000U);
    for (i = 0; i < 100; i++) {
        __ExpectDelay(OCR_RETRY_US);
        __ExpectCommand(55, 0, SdResponseR1);
        __ExpectCommand(41, 0x40300000U, SdResponseR3);
    }
    __CheckEvents();
    assert(__CountCommands(41) == 101 && __CountCommands(55) == 101);
    assert(g_nowUs == 36185U + 10000U + 100U * OCR_RETRY_US);
    __ExpectSetupFailed(OS_ETIMEOUT, "ACMD41");

    // Only the voltage bits of the host's window go into the argument.
    __ResetHost();
    g_host.Limits.SupplyVoltages = 0x00FF8080U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_events[7].Kind == EventCommand && g_events[7].Request.Opcode == 41);
    assert(g_events[7].Request.Argument == 0x40FF8000U);
    assert(g_setupResult == SdCardSetupReady);

    // A host without any SD voltage cannot set up a card.
    __ResetHost();
    g_host.Limits.SupplyVoltages = 0x00000080U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(__CountCommands(41) == 0);
    __ExpectSetupHostFailed(OS_ENOTSUPPORTED, "ACMD41");

    // A high capacity answer from a card that did not answer CMD8 is wrong.
    __ResetHost();
    __ModelLegacy(&model);
    model.HighCapacity = true;
    model.CcsWithoutHcs = true;
    __Setup(&model);
    assert(__CountCommands(2) == 0);
    __ExpectSetupFailed(OS_EPROTOCOL, "ACMD41");
    printf("Setup ACMD41 outcomes passed\n");
}

static void
TestSetupNoCard(void)
{
    struct CardModel model;

    // An empty slot: no answer to CMD8 and CMD55. Three commands, no error.
    __ResetHost();
    memset(&model, 0, sizeof(model));
    model.Kind = CardEmpty;
    __Setup(&model);
    __ExpectPowerCycle(400000, POWER_ON_BASE_US + 185);
    __ExpectCommand(0, 0, SdResponseNone);
    __ExpectCommand(8, 0x1AA, SdResponseR7);
    __ExpectCommand(55, 0, SdResponseR1);
    __CheckEvents();
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);
    assert(g_card.State == SdCardStateNoCard);
    assert(g_errorLogs == 0 && g_traceLogs == 1);

    // A device that answers CMD55 without APP_CMD is no SD card either.
    __ResetHost();
    __ModelLegacy(&model);
    model.NoAppCommand = true;
    __Setup(&model);
    assert(g_eventCount == 7);
    assert(g_setupResult == SdCardSetupNoCard && g_errorLogs == 0);

    // Nor one that answers CMD55 but not ACMD41.
    __ResetHost();
    __ModelLegacy(&model);
    model.SilentOpCond = true;
    __Setup(&model);
    assert(g_eventCount == 8);
    assert(g_setupResult == SdCardSetupNoCard && g_errorLogs == 0);

    // A card that answered CMD8 and then stops answering has failed.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 3;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_TIMEOUT;
    __Setup(&model);
    __ExpectSetupFailed(OS_ETIMEOUT, "CMD55");
    __ResetHost();
    __ModelVersion2(&model);
    model.NoAppCommand = true;
    __Setup(&model);
    __ExpectSetupFailed(OS_EPROTOCOL, "CMD55");

    // And so has a card that answered ACMD41 before.
    __ResetHost();
    __ModelLegacy(&model);
    model.BusyAnswers = 1;
    model.FaultAt = 6;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_TIMEOUT;
    __Setup(&model);
    __ExpectSetupFailed(OS_ETIMEOUT, "ACMD41");
    printf("Setup of an empty slot passed\n");
}

static void
TestSetupRelativeAddress(void)
{
    struct CardModel model;
    uint32_t         address = (uint32_t)CARD_ADDRESS << 16;

    // Two zero addresses, then a valid one: CMD3 three times.
    __ResetHost();
    __ModelVersion2(&model);
    model.ZeroAddressAnswers = 2;
    __Setup(&model);
    __ExpectVersion2Start(0x40300000U);
    __ExpectCommand(2, 0, SdResponseR2);
    __ExpectCommand(3, 0, SdResponseR6);
    __ExpectCommand(3, 0, SdResponseR6);
    __ExpectCommand(3, 0, SdResponseR6);
    __ExpectCommand(9, address, SdResponseR2);
    __ExpectCommand(7, address, SdResponseR1b);
    __ExpectCommand(55, address, SdResponseR1);
    __ExpectCommand(51, 0, SdResponseR1);
    __ExpectCommand(13, address, SdResponseR1);
    __Expect(EventCardState);
    __ExpectBus(25000000U, SdBusPowerOn);
    __CheckEvents();
    assert(g_setupResult == SdCardSetupReady && g_card.RelativeAddress == CARD_ADDRESS);

    // Three zero addresses fail setup.
    __ResetHost();
    __ModelVersion2(&model);
    model.ZeroAddressAnswers = 3;
    __Setup(&model);
    assert(__CountCommands(3) == 3 && __CountCommands(9) == 0);
    __ExpectSetupFailed(OS_EPROTOCOL, "CMD3");

    // The ERROR bit of the R6 status fails setup.
    __ResetHost();
    __ModelVersion2(&model);
    model.CardErrorAt = 6;
    model.CardErrorBits = SD_R6_ERROR;
    __Setup(&model);
    assert(__CountCommands(9) == 0);
    __ExpectSetupFailed(OS_EDEVFAULT, "CMD3");
    printf("Setup relative address passed\n");
}

static void
TestSetupCardErrors(void)
{
    struct CardModel model;

    // An error bit in the R1b answer of CMD7 fails setup at CMD7.
    __ResetHost();
    __ModelVersion2(&model);
    model.CardErrorAt = 8;
    model.CardErrorBits = SD_R1_ERROR;
    __Setup(&model);
    assert(g_events[g_eventCount - 1].Request.Opcode == 7);
    __ExpectSetupFailed(OS_EDEVFAULT, "CMD7");

    // A card that is not in the transfer state when CMD13 runs.
    __ResetHost();
    __ModelVersion2(&model);
    model.FinalState = SD_STATE_STANDBY;
    __Setup(&model);
    __ExpectSetupFailed(OS_EPROTOCOL, "CMD13");

    // A CSD version that does not match the addressing from ACMD41.
    __ResetHost();
    __ModelVersion2(&model);
    memcpy(model.Csd, g_csd2GiB, sizeof(g_csd2GiB));
    __Setup(&model);
    __ExpectSetupFailed(OS_EPROTOCOL, "CMD9");

    // An SDUC card (CSD version 3).
    __ResetHost();
    __ModelVersion2(&model);
    __PutBits(model.Csd, 126, 2, 2);
    __Setup(&model);
    __ExpectSetupFailed(OS_ENOTSUPPORTED, "CMD9");

    // An SCR with an unknown structure version.
    __ResetHost();
    __ModelVersion2(&model);
    model.Scr[0] = 0x12;
    __Setup(&model);
    __ExpectSetupFailed(OS_ENOTSUPPORTED, "ACMD51");
    printf("Setup card errors passed\n");
}

// A transport error at every command of a full setup stops it right there.
static void
TestSetupTransportErrors(void)
{
    static const char* names[] = {
        "CMD0", "CMD8", "CMD55 APP_CMD for ACMD41", "ACMD41", "CMD2", "CMD3", "CMD9", "CMD7",
        "CMD55 APP_CMD for ACMD51", "ACMD51", "CMD13"
    };
    struct CardModel   model;
    int                i;

    for (i = 0; i < 11; i++) {
        __ResetHost();
        __ModelVersion2(&model);
        model.FaultAt = i + 1;
        model.FaultStatus = OS_EDEVFAULT;
        model.FaultTransport = SD_TRANSPORT_COMMAND_CRC;
        __Setup(&model);
        assert(g_submitCount == i + 1);
        __ExpectSetupFailed(OS_EDEVFAULT, names[i]);
    }

    // A removed card is reported as no card.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 5;
    model.FaultStatus = OS_ENOENT;
    model.FaultTransport = SD_TRANSPORT_CARD_REMOVED;
    __Setup(&model);
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);
    printf("Setup transport errors passed\n");
}

static void
TestSetupScrRead(void)
{
    struct CardModel model;

    // The SCR is read only after the ACMD51 completion, with HardwareStopped.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 10;
    model.FaultStatus = OS_EOK;
    model.FaultHardwareRunning = true;
    __Setup(&model);
    assert(g_setupResult == SdCardSetupFailed && g_setupStatus == SD_SEQUENCE_HARDWARE_RUNNING);
    assert(g_card.Configuration.BusWidths == 0);
    assert(g_errorLogs == 1 && strstr(g_lastError, "ACMD51") != NULL);

    // A control buffer that is not four-byte aligned.
    __ResetHost();
    g_controlBuffer.Segments[0].Address = CONTROL_ADDRESS + 2;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(__CountCommands(51) == 0);
    __ExpectSetupHostFailed(OS_EINVALPARAMS, "ACMD51");

    // A host without a control buffer.
    __ResetHost();
    g_host.ControlBuffer = NULL;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectSetupHostFailed(OS_ENOTSUPPORTED, "ACMD51");
    printf("Setup SCR read passed\n");
}

static void
TestSetupBus(void)
{
    struct CardModel model;

    // A BCM2711-like host: 200 kHz minimum and a base clock that gives
    // 396,825 Hz for a 400 kHz request. 74 cycles take 187 microseconds.
    __ResetHost();
    g_host.Limits.MinimumClockHz = 200000U;
    g_baseClockHz = 41666666U;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectVersion2Start(0x40300000U);
    g_expected[2].ClockHz = 400000U;
    g_expected[3].DelayUs = POWER_ON_BASE_US + 187U;
    __ExpectIdentification(false, 25000000U);
    __CheckEvents();
    assert(g_card.ClockHz == 41666666U / 2U);

    // A host whose fastest clock is below 400 kHz sets up at that clock, and
    // stays there.
    __ResetHost();
    g_host.Limits.MaximumClockHz = 250000U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_events[2].Kind == EventBus && g_events[2].Bus.ClockHz == 250000U);
    assert(g_events[3].DelayUs == POWER_ON_BASE_US + 296U);
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 250000U);
    assert(g_setupResult == SdCardSetupReady);

    // A host with a 20 MHz maximum raises the clock to 20 MHz only.
    __ResetHost();
    g_host.Limits.MaximumClockHz = 20000000U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 20000000U);
    assert(g_card.ClockHz == 20000000U);

    // A host that cannot run at 400 kHz or below cannot set up a card.
    __ResetHost();
    g_host.Limits.MinimumClockHz = 500000U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_submitCount == 0 && g_eventCount == 2);
    __ExpectSetupHostFailed(OS_ENOTSUPPORTED, "power on");

    // SetBus failures at power off, power on and the final clock raise.
    __ResetHost();
    g_setBusFailAt = 1;
    g_setBusFailStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "power off");
    __ResetHost();
    g_setBusFailAt = 2;
    g_setBusFailStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_submitCount == 0);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "power on");
    __ResetHost();
    g_setBusFailAt = 3;
    g_setBusFailStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "data clock");
    printf("Setup bus settings passed\n");
}

// Every step of setup that fails in the host, before a command reaches the
// card, reports SdCardSetupHostFailed with the host's error. A card command
// that fails right after a host step still reports SdCardSetupFailed.
static void
TestSetupHostFailures(void)
{
    static const struct {
        int         SetBusCall;
        const char* StepName;
        int         Submits;
    } setBusSteps[] = {
        { 1, "power off", 0 },
        { 2, "power on", 0 },
        { 3, "data clock", 11 },
    };
    struct CardModel model;
    unsigned int     i;

    // SetBus fails with OS_EDEVFAULT, as on a Raspberry Pi 4 whose firmware
    // cannot switch the card supply.
    for (i = 0; i < sizeof(setBusSteps) / sizeof(setBusSteps[0]); i++) {
        __ResetHost();
        g_setBusFailAt = setBusSteps[i].SetBusCall;
        g_setBusFailStatus = OS_EDEVFAULT;
        __ModelVersion2(&model);
        __Setup(&model);
        assert(g_submitCount == setBusSteps[i].Submits);
        __ExpectSetupHostFailed(OS_EDEVFAULT, setBusSteps[i].StepName);
        assert(g_card.Setup.Sequence.Failure == SdSequenceFailureHost);
    }

    // The host refuses a command: the card never sees it.
    __ResetHost();
    g_submitFailAt = 4;
    g_submitFailStatus = OS_EDEVFAULT;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectSetupHostFailed(OS_EDEVFAULT, "ACMD41");

    // A host that refuses because it knows the slot is empty: no card.
    __ResetHost();
    g_submitFailAt = 1;
    g_submitFailStatus = OS_ENOENT;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);

    // Reading the write protect switch fails.
    __ResetHost();
    g_cardStateStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "write protect");

    // A command that fails after the host steps is still the card's.
    __ResetHost();
    __ModelVersion2(&model);
    model.FaultAt = 1;
    model.FaultStatus = OS_EDEVFAULT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_CRC;
    __Setup(&model);
    __ExpectSetupFailed(OS_EDEVFAULT, "CMD0");
    assert(g_card.Setup.Sequence.Failure == SdSequenceFailureCommand);
    printf("Setup host failures passed\n");
}

static void
TestSetupWriteProtect(void)
{
    struct CardModel model;

    // The switch is set.
    __ResetHost();
    g_cardState = SD_CARD_STATE_PRESENT | SD_CARD_STATE_WRITE_PROTECTED;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupReady && g_card.WriteProtected);

    // The CSD says the card is protected for now.
    __ResetHost();
    __ModelVersion2(&model);
    __PutBits(model.Csd, 12, 1, 1);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupReady && g_card.WriteProtected);

    // No working card detect: the CSD decides.
    __ResetHost();
    g_cardState = SD_CARD_STATE_PRESENT | SD_CARD_STATE_WRITE_PROTECTED;
    g_cardStateStatus = OS_ENOTSUPPORTED;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupReady && !g_card.WriteProtected);

    // A host without a switch is not asked.
    __ResetHost();
    g_host.Limits.Flags = SD_HOST_FLAG_REMOVABLE;
    g_cardState = SD_CARD_STATE_PRESENT | SD_CARD_STATE_WRITE_PROTECTED;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupReady && !g_card.WriteProtected);
    assert(g_events[g_eventCount - 2].Kind == EventCommand);

    // The card is gone by the end of setup.
    __ResetHost();
    g_cardState = 0;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_setupResult == SdCardSetupNoCard && g_setupStatus == OS_ENOENT);
    printf("Setup write protection passed\n");
}

static void
TestSetupCancel(void)
{
    struct CardModel model;
    SdResult_t       result;
    int              guard;

    // Cancel while ACMD41 waits for its next attempt.
    __ResetHost();
    __ModelVersion2(&model);
    model.BusyAnswers = -1;
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup();
    for (guard = 0; guard < 20; guard++) {
        if (g_pending != NULL) {
            result = __Answer(&model, g_pending);
            __Complete(&result);
        } else if (g_scheduled != NULL && __CountCommands(41) < 2) {
            __RunScheduled();
        }
    }
    assert(g_scheduled != NULL && g_setupDoneCount == 0);
    assert(SdCardSetupCancel(&g_card) == OS_EOK);
    assert(g_setupDoneCount == 1 && g_setupResult == SdCardSetupFailed && g_setupStatus == OS_ECANCELLED);
    assert(g_scheduled == NULL && g_card.State == SdCardStateFailed && g_errorLogs == 0);

    // Cancel while CMD2 is with the host.
    __ResetHost();
    __ModelVersion2(&model);
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup();
    while (__CountCommands(2) == 0) {
        if (g_pending != NULL) {
            result = __Answer(&model, g_pending);
            __Complete(&result);
        } else {
            __RunScheduled();
        }
    }
    assert(g_pending != NULL);
    assert(SdCardSetupCancel(&g_card) == OS_EOK);
    assert(g_events[g_eventCount - 1].Kind == EventStop && g_setupDoneCount == 0);
    result = __Success(0);
    result.Status = OS_ECANCELLED;
    __Complete(&result);
    assert(g_setupDoneCount == 1 && g_setupStatus == OS_ECANCELLED);
    assert(SdCardSetupCancel(&g_card) == OS_ENOENT);
    printf("Setup cancellation passed\n");
}


/////////////////////////////////////////////////////////////////////////////
// Four data lines and high speed.

#define ADDRESS_ARG ((uint32_t)CARD_ADDRESS << 16)

// Command numbers of a version 2 setup with four lines and high speed.
#define NUMBER_WIDTH_APP     11
#define NUMBER_BUS_WIDTH     12
#define NUMBER_STATUS_APP    13
#define NUMBER_SD_STATUS     14
#define NUMBER_SWITCH_CHECK  15
#define NUMBER_SWITCH        16
#define NUMBER_SEND_STATUS   17

// A host wired for four lines, with high speed.
static void
__ResetFeatureHost(void)
{
    __ResetHost();
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4;
    g_host.Limits.Flags |= SD_HOST_FLAG_HIGH_SPEED;
}

// A version 2 setup from power on up to ACMD51.
static void
__ExpectUntilScr(void)
{
    __ExpectVersion2Start(0x40300000U);
    __ExpectCommand(2, 0, SdResponseR2);
    __ExpectCommand(3, 0, SdResponseR6);
    __ExpectCommand(9, ADDRESS_ARG, SdResponseR2);
    __ExpectCommand(7, ADDRESS_ARG, SdResponseR1b);
    __ExpectCommand(55, ADDRESS_ARG, SdResponseR1);
    __ExpectCommand(51, 0, SdResponseR1);
}

// The card switches first, then the host, then the SD status is read on
// four lines at the setup clock.
static void
__ExpectWidthSwitch(void)
{
    __ExpectCommand(55, ADDRESS_ARG, SdResponseR1);
    __ExpectCommand(6, 2, SdResponseR1);
    __ExpectBusWith(400000U, SdBusWidth4, SdBusTimingDefault, SdBusPowerOn);
    __ExpectCommand(55, ADDRESS_ARG, SdResponseR1);
    __ExpectCommand(13, 0, SdResponseR1);
}

// Check mode, switch mode, the 8 clock wait and then the host timing, still
// at the setup clock.
static void
__ExpectSpeedSwitch(
    _In_ enum SdBusWidth width)
{
    __ExpectCommand(6, SWITCH_CHECK_ARG, SdResponseR1);
    __ExpectCommand(6, SWITCH_SET_ARG, SdResponseR1);
    __ExpectDelay(SWITCH_WAIT_US);
    __ExpectBusWith(400000U, width, SdBusTimingHighSpeed, SdBusPowerOn);
}

// CMD13, the write protect switch and the data clock.
static void
__ExpectFinal(
    _In_ uint32_t         clockHz,
    _In_ enum SdBusWidth  width,
    _In_ enum SdBusTiming timing)
{
    __ExpectCommand(13, ADDRESS_ARG, SdResponseR1);
    __Expect(EventCardState);
    __ExpectBusWith(clockHz, width, timing, SdBusPowerOn);
}

static void
__ExpectReady(
    _In_ enum SdBusWidth  width,
    _In_ enum SdBusTiming timing,
    _In_ uint32_t         clockHz)
{
    if (g_setupResult != SdCardSetupReady) {
        fprintf(stderr, "setup result %d after \"%s\"\n", (int)g_setupResult, g_lastError);
    }
    assert(g_setupResult == SdCardSetupReady && g_setupStatus == OS_EOK);
    assert(g_card.State == SdCardStateReady);
    assert(g_card.Width == width && g_card.Timing == timing && g_card.ClockHz == clockHz);
    assert(g_errorLogs == 0);
}

static void
TestSetupFourLinesHighSpeed(void)
{
    struct CardModel model;

    __ResetFeatureHost();
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectSpeedSwitch(SdBusWidth4);
    __ExpectFinal(50000000U, SdBusWidth4, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 50000000U);
    assert(model.BusWidthArgument == 2 && model.AccessMode == 1);
    // Setup start, card ready, four lines and high-speed timing.
    assert(g_traceLogs == 4);

    // A byte addressed card takes the same path after CMD16.
    __ResetFeatureHost();
    __ModelLegacy(&model);
    __Setup(&model);
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 50000000U);
    printf("Setup with four data lines and high speed passed\n");
}

static void
TestSetupWithoutFeatures(void)
{
    struct CardModel model;

    // An SCR without four lines: no ACMD6, high speed on one line.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Scr[1] = 0x31;
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectSpeedSwitch(SdBusWidth1);
    __ExpectFinal(50000000U, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // A host wired for one line: the same.
    __ResetFeatureHost();
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectSpeedSwitch(SdBusWidth1);
    __ExpectFinal(50000000U, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // A host without high speed: four lines, no CMD6, 25 MHz.
    __ResetFeatureHost();
    g_host.Limits.Flags &= ~SD_HOST_FLAG_HIGH_SPEED;
    __ModelVersion2(&model);
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectFinal(25000000U, SdBusWidth4, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // A card that follows version 1.0 or 1.01 (SD_SPEC 0) has no CMD6.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Scr[0] = 0x00;
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectFinal(25000000U, SdBusWidth4, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // A card that follows version 1.10 (SD_SPEC 1) has CMD6.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Scr[0] = 0x01;
    __Setup(&model);
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 50000000U);

    // A card outside command class 10 (CSD bit 94) has no CMD6.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    __PutBits(model.Csd, 94, 1, 0);
    __Setup(&model);
    assert(__CountCommands(6) == 1);
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);
    printf("Setup without four lines or high speed passed\n");
}

static void
TestSetupSpeedNotOffered(void)
{
    struct CardModel model;

    // Check mode reports the error value: a card without high speed is
    // normal, a TRACE line and default speed.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Group1Support = 0x8001U;
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectCommand(6, SWITCH_CHECK_ARG, SdResponseR1);
    __ExpectFinal(25000000U, SdBusWidth4, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);
    assert(g_traceLogs == 4 && model.AccessMode == 0);

    // A card that names function 1 without listing it as supported.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Group1Support = 0x8001U;
    model.EchoSelection = true;
    __Setup(&model);
    assert(__CountCommands(6) == 2);
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // A check mode status the decoder rejects changes nothing either.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SwitchVersion = 7;
    __Setup(&model);
    assert(__CountCommands(6) == 2);
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // Nor does an error bit in the answer to check mode.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.CardErrorAt = NUMBER_SWITCH_CHECK;
    model.CardErrorBits = SD_R1_ERROR;
    __Setup(&model);
    assert(__CountCommands(6) == 2);
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // Switch mode reports the error value: the card stays at default speed.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SwitchSelection = 0xF;
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectCommand(6, SWITCH_CHECK_ARG, SdResponseR1);
    __ExpectCommand(6, SWITCH_SET_ARG, SdResponseR1);
    __ExpectFinal(25000000U, SdBusWidth4, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // Switch mode reports another function: the card's timing is unknown.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SwitchSelection = 0;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_EPROTOCOL, "CMD6 SWITCH_FUNC switch");
    printf("Setup when the card offers no high speed passed\n");
}

static void
TestSetupWidthRefused(void)
{
    struct CardModel model;

    // ACMD6 with an error bit: the card did not switch. One line, and high
    // speed goes on.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.CardErrorAt = NUMBER_BUS_WIDTH;
    model.CardErrorBits = SD_R1_ERROR;
    __Setup(&model);
    __ExpectUntilScr();
    __ExpectCommand(55, ADDRESS_ARG, SdResponseR1);
    __ExpectCommand(6, 2, SdResponseR1);
    __ExpectSpeedSwitch(SdBusWidth1);
    __ExpectFinal(50000000U, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // ACMD6 without an answer, as for an illegal command.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SilentBusWidth = true;
    __Setup(&model);
    assert(__CountCommands(6) == 3);
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // CMD55 before ACMD6 with an error bit.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.CardErrorAt = NUMBER_WIDTH_APP;
    model.CardErrorBits = SD_R1_ERROR;
    __Setup(&model);
    assert(__CountCommands(6) == 2);
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // After a refused width, a later failure is no width failure.
    __ResetFeatureHost();
    g_host.Limits.Flags &= ~SD_HOST_FLAG_HIGH_SPEED;
    __ModelVersion2(&model);
    model.SilentBusWidth = true;
    model.FinalState = SD_STATE_STANDBY;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupFailed, OS_EPROTOCOL, "CMD13");
    printf("Setup when the card refuses four lines passed\n");
}

static void
TestSetupFeatureFailures(void)
{
    static const struct {
        int                    Number;
        uint32_t               Transport;
        enum SdCardSetupResult Result;
        const char*            Name;
    } faults[] = {
        { NUMBER_WIDTH_APP,    SD_TRANSPORT_COMMAND_CRC, SdCardSetupBusWidthFailed,  "CMD55 APP_CMD for ACMD6" },
        { NUMBER_BUS_WIDTH,    SD_TRANSPORT_COMMAND_CRC, SdCardSetupBusWidthFailed,  "ACMD6" },
        { NUMBER_STATUS_APP,   SD_TRANSPORT_COMMAND_CRC, SdCardSetupBusWidthFailed,  "CMD55 APP_CMD for ACMD13" },
        { NUMBER_SD_STATUS,    SD_TRANSPORT_DATA_CRC,    SdCardSetupBusWidthFailed,  "ACMD13" },
        { NUMBER_SWITCH_CHECK, SD_TRANSPORT_DATA_CRC,    SdCardSetupHighSpeedFailed, "CMD6 SWITCH_FUNC check" },
        { NUMBER_SWITCH,       SD_TRANSPORT_COMMAND_CRC, SdCardSetupHighSpeedFailed, "CMD6 SWITCH_FUNC switch" },
        { NUMBER_SEND_STATUS,  SD_TRANSPORT_COMMAND_CRC, SdCardSetupHighSpeedFailed, "CMD13" },
    };
    struct CardModel model;
    unsigned int     i;

    // A transport error at each new command, and at CMD13 after high speed.
    for (i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        __ResetFeatureHost();
        __ModelVersion2(&model);
        model.FaultAt = faults[i].Number;
        model.FaultStatus = OS_EDEVFAULT;
        model.FaultTransport = faults[i].Transport;
        __Setup(&model);
        assert(g_submitCount == faults[i].Number);
        __ExpectSetupResult(faults[i].Result, OS_EDEVFAULT, faults[i].Name);
    }

    // A data timeout in check mode: the card may still be sending.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.FaultAt = NUMBER_SWITCH_CHECK;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_DATA_TIMEOUT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_ETIMEOUT, "CMD6 SWITCH_FUNC check");

    // No answer to switch mode.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.FaultAt = NUMBER_SWITCH;
    model.FaultStatus = OS_ETIMEOUT;
    model.FaultTransport = SD_TRANSPORT_COMMAND_TIMEOUT;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_ETIMEOUT, "CMD6 SWITCH_FUNC switch");

    // A switch mode status the decoder rejects.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.CorruptSwitchMode = true;
    __Setup(&model);
    assert(__CountCommands(6) == 3);
    __ExpectSetupResult(SdCardSetupHighSpeedFailed, OS_ENOTSUPPORTED, "CMD6 SWITCH_FUNC switch");

    // The SD status says one line after the switch, or a reserved width.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SdStatusWidth = 0;
    __Setup(&model);
    assert(__CountCommands(6) == 1);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "ACMD13");
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.SdStatusWidth = 3;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "ACMD13");

    // CMD13 fails after four lines without high speed: a width failure.
    __ResetFeatureHost();
    g_host.Limits.Flags &= ~SD_HOST_FLAG_HIGH_SPEED;
    __ModelVersion2(&model);
    model.FinalState = SD_STATE_STANDBY;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "CMD13");

    // And after a card that offers no high speed.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.Group1Support = 0x8001U;
    model.FinalState = SD_STATE_STANDBY;
    __Setup(&model);
    __ExpectSetupResult(SdCardSetupBusWidthFailed, OS_EPROTOCOL, "CMD13");

    // A request without stop proof in the width part needs a host reset,
    // whatever the part.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    model.FaultAt = NUMBER_SD_STATUS;
    model.FaultStatus = OS_EOK;
    model.FaultHardwareRunning = true;
    __Setup(&model);
    assert(g_setupResult == SdCardSetupFailed && g_setupStatus == SD_SEQUENCE_HARDWARE_RUNNING);

    // SetBus fails after the card switched: a host failure.
    __ResetFeatureHost();
    g_setBusFailAt = 3;
    g_setBusFailStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_submitCount == NUMBER_BUS_WIDTH);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "host bus width");
    __ResetFeatureHost();
    g_setBusFailAt = 4;
    g_setBusFailStatus = OS_ETIMEOUT;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_submitCount == NUMBER_SWITCH);
    __ExpectSetupHostFailed(OS_ETIMEOUT, "host timing");
    printf("Setup failures with four lines and high speed passed\n");
}

static void
TestSetupFeatureLimits(void)
{
    SdCardSetupOptions_t options;
    struct CardModel     model;

    // Width limited to one line: no ACMD6, high speed on one line.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    memset(&options, 0, sizeof(options));
    options.WidthLimit = SdCardWidthLimitOneLine;
    __SetupWithOptions(&model, &options);
    __ExpectUntilScr();
    __ExpectSpeedSwitch(SdBusWidth1);
    __ExpectFinal(50000000U, SdBusWidth1, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth1, SdBusTimingHighSpeed, 50000000U);

    // Timing limited to default speed: four lines, no CMD6, 25 MHz.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    memset(&options, 0, sizeof(options));
    options.TimingLimit = SdCardTimingLimitDefaultSpeed;
    __SetupWithOptions(&model, &options);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectFinal(25000000U, SdBusWidth4, SdBusTimingDefault);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingDefault, 25000000U);

    // A data clock limit below 50 MHz caps high speed. The fake host makes
    // 25 MHz of a 30 MHz request.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    memset(&options, 0, sizeof(options));
    options.DataClockLimitHz = 30000000U;
    __SetupWithOptions(&model, &options);
    __ExpectUntilScr();
    __ExpectWidthSwitch();
    __ExpectSpeedSwitch(SdBusWidth4);
    __ExpectFinal(30000000U, SdBusWidth4, SdBusTimingHighSpeed);
    __CheckEvents();
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 25000000U);

    // A limit above 50 MHz changes nothing, and the host maximum caps the
    // rate: QEMU's sdhci-pci has a 52 MHz base clock.
    __ResetFeatureHost();
    g_host.Limits.MaximumClockHz = 52000000U;
    g_baseClockHz = 52000000U;
    __ModelVersion2(&model);
    memset(&options, 0, sizeof(options));
    options.DataClockLimitHz = 80000000U;
    __SetupWithOptions(&model, &options);
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 50000000U);
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 26000000U);
    __ResetFeatureHost();
    g_host.Limits.MaximumClockHz = 40000000U;
    __ModelVersion2(&model);
    __Setup(&model);
    assert(g_events[g_eventCount - 1].Bus.ClockHz == 40000000U);
    __ExpectReady(SdBusWidth4, SdBusTimingHighSpeed, 33333333U);
    printf("Setup limits for four lines and high speed passed\n");
}

static void
TestSetupFeatureCancel(void)
{
    struct CardModel model;
    SdResult_t       result;

    // A setup cancelled in the width part is no width failure.
    __ResetFeatureHost();
    __ModelVersion2(&model);
    memset(&g_card, 0, sizeof(g_card));
    __StartSetup();
    while (__CountCommands(13) == 0) {
        if (g_pending != NULL) {
            result = __Answer(&model, g_pending);
            __Complete(&result);
        } else {
            __RunScheduled();
        }
    }
    assert(g_pending != NULL && g_pending->Opcode == 13);
    assert(SdCardSetupCancel(&g_card) == OS_EOK);
    result = __Success(0);
    result.Status = OS_ECANCELLED;
    __Complete(&result);
    assert(g_setupDoneCount == 1 && g_setupResult == SdCardSetupFailed && g_setupStatus == OS_ECANCELLED);
    printf("Setup cancellation in the width part passed\n");
}

int
main(void)
{
    TestCid();
    TestCsdVersion1();
    TestCsdVersion2();
    TestScr();
    TestSdStatus();
    TestSwitchStatus();
    TestRunnerNext();
    TestRunnerGoTo();
    TestRunnerStartKinds();
    TestRunnerFailures();
    TestRunnerHardwareRunning();
    TestRunnerCancel();
    TestRunnerArguments();
    TestSetupVersion2();
    TestSetupLegacy();
    TestSetupInterfaceCondition();
    TestSetupOperatingCondition();
    TestSetupNoCard();
    TestSetupRelativeAddress();
    TestSetupCardErrors();
    TestSetupTransportErrors();
    TestSetupScrRead();
    TestSetupBus();
    TestSetupHostFailures();
    TestSetupWriteProtect();
    TestSetupCancel();
    TestSetupFourLinesHighSpeed();
    TestSetupWithoutFeatures();
    TestSetupSpeedNotOffered();
    TestSetupWidthRefused();
    TestSetupFeatureFailures();
    TestSetupFeatureLimits();
    TestSetupFeatureCancel();
    printf("SD card tests passed\n");
    return 0;
}
