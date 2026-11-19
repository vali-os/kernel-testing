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
 * Host test of the Raspberry Pi firmware driver (modules/platform/rpi-firmware).
 * main.c is included below, and the other module sources except timer.c are
 * linked in. Everything outside the module is faked here:
 *
 * - The mailbox registers, with a small firmware behind them. The firmware
 *   reads each message from the DMA page at the device address the driver
 *   wrote to MAIL1, answers the tags the driver uses, and can be scripted to
 *   hold answers back, keep MAIL1 full, send stray words or answer wrongly.
 *   Every register access must be 32 bits wide, MAIL0 may only be read after
 *   its status showed a word, and MAIL1 may only be written after its status
 *   showed room.
 * - DeviceActivate, DeviceGetResources, Acquire/ReleaseDeviceIo, the DMA
 *   calls and DeviceControllerEnable/Disable. Each records an event and
 *   asserts that no GPIO or clock request is being handled on the calling
 *   thread, since those calls reach deviced.
 * - The usched lock over pthread mutexes, so two requests can run on two
 *   threads, and the usched job calls.
 * - The driver's timer functions, over a fake clock that only moves when the
 *   driver waits.
 * - gracht: protocol registration and the answer functions.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ctt_clock_service_server.h>
#include <ctt_driver_service_server.h>
#include <ctt_gpio_service_server.h>
#include <ddk/controller.h>
#include <ddk/dma.h>
#include <ddk/resources.h>
#include <ddk/utils.h>
#include <os/usched/job.h>
#include <os/usched/mutex.h>

#include "firmware.h"
#include "mailbox.h"
#include "property.h"
#include "timer.h"

#define DEVICE_ID      9U
#define BUS_BASE       0xC0042000U
#define PAGE_SIZE      4096U
#define FOREVER        UINT32_MAX
#define MAX_EVENTS     1024U
#define MAX_SENT       256U
#define FIFO_DEPTH     8U
#define WINDOW_ID      77U

// Board information the fake firmware reports.
#define FW_REVISION    0x5F5E1000U
#define BOARD_MODEL    0U
#define BOARD_REVISION 0x00C03111U
#define ARM_MEMORY     0x3B400000U

/////////////////////////////////////////////////////////////////////////////
// Events

enum EventKind {
    EvPrepare,
    EvStart,
    EvStop,
    EvResources,
    EvAcquire,
    EvRelease,
    EvDmaOpen,
    EvDmaAllocate,
    EvDmaFree,
    EvDmaClose,
    EvEnable,
    EvDisable,
    EvConfigWrite,
    EvSend,
    EvProtocol
};

struct Event {
    enum EventKind Kind;
    uint32_t       Value;
};

// Points where setup or teardown can be made to fail.
enum FailPoint {
    FailNone,
    FailPrepare,
    FailResources,
    FailSmallWindow,
    FailPortWindow,
    FailAcquire,
    FailDmaOpen,
    FailDmaOpenCleanup,
    FailAllocate,
    FailStart
};

// How the fake firmware answers.
enum FirmwareMode {
    FwNormal,
    // GPIO tags as QEMU's raspi4b answers them: marked answered with
    // length 0, the request bytes left as they were.
    FwQemu,
    FwParseError,
    FwBadCode,
    FwNotAnswered,
    FwShort,
    FwLong,
    FwGpioError,
    FwClockMismatch,
    // Set clock state answers with the clock still on, as Linux says the
    // firmware does when a clock is switched off.
    FwClockStaysOn
};

struct SentMessage {
    uint32_t Word;
    uint32_t Words[16];
};

struct Line {
    uint32_t Direction;
    uint32_t Polarity;
    uint32_t State;
};

static pthread_mutex_t   g_modelLock = PTHREAD_MUTEX_INITIALIZER;
static struct Event      g_events[MAX_EVENTS];
static uint32_t          g_eventCount;
static char              g_log[1 << 16];
static size_t            g_logLength;
static _Atomic uint64_t  g_nowUs;
static _Atomic uint32_t  g_stalls;
static _Atomic uint32_t  g_sleeps;
static _Thread_local int t_inHandler;

// Device model.
static enum FailPoint    g_failPoint;
static bool              g_prepared;
static bool              g_started;
static bool              g_acquired;
static uint32_t          g_liveContexts;
static uint32_t          g_liveBuffers;
static uint32_t          g_failFrees;
static uint32_t          g_failStops;
static uint64_t          g_openLimit;
static struct DMARequirements g_allocation;
static _Alignas(64) uint8_t g_page[PAGE_SIZE];

// Controller model.
static oserr_t           g_enableResults[4];
static uint32_t          g_enableCalls;
static oserr_t           g_disableResults[4];
static uint32_t          g_disableCalls;
static bool              g_requestDuringEnable;
static oserr_t           g_requestDuringEnableResult;

// Mailbox model.
static uint32_t          g_mail0[FIFO_DEPTH * 4];
static uint32_t          g_mail0Count;
static bool              g_mail0Shown;
static bool              g_mail1Room;
static uint32_t          g_mail1FullReads;
static uint32_t          g_config = 0xFFFFFFFFU;
// Messages the firmware got and has not answered yet, oldest first.
static uint32_t          g_pending[MAX_SENT];
static uint32_t          g_pendingCount;
static bool              g_holdAnswers;
// A word the firmware sends instead of the next answer, and whether the real
// answer is then held back.
static uint32_t          g_strayWord;
static bool              g_strayHoldsAnswer;
// Answer words written and not yet read by the driver.
static uint32_t          g_unread;
static bool              g_oneAtATime;
static struct SentMessage g_sent[MAX_SENT];
static uint32_t          g_sentCount;

// Firmware model.
static enum FirmwareMode g_mode;
static struct Line       g_lines[RPI_GPIO_LINE_COUNT];
static uint32_t          g_clockOn[32];
static uint32_t          g_clockRate[32];

// Scheduler model.
static usched_task_fn    g_jobEntry;
static void            (*g_onPause)(void);

/////////////////////////////////////////////////////////////////////////////
// Logging and events

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;
    int     written;

    (void)level;
    pthread_mutex_lock(&g_modelLock);
    va_start(arguments, format);
    written = vsnprintf(g_log + g_logLength, sizeof(g_log) - g_logLength, format, arguments);
    va_end(arguments);
    if (written > 0 && g_logLength + (size_t)written + 1 < sizeof(g_log)) {
        g_logLength += (size_t)written;
        g_log[g_logLength++] = '\n';
        g_log[g_logLength] = '\0';
    }
    pthread_mutex_unlock(&g_modelLock);
}

static uint32_t
__CountLog(
    _In_ const char* text)
{
    uint32_t    count = 0;
    const char* position = g_log;

    while ((position = strstr(position, text)) != NULL) {
        count++;
        position += strlen(text);
    }
    return count;
}

static void
__ClearLog(void)
{
    g_log[0] = '\0';
    g_logLength = 0;
}

static void
__Record(
    _In_ enum EventKind kind,
    _In_ uint32_t       value)
{
    assert(g_eventCount < MAX_EVENTS);
    g_events[g_eventCount++] = (struct Event) { kind, value };
}

// Every call that reaches deviced must come from registration or teardown,
// never from a GPIO or clock request.
static void
__DevicedCall(
    _In_ enum EventKind kind,
    _In_ uint32_t       value)
{
    assert(!t_inHandler);
    __Record(kind, value);
}

static bool
__HasEvent(
    _In_ enum EventKind kind)
{
    for (uint32_t i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind) {
            return true;
        }
    }
    return false;
}

// Checks that the recorded events, without EvSend and EvConfigWrite, are
// exactly the given list.
static void
__ExpectLifecycle(
    _In_ const enum EventKind* expected,
    _In_ uint32_t              count)
{
    uint32_t index = 0;

    for (uint32_t i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == EvSend || g_events[i].Kind == EvConfigWrite) {
            continue;
        }
        assert(index < count);
        assert(g_events[i].Kind == expected[index]);
        index++;
    }
    assert(index == count);
}

/////////////////////////////////////////////////////////////////////////////
// Time

static void
__Pause(void)
{
    if (g_onPause != NULL) {
        g_onPause();
    }
}

uint64_t
RpiFirmwareTimerNowUs(void)
{
    return atomic_load(&g_nowUs);
}

