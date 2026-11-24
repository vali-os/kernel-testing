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
 * Host test for the SD slot. The real modules/storage/sd/sequence.c,
 * registers.c, card.c, sectors.c, storage.c and slot.c run against fakes,
 * and the test drives the slot only through the host contract and the
 * module callbacks:
 *
 * - A host with a small SD card model. Submit records the request, and the
 *   worker loop answers it later, from outside Submit, the way a version 2
 *   high capacity card would: card setup, CMD13 and data commands. The
 *   model can also be a sector addressed eMMC device, which answers no
 *   SD-only command and the MMC setup, CMD13 and data commands. A card
 *   can be taken out, swapped for another card (another CID), made to fail
 *   a given command, or made to hold a command until the test lets it go.
 *   Schedule keeps work items on a fake clock in due order, and among equal
 *   due times in the order they were scheduled. Host events are delivered as
 *   steps of their own, as host.h requires.
 * - DMA mappings, transfers, the control buffer and attachments, each with
 *   scripted results.
 * - The IPC answer, filed's register and unregister answers, and a module
 *   that keeps the disk list and hands out storage device IDs.
 *
 * Every call is written to one event list as a short line of text. Tests look
 * for lines and their order. The fakes check invariants on every call: one
 * answer per request, never while that request still has a mapping or an
 * attachment, no second request while one is with the host, and no cleanup
 * call on a released object.
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
#include "sd/mmc_commands.h"
#include "sd/slot.h"

#define MAX_EVENTS   16384
#define EVENT_LENGTH 128
#define MAX_LOGS     1024
#define MAX_MESSAGES 128
#define MAX_FAULTS   64
#define MAX_DISKS    16

// Values the production code must use, written out independently.
#define CARD_ADDRESS       0xB368U
#define DRIVER_ID          41U
#define FILE_SERVICE       7U
#define FIRST_STORAGE_ID   100U
#define CONTROL_ADDRESS    0x2000ULL
#define ATTACH_OFFSET      4096U
#define SECOND_US          1000000ULL
// Power off (10 ms), then power on: 35 ms ramp, 1 ms and 74 clocks at
// 400 kHz (185 us).
#define POWER_CYCLE_US     46185ULL
#define CMD13_ARGUMENT     ((uint32_t)CARD_ADDRESS << 16)
// Card status error masks: SD, and MMC (bit 7 instead of bit 3).
#define SD_MASK            0xFD390008U
#define MMC_MASK           0xFD390080U

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
        fprintf(stderr, "  %4d: %s\n", i, g_events[i]);
    }
}