oserr_t
RpiFirmwareTimerStallUs(
    _In_ uint32_t microseconds)
{
    atomic_fetch_add(&g_nowUs, microseconds);
    atomic_fetch_add(&g_stalls, 1);
    __Pause();
    return OS_EOK;
}

oserr_t
RpiFirmwareTimerSleepUs(
    _In_ uint32_t microseconds)
{
    atomic_fetch_add(&g_nowUs, microseconds);
    atomic_fetch_add(&g_sleeps, 1);
    __Pause();
    return OS_EOK;
}

/////////////////////////////////////////////////////////////////////////////
// Fake firmware

static volatile uint32_t*
__SlotWords(
    _In_ uint32_t word)
{
    uint32_t address = word & ~0xFU;

    assert(address >= BUS_BASE && address + RPI_MAILBOX_SLOT_SIZE <= BUS_BASE + PAGE_SIZE);
    return (volatile uint32_t*)&g_page[address - BUS_BASE];
}

static void
__AnswerGpio(
    _In_ volatile uint32_t* words,
    _In_ uint32_t           tag)
{
    uint32_t gpio = words[5];
    uint32_t line = gpio - RPI_GPIO_FIRMWARE_BASE;
    uint32_t length = 0;

    if (g_mode == FwQemu) {
        words[4] = 0x80000000U;
        return;
    }
    if (gpio < RPI_GPIO_FIRMWARE_BASE || line >= RPI_GPIO_LINE_COUNT) {
        words[4] = 0x80000000U | 4U;
        return;
    }

    switch (tag) {
        case RPI_TAG_GET_GPIO_STATE:
            words[6] = g_lines[line].State;
            length = 8;
            break;
        case RPI_TAG_SET_GPIO_STATE:
            g_lines[line].State = words[6];
            length = 8;
            break;
        case RPI_TAG_GET_GPIO_CONFIG:
            words[6] = g_lines[line].Direction;
            words[7] = g_lines[line].Polarity;
            words[8] = 0;
            words[9] = 0;
            length = 20;
            break;
        case RPI_TAG_SET_GPIO_CONFIG:
            g_lines[line].Direction = words[6];
            g_lines[line].Polarity = words[7];
            if (words[6] == 1) {
                g_lines[line].State = words[10];
            }
            length = 24;
            break;
        default:
            assert(0);
    }
    words[5] = g_mode == FwGpioError ? 1U : 0U;
    words[4] = 0x80000000U | length;
}

static void
__AnswerClock(
    _In_ volatile uint32_t* words,
    _In_ uint32_t           tag)
{
    uint32_t clock = words[5];
    bool     known = clock < 32 && g_clockRate[clock] != 0;

    // Linux sends {id, value, disable_turbo} with disable_turbo 0.
    assert(words[3] == 12 && words[7] == 0);

    switch (tag) {
        case RPI_TAG_GET_CLOCK_STATE:
            words[6] = known ? g_clockOn[clock] : 0x2U;
            break;
        case RPI_TAG_SET_CLOCK_STATE:
            assert((words[6] & ~1U) == 0);
            if (known && g_mode == FwClockStaysOn) {
                words[6] = 1;
            } else if (known) {
                g_clockOn[clock] = words[6];
            } else {
                words[6] = 0x2U;
            }
            break;
        case RPI_TAG_GET_CLOCK_RATE:
            words[6] = known ? g_clockRate[clock] : 0;
            break;
        default:
            assert(0);
    }
    if (g_mode == FwClockMismatch) {
        words[5] = clock + 1;
    }
    words[4] = 0x80000000U | 8U;
}

// The firmware handles one message: it checks the request layout and writes
// its answer into the slot.
static void
__Process(
    _In_ uint32_t word)
{
    volatile uint32_t* words = __SlotWords(word);
    uint32_t           size = words[0];
    uint32_t           tag = words[2];

    // The request layout the property interface needs.
    assert(size % 4 == 0 && size == 24 + words[3]);
    assert(words[1] == 0);
    assert(words[4] == 0);
    assert(words[size / 4 - 1] == 0);

    switch (g_mode) {
        case FwParseError:  words[1] = 0x80000001U; return;
        case FwBadCode:     return;
        case FwNotAnswered: words[1] = 0x80000000U; return;
        default:            break;
    }

    switch (tag) {
        case RPI_TAG_GET_FIRMWARE_REVISION:
            words[5] = FW_REVISION;
            words[4] = 0x80000004U;
            break;
        case RPI_TAG_GET_BOARD_MODEL:
            words[5] = BOARD_MODEL;
            words[4] = 0x80000004U;
            break;
        case RPI_TAG_GET_BOARD_REVISION:
            words[5] = BOARD_REVISION;
            words[4] = 0x80000004U;
            break;
        case RPI_TAG_GET_ARM_MEMORY:
            words[5] = 0;
            words[6] = ARM_MEMORY;
            words[4] = 0x80000008U;
            break;
        case RPI_TAG_GET_GPIO_STATE:
        case RPI_TAG_SET_GPIO_STATE:
        case RPI_TAG_GET_GPIO_CONFIG:
        case RPI_TAG_SET_GPIO_CONFIG:
            __AnswerGpio(words, tag);
            break;
        case RPI_TAG_GET_CLOCK_STATE:
        case RPI_TAG_SET_CLOCK_STATE:
        case RPI_TAG_GET_CLOCK_RATE:
            __AnswerClock(words, tag);
            break;
        default:
            assert(0);
    }

    if (g_mode == FwShort) {
        words[4] = 0x80000000U;
    } else if (g_mode == FwLong) {
        words[4] = 0x80000000U | (words[3] + 4);
    }
    words[1] = 0x80000000U;
}

static void
__PushMail0(
    _In_ uint32_t word)
{
    assert(g_mail0Count < sizeof(g_mail0) / sizeof(g_mail0[0]));
    g_mail0[g_mail0Count++] = word;
}

// Answers pending messages in order, unless they are held back.
static void
__Deliver(void)
{
    while (!g_holdAnswers && g_pendingCount > 0) {
        uint32_t word = g_pending[0];

        memmove(&g_pending[0], &g_pending[1], (g_pendingCount - 1) * sizeof(uint32_t));
        g_pendingCount--;
        __Process(word);
        __PushMail0(word);
        g_unread++;
    }
}

// Lets held answers go, as a firmware that was slow would.
static void
__ReleaseHeld(void)
{
    pthread_mutex_lock(&g_modelLock);
    g_holdAnswers = false;
    __Deliver();
    pthread_mutex_unlock(&g_modelLock);
}

static void
__Send(
    _In_ uint32_t word)
{
    struct SentMessage* sent;

    // A message word: property channel, 16-byte aligned address inside the
    // page, at the start of a slot.
    assert((word & 0xFU) == RPI_MAILBOX_CHANNEL_PROPERTY);
    assert(((word & ~0xFU) - BUS_BASE) % RPI_MAILBOX_SLOT_SIZE == 0);
    if (g_oneAtATime) {
        assert(g_pendingCount == 0 && g_unread == 0);
    }

    assert(g_sentCount < MAX_SENT);
    sent = &g_sent[g_sentCount++];
    sent->Word = word;
    for (uint32_t i = 0; i < 16; i++) {
        sent->Words[i] = __SlotWords(word)[i];
    }
    __Record(EvSend, sent->Words[2]);

    if (g_strayWord != 0) {
        __PushMail0(g_strayWord);
        g_strayWord = 0;
        if (g_strayHoldsAnswer) {
            g_holdAnswers = true;
        }
    }
    g_pending[g_pendingCount++] = word;
    __Deliver();
}

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    uint32_t value = 0;

    // Only the acquired window, and only 32-bit accesses.
    assert(g_acquired && ioSpace->Id == WINDOW_ID);
    assert(length == 4);
    pthread_mutex_lock(&g_modelLock);
    switch (offset) {
        case RPI_MAILBOX_MAIL0_STATUS:
            g_mail0Shown = g_mail0Count > 0;
            value = g_mail0Count == 0 ? RPI_MAILBOX_STATUS_EMPTY : 0;
            break;
        case RPI_MAILBOX_MAIL0_READ:
            // Never read an empty mailbox.
            assert(g_mail0Shown && g_mail0Count > 0);
            value = g_mail0[0];
            memmove(&g_mail0[0], &g_mail0[1], (g_mail0Count - 1) * sizeof(uint32_t));
            g_mail0Count--;
            g_mail0Shown = false;
            if (g_unread > 0) {
                g_unread--;
            }
            break;
        case RPI_MAILBOX_MAIL1_STATUS:
            if (g_mail1FullReads > 0) {
                if (g_mail1FullReads != FOREVER) {
                    g_mail1FullReads--;
                }
                g_mail1Room = false;
                value = RPI_MAILBOX_STATUS_FULL;
            } else {
                g_mail1Room = true;
            }
            break;
        default:
            assert(0);
    }
    pthread_mutex_unlock(&g_modelLock);
    return value;
}

oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(g_acquired && ioSpace->Id == WINDOW_ID);
    assert(length == 4);
    pthread_mutex_lock(&g_modelLock);
    if (offset == RPI_MAILBOX_MAIL0_CONFIG) {
        g_config = (uint32_t)value;
        __Record(EvConfigWrite, (uint32_t)value);
    } else {
        // Only after the status showed room, and only once per status read.
        assert(offset == RPI_MAILBOX_MAIL1_WRITE);
        assert(g_mail1Room);
        g_mail1Room = false;
        // Messages go out only once the device may reach memory.
        assert(g_started);
        __Send((uint32_t)value);
    }
    pthread_mutex_unlock(&g_modelLock);
    return OS_EOK;
}

/////////////////////////////////////////////////////////////////////////////
// deviced and DMA fakes

oserr_t
DeviceActivate(
    _In_ uuid_t                device,
    _In_ enum DeviceActivation stage)
{
    assert(device == DEVICE_ID);
    if (stage == DevicePrepare) {
        __DevicedCall(EvPrepare, 0);
        if (g_failPoint == FailPrepare) {
            return OS_EPERMISSIONS;
        }
        assert(!g_prepared);
        g_prepared = true;
        return OS_EOK;
    }
    if (stage == DeviceStart) {
        __DevicedCall(EvStart, 0);
        assert(g_prepared && g_acquired && g_liveBuffers == 1);
        if (g_failPoint == FailStart) {
            return OS_EUNKNOWN;
        }
        g_started = true;
        return OS_EOK;
    }
    __DevicedCall(EvStop, 0);
    assert(g_prepared);
    // The page, the context and the window are gone before the stop.
    assert(g_liveBuffers == 0 && g_liveContexts == 0 && !g_acquired);
    if (g_failStops > 0) {
        g_failStops--;
        return OS_EBUSY;
    }
    g_prepared = false;
    g_started = false;
    return OS_EOK;
}

oserr_t
DeviceGetResources(
    _In_  uuid_t                  device,
    _Out_ struct DeviceResources* resources)
{
    assert(device == DEVICE_ID);
    assert(g_prepared);
    __DevicedCall(EvResources, 0);
    if (g_failPoint == FailResources) {
        return OS_ENOENT;
    }
    memset(resources, 0, sizeof(*resources));
    resources->Count = 1;
    resources->Registers[0].Id = WINDOW_ID;
    resources->Registers[0].Type = g_failPoint == FailPortWindow ? DeviceIoPortBased : DeviceIoMemoryBased;
    resources->Registers[0].Access.Memory.PhysicalBase = 0xFE00B880U;
    resources->Registers[0].Access.Memory.Length = g_failPoint == FailSmallWindow ? 0x20U : RPI_MAILBOX_WINDOW_SIZE;
    return OS_EOK;
}

oserr_t
AcquireDeviceIo(
    _In_ DeviceIo_t* ioSpace)
{
    __DevicedCall(EvAcquire, 0);
    assert(ioSpace->Id == WINDOW_ID && ioSpace->Access.Memory.Length >= RPI_MAILBOX_WINDOW_SIZE);
    if (g_failPoint == FailAcquire) {
        return OS_EPERMISSIONS;
    }
    assert(!g_acquired);
    g_acquired = true;
    ioSpace->Access.Memory.VirtualBase = 0x1000;
    return OS_EOK;
}

oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* ioSpace)
{
    __DevicedCall(EvRelease, 0);
    assert(ioSpace->Id == WINDOW_ID && g_acquired);
    // The page and the context are gone before the window.
    assert(g_liveBuffers == 0 && g_liveContexts == 0);
    g_acquired = false;
    return OS_EOK;
}

struct DMAContext {
    int Open;
};

struct DMABuffer {
    struct DMASegment Segment;
};

static struct DMAContext g_context;
static struct DMABuffer  g_buffer;

oserr_t
DMAOpen(
    _In_    uuid_t              device,
    _In_    uint64_t            addressLimit,
    _InOut_ struct DMAContext** contextOut)
{
    assert(device == DEVICE_ID && *contextOut == NULL);
    assert(g_prepared && g_acquired);
    __DevicedCall(EvDmaOpen, 0);
    g_openLimit = addressLimit;
    if (g_failPoint == FailDmaOpen) {
        return OS_EPROTOCOL;
    }
    g_liveContexts++;
    *contextOut = &g_context;
    if (g_failPoint == FailDmaOpenCleanup) {
        return OS_EPROTOCOL;
    }
    return OS_EOK;
}

oserr_t
DMAClose(
    _InOut_ struct DMAContext** context)
{
    __DevicedCall(EvDmaClose, 0);
    assert(*context == &g_context && g_liveContexts == 1);
    assert(g_liveBuffers == 0);
    g_liveContexts--;
    *context = NULL;
    return OS_EOK;
}

oserr_t
DMAAllocate(
    _In_    struct DMAContext*            context,
    _In_    const struct DMARequirements* requirements,
    _InOut_ struct DMABuffer**            bufferOut)
{
    __DevicedCall(EvDmaAllocate, 0);
    assert(context == &g_context && *bufferOut == NULL);
    g_allocation = *requirements;
    if (g_failPoint == FailAllocate) {
        return OS_EOOM;
    }
    memset(g_page, 0, sizeof(g_page));
    g_buffer.Segment = (struct DMASegment) { BUS_BASE, PAGE_SIZE };
    g_liveBuffers++;
    *bufferOut = &g_buffer;
    return OS_EOK;
}

void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    assert(buffer == &g_buffer && !t_inHandler);
    return g_page;
}

size_t
DMABufferSize(
    _In_ const struct DMABuffer* buffer)
{
    assert(buffer == &g_buffer && !t_inHandler);
    return PAGE_SIZE;
}

const struct DMASegment*
DMABufferSegments(
    _In_  const struct DMABuffer* buffer,
    _Out_ uint32_t*               countOut)
{
    assert(buffer == &g_buffer && !t_inHandler);
    *countOut = 1;
    return &buffer->Segment;
}

oserr_t
DMAFree(
    _InOut_ struct DMABuffer** buffer)
{
    __DevicedCall(EvDmaFree, 0);
    assert(*buffer == &g_buffer && g_liveBuffers == 1);
    if (g_failFrees > 0) {
        g_failFrees--;
        return OS_EUNKNOWN;
    }
    g_liveBuffers--;
    *buffer = NULL;
    return OS_EOK;
}

static void __HandlerGetValue(uint32_t line, oserr_t* resultOut, uint32_t* valueOut);

oserr_t
DeviceControllerEnable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds)
{
    oserr_t result;

    assert(device == DEVICE_ID);
    __DevicedCall(EvEnable, kinds);
    result = g_enableCalls < 4 ? g_enableResults[g_enableCalls] : OS_EOK;
    g_enableCalls++;

    // deviced may hand a waiting device to its driver before this returns,
    // and that driver's setup sends a request to this controller.
    if (result == OS_EOK && g_requestDuringEnable) {
        uint32_t value;
        __HandlerGetValue(6, &g_requestDuringEnableResult, &value);
    }
    return result;
}

oserr_t
DeviceControllerDisable(
    _In_ uuid_t   device,
    _In_ uint32_t kinds)
{
    oserr_t result;

    assert(device == DEVICE_ID);
    __DevicedCall(EvDisable, kinds);
    result = g_disableCalls < 4 ? g_disableResults[g_disableCalls] : OS_EOK;
    g_disableCalls++;
    return result;
}