// Index of the first event at or after start that starts with prefix, or -1.
static int
__Find(
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

static int
__Count(
    _In_ const char* prefix,
    _In_ int         start)
{
    int count = 0;
    int i;

    for (i = start; i < g_eventCount; i++) {
        if (strncmp(g_events[i], prefix, strlen(prefix)) == 0) {
            count++;
        }
    }
    return count;
}

// Data commands that move sectors (CMD17, CMD18, CMD24 and CMD25) from start
// on. Card setup's ACMD51 is a data command too, but moves no sectors.
static int
__SectorCommands(
    _In_ int start)
{
    return __Count("data CMD17", start) + __Count("data CMD18", start) + __Count("data CMD24", start) +
           __Count("data CMD25", start);
}

// Asserts that an event with prefix exists at or after start, and returns
// its index. Dumps the events when it does not.
static int
__Expect(
    _In_ const char* prefix,
    _In_ int         start)
{
    int index = __Find(prefix, start);

    if (index < 0) {
        fprintf(stderr, "no event \"%s\" from %d on\n", prefix, start);
        __DumpEvents();
    }
    assert(index >= 0);
    return index;
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

// Logs at this level from index start on.
static int
__LogsAtLevel(
    _In_ enum OSSysLogLevel level,
    _In_ int                start)
{
    int count = 0;
    int i;

    for (i = start; i < g_logCount; i++) {
        if (g_logLevels[i] == (int)level) {
            count++;
        }
    }
    return count;
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
// Fake control buffer.

struct DMABuffer {
    uint8_t           Memory[512];
    struct DMASegment Segment;
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
    *countOut = 1;
    return &buffer->Segment;
}

/////////////////////////////////////////////////////////////////////////////
// Card model.

// CID of card A: MID 0x03, OID "SD", PNM "SU04G", PRV 8.0, PSN 0x12345678,
// April 2013. Card B differs only in the serial number.
static const uint32_t g_cidA[4] = { 0x03534453U, 0x55303447U, 0x80123456U, 0x7800D400U };
static const uint32_t g_cidB[4] = { 0x03534453U, 0x55303447U, 0x80123457U, 0x7800D400U };
// CSD version 2 of a 32 GiB card.
static const uint32_t g_csd[4] = { 0x400E0032U, 0x5B590000U, 0xFFFF7F80U, 0x0A400000U };
// SCR with one and four data lines.
static const uint8_t g_scr[8] = { 0x02, 0x35, 0x84, 0x03, 0x00, 0x00, 0x00, 0x00 };

struct CardModel {
    bool     Present;
    // The card is an eMMC device. It answers neither CMD8 SEND_IF_COND nor
    // CMD55, and the MMC setup instead.
    bool     Mmc;
    // The card answers nothing at all, like a card of another kind.
    bool     Silent;
    char     Label;
    uint32_t State;
    bool     AppCommand;
    // Bus width field the last ACMD6 set, as in its argument (0 or 2).
    uint32_t BusWidthArgument;
    // Function of the access mode group (CMD6 group 1) the card uses.
    uint32_t AccessMode;
    // eMMC only: the EXT_CSD, which CMD6 changes.
    uint8_t  ExtCsd[512];
};

struct Fault {
    uint8_t    Opcode;
    // The fault is for the application command with this opcode (after
    // CMD55), such as ACMD13, and not for the plain command, such as CMD13.
    bool       AppCommand;
    SdResult_t Result;
    bool       Used;
};

static struct CardModel g_model;
static struct Fault     g_faults[MAX_FAULTS];
static int              g_faultCount;
// The card is taken out after g_removeAfterMatches more commands with this
// opcode. -1 for none.
static int              g_removeAfterOpcode;
static int              g_removeAfterMatches;

static SdResult_t
__Success(
    _In_ uint32_t response)
{
    SdResult_t result = { 0 };

    result.Status = OS_EOK;
    result.StopStatus = OS_EOK;
    result.Response[0] = response;
    result.HardwareStopped = true;
    return result;
}

static SdResult_t
__NoAnswerResult(void)
{
    SdResult_t result = { 0 };

    result.Status = OS_ETIMEOUT;
    result.TransportErrors = SD_TRANSPORT_COMMAND_TIMEOUT;
    result.HardwareStopped = true;
    return result;
}

// Makes the next unused fault for opcode return this result instead of the
// card's answer.
static void
__Fault(
    _In_ uint8_t  opcode,
    _In_ oserr_t  status,
    _In_ uint32_t transportErrors,
    _In_ bool     hardwareStopped)
{
    struct Fault* fault;

    assert(g_faultCount < MAX_FAULTS);
    fault = &g_faults[g_faultCount++];
    memset(fault, 0, sizeof(*fault));
    fault->Opcode = opcode;
    fault->Result.Status = status;
    fault->Result.TransportErrors = transportErrors;
    fault->Result.HardwareStopped = hardwareStopped;
}

// As __Fault, for an application command such as ACMD6 or ACMD13.
static void
__FaultApp(
    _In_ uint8_t  opcode,
    _In_ oserr_t  status,
    _In_ uint32_t transportErrors)
{
    __Fault(opcode, status, transportErrors, true);
    g_faults[g_faultCount - 1].AppCommand = true;
}

static void
__InsertCard(
    _In_ char label)
{
    memset(&g_model, 0, sizeof(g_model));
    g_model.Present = true;
    g_model.Label = label;
}

// Sets a field in a normalized R2 response.
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

// Puts an eMMC device in the slot: MID 0x15, PNM "8GTF4R", PSN 0xA1B2C3D4
// for device A and 0xA1B2C3D5 for device B. EXT_CSD revision 8 with high
// speed at 52 MHz, 15,269,888 sectors and GENERIC_CMD6_TIME 250 ms.
static void
__InsertMmc(
    _In_ char label)
{
    __InsertCard(label);
    g_model.Mmc = true;
    g_model.ExtCsd[192] = 8;
    g_model.ExtCsd[194] = 2;
    g_model.ExtCsd[196] = 0x03;
    g_model.ExtCsd[214] = 0xE9;
    g_model.ExtCsd[248] = 25;
}

static void
__MmcCid(
    _Out_ uint32_t words[4])
{
    const char*  name = "8GTF4R";
    unsigned int i;

    memset(words, 0, 4 * sizeof(uint32_t));
    __PutBits(words, 120, 8, 0x15);
    __PutBits(words, 112, 2, 1);
    for (i = 0; i < 6; i++) {
        __PutBits(words, 96 - 8 * i, 8, (uint8_t)name[i]);
    }
    __PutBits(words, 16, 32, g_model.Label == 'A' ? 0xA1B2C3D4U : 0xA1B2C3D5U);
    __PutBits(words, 12, 4, 6);
    __PutBits(words, 8, 4, 3);
}

// CSD of a system specification 4 device with TAAC 1.5 ms and R2W_FACTOR 2.
static void
__MmcCsd(
    _Out_ uint32_t words[4])
{
    memset(words, 0, 4 * sizeof(uint32_t));
    __PutBits(words, 126, 2, 3);
    __PutBits(words, 122, 4, 4);
    __PutBits(words, 112, 8, 0x26);
    __PutBits(words, 104, 8, 0x01);
    __PutBits(words, 96, 8, 0x32);
    __PutBits(words, 80, 4, 9);
    __PutBits(words, 62, 12, 0xFFF);
    __PutBits(words, 47, 3, 7);
    __PutBits(words, 26, 3, 2);
}

static uint32_t
__CardStatus(void)
{
    return g_model.State << 9;
}

// CMD6: the switch status of a card that supports default speed and high
// speed in group 1. Switch mode makes the change.
static SdResult_t
__AnswerSwitch(
    _In_ const SdRequest_t* request)
{
    uint32_t   function = request->Argument & 0xFU;
    SdResult_t result;

    memset(g_controlBuffer.Memory, 0, 64);
    g_controlBuffer.Memory[1] = 100;
    g_controlBuffer.Memory[12] = 0x80;
    g_controlBuffer.Memory[13] = 0x03;
    if (function > 1U) {
        function = 0xFU;
    } else if ((request->Argument & 0x80000000U) != 0) {
        g_model.AccessMode = function;
    }
    g_controlBuffer.Memory[16] = (uint8_t)(0xF0U | function);
    result = __Success(__CardStatus() | SD_R1_READY_FOR_DATA);
    result.ConfirmedBytes = 64;
    return result;
}

// The answers of the eMMC device. SD-only commands get no answer.
static SdResult_t
__AnswerMmc(
    _In_ const SdRequest_t* request)
{
    uint32_t   ready = (MMC_STATE_TRANSFER << 9) | MMC_R1_READY_FOR_DATA;
    SdResult_t result;

    switch (request->Opcode) {
        case MMC_CMD_GO_IDLE_STATE:
            g_model.State = MMC_STATE_IDLE;
            return __Success(0);
        case MMC_CMD_SEND_OP_COND:
            g_model.State = MMC_STATE_READY;
            return __Success(0xC0FF8080U);
        case MMC_CMD_ALL_SEND_CID:
            result = __Success(0);
            __MmcCid(result.Response);
            g_model.State = MMC_STATE_IDENT;
            return result;
        case MMC_CMD_SET_RELATIVE_ADDR:
            assert(request->Argument == 0x00010000U);
            result = __Success(__CardStatus());
            g_model.State = MMC_STATE_STANDBY;
            return result;
        case MMC_CMD_SEND_CSD:
            result = __Success(0);
            __MmcCsd(result.Response);
            return result;
        case MMC_CMD_SELECT_CARD:
            result = __Success(__CardStatus());
            g_model.State = MMC_STATE_TRANSFER;
            return result;
        case MMC_CMD_SEND_EXT_CSD:
            if (request->Direction == SdDataNone) {
                // SD's CMD8 SEND_IF_COND, illegal in the idle state.
                return __NoAnswerResult();
            }
            memcpy(g_controlBuffer.Memory, g_model.ExtCsd, sizeof(g_model.ExtCsd));
            result = __Success(ready);
            result.ConfirmedBytes = sizeof(g_model.ExtCsd);
            return result;
        case MMC_CMD_SWITCH:
            g_model.ExtCsd[(request->Argument >> 16) & 0xFFU] = (uint8_t)(request->Argument >> 8);
            return __Success(ready);
        case MMC_CMD_SEND_STATUS:
            return __Success(ready);
        case SD_CMD_READ_SINGLE_BLOCK:
        case SD_CMD_READ_MULTIPLE_BLOCK:
        case SD_CMD_WRITE_BLOCK:
        case SD_CMD_WRITE_MULTIPLE_BLOCK:
            result = __Success(ready);
            result.ConfirmedBytes = request->Data.Length;
            return result;
        case SD_CMD_APP_CMD:
            return __NoAnswerResult();
        default:
            fprintf(stderr, "unexpected CMD%u to the eMMC device\n", request->Opcode);
            assert(!"unexpected command");
            return __NoAnswerResult();
    }
}

static SdResult_t
__AnswerByOpcode(
    _In_ const SdRequest_t* request,
    _In_ bool               appCommand)
{
    SdResult_t result;

    if (g_model.Mmc) {
        return __AnswerMmc(request);
    }
    // An SD card never gets CMD1.
    assert(request->Opcode != MMC_CMD_SEND_OP_COND);
    switch (request->Opcode) {
        case SD_CMD_GO_IDLE_STATE:
            g_model.State = SD_STATE_IDLE;
            return __Success(0);
        case SD_CMD_SEND_IF_COND:
            return __Success(request->Argument & 0xFFFU);
        case SD_CMD_APP_CMD:
            g_model.AppCommand = true;
            return __Success(__CardStatus() | SD_R1_APP_CMD);
        case SD_ACMD_SD_SEND_OP_COND:
            assert(appCommand);
            g_model.State = SD_STATE_READY;
            return __Success(0x00FF8000U | SD_OCR_READY | SD_OCR_CCS);
        case SD_CMD_ALL_SEND_CID:
            result = __Success(0);
            memcpy(result.Response, g_model.Label == 'A' ? g_cidA : g_cidB, sizeof(g_cidA));
            g_model.State = SD_STATE_IDENT;
            return result;
        case SD_CMD_SEND_RELATIVE_ADDR:
            result = __Success(((uint32_t)CARD_ADDRESS << 16) | __CardStatus());
            g_model.State = SD_STATE_STANDBY;
            return result;
        case SD_CMD_SEND_CSD:
            result = __Success(0);
            memcpy(result.Response, g_csd, sizeof(g_csd));
            return result;
        case SD_CMD_SELECT_CARD:
            result = __Success(__CardStatus());
            g_model.State = SD_STATE_TRANSFER;
            return result;
        case SD_ACMD_SEND_SCR:
            assert(appCommand);
            memcpy(g_controlBuffer.Memory, g_scr, sizeof(g_scr));
            result = __Success(__CardStatus() | SD_R1_READY_FOR_DATA | SD_R1_APP_CMD);
            result.ConfirmedBytes = sizeof(g_scr);
            return result;
        case SD_CMD_SEND_STATUS:
            if (appCommand) {
                // ACMD13: the SD status, with the bus width ACMD6 set.
                memset(g_controlBuffer.Memory, 0, 64);
                g_controlBuffer.Memory[0] = (uint8_t)(g_model.BusWidthArgument << 6);
                result = __Success(__CardStatus() | SD_R1_READY_FOR_DATA | SD_R1_APP_CMD);
                result.ConfirmedBytes = 64;
                return result;
            }
            return __Success((SD_STATE_TRANSFER << 9) | SD_R1_READY_FOR_DATA);
        case SD_CMD_SWITCH_FUNC:
            if (appCommand) {
                // ACMD6.
                g_model.BusWidthArgument = request->Argument & 0x3U;
                return __Success(__CardStatus() | SD_R1_APP_CMD);
            }
            return __AnswerSwitch(request);
        case SD_CMD_READ_SINGLE_BLOCK:
        case SD_CMD_READ_MULTIPLE_BLOCK:
        case SD_CMD_WRITE_BLOCK:
        case SD_CMD_WRITE_MULTIPLE_BLOCK:
            result = __Success((SD_STATE_TRANSFER << 9) | SD_R1_READY_FOR_DATA);
            result.ConfirmedBytes = request->Data.Length;
            return result;
        default:
            fprintf(stderr, "unexpected CMD%u\n", request->Opcode);
            assert(!"unexpected command");
            return __NoAnswerResult();
    }
}

// The answer of the card, a scripted fault, or nothing when no card is in
// the slot. The card error check of the host is applied to R1 answers.
static SdResult_t
__CardAnswer(
    _In_ const SdRequest_t* request)
{
    bool       appCommand = g_model.AppCommand;
    SdResult_t result;
    int        i;

    g_model.AppCommand = false;
    for (i = 0; i < g_faultCount; i++) {
        if (!g_faults[i].Used && g_faults[i].Opcode == request->Opcode && g_faults[i].AppCommand == appCommand) {
            g_faults[i].Used = true;
            return g_faults[i].Result;
        }
    }
    // An empty slot answers nothing. A command without a response, such as
    // CMD0, still completes, because the host waits for no answer.
    if (!g_model.Present || g_model.Silent) {
        return request->ResponseType == SdResponseNone ? __Success(0) : __NoAnswerResult();
    }
    result = __AnswerByOpcode(request, appCommand);
    if (request->ResponseType == SdResponseR1 || request->ResponseType == SdResponseR1b) {
        result.CardErrors = result.Response[0] & request->CardErrorMask;
    }
    return result;
}

/////////////////////////////////////////////////////////////////////////////
// Fake host.

static SdHost_t      g_host;
static bool          g_detectLine;
static SdHostEventFn g_eventHandler;
static void*         g_eventContext;
static SdRequest_t*  g_pending;
// The pending request is held: it completes only after a Stop, or once the
// test lets it go. The first g_holdSkip commands with g_holdOpcode pass.
static bool          g_holding;
static int           g_holdOpcode;
static int           g_holdSkip;
// Stop was called on the pending request. It completes with OS_ECANCELLED.
static bool          g_pendingStopped;
// HardwareStopped of a stopped request.
static bool          g_stopProof;
// The host has failed: Submit refuses with OS_EDEVFAULT until a reset works.
static bool          g_hostFailed;
// __Run leaves requests with the host alone while set.
static bool          g_pauseCompletions;
static SdWork_t*     g_workList;
static uint64_t      g_nowUs;
static oserr_t       g_resetStatus;
static int           g_resetCount;
// SetBus with the card supply off returns this, as on a Raspberry Pi 4
// model whose firmware cannot switch the supply. Counted in g_powerOffCount.
static oserr_t       g_powerOffStatus;
static int           g_powerOffCount;
// Times of the last 64 SD setups, taken at their CMD8 SEND_IF_COND. Each
// setup starts with the SD sequence, and only SD sends CMD8 without data.
// The fake clock does not move between CMD0 and CMD8.
static uint64_t      g_setupTimes[64];
static int           g_setupCount;
// Width and timing of the last SetBus call, to log each change.
static enum SdBusWidth  g_busWidth;
static enum SdBusTiming g_busTiming;

static oserr_t
__FakeSubmit(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    assert(host == &g_host);
    // One request at a time is the host contract. A second one is a bug in
    // the caller, not a busy host.
    assert(g_pending == NULL);
    assert(request->Complete != NULL && request->CommandTimeoutUs != 0);
    if (request->Opcode == SD_CMD_SEND_IF_COND && request->Direction == SdDataNone) {
        g_setupTimes[g_setupCount++ % 64] = g_nowUs;
    }
    if (request->Direction == SdDataNone) {
        __Event("CMD%u arg=0x%x mask=0x%x", request->Opcode, request->Argument, request->CardErrorMask);
    } else {
        __Event("data CMD%u card=%c arg=0x%x blocks=%u mask=0x%x", request->Opcode,
                g_model.Present ? g_model.Label : '-', request->Argument, request->BlockCount,
                request->CardErrorMask);
    }
    if (g_hostFailed) {
        __Event("refused CMD%u", request->Opcode);
        return OS_EDEVFAULT;
    }
    // A host with a detect line knows when the slot is empty.
    if (g_detectLine && !g_model.Present) {
        __Event("refused CMD%u", request->Opcode);
        return OS_ENOENT;
    }
    g_pending = request;
    g_pendingStopped = false;
    g_holding = false;
    if (request->Opcode == g_holdOpcode) {
        g_holding = g_holdSkip == 0;
        if (g_holdSkip > 0) {
            g_holdSkip--;
        }
    }
    return OS_EOK;
}

static oserr_t
__FakeSetBus(
    _In_  struct SdHost*         host,
    _In_  const SdBusSettings_t* settings,
    _Out_ uint32_t*              clockHzOut)
{
    assert(host == &g_host && g_pending == NULL);
    assert(settings->ClockHz == 0 || settings->ClockHz >= g_host.Limits.MinimumClockHz);
    if (settings->Power == SdBusPowerOff) {
        g_powerOffCount++;
        if (g_powerOffStatus != OS_EOK) {
            __Event("power off failed %u", g_powerOffStatus);
            return g_powerOffStatus;
        }
    }
    *clockHzOut = settings->ClockHz;
    if (settings->Width != g_busWidth || settings->Timing != g_busTiming) {
        g_busWidth = settings->Width;
        g_busTiming = settings->Timing;
        __Event("bus %u lines timing %u", settings->Width, settings->Timing);
    }
    if (settings->ClockHz > 400000U) {
        __Event("clock %u", settings->ClockHz);
    }
    return OS_EOK;
}

static oserr_t
__FakeStop(
    _In_ struct SdHost*    host,
    _In_ struct SdRequest* request)
{
    assert(host == &g_host);
    if (request != g_pending) {
        return OS_ENOENT;
    }
    __Event("host stop CMD%u", request->Opcode);
    g_pendingStopped = true;
    return OS_EOK;
}

static oserr_t
__FakeReset(
    _In_ struct SdHost* host)
{
    assert(host == &g_host && g_pending == NULL);
    g_resetCount++;
    __Event("reset");
    if (g_resetStatus == OS_EOK) {
        g_hostFailed = false;
    }
    return g_resetStatus;
}

static oserr_t
__FakeGetCardState(
    _In_  struct SdHost* host,
    _Out_ uint32_t*      stateOut)
{
    assert(host == &g_host);
    if (!g_detectLine) {
        return OS_ENOTSUPPORTED;
    }
    *stateOut = g_model.Present ? SD_CARD_STATE_PRESENT : 0;
    return OS_EOK;
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
    if (work->HostScheduled) {
        return OS_EBUSY;
    }
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

static oserr_t
__FakeSetEventHandler(
    _In_ struct SdHost* host,
    _In_ SdHostEventFn  handler,
    _In_ void*          context)
{
    assert(host == &g_host);
    __Event(handler != NULL ? "event handler set" : "event handler cleared");
    g_eventHandler = handler;
    g_eventContext = context;
    return OS_EOK;
}

static const SdHostOperations_t g_operations = {
    .Submit = __FakeSubmit,
    .SetBus = __FakeSetBus,
    .Stop = __FakeStop,
    .Reset = __FakeReset,
    .GetCardState = __FakeGetCardState,
    .Schedule = __FakeSchedule,
    .CancelWork = __FakeCancelWork,
    .SetEventHandler = __FakeSetEventHandler
};

static void
__CompletePending(void)
{
    SdRequest_t* request = g_pending;
    SdResult_t   result;

    if (g_pendingStopped) {
        memset(&result, 0, sizeof(result));
        result.Status = OS_ECANCELLED;
        result.HardwareStopped = g_stopProof;
    } else {
        result = __CardAnswer(request);
    }
    if (g_removeAfterOpcode == request->Opcode && --g_removeAfterMatches == 0) {
        g_removeAfterOpcode = -1;
        g_model.Present = false;
    }
    // A request without stop proof marks the host failed until a reset.
    if (!result.HardwareStopped) {
        g_hostFailed = true;
    }
    g_pending = NULL;
    g_holding = false;
    g_pendingStopped = false;
    request->Result = result;
    request->Complete(request);
}

static void
__RunWorkItem(void)
{
    SdWork_t* work = g_workList;

    g_workList = work->HostNext;
    work->HostScheduled = false;
    if (work->HostDueUs > g_nowUs) {
        g_nowUs = work->HostDueUs;
    }
    work->Function(work);
}

// Runs the worker up to durationUs from now: completes requests that are
// not held, and runs work items as they become due.
static void
__Run(
    _In_ uint64_t durationUs)
{
    uint64_t until = g_nowUs + durationUs;
    int      steps = 0;

    for (;;) {
        assert(steps++ < 2000000);
        if (g_pending != NULL && !g_pauseCompletions && (!g_holding || g_pendingStopped)) {
            __CompletePending();
            continue;
        }
        if (g_workList != NULL && g_workList->HostDueUs <= until) {
            __RunWorkItem();
            continue;
        }
        break;
    }
    if (g_nowUs < until) {
        g_nowUs = until;
    }
}

// Lets a held request complete with the card's answer, in the next step.
static void
__LetGo(void)
{
    assert(g_pending != NULL && g_holding);
    g_holding = false;
    g_holdOpcode = -1;
}

// A host event, as a step of its own on the worker.
static void
__SendEvent(
    _In_ enum SdHostEvent event)
{
    assert(g_eventHandler != NULL);
    __Event("host event %u", (unsigned int)event);
    g_eventHandler(&g_host, event, g_eventContext);
}

// Takes the card out of a slot with a detect line. A request with the host
// completes first with OS_ENOENT and SD_TRANSPORT_CARD_REMOVED after its
// stop proof, then the removal event follows (host.h).
static void
__PullCard(void)
{
    SdRequest_t* request = g_pending;

    g_model.Present = false;
    if (request != NULL) {
        g_pending = NULL;
        g_holding = false;
        memset(&request->Result, 0, sizeof(request->Result));
        request->Result.Status = OS_ENOENT;
        request->Result.TransportErrors = SD_TRANSPORT_CARD_REMOVED;
        request->Result.HardwareStopped = true;
        request->Complete(request);
    }
    __SendEvent(SdHostEventCardRemoved);
}

/////////////////////////////////////////////////////////////////////////////
// Fake DMA, attachments and IPC.

struct DMAMapping {
    bool              Live;
    bool              Active;
    int               Owner;
    size_t            Length;
    struct DMASegment Segment;
};

static struct DMAMapping      g_mappings[1024];
static int                    g_mappingCount;
static oserr_t                g_endScript[8];
static int                    g_endScriptCount;
static oserr_t                g_unmapScript[8];
static int                    g_unmapScriptCount;
static oserr_t                g_releaseScript[8];
static int                    g_releaseScriptCount;
static int                    g_currentId;
static bool                   g_attached[MAX_MESSAGES];
static int                    g_answers[MAX_MESSAGES];
static oserr_t                g_answerStatus[MAX_MESSAGES];
static size_t                 g_answerSectors[MAX_MESSAGES];
static struct gracht_message* g_messages[MAX_MESSAGES];
static struct sys_shm_transfer g_transfer;
// The request whose chunk ended without stop proof. It is answered while
// its memory stays mapped and attached. 0 for none.
static int                    g_keptId;

// Takes the first scripted status, or OS_EOK when the script is empty.
static oserr_t
__TakeScripted(
    _InOut_ oserr_t* script,
    _InOut_ int*     count)
{
    oserr_t status;

    if (*count == 0) {
        return OS_EOK;
    }
    status = script[0];
    memmove(script, script + 1, (size_t)(*count - 1) * sizeof(oserr_t));
    (*count)--;
    return status;
}

static int
__LiveMappingsOf(
    _In_ int owner)
{
    int count = 0;
    int i;

    for (i = 0; i < g_mappingCount; i++) {
        if (g_mappings[i].Live && g_mappings[i].Owner == owner) {
            count++;
        }
    }
    return count;
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
    struct DMAMapping* mapping;
    int                owner = (int)(buffer->ID - 1000U);

    (void)context;
    (void)direction;
    (void)requirements;
    assert(*mappingOut == NULL && g_attached[owner]);
    assert(g_mappingCount < (int)(sizeof(g_mappings) / sizeof(g_mappings[0])));
    if (length > limits->DirectLength) {
        length = limits->DirectLength;
    }
    __Event("map id=%d", owner);
    mapping = &g_mappings[g_mappingCount++];
    memset(mapping, 0, sizeof(*mapping));
    mapping->Live = true;
    mapping->Owner = owner;
    mapping->Length = length;
    mapping->Segment.Address = 0x10000000ULL + offset;
    mapping->Segment.Length = length;
    *mappingOut = mapping;
    *mappedLengthOut = length;
    return OS_EOK;
}

const struct DMASegment*
DMAMappingSegments(
    _In_  const struct DMAMapping* mapping,
    _Out_ uint32_t*                countOut)
{
    assert(mapping->Live);
    *countOut = 1;
    return &mapping->Segment;
}

oserr_t
DMABeginTransfer(
    _In_ struct DMAMapping*   mapping,
    _In_ size_t               offset,
    _In_ size_t               length,
    _In_ enum SHMWaitMode     mode,
    _In_ const OSTimestamp_t* deadline)
{
    (void)offset;
    (void)length;
    (void)mode;
    (void)deadline;
    assert(mapping->Live && !mapping->Active);
    mapping->Active = true;
    return OS_EOK;
}

oserr_t
DMAEndTransfer(
    _In_ struct DMAMapping* mapping,
    _In_ size_t             completedLength)
{
    oserr_t status;

    assert(mapping->Live && mapping->Active);
    // The transfer ends only once the request is off the host.
    assert(g_pending == NULL || g_pending->Direction == SdDataNone);
    __Event("end id=%d %zu", mapping->Owner, completedLength);
    status = __TakeScripted(g_endScript, &g_endScriptCount);
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
    __Event("unmap id=%d", (*mapping)->Owner);
    status = __TakeScripted(g_unmapScript, &g_unmapScriptCount);
    if (status == OS_EOK) {
        (*mapping)->Live = false;
        *mapping = NULL;
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
    (void)sectorSize;
    (void)count;
    (void)write;
    assert(description == &g_transfer);
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
    // The attachment is kept until every chunk of its request is unmapped.
    assert(__LiveMappingsOf(id) == 0);
    __Event("release id=%d", id);
    status = __TakeScripted(g_releaseScript, &g_releaseScriptCount);
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

    memcpy(&id, message->payload, sizeof(id));
    assert(id > 0 && id < MAX_MESSAGES);
    __Event("answer id=%d status=%u sectors=%zu", id, result, sectorsTransferred);
    // One answer per request, and only once its memory is handed back.
    assert(g_answers[id] == 0);
    if (id != g_keptId) {
        assert(!g_attached[id] && __LiveMappingsOf(id) == 0);
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
static int     g_statusDefault;
static oserr_t g_registerResult;
static oserr_t g_unregisterResult;

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
    (void)context;
    assert(client == GetGrachtClient() && driverId == DRIVER_ID);
    __Event("register device=%u flags=%u", deviceId, (unsigned int)flags);
    return 0;
}

int
sys_storage_unregister(
    gracht_client_t*               client,
    struct gracht_message_context* context,
    const uuid_t                   driverId,
    const uuid_t                   deviceId,
    const uint8_t                  forced)
{
    (void)context;
    assert(client == GetGrachtClient() && driverId == DRIVER_ID);
    __Event("unregister device=%u forced=%u", deviceId, forced);
    return 0;
}

int
gracht_client_get_status(
    gracht_client_t*               client,
    struct gracht_message_context* context)
{
    (void)context;
    assert(client == GetGrachtClient());
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
    __Event("register answer %u", g_registerResult);
    *resultOut = g_registerResult;
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
    __Event("unregister answer %u", g_unregisterResult);
    *resultOut = g_unregisterResult;
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
// The module: storage device IDs, the disk list and the slot callbacks.

struct ModuleDisk {
    uuid_t       Id;
    SDStorage_t* Adapter;
    bool         Listed;
};

static SDSlot_t*         g_slot;
static uuid_t            g_nextStorageId;
static struct ModuleDisk g_disks[MAX_DISKS];
static int               g_diskCount;
static int               g_stoppedCount;
static oserr_t           g_stoppedStatus;
static int               g_hostFailedCount;

static uuid_t
__AllocateStorageId(
    _In_ void* context)
{
    assert(context == &g_disks);
    return g_nextStorageId++;
}

static bool
__SerialInUse(
    _In_ const char* serial,
    _In_ void*       context)
{
    (void)serial;
    assert(context == &g_disks);
    return false;
}

static void
__DiskAdded(
    _In_ SDSlot_t*    slot,
    _In_ SDStorage_t* adapter,
    _In_ uuid_t       storageId,
    _In_ void*        context)
{
    int i;

    assert(slot == g_slot && context == &g_disks);
    // Never two listed disks for one slot, and never an ID twice.
    for (i = 0; i < g_diskCount; i++) {
        assert(!g_disks[i].Listed && g_disks[i].Id != storageId);
    }
    assert(g_diskCount < MAX_DISKS);
    g_disks[g_diskCount++] = (struct ModuleDisk){ storageId, adapter, true };
    __Event("disk added %u", storageId);
}

static void
__DiskRemoved(
    _In_ SDSlot_t*    slot,
    _In_ SDStorage_t* adapter,
    _In_ uuid_t       storageId,
    _In_ void*        context)
{
    int i;

    assert(slot == g_slot && context == &g_disks);
    for (i = 0; i < g_diskCount; i++) {
        if (g_disks[i].Id == storageId) {
            assert(g_disks[i].Listed && g_disks[i].Adapter == adapter);
            g_disks[i].Listed = false;
            __Event("disk removed %u", storageId);
            return;
        }
    }
    assert(!"removed a disk that was never added");
}

static void
__HostFailed(
    _In_ SDSlot_t* slot,
    _In_ void*     context)
{
    assert(slot == g_slot && context == &g_disks);
    g_hostFailedCount++;
    __Event("host failed");
}

static void
__Stopped(
    _In_ SDSlot_t* slot,
    _In_ oserr_t   status,
    _In_ void*     context)
{
    assert(slot == g_slot && context == &g_disks);
    // Exactly once.
    assert(g_stoppedCount == 0);
    g_stoppedCount++;
    g_stoppedStatus = status;
    __Event("stopped %u", status);
}

static const SDSlotCallbacks_t g_callbacks = {
    .AllocateStorageId = __AllocateStorageId,
    .SerialInUse = __SerialInUse,
    .DiskAdded = __DiskAdded,
    .DiskRemoved = __DiskRemoved,
    .HostFailed = __HostFailed,
    .Stopped = __Stopped
};

// The listed adapter of a storage device ID, as the ctt_storage handlers
// look it up. NULL for an unknown or removed ID.
static SDStorage_t*
__ListedAdapter(
    _In_ uuid_t storageId)
{
    int i;

    for (i = 0; i < g_diskCount; i++) {
        if (g_disks[i].Id == storageId && g_disks[i].Listed) {
            return g_disks[i].Adapter;
        }
    }
    return NULL;
}

// A ctt_storage transfer through the module's lookup. An ID that is not
// listed is answered at once with OS_ENOENT, as main.c does.
static void
__Transfer(
    _In_ int                         id,
    _In_ uuid_t                      storageId,
    _In_ enum sys_transfer_direction direction,
    _In_ uint64_t                    firstSector,
    _In_ size_t                      sectorCount)
{
    struct gracht_message* message = __Message(id);
    SDStorage_t*           adapter = __ListedAdapter(storageId);

    g_currentId = id;
    if (adapter == NULL) {
        assert(ctt_storage_transfer_response(message, OS_ENOENT, 0) == 0);
        return;
    }
    SDStorageTransfer(adapter, message, direction, firstSector, sectorCount, &g_transfer);
}

static void
__Read(
    _In_ int      id,
    _In_ uuid_t   storageId,
    _In_ uint64_t firstSector,
    _In_ size_t   sectorCount)
{
    __Transfer(id, storageId, SYS_TRANSFER_DIRECTION_READ, firstSector, sectorCount);
}

static void
__Write(
    _In_ int      id,
    _In_ uuid_t   storageId,
    _In_ uint64_t firstSector,
    _In_ size_t   sectorCount)
{
    __Transfer(id, storageId, SYS_TRANSFER_DIRECTION_WRITE, firstSector, sectorCount);
}

static void
__ExpectAnswer(
    _In_ int     id,
    _In_ oserr_t status,
    _In_ size_t  sectors)
{
    if (g_answers[id] != 1 || g_answerStatus[id] != status || g_answerSectors[id] != sectors) {
        fprintf(stderr, "request %d: %d answers, status %u, %zu sectors; expected 1, %u, %zu\n", id,
                g_answers[id], g_answerStatus[id], g_answerSectors[id], status, sectors);
        __DumpEvents();
    }
    assert(g_answers[id] == 1 && g_answerStatus[id] == status && g_answerSectors[id] == sectors);
}

/////////////////////////////////////////////////////////////////////////////
// Test setup.

static void
__Reset(void)
{
    int i;

    // A slot that keeps DMA memory refuses to be freed, on purpose. Such a
    // slot is left behind, as the module leaves its device.
    if (g_slot != NULL && SDSlotDestroy(g_slot) != OS_EOK) {
        assert(g_stoppedStatus == OS_EBUSY);
    }
    g_slot = NULL;
    memset(&g_host, 0, sizeof(g_host));
    g_host.Operations = &g_operations;
    g_host.Limits.MaximumClockHz = 50000000U;
    g_host.Limits.MinimumClockHz = 100000U;
    g_host.Limits.BusWidths = SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4;
    g_host.Limits.SignalVoltages = SD_HOST_SIGNAL_330;
    g_host.Limits.SupplyVoltages = 0x00300000U;
    g_host.Limits.MaximumBlockSize = 512U;
    g_host.Limits.MaximumBlockCount = 65535U;
    g_host.Limits.Flags = SD_HOST_FLAG_REMOVABLE;
    g_host.DMAContext = (struct DMAContext*)&g_mappings;
    g_host.ControlBuffer = &g_controlBuffer;
    g_controlBuffer.Segment.Address = CONTROL_ADDRESS;
    g_controlBuffer.Segment.Length = sizeof(g_controlBuffer.Memory);

    memset(&g_model, 0, sizeof(g_model));
    g_faultCount = 0;
    g_removeAfterOpcode = -1;
    g_removeAfterMatches = 0;
    g_detectLine = false;
    g_eventHandler = NULL;
    g_eventContext = NULL;
    g_pending = NULL;
    g_holding = false;
    g_holdOpcode = -1;
    g_holdSkip = 0;
    g_pendingStopped = false;
    g_stopProof = true;
    g_hostFailed = false;
    g_pauseCompletions = false;
    g_workList = NULL;
    g_nowUs = 0;
    g_resetStatus = OS_EOK;
    g_resetCount = 0;
    g_powerOffStatus = OS_EOK;
    g_powerOffCount = 0;
    g_setupCount = 0;
    g_busWidth = SdBusWidth1;
    g_busTiming = SdBusTimingDefault;

    g_eventCount = 0;
    g_logCount = 0;
    g_mappingCount = 0;
    g_endScriptCount = 0;
    g_unmapScriptCount = 0;
    g_releaseScriptCount = 0;
    for (i = 0; i < MAX_MESSAGES; i++) {
        free(g_messages[i]);
        g_messages[i] = NULL;
        g_attached[i] = false;
        g_answers[i] = 0;
        g_answerStatus[i] = OS_EOK;
        g_answerSectors[i] = 0;
    }
    g_currentId = 0;
    g_keptId = 0;
    g_statusDefault = GRACHT_MESSAGE_COMPLETED;
    g_registerResult = OS_EOK;
    g_unregisterResult = OS_EOK;

    g_nextStorageId = FIRST_STORAGE_ID;
    g_diskCount = 0;
    g_stoppedCount = 0;
    g_stoppedStatus = OS_EUNKNOWN;
    g_hostFailedCount = 0;
}

static void
__StartSlot(
    _In_ bool detectLine)
{
    SDSlotParameters_t parameters = {
        .LogId = 9,
        .DriverId = DRIVER_ID,
        .Callbacks = &g_callbacks,
        .Context = &g_disks
    };

    g_detectLine = detectLine;
    assert(SDSlotCreate(&g_host, &parameters, &g_slot) == OS_EOK);
    assert(SDSlotStart(g_slot) == OS_EOK);
}

// The storage device ID of the listed disk. Asserts that there is one.
static uuid_t
__ListedId(void)
{
    int i;

    for (i = 0; i < g_diskCount; i++) {
        if (g_disks[i].Listed) {
            return g_disks[i].Id;
        }
    }
    __DumpEvents();
    assert(!"no listed disk");
    return 0;
}

static bool
__AnyListed(void)
{
    int i;

    for (i = 0; i < g_diskCount; i++) {
        if (g_disks[i].Listed) {
            return true;
        }
    }
    return false;
}

// Starts a slot with card A and runs until its disk is registered.
static void
__ReadySlot(
    _In_ bool detectLine)
{
    __Reset();
    __InsertCard('A');
    __StartSlot(detectLine);
    __Run(100000);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __Expect("register answer 0", 0);
}

// Stops the slot, runs until it has stopped, and checks the outcome.
static void
__StopSlot(
    _In_ oserr_t expected)
{
    assert(SDSlotStop(g_slot) == OS_EOK);
    __Run(60 * SECOND_US);
    assert(g_stoppedCount == 1 && g_stoppedStatus == expected);
    assert(!__AnyListed());
    assert(g_eventHandler == NULL);
    if (expected == OS_EOK) {
        // Nothing of the slot or its adapter may still be due once it has
        // stopped, so freeing it is safe.
        assert(g_workList == NULL && g_pending == NULL);
        assert(SDSlotDestroy(g_slot) == OS_EOK);
        g_slot = NULL;
        return;
    }
    assert(SDSlotDestroy(g_slot) == OS_EBUSY);
}

// Logs at the always-visible levels (NOTICE, WARNING, ERROR) from index on.
static int
__VisibleLogsFrom(
    _In_ int start)
{
    return g_logCount - start;
}

// Card setups (their SD CMD8) whose start lies in [fromUs, toUs).
static int
__SetupsBetween(
    _In_ uint64_t fromUs,
    _In_ uint64_t toUs)
{
    int count = 0;
    int i;

    for (i = 0; i < g_setupCount && i < 64; i++) {
        if (g_setupTimes[i] >= fromUs && g_setupTimes[i] < toUs) {
            count++;
        }
    }
    return count;
}

/////////////////////////////////////////////////////////////////////////////
// Start and detection.

static void
TestStartWithCardNoDetectLine(void)
{
    int added;
    int check;

    __ReadySlot(false);
    added = __Expect("disk added 100", 0);
    // The disk is listed before filed hears of it, and filed hears of it as
    // a removable disk.
    assert(__Expect("register device=100 flags=1", added) > added);
    assert(__Count("CMD0", 0) == 1);
    // An SD card is set up as SD: no MMC command reaches it.
    assert(__Count("CMD1 ", 0) == 0);
    assert(__Count("CMD6 arg=0x3", 0) == 0);

    // Idle for 1 s: one CMD13 to the card's address, and one every second
    // after that.
    check = g_eventCount;
    __Run(POWER_CYCLE_US + SECOND_US - g_nowUs - 1);
    assert(__Count("CMD13", check) == 0);
    __Run(1);
    __Expect("CMD13 arg=0xb3680000 mask=0xfd390008", check);
    assert(__Count("CMD13", check) == 1);
    __Run(3 * SECOND_US);
    assert(__Count("CMD13", check) == 4);

    // No idle check while a request runs, however long it takes.
    check = g_eventCount;
    g_holdOpcode = SD_CMD_READ_MULTIPLE_BLOCK;
    __Read(1, FIRST_STORAGE_ID, 64, 8);
    __Run(5 * SECOND_US);
    assert(__Count("CMD13", check) == 0);
    __LetGo();
    __Run(0);
    __ExpectAnswer(1, OS_EOK, 8);
    // Once idle again, the next check comes 1 s after the answer.
    check = g_eventCount;
    __Run(SECOND_US - 1);
    assert(__Count("CMD13", check) == 0);
    __Run(1);
    assert(__Count("CMD13", check) == 1);
    __StopSlot(OS_EOK);
    printf("Start with a card on a slot without a detect line passed\n");
}

static void
TestProbeScheduleNoDetectLine(void)
{
    uint64_t start;
    int      logs;
    int      i;

    __Reset();
    __StartSlot(false);
    __Run(0);
    // The start says how cards are found, and from then on an empty slot
    // logs nothing at the always-visible levels.
    assert(__CountLogs("no card detect line") == 1);
    logs = g_logCount;

    // Ten tries 1 s apart (counted from the end of the last try), then 3 s.
    // Each empty probe is the SD setup (a power cycle, CMD0, CMD8 and CMD55)
    // and then the MMC setup (CMD0 and CMD1 on the powered slot). A probe
    // has exactly one power cycle.
    __Run(40 * SECOND_US);
    assert(g_setupCount >= 18);
    assert(g_setupTimes[0] == POWER_CYCLE_US);
    for (i = 1; i < 10; i++) {
        assert(g_setupTimes[i] - g_setupTimes[i - 1] == SECOND_US + POWER_CYCLE_US);
    }
    for (i = 10; i < 18; i++) {
        assert(g_setupTimes[i] - g_setupTimes[i - 1] == 3 * SECOND_US + POWER_CYCLE_US);
    }
    assert(g_powerOffCount == g_setupCount);
    assert(__Count("CMD0", 0) == 2 * g_setupCount);
    assert(__Count("CMD8", 0) == g_setupCount && __Count("CMD55", 0) == g_setupCount);
    assert(__Count("CMD1 ", 0) == g_setupCount);
    assert(__Count("CMD41", 0) == 0);
    assert(__VisibleLogsFrom(logs) == 0);
    assert(!__AnyListed());

    // A card put in is found by the next probe.
    __InsertCard('A');
    start = g_nowUs;
    __Run(3 * SECOND_US + POWER_CYCLE_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(__SetupsBetween(start, g_nowUs) == 1);
    __StopSlot(OS_EOK);
    printf("Empty slot probe schedule without a detect line passed\n");
}

static void
TestDetectLine(void)
{
    int mark;

    // With a card: set up at once.
    __ReadySlot(true);
    assert(__CountLogs("detect line reports a card") == 1);
    // No idle check with a detect line.
    mark = g_eventCount;
    __Run(10 * SECOND_US);
    assert(__Count("CMD", mark) == 0);
    __StopSlot(OS_EOK);

    // Without a card: nothing at all until the insertion event.
    __Reset();
    __StartSlot(true);
    __Run(30 * SECOND_US);
    assert(__Count("CMD", 0) == 0 && g_setupCount == 0);
    __InsertCard('A');
    __SendEvent(SdHostEventCardInserted);
    __Run(100000);
    assert(__ListedId() == FIRST_STORAGE_ID);

    // Removal by event while idle: off the list, unregistered, and back to
    // waiting without commands.
    mark = g_eventCount;
    __PullCard();
    __Run(SECOND_US);
    assert(__Expect("disk removed 100", mark) < __Expect("unregister device=100 forced=1", mark));
    assert(!__AnyListed());
    mark = g_eventCount;
    __Run(30 * SECOND_US);
    assert(__Count("CMD", mark) == 0);
    __StopSlot(OS_EOK);
    printf("Slot with a detect line passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Removal.

static void
TestRemovalWithRequests(void)
{
    int removed;
    int i;

    // A chunk running and two requests queued when the card is pulled. The
    // running one ends with the host's removal result after its stop proof,
    // and all three are answered once with OS_ENOENT.
    __ReadySlot(true);
    g_holdOpcode = SD_CMD_READ_MULTIPLE_BLOCK;
    __Read(1, FIRST_STORAGE_ID, 0, 8);
    __Read(2, FIRST_STORAGE_ID, 8, 8);
    __Write(3, FIRST_STORAGE_ID, 16, 8);
    __Run(0);
    assert(g_pending != NULL && g_answers[1] == 0);
    removed = g_eventCount;
    __PullCard();
    __Run(SECOND_US);
    for (i = 1; i <= 3; i++) {
        __ExpectAnswer(i, OS_ENOENT, 0);
    }
    // Only the first request reached the card.
    assert(__SectorCommands(0) == 1);
    assert(__Expect("disk removed 100", removed) < __Expect("unregister device=100", removed));
    __StopSlot(OS_EOK);

    // Removal noticed by a CMD13 without answer on a slot without a detect
    // line. The next probe follows 1 s later.
    __ReadySlot(false);
    g_model.Present = false;
    removed = g_eventCount;
    __Run(SECOND_US);
    __Expect("CMD13", removed);
    __Expect("disk removed 100", removed);
    __Expect("unregister answer 0", removed);
    assert(__CountLogs("does not answer") == 1);
    removed = g_setupCount;
    __Run(SECOND_US + POWER_CYCLE_US);
    assert(g_setupCount == removed + 1);
    __StopSlot(OS_EOK);

    // On a slot without a detect line, a read to a card that is gone fails,
    // the CMD13 that follows gets no answer, and the queue fails with
    // OS_ENOENT.
    __ReadySlot(false);
    g_model.Present = false;
    __Read(1, FIRST_STORAGE_ID, 0, 1);
    __Read(2, FIRST_STORAGE_ID, 1, 1);
    __Run(SECOND_US / 2);
    __ExpectAnswer(1, OS_ETIMEOUT, 0);
    __ExpectAnswer(2, OS_ENOENT, 0);
    assert(!__AnyListed());
    __StopSlot(OS_EOK);
    printf("Removal with requests passed\n");
}

static void
TestRemovalDuringRegistration(void)
{
    int mark;

    // filed's answer to the register is still on its way when the card is
    // pulled. The unregister follows that answer, never before it.
    __Reset();
    __InsertCard('A');
    g_statusDefault = GRACHT_MESSAGE_INPROGRESS;
    __StartSlot(true);
    __Run(100000);
    __Expect("register device=100", 0);
    mark = g_eventCount;
    __PullCard();
    __Run(SECOND_US);
    __Expect("disk removed 100", mark);
    assert(__Find("unregister", mark) < 0);
    g_statusDefault = GRACHT_MESSAGE_COMPLETED;
    __Run(SECOND_US);
    assert(__Expect("register answer 0", mark) < __Expect("unregister device=100", mark));
    __Expect("unregister answer 0", mark);

    // The next card is registered only after that.
    __InsertCard('A');
    __SendEvent(SdHostEventCardInserted);
    __Run(SECOND_US);
    assert(__Expect("unregister answer 0", mark) < __Expect("register device=101", mark));
    __StopSlot(OS_EOK);
    printf("Removal during registration passed\n");
}

static void
TestRemovalDuringReinitialisation(void)
{
    int i;

    // A read fails three times (the sector layer's own retries), the card
    // answers CMD13, and setup runs again. The card is pulled during that
    // setup.
    __ReadySlot(true);
    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_READ_SINGLE_BLOCK, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    }
    __Read(1, FIRST_STORAGE_ID, 5, 1);
    __Read(2, FIRST_STORAGE_ID, 6, 1);
    g_holdOpcode = SD_CMD_ALL_SEND_CID;
    __Run(SECOND_US);
    assert(g_pending != NULL && g_pending->Opcode == SD_CMD_ALL_SEND_CID);
    assert(g_answers[1] == 0 && g_answers[2] == 0);
    __PullCard();
    __Run(SECOND_US);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);
    __ExpectAnswer(2, OS_ENOENT, 0);
    __Expect("unregister answer 0", 0);
    assert(!__AnyListed());
    __StopSlot(OS_EOK);
    printf("Removal during reinitialisation passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Insertion and identity.

static void
TestInsertionIdentity(void)
{
    int mark;

    __ReadySlot(true);
    __Read(1, FIRST_STORAGE_ID, 0, 1);
    __Run(0);
    __ExpectAnswer(1, OS_EOK, 1);

    // The same card again: a new storage device ID, and the old one is
    // unregistered before the new one is registered.
    __PullCard();
    __Run(SECOND_US);
    __InsertCard('A');
    __SendEvent(SdHostEventCardInserted);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    assert(__Expect("unregister device=100", 0) < __Expect("register device=101", 0));

    // A request for the old ID never reaches the new card.
    mark = g_eventCount;
    __Read(2, FIRST_STORAGE_ID, 0, 1);
    __Run(0);
    __ExpectAnswer(2, OS_ENOENT, 0);
    assert(__SectorCommands(mark) == 0);

    // Another card: yet another ID, again after the unregister.
    __PullCard();
    __Run(SECOND_US);
    __InsertCard('B');
    __SendEvent(SdHostEventCardInserted);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 2);
    assert(__Expect("unregister device=101", 0) < __Expect("register device=102", 0));
    mark = g_eventCount;
    __Read(3, FIRST_STORAGE_ID + 1, 0, 1);
    __Read(4, FIRST_STORAGE_ID + 2, 0, 1);
    __Run(0);
    __ExpectAnswer(3, OS_ENOENT, 0);
    __ExpectAnswer(4, OS_EOK, 1);
    assert(__Count("data CMD17 card=B", mark) == 1 && __SectorCommands(mark) == 1);
    __StopSlot(OS_EOK);
    printf("New identity for every insertion passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Reinitialisation.

static void
TestReinitSameCardRead(void)
{
    int first;
    int i;

    // A read whose retries are used up, with three requests behind it. The
    // same card comes back: the read is sent once more, the queue follows
    // in arrival order, and the disk keeps its ID.
    __ReadySlot(false);
    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_READ_MULTIPLE_BLOCK, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    }
    first = g_eventCount;
    g_holdOpcode = SD_CMD_READ_MULTIPLE_BLOCK;
    g_holdSkip = 3;
    __Read(1, FIRST_STORAGE_ID, 0x100, 4);
    __Read(2, FIRST_STORAGE_ID, 0x200, 1);
    __Read(3, FIRST_STORAGE_ID, 0x300, 1);
    __Write(4, FIRST_STORAGE_ID, 0x400, 1);
    __Run(SECOND_US / 2);
    // The read sent once more takes 2 s. The idle check that came back
    // with Ready must not send CMD13 meanwhile.
    assert(g_pending != NULL && g_pending->Opcode == SD_CMD_READ_MULTIPLE_BLOCK && g_holding);
    i = g_eventCount;
    __Run(2 * SECOND_US);
    assert(__Count("CMD13", i) == 0);
    __LetGo();
    __Run(SECOND_US / 2);
    for (i = 1; i <= 4; i++) {
        assert(g_answers[i] == 1 && g_answerStatus[i] == OS_EOK);
    }
    assert(g_answerSectors[1] == 4);
    // Three failed reads, the card check, setup, and the read once more.
    assert(__Count("data CMD18 card=A arg=0x100", first) == 4);
    assert(__Find("data CMD18 card=A arg=0x100", __Expect("CMD0", first)) > 0);
    // The queue after the read, in arrival order.
    assert(__Find("data CMD17 card=A arg=0x200", first) < __Find("data CMD17 card=A arg=0x300", first));
    assert(__Find("data CMD17 card=A arg=0x300", first) < __Find("data CMD24 card=A arg=0x400", first));
    // Same disk: nothing removed or added, nothing unregistered.
    assert(g_diskCount == 1 && __ListedId() == FIRST_STORAGE_ID);
    assert(__Count("unregister", 0) == 0);
    assert(__CountLogs("same card is back") == 1);
    __StopSlot(OS_EOK);
    printf("Reinitialisation of the same card after a failed read passed\n");
}

static void
TestReinitSameCardWrite(void)
{
    int first;

    // A write whose result is uncertain is never sent again. The read
    // behind it runs after the card came back.
    __ReadySlot(false);
    __Fault(SD_CMD_WRITE_MULTIPLE_BLOCK, OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, true);
    first = g_eventCount;
    __Write(1, FIRST_STORAGE_ID, 0x100, 8);
    __Read(2, FIRST_STORAGE_ID, 0x200, 2);
    __Run(SECOND_US / 2);
    __ExpectAnswer(1, OS_ETIMEOUT, 0);
    __ExpectAnswer(2, OS_EOK, 2);
    assert(__Count("data CMD25", first) == 1);
    __Expect("CMD0", first);
    assert(__ListedId() == FIRST_STORAGE_ID && g_diskCount == 1);
    __StopSlot(OS_EOK);
    printf("Reinitialisation never sends a write again passed\n");
}

static void
TestReinitDifferentOrNoCard(void)
{
    int mark;
    int i;

    // During the setup after a failed read, another card answers: a removal
    // followed by an insertion. Old requests fail, the new card gets a new
    // ID, and nothing for the old ID reaches it.
    __ReadySlot(false);
    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_READ_SINGLE_BLOCK, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    }
    g_removeAfterOpcode = SD_CMD_SEND_STATUS;
    g_removeAfterMatches = 3;
    __Read(1, FIRST_STORAGE_ID, 7, 1);
    __Read(2, FIRST_STORAGE_ID, 8, 1);
    // The card leaves after answering CMD13, and card B is put in before
    // setup runs.
    __Run(0);
    __InsertCard('B');
    mark = g_eventCount;
    __Run(SECOND_US);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);
    __ExpectAnswer(2, OS_ENOENT, 0);
    assert(__CountLogs("another card answered") == 1);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    assert(__Expect("unregister device=100", mark) < __Expect("register device=101", mark));
    assert(__SectorCommands(mark) == 0);
    __Read(3, FIRST_STORAGE_ID, 7, 1);
    __Run(0);
    __ExpectAnswer(3, OS_ENOENT, 0);
    __StopSlot(OS_EOK);

    // No card answers the setup: a removal.
    __ReadySlot(false);
    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_READ_SINGLE_BLOCK, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    }
    g_removeAfterOpcode = SD_CMD_SEND_STATUS;
    g_removeAfterMatches = 3;
    __Read(1, FIRST_STORAGE_ID, 7, 1);
    __Run(SECOND_US / 2);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);
    assert(__CountLogs("no card answered") == 1);
    assert(!__AnyListed());
    __StopSlot(OS_EOK);
    printf("Reinitialisation finding another card or none passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// The recovery ladder.

// One failed read with its retries used up, answered once more after the
// recovery. Returns the event index where it started.
static int
__FailOneRead(
    _In_ int id)
{
    int mark = g_eventCount;
    int i;

    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_READ_SINGLE_BLOCK, OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, true);
    }
    __Read(id, __ListedId(), (uint64_t)id, 1);
    __Run(SECOND_US / 2);
    return mark;
}

static void
TestRecoveryLadder(void)
{
    int mark;

    // First recovery: the same 25 MHz. Second within 60 s: half of it.
    // Third within 60 s: the disk fails.
    __ReadySlot(true);
    mark = __FailOneRead(1);
    __ExpectAnswer(1, OS_EOK, 1);
    __Expect("clock 25000000", mark);
    __Run(10 * SECOND_US);
    mark = __FailOneRead(2);
    __ExpectAnswer(2, OS_EOK, 1);
    __Expect("clock 12500000", mark);
    assert(__Find("clock 25000000", mark) < 0);
    __Run(10 * SECOND_US);
    mark = __FailOneRead(3);
    __ExpectAnswer(3, OS_ETIMEOUT, 0);
    assert(__Find("CMD0", mark) < 0);
    __Expect("disk removed 100", mark);
    __Expect("unregister answer 0", mark);
    assert(__CountLogs("taking it away") == 1);
    // The failed card stays unused: no probing while it is in the slot.
    mark = g_eventCount;
    __Run(60 * SECOND_US);
    assert(__Count("CMD", mark) == 0);
    // Pulled and put back, it gets a new disk.
    __PullCard();
    __InsertCard('A');
    __SendEvent(SdHostEventCardInserted);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    __StopSlot(OS_EOK);

    // The count starts again 60 s after the first recovery, and the lower
    // clock stays.
    __ReadySlot(true);
    __FailOneRead(1);
    __FailOneRead(2);
    __Run(60 * SECOND_US);
    mark = __FailOneRead(3);
    __ExpectAnswer(3, OS_EOK, 1);
    __Expect("clock 12500000", mark);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __StopSlot(OS_EOK);

    // The clock is never halved below the 400 kHz setup clock.
    __Reset();
    g_host.Limits.MaximumClockHz = 600000U;
    __InsertCard('A');
    __StartSlot(true);
    __Run(SECOND_US);
    __FailOneRead(1);
    mark = __FailOneRead(2);
    __ExpectAnswer(2, OS_EOK, 1);
    assert(__Find("clock 300000", mark) < 0 && __Find("clock 600000", mark) < 0);
    assert(__CountLogs("data clock limit 400000 Hz") == 1);
    __StopSlot(OS_EOK);
    printf("Recovery ladder passed\n");
}

static void
TestFailedCardWithoutDetectLine(void)
{
    int mark;

    // Without a detect line the failed card is set up quietly every 10 s and
    // recognised by its CID. Another card ends the wait.
    __ReadySlot(false);
    __FailOneRead(1);
    __FailOneRead(2);
    __FailOneRead(3);
    __ExpectAnswer(3, OS_ETIMEOUT, 0);
    assert(!__AnyListed());
    mark = g_logCount;
    __Run(35 * SECOND_US);
    assert(!__AnyListed() && g_diskCount == 1);
    assert(__SetupsBetween(g_nowUs - 35 * SECOND_US, g_nowUs) == 3);
    assert(__VisibleLogsFrom(mark) == 0);
    __InsertCard('B');
    __Run(11 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    __StopSlot(OS_EOK);
    printf("Failed card on a slot without a detect line passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Host failure.

static void
TestHostFailedFromReady(void)
{
    int mark;

    // A chunk ends without stop proof: its memory is kept and the request
    // is answered OS_EDEVFAULT. The host's failure event takes the disk
    // away. The reset 1 s later works, and the card gets a new disk.
    __ReadySlot(true);
    g_keptId = 1;
    __Fault(SD_CMD_READ_MULTIPLE_BLOCK, OS_ETIMEOUT, SD_TRANSPORT_DEADLINE, false);
    __Read(1, FIRST_STORAGE_ID, 0, 8);
    __Read(2, FIRST_STORAGE_ID, 8, 8);
    __Run(0);
    mark = g_eventCount;
    __SendEvent(SdHostEventFailed);
    __Run(500000);
    assert(g_hostFailedCount == 1);
    __Expect("disk removed 100", mark);
    __Expect("unregister answer 0", mark);
    // The kept chunk: mapped and attached, never released.
    assert(g_attached[1] && __LiveMappingsOf(1) == 1);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);
    __ExpectAnswer(2, OS_EDEVFAULT, 0);
    assert(g_resetCount == 0);
    __Run(600000);
    assert(g_resetCount == 1);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    // Stopping keeps the memory, so the slot cannot be destroyed.
    __StopSlot(OS_EBUSY);
    assert(g_attached[1] && __LiveMappingsOf(1) == 1);
    printf("Host failure from Ready passed\n");
}

static void
TestHostFailedFromOtherStates(void)
{
    // During setup: setup is stopped through the host, the reset fails, and
    // the slot stays failed without trying again.
    __Reset();
    __InsertCard('A');
    g_holdOpcode = SD_CMD_ALL_SEND_CID;
    __StartSlot(true);
    __Run(SECOND_US);
    assert(g_pending != NULL);
    g_resetStatus = OS_ETIMEOUT;
    __SendEvent(SdHostEventFailed);
    __Run(0);
    __Expect("host stop CMD2", 0);
    __Run(30 * SECOND_US);
    assert(g_resetCount == 1 && g_hostFailedCount == 1 && !__AnyListed());
    // A second event while failed is not reported again.
    __SendEvent(SdHostEventFailed);
    assert(g_hostFailedCount == 1);
    __StopSlot(OS_EOK);

    // From Empty on a slot without a detect line: probing stops, and starts
    // again after the reset.
    __Reset();
    __StartSlot(false);
    __Run(5 * SECOND_US);
    __SendEvent(SdHostEventFailed);
    __Run(0);
    assert(g_hostFailedCount == 1 && g_resetCount == 0);
    __InsertCard('A');
    __Run(SECOND_US + 100000);
    assert(g_resetCount == 1);
    assert(__ListedId() == FIRST_STORAGE_ID);
    // A failure after a working reset is reported again.
    __SendEvent(SdHostEventFailed);
    __Run(0);
    assert(g_hostFailedCount == 2);
    __StopSlot(OS_EOK);
    printf("Host failure from setup and from an empty slot passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Card setup that fails in the host.

static void __StopTwice(oserr_t expected);

// The setup that just failed at power off was handled as a host failure:
// HostFailed told once more, one ERROR line from the sequence runner that
// names the step and the error, no WARNING, and no card command sent.
static void
__ExpectSetupHostFailure(
    _In_ int mark,
    _In_ int hostFailures)
{
    if (g_hostFailedCount != hostFailures || __LogsAtLevel(OSSYSLOGLEVEL_ERROR, mark) != 1) {
        __DumpEvents();
    }
    assert(g_hostFailedCount == hostFailures);
    assert(__LogsAtLevel(OSSYSLOGLEVEL_ERROR, mark) == 1);
    assert(__LogsAtLevel(OSSYSLOGLEVEL_WARNING, mark) == 0);
    assert(__CountLogs("SD setup: step power off failed: 24") == hostFailures);
    assert(__CountLogs("card setup failed 3 times in a row") == 0);
}

// Starts a slot whose host cannot switch the card supply. The first probe
// fails, the reset 1 s later works, and the probe after it fails the same
// way. The slot then stays failed: no second reset, no more probes, nothing
// in the log. A stop still finishes once.
static void
__ExpectSupplyNeverSwitches(
    _In_ bool detectLine)
{
    int mark;

    __Reset();
    __InsertCard('A');
    g_powerOffStatus = OS_EDEVFAULT;
    mark = g_logCount;
    __StartSlot(detectLine);
    __Run(500000);
    assert(g_powerOffCount == 1 && g_setupCount == 0 && g_resetCount == 0);
    __ExpectSetupHostFailure(mark, 1);

    mark = g_logCount;
    __Run(600000);
    assert(g_resetCount == 1 && g_powerOffCount == 2 && g_setupCount == 0);
    __ExpectSetupHostFailure(mark, 2);
    assert(__CountLogs("the slot stays failed until it is stopped") == 1);

    mark = g_logCount;
    __Run(60 * SECOND_US);
    assert(g_resetCount == 1 && g_powerOffCount == 2 && g_hostFailedCount == 2);
    assert(g_workList == NULL && __VisibleLogsFrom(mark) == 0);
    // A host failure event while failed is not reported again.
    __SendEvent(SdHostEventFailed);
    assert(g_hostFailedCount == 2);
    __StopTwice(OS_EOK);
    assert(g_diskCount == 0);
}

static void
TestSetupHostFailureAtStart(void)
{
    __ExpectSupplyNeverSwitches(false);
    printf("Setup host failure at start without a detect line passed\n");
    __ExpectSupplyNeverSwitches(true);
    printf("Setup host failure at start with a detect line passed\n");
}

static void
TestSetupHostFailureAfterReady(void)
{
    int mark;

    // Without a detect line: the card was Ready, its removal is noticed by
    // the idle check, and the next probe fails at power off. One reset,
    // then the slot stays failed.
    __ReadySlot(false);
    g_model.Present = false;
    g_powerOffStatus = OS_EDEVFAULT;
    g_powerOffCount = 0;
    mark = g_logCount;
    // Idle check 1 s after the last request, then a probe 1 s later.
    __Run(2 * SECOND_US + 200000);
    assert(!__AnyListed() && g_powerOffCount == 1);
    __ExpectSetupHostFailure(mark, 1);
    __Run(60 * SECOND_US);
    assert(g_resetCount == 1 && g_powerOffCount == 2 && g_hostFailedCount == 2);
    assert(g_workList == NULL);
    __StopSlot(OS_EOK);

    // With a detect line: the reset works and the next setup works too, so
    // the card becomes Ready. A later host failure gets its reset again.
    __Reset();
    __InsertCard('A');
    g_powerOffStatus = OS_EDEVFAULT;
    __StartSlot(true);
    __Run(500000);
    assert(g_hostFailedCount == 1 && g_resetCount == 0);
    g_powerOffStatus = OS_EOK;
    __Run(600000);
    assert(g_resetCount == 1 && __ListedId() == FIRST_STORAGE_ID);
    __PullCard();
    __Run(SECOND_US);
    assert(!__AnyListed());
    g_powerOffStatus = OS_EDEVFAULT;
    __InsertCard('A');
    mark = g_logCount;
    __SendEvent(SdHostEventCardInserted);
    __Run(500000);
    __ExpectSetupHostFailure(mark, 2);
    assert(g_resetCount == 1);
    __Run(600000);
    assert(g_resetCount == 2 && g_hostFailedCount == 3);
    __Run(60 * SECOND_US);
    assert(g_resetCount == 2 && g_hostFailedCount == 3 && g_workList == NULL);
    __StopSlot(OS_EOK);
    printf("Setup host failure after a ready card passed\n");
}

static void
TestSetupCardFailures(void)
{
    uint64_t failedUs;
    int      i;

    // A card command that fails during setup is the card's failure. After
    // three in a row the card stays unused: no host failure, no reset, and
    // the next quiet probe comes 10 s later.
    __Reset();
    __InsertCard('A');
    for (i = 0; i < 3; i++) {
        __Fault(SD_CMD_ALL_SEND_CID, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC, true);
    }
    __StartSlot(false);
    __Run(2500000);
    assert(g_setupCount == 3 && __CountLogs("card setup failed 3 times in a row") == 1);
    assert(g_hostFailedCount == 0 && g_resetCount == 0 && !__AnyListed());
    failedUs = g_nowUs;
    __Run(9 * SECOND_US);
    assert(__SetupsBetween(failedUs, g_nowUs) == 0);
    __Run(2 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID && g_hostFailedCount == 0);
    __StopSlot(OS_EOK);
    printf("Setup card failures passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// The stage's acceptance cases: faults at every step of a request.

static void
TestFaultsAtEveryStep(void)
{
    static const struct {
        const char* Name;
        oserr_t     Status;
        uint32_t    Transport;
    } faults[] = {
        { "command timeout", OS_ETIMEOUT, SD_TRANSPORT_COMMAND_TIMEOUT },
        { "lost interrupt", OS_ETIMEOUT, SD_TRANSPORT_DEADLINE },
        { "data CRC", OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC },
        { "ADMA error", OS_EDEVFAULT, SD_TRANSPORT_DMA },
    };
    unsigned int f;
    int          i;

    for (f = 0; f < sizeof(faults) / sizeof(faults[0]); f++) {
        // A read: one answer, OK, after the recovery. A write: one answer
        // with the error, sent once.
        __ReadySlot(true);
        for (i = 0; i < 3; i++) {
            __Fault(SD_CMD_READ_MULTIPLE_BLOCK, faults[f].Status, faults[f].Transport, true);
        }
        __Read(1, FIRST_STORAGE_ID, 0, 16);
        __Run(SECOND_US / 2);
        __ExpectAnswer(1, OS_EOK, 16);
        __Fault(SD_CMD_WRITE_MULTIPLE_BLOCK, faults[f].Status, faults[f].Transport, true);
        i = g_eventCount;
        __Write(2, FIRST_STORAGE_ID, 0, 16);
        __Run(SECOND_US / 2);
        __ExpectAnswer(2, faults[f].Status, 0);
        assert(__Count("data CMD25", i) == 1);
        __StopSlot(OS_EOK);
        printf("Fault %s passed\n", faults[f].Name);
    }

    // Failed cleanup calls while the card is pulled with a chunk running:
    // the answer waits for every cleanup call to succeed, and so does the
    // end of Leaving.
    __ReadySlot(true);
    g_endScript[g_endScriptCount++] = OS_EUNKNOWN;
    g_endScript[g_endScriptCount++] = OS_EUNKNOWN;
    g_unmapScript[g_unmapScriptCount++] = OS_EUNKNOWN;
    g_releaseScript[g_releaseScriptCount++] = OS_EUNKNOWN;
    g_holdOpcode = SD_CMD_READ_MULTIPLE_BLOCK;
    __Read(1, FIRST_STORAGE_ID, 0, 8);
    __Read(2, FIRST_STORAGE_ID, 8, 8);
    __Run(0);
    __PullCard();
    __Run(5000);
    assert(g_answers[1] == 0 && g_attached[1]);
    __Run(SECOND_US);
    __ExpectAnswer(1, OS_ENOENT, 0);
    __ExpectAnswer(2, OS_ENOENT, 0);
    assert(__Count("end id=1", 0) == 3 && __Count("unmap id=1", 0) == 2 && __Count("release id=1", 0) == 2);
    assert(__Expect("release id=1", __Find("unmap id=1", 0)) < __Expect("answer id=1", 0));
    __StopSlot(OS_EOK);
    printf("Failed cleanup calls passed\n");

    // A chunk stopped by teardown: the mapping is released only after the
    // host's stop proof.
    __ReadySlot(true);
    g_holdOpcode = SD_CMD_WRITE_MULTIPLE_BLOCK;
    __Write(1, FIRST_STORAGE_ID, 0, 8);
    __Run(0);
    assert(SDSlotStop(g_slot) == OS_EOK);
    // Run the stop step only. The stopped request has not completed yet.
    g_pauseCompletions = true;
    __Run(0);
    __Expect("host stop CMD25", 0);
    assert(__LiveMappingsOf(1) == 1 && g_attached[1] && g_answers[1] == 0);
    g_pauseCompletions = false;
    __Run(60 * SECOND_US);
    __ExpectAnswer(1, OS_ECANCELLED, 0);
    assert(g_stoppedCount == 1 && g_stoppedStatus == OS_EOK);
    assert(SDSlotDestroy(g_slot) == OS_EOK);
    g_slot = NULL;

    // The same without stop proof: the memory is kept, and so is the slot.
    __ReadySlot(true);
    g_holdOpcode = SD_CMD_WRITE_MULTIPLE_BLOCK;
    g_stopProof = false;
    g_keptId = 1;
    __Write(1, FIRST_STORAGE_ID, 0, 8);
    __Run(0);
    __StopSlot(OS_EBUSY);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);
    assert(__LiveMappingsOf(1) == 1 && g_attached[1]);
    printf("Stop proof before release passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Leaving from every state.

static void
__StopTwice(
    _In_ oserr_t expected)
{
    assert(SDSlotStop(g_slot) == OS_EOK);
    assert(SDSlotStop(g_slot) == OS_EOK);
    __Run(0);
    assert(SDSlotStop(g_slot) == OS_EOK);
    __StopSlot(expected);
}

static void
TestLeaveFromEveryState(void)
{
    SDSlotParameters_t parameters = { .LogId = 9, .DriverId = DRIVER_ID, .Callbacks = &g_callbacks,
                                      .Context = &g_disks };

    // New: never started.
    __Reset();
    assert(SDSlotCreate(&g_host, &parameters, &g_slot) == OS_EOK);
    __StopTwice(OS_EOK);
    assert(g_setupCount == 0);

    // Empty.
    __Reset();
    __StartSlot(false);
    __Run(1500000);
    __StopTwice(OS_EOK);

    // SettingUp, with a command held by the host.
    __Reset();
    __InsertCard('A');
    g_holdOpcode = SD_CMD_SEND_CSD;
    __StartSlot(false);
    __Run(SECOND_US);
    assert(g_pending != NULL);
    __StopTwice(OS_EOK);
    __Expect("host stop CMD9", 0);
    assert(g_diskCount == 0);

    // Ready and idle.
    __ReadySlot(false);
    __StopTwice(OS_EOK);
    __Expect("unregister device=100", 0);

    // Ready with a chunk running and a queue.
    __ReadySlot(true);
    g_holdOpcode = SD_CMD_READ_MULTIPLE_BLOCK;
    __Read(1, FIRST_STORAGE_ID, 0, 8);
    __Read(2, FIRST_STORAGE_ID, 8, 8);
    __Run(0);
    __StopTwice(OS_EOK);
    __ExpectAnswer(1, OS_ECANCELLED, 0);
    __ExpectAnswer(2, OS_ECANCELLED, 0);

    // Checking, with the idle CMD13 held.
    __ReadySlot(false);
    g_holdOpcode = SD_CMD_SEND_STATUS;
    __Run(SECOND_US);
    assert(g_pending != NULL && g_pending->Opcode == SD_CMD_SEND_STATUS);
    __Read(1, FIRST_STORAGE_ID, 0, 1);
    __Run(0);
    __StopTwice(OS_EOK);
    __ExpectAnswer(1, OS_ECANCELLED, 0);
    assert(__SectorCommands(0) == 0);

    // Reinitialising, with CMD2 held.
    __ReadySlot(true);
    __Fault(SD_CMD_WRITE_BLOCK, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    __Write(1, FIRST_STORAGE_ID, 0, 1);
    g_holdOpcode = SD_CMD_ALL_SEND_CID;
    __Run(SECOND_US / 2);
    assert(g_pending != NULL && g_pending->Opcode == SD_CMD_ALL_SEND_CID);
    __StopTwice(OS_EOK);
    __ExpectAnswer(1, OS_EDEVFAULT, 0);

    // Leaving, with filed's register answer still on its way.
    __Reset();
    __InsertCard('A');
    g_statusDefault = GRACHT_MESSAGE_INPROGRESS;
    __StartSlot(true);
    __Run(100000);
    __PullCard();
    __Run(0);
    assert(SDSlotStop(g_slot) == OS_EOK);
    __Run(SECOND_US);
    assert(g_stoppedCount == 0);
    g_statusDefault = GRACHT_MESSAGE_COMPLETED;
    __StopTwice(OS_EOK);
    assert(__Expect("register answer", 0) < __Expect("unregister device=100", 0));

    // Failed, waiting for its reset.
    __ReadySlot(true);
    __SendEvent(SdHostEventFailed);
    __Run(500000);
    __StopTwice(OS_EOK);
    assert(g_resetCount == 0);
    printf("Leaving from every state passed\n");
}


/////////////////////////////////////////////////////////////////////////////
// Four data lines and high speed.

// The slot host with high speed, so setup ends at four lines, high speed
// and 50 MHz.
static void
__ResetHighSpeed(void)
{
    __Reset();
    g_host.Limits.Flags |= SD_HOST_FLAG_HIGH_SPEED;
}

static void
TestSetupLimitsAfterFailures(void)
{
    int mark;

    // A width failure: the next setup has no ACMD6 and ends at one line,
    // high speed.
    __ResetHighSpeed();
    __InsertCard('A');
    __FaultApp(SD_CMD_SEND_STATUS, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    __StartSlot(false);
    __Run(400000);
    assert(g_setupCount == 1 && !__AnyListed());
    assert(__CountLogs("the next setup uses one data line") == 1);
    mark = g_eventCount;
    __Run(2 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(__Find("CMD6 arg=0x2", mark) < 0);
    __Expect("data CMD6 card=A arg=0xfffff1", mark);
    __Expect("bus 1 lines timing 1", mark);
    __Expect("clock 50000000", mark);
    assert(__CountLogs("card ready, 1 data line, high speed, 50000000 Hz") == 1);
    __StopSlot(OS_EOK);

    // A speed failure: the next setup has no CMD6 and ends at four lines,
    // default speed, 25 MHz.
    __ResetHighSpeed();
    __InsertCard('A');
    __Fault(SD_CMD_SWITCH_FUNC, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC, true);
    __StartSlot(false);
    __Run(400000);
    assert(g_setupCount == 1 && !__AnyListed());
    assert(__CountLogs("the next setup uses default speed") == 1);
    mark = g_eventCount;
    __Run(2 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __Expect("CMD6 arg=0x2", mark);
    assert(__Find("data CMD6", mark) < 0);
    __Expect("clock 25000000", mark);
    assert(__CountLogs("card ready, 4 data lines, default speed, 25000000 Hz") == 1);
    __StopSlot(OS_EOK);

    // Both limits, and the limit of three failed setups still holds: width,
    // speed and then a plain failure leave the card unused.
    __ResetHighSpeed();
    __InsertCard('A');
    __FaultApp(SD_CMD_SWITCH_FUNC, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC);
    __Fault(SD_CMD_SWITCH_FUNC, OS_ETIMEOUT, SD_TRANSPORT_DATA_TIMEOUT, true);
    __Fault(SD_CMD_ALL_SEND_CID, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC, true);
    __StartSlot(false);
    __Run(2500000);
    assert(g_setupCount == 3 && !__AnyListed());
    assert(__CountLogs("the next setup uses one data line") == 1);
    assert(__CountLogs("the next setup uses default speed") == 1);
    assert(__CountLogs("card setup failed 3 times in a row") == 1);
    // The card is left unused, so its limits are gone: the quiet probe 10 s
    // later tries every feature again and finds the card working.
    mark = g_eventCount;
    __Run(11 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __Expect("CMD6 arg=0x2", mark);
    __Expect("clock 50000000", mark);
    __StopSlot(OS_EOK);
    printf("Setup limits after failures passed\n");
}

static void
TestSetupLimitsClearedForNewCard(void)
{
    int mark;

    // Card A comes up at one line after a width failure. Card B, put in
    // after A was removed, gets four lines again.
    __ResetHighSpeed();
    __InsertCard('A');
    __FaultApp(SD_CMD_SEND_STATUS, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    __StartSlot(true);
    __Run(2 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(__CountLogs("card ready, 1 data line, high speed, 50000000 Hz") == 1);
    __PullCard();
    __Run(SECOND_US);
    assert(!__AnyListed());
    mark = g_eventCount;
    __InsertCard('B');
    __SendEvent(SdHostEventCardInserted);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    __Expect("CMD6 arg=0x2", mark);
    __Expect("bus 4 lines timing 1", mark);
    assert(__CountLogs("card ready, 4 data lines, high speed, 50000000 Hz") == 1);
    __StopSlot(OS_EOK);

    // A card the slot did not find answers no setup limit either: a width
    // failure, then an empty slot, then a card.
    __ResetHighSpeed();
    __InsertCard('A');
    __FaultApp(SD_CMD_SEND_STATUS, OS_EDEVFAULT, SD_TRANSPORT_DATA_CRC);
    __StartSlot(false);
    __Run(400000);
    assert(__CountLogs("the next setup uses one data line") == 1);
    g_model.Present = false;
    __Run(1200000);
    mark = g_eventCount;
    __InsertCard('B');
    __Run(2 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __Expect("CMD6 arg=0x2", mark);
    __StopSlot(OS_EOK);
    printf("Setup limits cleared for a new card passed\n");
}

static void
TestRecoveryLadderHighSpeed(void)
{
    int mark;

    // First recovery: the same high speed at 50 MHz. Second within 60 s:
    // default speed at 25 MHz, not half of 50 MHz. Third: the disk fails.
    __ResetHighSpeed();
    __InsertCard('A');
    __StartSlot(true);
    __Run(SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(__CountLogs("card ready, 4 data lines, high speed, 50000000 Hz") == 1);
    mark = __FailOneRead(1);
    __ExpectAnswer(1, OS_EOK, 1);
    __Expect("data CMD6 card=A arg=0x80fffff1", mark);
    __Expect("clock 50000000", mark);
    __Run(10 * SECOND_US);
    mark = __FailOneRead(2);
    __ExpectAnswer(2, OS_EOK, 1);
    assert(__Find("data CMD6", mark) < 0);
    assert(__Find("bus 4 lines timing 1", mark) < 0);
    __Expect("CMD6 arg=0x2", mark);
    __Expect("clock 25000000", mark);
    assert(__Find("clock 12500000", mark) < 0);
    assert(__CountLogs("recovery 2 within 60 s, default speed") == 1);
    assert(__CountLogs("card ready, 4 data lines, default speed, 25000000 Hz") == 1);
    __Run(10 * SECOND_US);
    mark = __FailOneRead(3);
    __ExpectAnswer(3, OS_ETIMEOUT, 0);
    assert(__Find("CMD0", mark) < 0);
    __Expect("disk removed 100", mark);
    __StopSlot(OS_EOK);

    // Once the window has ended, the card keeps default speed for the life
    // of the disk, and the next second recovery halves the 25 MHz.
    __ResetHighSpeed();
    __InsertCard('A');
    __StartSlot(true);
    __Run(SECOND_US);
    __FailOneRead(1);
    __FailOneRead(2);
    __Run(60 * SECOND_US);
    mark = __FailOneRead(3);
    __ExpectAnswer(3, OS_EOK, 1);
    assert(__Find("data CMD6", mark) < 0);
    __Expect("clock 25000000", mark);
    __Run(10 * SECOND_US);
    mark = __FailOneRead(4);
    __ExpectAnswer(4, OS_EOK, 1);
    __Expect("clock 12500000", mark);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __StopSlot(OS_EOK);

    // The first recovery fails in its setup, so the second follows at once,
    // and it still knows that the card ran at high speed.
    __ResetHighSpeed();
    __InsertCard('A');
    __StartSlot(true);
    __Run(SECOND_US);
    __Fault(SD_CMD_ALL_SEND_CID, OS_EDEVFAULT, SD_TRANSPORT_COMMAND_CRC, true);
    mark = __FailOneRead(1);
    __ExpectAnswer(1, OS_EOK, 1);
    mark = __Expect("CMD2", __Expect("CMD2", mark) + 1);
    assert(__Find("data CMD6", mark) < 0);
    __Expect("clock 25000000", mark);
    assert(__CountLogs("recovery 2 within 60 s, default speed") == 1);
    __StopSlot(OS_EOK);
    printf("Recovery ladder from high speed passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// eMMC devices.

// The slot host with eight lines and high speed.
static void
__ResetEightLines(void)
{
    __Reset();
    g_host.Limits.BusWidths |= SD_HOST_BUS_WIDTH_8;
    g_host.Limits.Flags |= SD_HOST_FLAG_HIGH_SPEED;
}

static void
TestMmcDevice(void)
{
    int sd;
    int mark;

    // The SD setup finds no SD card, then the MMC setup finds the device.
    __ResetEightLines();
    __InsertMmc('A');
    __StartSlot(false);
    __Run(300000);
    assert(__ListedId() == FIRST_STORAGE_ID);
    sd = __Expect("CMD8 arg=0x1aa", 0);
    assert(sd < __Expect("CMD55 arg=0x0", sd));
    assert(__Expect("CMD55 arg=0x0", sd) < __Expect("CMD1 arg=0x40300000 mask=0x0", sd));
    assert(__Count("CMD41", 0) == 0);
    // Eight lines, then high speed, with the MMC card error mask.
    __Expect("CMD6 arg=0x3b70201 mask=0xfd390080", 0);
    __Expect("CMD6 arg=0x3b90101 mask=0xfd390080", 0);
    __Expect("register device=100 flags=1", 0);
    assert(__CountLogs("MMC: device ready, manufacturer 0x15, product \"8GTF4R\", 7456 MiB, sector addressing, "
                       "8 data lines, high speed, 50000000 Hz") == 1);
    assert(__CountLogs("card ready, 8 data lines, high speed, 50000000 Hz") == 1);
    assert(__CountLogs("disk M15-8GTF4R-A1B2C3D4, 15269888 sectors") == 1);
    assert(__LogsAtLevel(OSSYSLOGLEVEL_ERROR, 0) == 0 && __LogsAtLevel(OSSYSLOGLEVEL_WARNING, 0) == 0);

    // The idle check uses the device's address and the MMC mask.
    mark = g_eventCount;
    __Run(SECOND_US);
    __Expect("CMD13 arg=0x10000 mask=0xfd390080", mark);

    // Data commands use the MMC mask and sector numbers.
    mark = g_eventCount;
    __Read(1, FIRST_STORAGE_ID, 0x40, 1);
    __Run(0);
    __ExpectAnswer(1, OS_EOK, 1);
    __Expect("data CMD17 card=A arg=0x40 blocks=1 mask=0xfd390080", mark);

    // A recovery: the read fails three times, CMD13, and the setup again,
    // SD first. The same device comes back as the same disk.
    __Run(SECOND_US / 2);
    mark = __FailOneRead(2);
    __ExpectAnswer(2, OS_EOK, 1);
    sd = __Expect("CMD8 arg=0x1aa", mark);
    assert(sd < __Expect("CMD1 arg=0x40300000", sd));
    assert(__CountLogs("same card is back") == 1);
    assert(g_diskCount == 1 && __ListedId() == FIRST_STORAGE_ID);
    __StopSlot(OS_EOK);

    // Another device in the slot after a recovery is another disk.
    __ResetEightLines();
    __InsertMmc('A');
    __StartSlot(false);
    __Run(300000);
    assert(__ListedId() == FIRST_STORAGE_ID);
    __Run(SECOND_US / 2);
    g_model.Label = 'B';
    __FailOneRead(1);
    __Run(SECOND_US);
    assert(__CountLogs("another card answered") == 1);
    assert(__ListedId() == FIRST_STORAGE_ID + 1);
    __StopSlot(OS_EOK);

    // A fixed slot registers the device without the removable flag.
    __ResetEightLines();
    g_host.Limits.Flags &= ~SD_HOST_FLAG_REMOVABLE;
    __InsertMmc('A');
    __StartSlot(false);
    __Run(300000);
    __Expect("register device=100 flags=0", 0);
    __StopSlot(OS_EOK);
    printf("eMMC device in a slot passed\n");
}

static void
TestMmcRemoval(void)
{
    int mark;

    // Without a detect line, the idle check finds the device gone.
    __ResetEightLines();
    __InsertMmc('A');
    __StartSlot(false);
    __Run(300000);
    assert(__ListedId() == FIRST_STORAGE_ID);
    g_model.Present = false;
    mark = g_eventCount;
    __Run(SECOND_US + 100000);
    __Expect("CMD13 arg=0x10000", mark);
    assert(__CountLogs("the card does not answer, it was removed") == 1);
    assert(!__AnyListed());
    __StopSlot(OS_EOK);

    // With a detect line, a card that answers neither SD nor MMC commands
    // waits for its removal, after both setups.
    __Reset();
    __InsertCard('A');
    g_model.Silent = true;
    __StartSlot(true);
    __Run(300000);
    assert(__Count("CMD55", 0) == 1 && __Count("CMD1 ", 0) == 1);
    assert(__CountLogs("is no SD card and no MMC device, waiting for it to be removed") == 1);
    mark = g_eventCount;
    __Run(30 * SECOND_US);
    assert(__Count("CMD", mark) == 0);
    __StopSlot(OS_EOK);
    printf("eMMC device removal passed\n");
}

static void
TestMmcWidthLadder(void)
{
    // The switch to eight lines fails, then the one to four. The third setup
    // stays at one line and the device becomes a disk.
    __ResetEightLines();
    __InsertMmc('A');
    __Fault(MMC_CMD_SWITCH, OS_ETIMEOUT, SD_TRANSPORT_BUSY_TIMEOUT, true);
    __Fault(MMC_CMD_SWITCH, OS_ETIMEOUT, SD_TRANSPORT_BUSY_TIMEOUT, true);
    __StartSlot(false);
    __Run(4 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(g_setupCount == 3);
    assert(__Count("CMD6 arg=0x3b70201", 0) == 1);
    assert(__Count("CMD6 arg=0x3b70101", 0) == 1);
    assert(__Find("CMD6 arg=0x3b70201", 0) < __Find("CMD6 arg=0x3b70101", 0));
    assert(__CountLogs("the next setup uses at most four") == 1);
    assert(__CountLogs("the next setup uses one data line") == 1);
    assert(__CountLogs("card ready, 1 data line, high speed, 50000000 Hz") == 1);
    __StopSlot(OS_EOK);

    // On a host wired for four lines, the first failure goes to one line.
    __Reset();
    g_host.Limits.Flags |= SD_HOST_FLAG_HIGH_SPEED;
    __InsertMmc('A');
    __Fault(MMC_CMD_SWITCH, OS_ETIMEOUT, SD_TRANSPORT_BUSY_TIMEOUT, true);
    __StartSlot(false);
    __Run(3 * SECOND_US);
    assert(__ListedId() == FIRST_STORAGE_ID);
    assert(g_setupCount == 2);
    assert(__CountLogs("the next setup uses at most four") == 0);
    assert(__CountLogs("the next setup uses one data line") == 1);
    assert(__CountLogs("card ready, 1 data line, high speed") == 1);
    __StopSlot(OS_EOK);
    printf("eMMC width ladder passed\n");
}

int
main(void)
{
    TestStartWithCardNoDetectLine();
    TestProbeScheduleNoDetectLine();
    TestDetectLine();
    TestRemovalWithRequests();
    TestRemovalDuringRegistration();
    TestRemovalDuringReinitialisation();
    TestInsertionIdentity();
    TestReinitSameCardRead();
    TestReinitSameCardWrite();
    TestReinitDifferentOrNoCard();
    TestRecoveryLadder();
    TestFailedCardWithoutDetectLine();
    TestSetupLimitsAfterFailures();
    TestSetupLimitsClearedForNewCard();
    TestRecoveryLadderHighSpeed();
    TestHostFailedFromReady();
    TestHostFailedFromOtherStates();
    TestSetupHostFailureAtStart();
    TestSetupHostFailureAfterReady();
    TestSetupCardFailures();
    TestFaultsAtEveryStep();
    TestLeaveFromEveryState();
    TestMmcDevice();
    TestMmcRemoval();
    TestMmcWidthLadder();
    __Reset();
    printf("SD slot tests passed\n");
    return 0;
}