/////////////////////////////////////////////////////////////////////////////
// Scheduler fakes

#define MAX_MUTEXES 256

static struct {
    struct usched_mtx* Mutex;
    pthread_mutex_t    Lock;
} g_mutexes[MAX_MUTEXES];
static uint32_t         g_mutexCount;
static _Atomic uint32_t g_contended;

static pthread_mutex_t*
__FindMutex(
    _In_ struct usched_mtx* mutex)
{
    for (uint32_t i = 0; i < g_mutexCount; i++) {
        if (g_mutexes[i].Mutex == mutex) {
            return &g_mutexes[i].Lock;
        }
    }
    return NULL;
}

void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    assert(type == USCHED_MUTEX_PLAIN);
    if (__FindMutex(mutex) != NULL) {
        return;
    }
    assert(g_mutexCount < MAX_MUTEXES);
    g_mutexes[g_mutexCount].Mutex = mutex;
    pthread_mutex_init(&g_mutexes[g_mutexCount].Lock, NULL);
    g_mutexCount++;
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    pthread_mutex_t* lock = __FindMutex(mutex);

    assert(lock != NULL);
    if (pthread_mutex_trylock(lock) == 0) {
        return;
    }
    atomic_fetch_add(&g_contended, 1);
    pthread_mutex_lock(lock);
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    pthread_mutex_t* lock = __FindMutex(mutex);

    assert(lock != NULL);
    assert(pthread_mutex_unlock(lock) == 0);
}

uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void*          argument)
{
    (void)argument;
    g_jobEntry = entry;
    return 5;
}

int
usched_job_cancel(
    _In_ uuid_t jobID)
{
    assert(jobID == 5);
    return 0;
}

int
usched_job_join(
    _In_  uuid_t jobID,
    _Out_ int*   exitCode)
{
    assert(jobID == 5);
    *exitCode = 0;
    return 0;
}

int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    (void)until;
    return 0;
}

// The cancellation token of the cleanup job is a pass counter.
bool
usched_is_cancelled(
    _In_ const void* cancellationToken)
{
    int* passes = (int*)cancellationToken;

    if (*passes == 0) {
        return true;
    }
    (*passes)--;
    return false;
}

// The list in main.c takes its own spinlock. One process-wide lock is
// enough here.
static pthread_mutex_t g_spinlock = PTHREAD_MUTEX_INITIALIZER;

void spinlock_init(spinlock_t* lock) { (void)lock; }
void spinlock_acquire(spinlock_t* lock) { (void)lock; pthread_mutex_lock(&g_spinlock); }
void spinlock_release(spinlock_t* lock) { (void)lock; pthread_mutex_unlock(&g_spinlock); }

static void
__RunCleanupPass(void)
{
    int passes = 1;

    assert(g_jobEntry != NULL);
    g_jobEntry(NULL, &passes);
}

/////////////////////////////////////////////////////////////////////////////
// gracht fakes

static gracht_server_t* const g_server = (gracht_server_t*)0x1234;
gracht_protocol_t ctt_driver_server_protocol = { .id = 1 };
gracht_protocol_t ctt_gpio_server_protocol = { .id = 62 };
gracht_protocol_t ctt_clock_server_protocol = { .id = 63 };

gracht_server_t*
__crt_get_module_server(void)
{
    return g_server;
}

int
gracht_server_register_protocol(
    gracht_server_t*   server,
    gracht_protocol_t* protocol)
{
    assert(server == g_server);
    __Record(EvProtocol, protocol->id);
    return 0;
}

// The gracht message ends in a flexible array, so it goes last, and the
// answer functions find the test message from it.
struct TestMessage {
    int                   Answers;
    oserr_t               Result;
    uint64_t              Value;
    struct gracht_message Base;
};

static int
__Answer(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result,
    _In_ uint64_t               value)
{
    struct TestMessage* test = (struct TestMessage*)((char*)message - offsetof(struct TestMessage, Base));

    test->Answers++;
    test->Result = result;
    test->Value = value;
    return 0;
}

int ctt_gpio_set_direction_response(struct gracht_message* m, oserr_t r) { return __Answer(m, r, 0); }
int ctt_gpio_set_value_response(struct gracht_message* m, oserr_t r) { return __Answer(m, r, 0); }
int ctt_gpio_get_value_response(struct gracht_message* m, oserr_t r, uint32_t v) { return __Answer(m, r, v); }
int ctt_clock_get_enabled_response(struct gracht_message* m, oserr_t r, uint8_t v) { return __Answer(m, r, v); }
int ctt_clock_set_enabled_response(struct gracht_message* m, oserr_t r) { return __Answer(m, r, 0); }
int ctt_clock_get_rate_response(struct gracht_message* m, oserr_t r, uint64_t v) { return __Answer(m, r, v); }
int ctt_driver_ioctl_response(struct gracht_message* m, uint8_t* d, uint32_t c, oserr_t r)
{
    (void)d;
    (void)c;
    return __Answer(m, r, 0);
}

/////////////////////////////////////////////////////////////////////////////
// The module

#include RPI_FIRMWARE_MAIN_SOURCE

/////////////////////////////////////////////////////////////////////////////
// Requests as deviced sends them. Each runs with the handler flag set, so
// every deviced fake asserts if the driver calls it from the request.

static void
__HandlerGetValue(
    _In_  uint32_t  line,
    _Out_ oserr_t*  resultOut,
    _Out_ uint32_t* valueOut)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_gpio_get_value_invocation(&message.Base, DEVICE_ID, line);
    t_inHandler--;
    assert(message.Answers == 1);
    *resultOut = message.Result;
    *valueOut = (uint32_t)message.Value;
}

static oserr_t
__SetDirection(
    _In_ uint32_t                line,
    _In_ enum ctt_gpio_direction direction,
    _In_ uint32_t                value)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_gpio_set_direction_invocation(&message.Base, DEVICE_ID, line, direction, value);
    t_inHandler--;
    assert(message.Answers == 1);
    return message.Result;
}

static oserr_t
__SetValue(
    _In_ uint32_t line,
    _In_ uint32_t value)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_gpio_set_value_invocation(&message.Base, DEVICE_ID, line, value);
    t_inHandler--;
    assert(message.Answers == 1);
    return message.Result;
}

static oserr_t
__GetValue(
    _In_  uint32_t  line,
    _Out_ uint32_t* valueOut)
{
    oserr_t result;

    __HandlerGetValue(line, &result, valueOut);
    return result;
}

static oserr_t
__ClockGetEnabled(
    _In_  uint32_t clock,
    _Out_ bool*    enabledOut)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_clock_get_enabled_invocation(&message.Base, DEVICE_ID, clock);
    t_inHandler--;
    assert(message.Answers == 1);
    *enabledOut = message.Value != 0;
    return message.Result;
}

static oserr_t
__ClockSetEnabled(
    _In_ uint32_t clock,
    _In_ bool     enabled)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_clock_set_enabled_invocation(&message.Base, DEVICE_ID, clock, enabled ? 1 : 0);
    t_inHandler--;
    assert(message.Answers == 1);
    return message.Result;
}

static oserr_t
__ClockGetRate(
    _In_  uint32_t  clock,
    _Out_ uint64_t* rateOut)
{
    struct TestMessage message = { 0 };

    t_inHandler++;
    ctt_clock_get_rate_invocation(&message.Base, DEVICE_ID, clock);
    t_inHandler--;
    assert(message.Answers == 1);
    *rateOut = message.Value;
    return message.Result;
}

/////////////////////////////////////////////////////////////////////////////
// Test setup

static void
__ResetModel(void)
{
    g_eventCount = 0;
    __ClearLog();
    atomic_store(&g_nowUs, 1000000);
    atomic_store(&g_stalls, 0);
    atomic_store(&g_sleeps, 0);
    atomic_store(&g_contended, 0);
    g_failPoint = FailNone;
    g_prepared = false;
    g_started = false;
    g_acquired = false;
    g_liveContexts = 0;
    g_liveBuffers = 0;
    g_failFrees = 0;
    g_failStops = 0;
    g_openLimit = 0;
    memset(&g_allocation, 0, sizeof(g_allocation));
    memset(g_enableResults, 0, sizeof(g_enableResults));
    memset(g_disableResults, 0, sizeof(g_disableResults));
    g_enableCalls = 0;
    g_disableCalls = 0;
    g_requestDuringEnable = false;
    g_requestDuringEnableResult = OS_EUNKNOWN;
    g_mail0Count = 0;
    g_mail0Shown = false;
    g_mail1Room = false;
    g_mail1FullReads = 0;
    g_config = 0xFFFFFFFFU;
    g_pendingCount = 0;
    g_holdAnswers = false;
    g_strayWord = 0;
    g_strayHoldsAnswer = false;
    g_unread = 0;
    g_oneAtATime = false;
    g_sentCount = 0;
    g_mode = FwNormal;
    memset(g_lines, 0, sizeof(g_lines));
    g_lines[4].Polarity = 1;
    memset(g_clockOn, 0, sizeof(g_clockOn));
    memset(g_clockRate, 0, sizeof(g_clockRate));
    g_clockRate[1] = 50000000U;
    g_clockRate[12] = 100000000U;
    g_clockOn[12] = 1;
    g_onPause = NULL;

    // The module keeps its protocol registration across devices. Each test
    // starts as a freshly loaded module.
    g_protocolsRegistered = false;
    g_jobEntry = NULL;
}

static void
__Load(void)
{
    __ResetModel();
    assert(OnLoad() == OS_EOK);
    assert(g_jobEntry != NULL);
    g_eventCount = 0;
}

static void
__Register(void)
{
    struct TestMessage message = { 0 };
    struct sys_device  description;

    memset(&description, 0, sizeof(description));
    description.content_type = SYS_DEVICE_CONTENT_PLATFORM;
    description.content.platform.id = DEVICE_ID;
    ctt_driver_register_device_invocation(&message.Base, &description);
}

static oserr_t
__Unregister(void)
{
    Device_t device = { .Id = DEVICE_ID };

    return OnUnregister(&device);
}

static bool
__DeviceListed(void)
{
    return list_find_value(&g_devices, (void*)(uintptr_t)DEVICE_ID) != NULL;
}

static void
__ExpectReleased(void)
{
    assert(!g_prepared && !g_started && !g_acquired);
    assert(g_liveContexts == 0 && g_liveBuffers == 0);
    assert(!__DeviceListed());
}

// Loads the module and registers a device that comes up normally.
static void
__Ready(void)
{
    __Load();
    __Register();
    assert(__DeviceListed() && g_started);
    g_eventCount = 0;
    g_sentCount = 0;
    __ClearLog();
}

static void
__Unload(void)
{
    OnUnload();
    __ExpectReleased();
}

static uint32_t
__SlotWord(
    _In_ uint32_t slot)
{
    return (BUS_BASE + slot * RPI_MAILBOX_SLOT_SIZE) | RPI_MAILBOX_CHANNEL_PROPERTY;
}

static void
__ExpectWords(
    _In_ const struct SentMessage* sent,
    _In_ const uint32_t*           words,
    _In_ uint32_t                  count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (sent->Words[i] != words[i]) {
            fprintf(stderr, "word %u: 0x%08x, expected 0x%08x\n", i, sent->Words[i], words[i]);
        }
        assert(sent->Words[i] == words[i]);
    }
    // The rest of the slot is cleared.
    for (uint32_t i = count; i < 16; i++) {
        assert(sent->Words[i] == 0);
    }
}

/////////////////////////////////////////////////////////////////////////////
// Setup and teardown

static void
TestSetupOrder(void)
{
    static const enum EventKind setup[] = {
        EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvDmaAllocate, EvStart,
        EvProtocol, EvProtocol, EvEnable
    };
    static const enum EventKind teardown[] = {
        EvDisable, EvDmaFree, EvDmaClose, EvRelease, EvStop
    };
    static const uint32_t tags[] = {
        RPI_TAG_GET_FIRMWARE_REVISION, RPI_TAG_GET_BOARD_MODEL, RPI_TAG_GET_BOARD_REVISION,
        RPI_TAG_GET_ARM_MEMORY
    };
    uint32_t sends = 0;
    uint32_t value;
    oserr_t  oserr;

    __Load();
    g_requestDuringEnable = true;
    __Register();
    __ExpectLifecycle(setup, sizeof(setup) / sizeof(setup[0]));

    // The interrupt is switched off before the first message, and the four
    // information requests run after DeviceStart and before the protocols.
    // The loop ends at the enable, after which deviced sends its request.
    for (uint32_t i = 0; i < g_eventCount && g_events[i].Kind != EvEnable; i++) {
        if (g_events[i].Kind == EvConfigWrite) {
            assert(g_events[i].Value == 0 && sends == 0);
            assert(g_events[i - 1].Kind == EvDmaAllocate);
        }
        if (g_events[i].Kind == EvSend) {
            assert(sends < 4 && g_events[i].Value == tags[sends]);
            assert(g_events[i - 1 - sends].Kind == EvStart);
            sends++;
        }
        if (g_events[i].Kind == EvProtocol) {
            assert(sends == 4);
        }
    }
    assert(sends == 4 && g_config == 0);

    // Events 0 to 6 are prepare to start with the interrupt switch-off, 7 to
    // 10 the information requests. The request deviced sends during the
    // enable comes last.
    assert(g_events[11].Kind == EvProtocol && g_events[11].Value == 62);
    assert(g_events[12].Kind == EvProtocol && g_events[12].Value == 63);
    assert(g_events[13].Kind == EvEnable);
    assert(g_events[13].Value == (DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK));
    assert(g_eventCount == 15 && g_events[14].Kind == EvSend);

    // The DMA context limits addresses to 32 bits, and the page is one
    // 16-byte aligned segment.
    assert(g_openLimit == 0xFFFFFFFFULL);
    assert(g_allocation.Length == PAGE_SIZE && g_allocation.Alignment == 16 && g_allocation.Contiguous);

    // A request deviced sends while the enable is still running is served.
    assert(g_requestDuringEnableResult == OS_EOK);

    // The board revision is in the log, for bring-up.
    assert(strstr(g_log, "board revision 0x00c03111") != NULL);
    assert(strstr(g_log, "firmware revision 0x5f5e1000") != NULL);
    assert(strstr(g_log, "ARM memory at 0x00000000, 948 MiB") != NULL);
    assert(__CountLog("failed") == 0);

    g_eventCount = 0;
    oserr = __Unregister();
    assert(oserr == OS_EOK);
    __ExpectLifecycle(teardown, sizeof(teardown) / sizeof(teardown[0]));
    assert(g_events[0].Value == (DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK));
    __ExpectReleased();

    // A request for the released device is refused without a message.
    g_sentCount = 0;
    assert(__GetValue(1, &value) == OS_ENOENT);
    assert(g_sentCount == 0);
    __Unload();
    printf("setup order and teardown order: passed\n");
}

static void
__RunSetupFailure(
    _In_ enum FailPoint        failPoint,
    _In_ const enum EventKind* expected,
    _In_ uint32_t              count,
    _In_ const char*           name)
{
    __Load();
    g_failPoint = failPoint;
    __Register();
    __ExpectLifecycle(expected, count);
    __ExpectReleased();
    assert(__CountLog("setup failed") == 1);
    __Unload();
    printf("setup failure released what was set up: %s\n", name);
}

static void
TestSetupFailures(void)
{
    static const enum EventKind prepare[] = { EvPrepare };
    static const enum EventKind resources[] = { EvPrepare, EvResources, EvStop };
    static const enum EventKind window[] = { EvPrepare, EvResources, EvStop };
    static const enum EventKind acquire[] = { EvPrepare, EvResources, EvAcquire, EvStop };
    static const enum EventKind open[] = { EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvRelease, EvStop };
    static const enum EventKind openCleanup[] = {
        EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvDmaClose, EvRelease, EvStop
    };
    static const enum EventKind allocate[] = {
        EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvDmaAllocate, EvDmaClose, EvRelease, EvStop
    };
    static const enum EventKind start[] = {
        EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvDmaAllocate, EvStart,
        EvDmaFree, EvDmaClose, EvRelease, EvStop
    };

    __RunSetupFailure(FailPrepare, prepare, 1, "prepare");
    __RunSetupFailure(FailResources, resources, 3, "resources");
    __RunSetupFailure(FailSmallWindow, window, 3, "window of 0x20 bytes");
    __RunSetupFailure(FailPortWindow, window, 3, "port window");
    __RunSetupFailure(FailAcquire, acquire, 4, "acquire");
    __RunSetupFailure(FailDmaOpen, open, 6, "DMA open");
    __RunSetupFailure(FailDmaOpenCleanup, openCleanup, 7, "DMA open with a cleanup-only context");
    __RunSetupFailure(FailAllocate, allocate, 8, "allocation");
    __RunSetupFailure(FailStart, start, 10, "start");
}

static void
TestEnableResults(void)
{
    static const enum EventKind refused[] = {
        EvPrepare, EvResources, EvAcquire, EvDmaOpen, EvDmaAllocate, EvStart, EvProtocol, EvProtocol,
        EvEnable, EvDmaFree, EvDmaClose, EvRelease, EvStop
    };
    uint32_t value;
    uint64_t rate;

    // No clock controller behind the device: GPIO alone is enabled.
    __Load();
    g_enableResults[0] = OS_ENOENT;
    g_enableResults[1] = OS_EOK;
    g_enableResults[2] = OS_ENOENT;
    __Register();
    assert(__DeviceListed() && g_enableCalls == 3);
    assert(g_events[g_eventCount - 2].Value == DEVICE_CONTROLLER_GPIO);
    assert(g_events[g_eventCount - 1].Value == DEVICE_CONTROLLER_CLOCK);
    assert(strstr(g_log, "has no clock controller") != NULL);
    assert(__GetValue(0, &value) == OS_EOK);
    assert(__ClockGetRate(12, &rate) == OS_EOK);
    g_eventCount = 0;
    assert(__Unregister() == OS_EOK);
    assert(g_events[0].Kind == EvDisable && g_events[0].Value == DEVICE_CONTROLLER_GPIO);
    __ExpectReleased();
    __Unload();

    // Neither kind: the device is released.
    __Load();
    g_enableResults[0] = OS_ENOENT;
    g_enableResults[1] = OS_ENOENT;
    g_enableResults[2] = OS_ENOENT;
    __Register();
    assert(g_enableCalls == 3 && !__HasEvent(EvDisable));
    __ExpectReleased();
    __Unload();

    // Any other refusal: the device is released, nothing to disable.
    __Load();
    g_enableResults[0] = OS_EPERMISSIONS;
    __Register();
    __ExpectLifecycle(refused, sizeof(refused) / sizeof(refused[0]));
    __ExpectReleased();
    __Unload();
    printf("controller enable results: passed\n");
}

static void
TestTeardownRetry(void)
{
    uint32_t value;

    __Ready();
    g_disableResults[0] = OS_EBUSY;
    g_disableResults[1] = OS_EBUSY;

    // Another device still uses a clock: nothing is released, and the
    // device keeps serving.
    assert(__Unregister() == OS_EBUSY);
    assert(__DeviceListed() && g_started && g_liveBuffers == 1);
    assert(__GetValue(2, &value) == OS_EOK);

    __RunCleanupPass();
    assert(__DeviceListed() && g_disableCalls == 2);

    // Disable succeeds, then the free fails once: the next pass goes on at
    // the free and does not disable again.
    g_failFrees = 1;
    __RunCleanupPass();
    assert(__DeviceListed() && g_disableCalls == 3 && g_liveBuffers == 1);
    assert(__GetValue(2, &value) == OS_ENOENT);
    g_eventCount = 0;
    g_failStops = 1;
    __RunCleanupPass();
    assert(!__HasEvent(EvDisable) && __HasEvent(EvDmaFree) && __HasEvent(EvStop) && __DeviceListed());
    g_eventCount = 0;
    __RunCleanupPass();
    assert(g_eventCount == 1 && g_events[0].Kind == EvStop);
    __ExpectReleased();

    // deviced has already ended the controller (device removed).
    __Ready();
    g_disableResults[0] = OS_ENOENT;
    assert(__Unregister() == OS_EOK);
    __ExpectReleased();
    __Unload();
    printf("teardown retry while Disable is busy: passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Transport

static void
TestTransport(void)
{
    uint32_t value;

    __Ready();
    g_lines[3].State = 1;
    assert(__GetValue(3, &value) == OS_EOK && value == 1);
    assert(g_sentCount == 1 && g_sent[0].Word == __SlotWord(0));
    // A normal call needs no wait at all here, the fake answers at once.
    assert(atomic_load(&g_sleeps) == 0);

    // MAIL1 full for a while: the driver waits, busy first.
    g_mail1FullReads = 30;
    assert(__GetValue(3, &value) == OS_EOK && value == 1);
    assert(atomic_load(&g_stalls) == 30 && atomic_load(&g_sleeps) == 0);
    assert(g_sentCount == 2);
    __Unload();
    printf("transport: write after room, read after a word, 32-bit accesses, channel 8: passed\n");
}

static void
TestWriteDeadline(void)
{
    uint64_t start;
    uint64_t elapsed;
    uint32_t value;

    __Ready();
    g_mail1FullReads = FOREVER;
    start = atomic_load(&g_nowUs);
    assert(__GetValue(1, &value) == OS_ETIMEOUT);
    elapsed = atomic_load(&g_nowUs) - start;

    // Nothing was sent, the deadline held, and the wait turned to sleeps
    // after the busy phase.
    assert(g_sentCount == 0);
    assert(elapsed >= RPI_MAILBOX_CALL_DEADLINE_US);
    assert(elapsed <= RPI_MAILBOX_CALL_DEADLINE_US + RPI_MAILBOX_SLEEP_INTERVAL_US);
    assert(atomic_load(&g_stalls) == RPI_MAILBOX_BUSY_PHASE_US / RPI_MAILBOX_BUSY_INTERVAL_US);
    assert(atomic_load(&g_sleeps) >= 18 && atomic_load(&g_sleeps) <= 20);
    assert(__CountLog("write mailbox stayed full") == 1);

    // Nothing waits for an answer, so the next call uses slot 0 again.
    g_mail1FullReads = 0;
    assert(__GetValue(1, &value) == OS_EOK);
    assert(g_sent[0].Word == __SlotWord(0));
    __Unload();
    printf("deadline on a full write mailbox: passed\n");
}

static void
TestLateAnswer(void)
{
    uint64_t start;
    uint32_t value;

    __Ready();

    // No answer: the call ends at the deadline and slot 0 waits.
    g_holdAnswers = true;
    g_lines[1].State = 0;
    start = atomic_load(&g_nowUs);
    assert(__GetValue(1, &value) == OS_ETIMEOUT);
    assert(atomic_load(&g_nowUs) - start >= RPI_MAILBOX_CALL_DEADLINE_US);
    assert(__CountLog("no answer before the deadline") == 1);

    // The firmware answers the first message late, while the second call
    // waits. The late answer is for slot 0 and must not be taken: the second
    // call used slot 1 and gets its own answer.
    g_lines[1].State = 1;
    g_lines[2].State = 1;
    pthread_mutex_lock(&g_modelLock);
    g_holdAnswers = true;
    pthread_mutex_unlock(&g_modelLock);
    g_onPause = __ReleaseHeld;
    assert(__GetValue(2, &value) == OS_EOK && value == 1);
    g_onPause = NULL;
    assert(g_sentCount == 2 && g_sent[1].Word == __SlotWord(1));

    // Slot 0 is free again.
    g_lines[3].State = 0;
    assert(__GetValue(3, &value) == OS_EOK && value == 0);
    assert(g_sent[2].Word == __SlotWord(0));

    // A late answer that comes between calls is drained before the next
    // call, which then uses slot 0 again.
    g_holdAnswers = true;
    assert(__GetValue(4, &value) == OS_ETIMEOUT);
    __ReleaseHeld();
    assert(__GetValue(5, &value) == OS_EOK);
    assert(g_sent[4].Word == __SlotWord(0));
    assert(g_mail0Count == 0);

    // Teardown keeps the page while the firmware owes an answer, and frees
    // it once the answer came.
    g_holdAnswers = true;
    assert(__GetValue(5, &value) == OS_ETIMEOUT);
    assert(__Unregister() == OS_EBUSY);
    assert(g_liveBuffers == 1 && g_acquired && __DeviceListed());
    assert(strstr(g_log, "still owes an answer") != NULL);
    __ReleaseHeld();
    __RunCleanupPass();
    __ExpectReleased();
    __Unload();
    printf("late answer after a deadline not taken for the next call: passed\n");
}

static void
TestStaleAnswersAtSetup(void)
{
    __Load();

    // A boot loader left two answers behind, one of them for the very
    // address of the driver's first slot.
    g_mail0[0] = __SlotWord(0);
    g_mail0[1] = 0x3B000008U;
    g_mail0Count = 2;

    // The firmware answers only once the driver polls, so a stale word for
    // slot 0 that was not drained would be read before the real answer.
    g_holdAnswers = true;
    g_onPause = __ReleaseHeld;
    __Register();
    assert(__DeviceListed());
    assert(g_mail0Count == 0);
    assert(__CountLog("dropped mailbox answer") == 2);
    assert(strstr(g_log, "board revision 0x00c03111") != NULL);
    __Unload();
    printf("stale answers drained before the first call: passed\n");
}

static void
TestWrongAnswer(void)
{
    uint32_t value;

    __Ready();

    // An answer on another channel: the call fails, and its slot waits for
    // the real answer.
    g_strayWord = __SlotWord(0) - RPI_MAILBOX_CHANNEL_PROPERTY + 9;
    g_strayHoldsAnswer = true;
    assert(__GetValue(1, &value) == OS_EPROTOCOL);
    assert(__CountLog("answer for another channel or buffer") == 1);
    __ReleaseHeld();
    assert(__GetValue(1, &value) == OS_EOK);
    assert(g_sent[1].Word == __SlotWord(0));

    // An answer for an address outside the page.
    g_strayWord = (BUS_BASE + PAGE_SIZE) | RPI_MAILBOX_CHANNEL_PROPERTY;
    g_strayHoldsAnswer = false;
    assert(__GetValue(2, &value) == OS_EPROTOCOL);
    assert(__CountLog("answer for another channel or buffer") == 2);

    // Its own answer is still in MAIL0, and the next call drains it first.
    assert(g_mail0Count == 1);
    assert(__GetValue(1, &value) == OS_EOK);
    assert(g_sent[3].Word == __SlotWord(0));
    __Unload();
    printf("answer on another channel or for another address: passed\n");
}

static void
TestOneCallAtATime(void);

/////////////////////////////////////////////////////////////////////////////
// Property layer

static void
TestRequestBytes(void)
{
    const uint32_t revision[] = { 28, 0, RPI_TAG_GET_FIRMWARE_REVISION, 4, 0, 0, 0 };
    const uint32_t model[] = { 28, 0, RPI_TAG_GET_BOARD_MODEL, 4, 0, 0, 0 };
    const uint32_t board[] = { 28, 0, RPI_TAG_GET_BOARD_REVISION, 4, 0, 0, 0 };
    const uint32_t memory[] = { 32, 0, RPI_TAG_GET_ARM_MEMORY, 8, 0, 0, 0, 0 };
    const uint32_t getConfig[] = { 44, 0, RPI_TAG_GET_GPIO_CONFIG, 20, 0, 132, 0, 0, 0, 0, 0 };
    const uint32_t setConfig[] = { 48, 0, RPI_TAG_SET_GPIO_CONFIG, 24, 0, 132, 1, 1, 0, 0, 1, 0 };
    const uint32_t setState[] = { 32, 0, RPI_TAG_SET_GPIO_STATE, 8, 0, 135, 1, 0 };
    const uint32_t getState[] = { 32, 0, RPI_TAG_GET_GPIO_STATE, 8, 0, 128, 0, 0 };
    const uint32_t clockState[] = { 36, 0, RPI_TAG_GET_CLOCK_STATE, 12, 0, 12, 0, 0, 0 };
    const uint32_t clockOn[] = { 36, 0, RPI_TAG_SET_CLOCK_STATE, 12, 0, 12, 1, 0, 0 };
    const uint32_t clockRate[] = { 36, 0, RPI_TAG_GET_CLOCK_RATE, 12, 0, 12, 0, 0, 0 };
    uint32_t       value;
    uint64_t       rate;
    bool           enabled;

    __Load();
    __Register();
    assert(g_sentCount == 4);
    __ExpectWords(&g_sent[0], revision, 7);
    __ExpectWords(&g_sent[1], model, 7);
    __ExpectWords(&g_sent[2], board, 7);
    __ExpectWords(&g_sent[3], memory, 8);
    g_sentCount = 0;

    // Old bytes in the slot must not reach the firmware.
    memset(g_page, 0xAA, RPI_MAILBOX_SLOT_SIZE);
    assert(__SetDirection(4, CTT_GPIO_DIRECTION_OUTPUT, 1) == OS_EOK);
    __ExpectWords(&g_sent[0], getConfig, 11);
    __ExpectWords(&g_sent[1], setConfig, 12);
    memset(g_page, 0xAA, RPI_MAILBOX_SLOT_SIZE);
    assert(__SetValue(7, 1) == OS_EOK);
    __ExpectWords(&g_sent[2], setState, 8);
    assert(__GetValue(0, &value) == OS_EOK);
    __ExpectWords(&g_sent[3], getState, 8);
    assert(__ClockGetEnabled(12, &enabled) == OS_EOK);
    __ExpectWords(&g_sent[4], clockState, 9);
    assert(__ClockSetEnabled(12, true) == OS_EOK);
    __ExpectWords(&g_sent[5], clockOn, 9);
    assert(__ClockGetRate(12, &rate) == OS_EOK);
    __ExpectWords(&g_sent[6], clockRate, 9);
    __Unload();
    printf("request bytes of every tag: passed\n");
}

static void
__ExpectAnswerCheck(
    _In_ enum FirmwareMode mode,
    _In_ oserr_t           expected,
    _In_ const char*       text)
{
    uint64_t rate;

    __Ready();
    g_mode = mode;
    assert(__ClockGetRate(12, &rate) == expected);
    assert(__CountLog(text) == 1);
    assert(__CountLog("failed") == 1);
    __Unload();
}

static void
TestAnswerChecks(void)
{
    __ExpectAnswerCheck(FwParseError, OS_EDEVFAULT, "firmware could not parse the request");
    __ExpectAnswerCheck(FwBadCode, OS_EPROTOCOL, "unknown buffer code");
    __ExpectAnswerCheck(FwNotAnswered, OS_ENOTSUPPORTED, "tag not answered");
    __ExpectAnswerCheck(FwLong, OS_EPROTOCOL, "answer longer than the buffer");
    __ExpectAnswerCheck(FwClockMismatch, OS_EPROTOCOL, "answer names another clock");
    printf("answer checks: buffer code, tag code bit, oversized answer: passed\n");
}

static void
TestShortAnswers(void)
{
    uint64_t rate = 0;
    uint32_t value = 0;

    // A short length with valid fields succeeds, as in Linux, and each tag
    // shows the length once.
    __Ready();
    g_mode = FwShort;
    assert(__ClockGetRate(12, &rate) == OS_EOK && rate == 100000000U);
    assert(__ClockGetRate(1, &rate) == OS_EOK && rate == 50000000U);
    assert(__CountLog("answered with 0 bytes, 8 expected") == 1);
    assert(__SetValue(3, 1) == OS_EOK && g_lines[3].State == 1);
    assert(__GetValue(3, &value) == OS_EOK && value == 1);
    assert(__SetDirection(4, CTT_GPIO_DIRECTION_OUTPUT, 0) == OS_EOK && g_lines[4].Polarity == 1);
    assert(__CountLog("answered with 0 bytes") == 5);
    assert(__CountLog("failed") == 0);

    // A clock that keeps running after it was switched off is shown once,
    // and the request succeeds, as Linux reads no state back.
    g_mode = FwClockStaysOn;
    assert(__ClockSetEnabled(12, false) == OS_EOK);
    assert(__ClockSetEnabled(12, false) == OS_EOK);
    assert(__CountLog("answered state 0x1 after switching it off") == 1);
    assert(__CountLog("failed") == 0);
    __Unload();
    printf("short answers decided by their fields: passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// GPIO service

static void
TestGpio(void)
{
    uint32_t value;

    __Ready();

    // Output: the configuration is read first and the polarity kept.
    assert(__SetDirection(4, CTT_GPIO_DIRECTION_OUTPUT, 1) == OS_EOK);
    assert(g_sentCount == 2);
    assert(g_sent[0].Words[2] == RPI_TAG_GET_GPIO_CONFIG && g_sent[0].Words[5] == 132);
    assert(g_sent[1].Words[2] == RPI_TAG_SET_GPIO_CONFIG && g_sent[1].Words[7] == 1);
    assert(g_lines[4].Direction == 1 && g_lines[4].Polarity == 1 && g_lines[4].State == 1);

    // Input: direction 0, termination off, no level.
    g_lines[5].Polarity = 0;
    g_lines[5].Direction = 1;
    assert(__SetDirection(5, CTT_GPIO_DIRECTION_INPUT, 1) == OS_EOK);
    assert(g_lines[5].Direction == 0);
    assert(g_sent[3].Words[6] == 0 && g_sent[3].Words[8] == 0 && g_sent[3].Words[10] == 0);

    // Levels, and the line numbers 0 to 7 as firmware GPIO 128 to 135.
    for (uint32_t line = 0; line < RPI_GPIO_LINE_COUNT; line++) {
        g_sentCount = 0;
        assert(__SetValue(line, line & 1) == OS_EOK);
        assert(g_sent[0].Words[5] == 128 + line && g_lines[line].State == (line & 1));
        assert(__GetValue(line, &value) == OS_EOK && value == (line & 1));
        assert(g_sent[1].Words[5] == 128 + line);
    }

    // Out of range: refused without a message.
    g_sentCount = 0;
    assert(__GetValue(8, &value) == OS_EINVALPARAMS);
    assert(__SetValue(8, 1) == OS_EINVALPARAMS);
    assert(__SetValue(1, 2) == OS_EINVALPARAMS);
    assert(__SetDirection(8, CTT_GPIO_DIRECTION_INPUT, 0) == OS_EINVALPARAMS);
    assert(__SetDirection(1, (enum ctt_gpio_direction)3, 0) == OS_EINVALPARAMS);
    assert(__SetDirection(1, CTT_GPIO_DIRECTION_OUTPUT, 2) == OS_EINVALPARAMS);
    assert(g_sentCount == 0);
    assert(__CountLog("failed") == 0);

    // A nonzero gpio field is an error.
    g_mode = FwGpioError;
    assert(__SetValue(2, 1) == OS_EDEVFAULT);
    assert(__CountLog("firmware did not clear the gpio field (0x1)") == 1);
    __Unload();
    printf("GPIO service: direction, levels, line mapping, range, gpio field: passed\n");
}

static void
TestGpioOnQemu(void)
{
    uint32_t value;

    __Ready();
    g_mode = FwQemu;
    assert(__SetDirection(6, CTT_GPIO_DIRECTION_OUTPUT, 1) == OS_EDEVFAULT);
    assert(__CountLog("failed") == 1);
    assert(strstr(g_log, "GPIO line 6 failed: firmware did not clear the gpio field (0x86)") != NULL);
    assert(__CountLog("answered with 0 bytes") == 1);

    // deviced trying again within the repeat interval adds no line.
    for (int i = 0; i < 4; i++) {
        assert(__SetDirection(6, CTT_GPIO_DIRECTION_OUTPUT, 1) != OS_EOK);
    }
    assert(__CountLog("failed") == 1);

    // After the interval the line comes again with the count.
    atomic_fetch_add(&g_nowUs, RPI_LOG_REPEAT_INTERVAL_US);
    assert(__SetDirection(6, CTT_GPIO_DIRECTION_OUTPUT, 1) != OS_EOK);
    assert(__CountLog("failed") == 2);
    assert(strstr(g_log, "4 repeats not logged") != NULL);

    // A different failure is written at once.
    assert(__GetValue(3, &value) != OS_EOK);
    assert(__CountLog("failed") == 3);
    __Unload();
    printf("QEMU-style GPIO answer: one rate-limited error line: passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Clock service

static void
TestClock(void)
{
    uint64_t rate = 1;
    bool     enabled = false;

    __Ready();
    assert(__ClockGetEnabled(12, &enabled) == OS_EOK && enabled);
    assert(__ClockSetEnabled(12, false) == OS_EOK && g_clockOn[12] == 0);
    assert(__ClockGetEnabled(12, &enabled) == OS_EOK && !enabled);
    assert(__ClockSetEnabled(12, true) == OS_EOK && g_clockOn[12] == 1);
    assert(__ClockGetRate(12, &rate) == OS_EOK && rate == 100000000U);
    assert(__ClockGetRate(1, &rate) == OS_EOK && rate == 50000000U);

    // A clock the firmware does not have: rate 0 is passed on, the state
    // tags say it does not exist.
    assert(__ClockGetRate(15, &rate) == OS_EOK && rate == 0);
    assert(__ClockGetEnabled(15, &enabled) == OS_ENOENT);
    assert(__ClockSetEnabled(15, true) == OS_ENOENT);
    assert(__CountLog("firmware has no such clock") == 2);

    // Out of range: refused without a message.
    g_sentCount = 0;
    assert(__ClockGetRate(0, &rate) == OS_EINVALPARAMS);
    assert(__ClockGetRate(RPI_CLOCK_LAST_ID + 1, &rate) == OS_EINVALPARAMS);
    assert(__ClockSetEnabled(0, true) == OS_EINVALPARAMS);
    assert(g_sentCount == 0);
    __Unload();
    printf("clock service: state, on and off, rate, unknown clock: passed\n");
}

/////////////////////////////////////////////////////////////////////////////
// Two requests at once

struct Worker {
    pthread_t Thread;
    uint32_t  Line;
    oserr_t   Result;
    uint32_t  Value;
};

static void*
__WorkerMain(
    _In_ void* argument)
{
    struct Worker* worker = argument;

    __HandlerGetValue(worker->Line, &worker->Result, &worker->Value);
    return NULL;
}

// The first request waits for its answer until the second one is blocked.
static void
__WaitForContention(void)
{
    struct timespec pause = { 0, 100000 };

    for (int i = 0; i < 20000 && atomic_load(&g_contended) == 0; i++) {
        nanosleep(&pause, NULL);
    }
    g_onPause = NULL;
    __ReleaseHeld();
}

static void
TestOneCallAtATime(void)
{
    struct Worker   first = { .Line = 1 };
    struct Worker   second = { .Line = 2 };
    struct timespec pause = { 0, 100000 };

    __Ready();
    g_lines[1].State = 1;
    g_lines[2].State = 0;
    g_oneAtATime = true;
    g_holdAnswers = true;
    g_onPause = __WaitForContention;

    assert(pthread_create(&first.Thread, NULL, __WorkerMain, &first) == 0);
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&g_modelLock);
        uint32_t sent = g_sentCount;
        pthread_mutex_unlock(&g_modelLock);
        if (sent == 1) {
            break;
        }
        nanosleep(&pause, NULL);
    }
    assert(pthread_create(&second.Thread, NULL, __WorkerMain, &second) == 0);
    assert(pthread_join(first.Thread, NULL) == 0);
    assert(pthread_join(second.Thread, NULL) == 0);

    // The second request waited on a lock, and its message went out only
    // after the first answer was read (g_oneAtATime asserts that).
    assert(atomic_load(&g_contended) > 0);
    assert(first.Result == OS_EOK && first.Value == 1);
    assert(second.Result == OS_EOK && second.Value == 0);
    assert(g_sentCount == 2 && g_sent[0].Words[5] == 129 && g_sent[1].Words[5] == 130);
    g_oneAtATime = false;
    __Unload();
    printf("one call at a time, a second request waits for the first: passed\n");
}

int
main(void)
{
    TestSetupOrder();
    TestSetupFailures();
    TestEnableResults();
    TestTeardownRetry();
    TestTransport();
    TestWriteDeadline();
    TestLateAnswer();
    TestStaleAnswersAtSetup();
    TestWrongAnswer();
    TestRequestBytes();
    TestAnswerChecks();
    TestShortAnswers();
    TestGpio();
    TestGpioOnQemu();
    TestClock();
    TestOneCallAtATime();
    printf("rpi-firmware: all tests passed\n");
    return 0;
}
