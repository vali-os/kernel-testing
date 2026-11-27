/**
 * Copyright 2026, Philip Meulengracht
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
 * syslogd serial output host test
 * - Built by syslogd_test.py with services/syslogd/serial.c and reader.c.
 * - The kernel log, deviced, the serial ports and their driver, the gracht
 *   client and the usched lock, condition and job calls are fakes here.
 * - The job runs on the test thread. Time only passes in the fake condition
 *   wait and the fake sleep. That is where a case delivers what would arrive
 *   from outside: a new port, a removed port or transmit_ready.
 * - The fake lock checks that the lock is never taken twice, and the fake
 *   log checks that nothing is logged while the lock is held.
 */

#include <errno.h>
#include <gracht/client.h>
#include <gracht/link/vali.h>
#include <os/osdefs.h>
#include <os/types/syslog.h>
#include <os/usched/cond.h>
#include <os/usched/mutex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ctt_serial_service.h>

#include "reader.h"
#include "serial.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        abort(); \
    } \
} while (0)

#define DEVICED_ID    7
#define PORT_A        40
#define PORT_A_DRIVER 41
#define PORT_B        50
#define PORT_B_DRIVER 51
#define SERIAL_ID     ((uint8_t)(SERVICE_CTT_SERIAL_ID & 0xFF))

#define MAX_READS 16
#define WIRE_SIZE 8192

// One scripted answer of OSKernelLogRead. Data is 8 byte aligned like the
// kernel's read buffer, because the records in it hold 64 bit fields.
struct __ReadReply {
    _Alignas(8) uint8_t Data[1024];
    uint32_t            Length;
    uint32_t            Flags;
    uint64_t            NextSequence;
    uint64_t            Dropped;
    oserr_t             Status;
};

// A serial port and its driver. Room is how many more bytes the transmit
// buffer takes. A wait that "passes time" sends the buffer and makes Room
// RoomAfterWait again.
struct __FakePort {
    uuid_t   DeviceId;
    uuid_t   DriverId;
    size_t   Room;
    size_t   RoomAfterWait;
    oserr_t  SubscribeResult;
    int      Subscribed;
    int      WriteCount;
    int      FailWriteAt;    // the write with this number fails, 0 for none
    oserr_t  FailStatus;     // answer of the failing write
    int      FailSend;       // the failing write is not sent at all
    int      ClaimTooMuch;   // the failing write claims one byte too many
    char     Wire[WIRE_SIZE];
    size_t   WireLength;
};

// What the fake condition wait delivers on its next call.
enum __WaitEvent {
    WAIT_EVENT_NONE,
    WAIT_EVENT_OTHER_PROTOCOL,
    WAIT_EVENT_PORT_B,
    WAIT_EVENT_REMOVE_PORT_A,
    WAIT_EVENT_REMOVE_PORT_B
};

static struct __ReadReply g_replies[MAX_READS];
static int                g_replyCount;
static int                g_readCount;
static uint64_t           g_readSequences[MAX_READS];

static struct __FakePort g_ports[2];
static int               g_portAtStart;      // deviced reports port A when asked
static int               g_deviceSubscribes;
static int               g_deviceSubscribeResult;
static int               g_portQueries;
static int               g_awaitResult;
static int               g_abandons;
static oserr_t           g_pendingResult;
static uint32_t          g_pendingWritten;

static enum __WaitEvent g_waitEvents[8];
static int              g_waitEventCount;
static int              g_waitCount;

static bool g_locked;
static bool g_cancelled;
static int  g_cancelAfterIdle;   // cancel once the job sleeps this many times
static int  g_sleepCount;
static int  g_yieldCount;

static int  g_errorCount;
static int  g_warningCount;
static char g_lastLog[512];

static int g_clientMarker;

// Fakes of the system log, the kernel log and the services.

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;

    // The log line enters the kernel log, which the job reads under no lock.
    // Logging with the lock held would also hold up the event handlers.
    CHECK(!g_locked);
    va_start(arguments, format);
    vsnprintf(&g_lastLog[0], sizeof(g_lastLog), format, arguments);
    va_end(arguments);
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errorCount++;
    } else if (level == OSSYSLOGLEVEL_WARNING) {
        g_warningCount++;
    }
}

oserr_t
OSKernelLogRead(
    _In_  uint64_t                 sequence,
    _In_  void*                    buffer,
    _In_  size_t                   bufferSize,
    _Out_ OSKernelLogReadResult_t* result)
{
    struct __ReadReply* reply;

    CHECK(!g_locked);
    CHECK(bufferSize == LOG_READER_BUFFER_SIZE);
    CHECK(g_readCount < MAX_READS);
    g_readSequences[g_readCount] = sequence;

    // Past the scripted answers the log has nothing new.
    if (g_readCount >= g_replyCount) {
        g_readCount++;
        result->NextSequence = sequence;
        result->Dropped      = 0;
        result->BytesRead    = 0;
        result->Flags        = 0;
        return OS_EOK;
    }

    reply = &g_replies[g_readCount++];
    if (reply->Status != OS_EOK) {
        return reply->Status;
    }
    memcpy(buffer, &reply->Data[0], reply->Length);
    result->NextSequence = reply->NextSequence;
    result->Dropped      = reply->Dropped;
    result->BytesRead    = reply->Length;
    result->Flags        = reply->Flags;
    return OS_EOK;
}

oserr_t
OSServiceWaitForDevice(
    _In_ size_t timeout)
{
    CHECK(timeout == 0);
    return OS_EOK;
}

uuid_t
OSServiceGetDevice(void)
{
    return DEVICED_ID;
}

gracht_client_t*
GetGrachtClient(void)
{
    return (gracht_client_t*)&g_clientMarker;
}

static uuid_t
__Target(
    _In_ struct gracht_message_context* context)
{
    return ((struct vali_link_message*)context)->address.Data.GlobalHandleID;
}

int
sys_device_subscribe(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    CHECK(client == GetGrachtClient());
    CHECK(__Target(context) == DEVICED_ID);
    g_deviceSubscribes++;
    return g_deviceSubscribeResult;
}

int
sys_device_get_devices_by_protocol(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uint8_t                  protocolId)
{
    CHECK(client == GetGrachtClient());
    CHECK(__Target(context) == DEVICED_ID);
    CHECK(protocolId == SERIAL_ID);

    // Ports must be asked for after the subscription, or one that registers
    // in between is never reported.
    CHECK(g_deviceSubscribes == 1);
    g_portQueries++;

    // deviced answers with an event per port. The server loop would deliver
    // it later. The job does not hold the lock here, so deliver it now.
    if (g_portAtStart) {
        SerialOutputPortFound(PORT_A, PORT_A_DRIVER, SERIAL_ID);
    }
    return 0;
}

// Fakes of the gracht client and the serial contract.

int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                   flags)
{
    CHECK(client == GetGrachtClient());
    CHECK(context != NULL);

    // The server loop delivers the answer, so the wait must let it run.
    CHECK(flags == GRACHT_AWAIT_ASYNC);
    CHECK(!g_locked);
    return g_awaitResult;
}

int
gracht_client_get_status(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    return GRACHT_MESSAGE_COMPLETED;
}

int
gracht_client_abandon(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    g_abandons++;
    return 0;
}

static struct __FakePort*
__FindPort(
    _In_ uuid_t deviceId,
    _In_ uuid_t driverId)
{
    for (int i = 0; i < 2; i++) {
        if (g_ports[i].DeviceId == deviceId) {
            // The call must go to the driver of the port.
            CHECK(g_ports[i].DriverId == driverId);
            return &g_ports[i];
        }
    }
    CHECK(false);
    return NULL;
}

int
ctt_serial_subscribe_port(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t                   deviceId)
{
    struct __FakePort* port = __FindPort(deviceId, __Target(context));

    CHECK(client == GetGrachtClient());
    g_pendingResult = port->SubscribeResult;
    if (port->SubscribeResult == OS_EOK || port->SubscribeResult == OS_EEXISTS) {
        port->Subscribed = 1;
    }
    return 0;
}

int
ctt_serial_subscribe_port_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                       resultOut)
{
    (void)client;
    (void)context;
    *resultOut = g_pendingResult;
    return 0;
}

int
ctt_serial_write(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t                   deviceId,
    _In_ const uint8_t*                 data,
    _In_ const uint32_t                 count)
{
    struct __FakePort* port = __FindPort(deviceId, __Target(context));
    size_t             taken;

    CHECK(client == GetGrachtClient());
    CHECK(count > 0 && count <= CTT_SERIAL_LIMIT_TRANSFER_BYTES);

    // No case needs this many writes. A job that writes in a loop without
    // waiting would spin forever otherwise.
    CHECK(port->WriteCount < 10000);
    port->WriteCount++;

    if (port->WriteCount == port->FailWriteAt) {
        if (port->FailSend) {
            return -1;
        }
        g_pendingResult  = port->FailStatus;
        g_pendingWritten = port->ClaimTooMuch ? count + 1 : 0;
        return 0;
    }

    taken = count < port->Room ? count : port->Room;
    CHECK(port->WireLength + taken <= WIRE_SIZE);
    memcpy(&port->Wire[port->WireLength], data, taken);
    port->WireLength += taken;
    port->Room       -= taken;
    g_pendingResult   = OS_EOK;
    g_pendingWritten  = (uint32_t)taken;
    return 0;
}

int
ctt_serial_write_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                       resultOut,
    _Out_ uint32_t*                      writtenOut)
{
    (void)client;
    (void)context;
    *resultOut  = g_pendingResult;
    *writtenOut = g_pendingWritten;
    return 0;
}

// Fakes of usched. Only one lock exists in serial.c, so one flag tracks it.

void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    (void)mutex;
    CHECK(type == USCHED_MUTEX_PLAIN);
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    CHECK(!g_locked);
    g_locked = true;
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    CHECK(g_locked);
    g_locked = false;
}

void
usched_cnd_init(
    _In_ struct usched_cnd* condition)
{
    (void)condition;
}

void
usched_cnd_notify_all(
    _In_ struct usched_cnd* condition)
{
    (void)condition;
    CHECK(g_locked);
}

int
usched_deadline_after_us(
    _In_  uint64_t         microseconds,
    _Out_ struct timespec* until)
{
    CHECK(microseconds > 0);
    memset(until, 0, sizeof(*until));
    return 0;
}

/**
 * The wait releases the lock while time passes. The ports send their
 * transmit buffers, and the next scripted event is delivered the way the
 * server loop would: through the public functions, which take the lock. A
 * port without an events subscription gives no wake-up, so the wait times
 * out. When nothing at all happens, the job is cancelled.
 */
int
usched_cnd_timedwait(
    _In_ struct usched_cnd*     condition,
    _In_ struct usched_mtx*     mutex,
    _In_ const struct timespec* until)
{
    enum __WaitEvent event    = WAIT_EVENT_NONE;
    int              woken    = 0;
    int              refilled = 0;

    (void)condition;
    CHECK(until != NULL);
    CHECK(g_locked);
    g_locked = false;
    g_waitCount++;

    for (int i = 0; i < 2; i++) {
        if (g_ports[i].Room == 0 && g_ports[i].RoomAfterWait != 0) {
            g_ports[i].Room = g_ports[i].RoomAfterWait;
            refilled = 1;
            if (g_ports[i].Subscribed) {
                SerialOutputTransmitReady(g_ports[i].DeviceId);
                woken = 1;
            }
        }
    }

    if (g_waitEventCount > 0) {
        event = g_waitEvents[0];
        memmove(&g_waitEvents[0], &g_waitEvents[1], sizeof(g_waitEvents) - sizeof(g_waitEvents[0]));
        g_waitEventCount--;
    }

    switch (event) {
        case WAIT_EVENT_OTHER_PROTOCOL:
            // A device of another contract is never taken as a port. The
            // device is port A, so a mistake shows up as writes to it.
            SerialOutputPortFound(PORT_A, PORT_A_DRIVER, (uint8_t)(SERIAL_ID + 1));
            break;
        case WAIT_EVENT_PORT_B:
            SerialOutputPortFound(PORT_B, PORT_B_DRIVER, SERIAL_ID);
            woken = 1;
            break;
        case WAIT_EVENT_REMOVE_PORT_A:
            SerialOutputPortRemoved(PORT_A);
            woken = 1;
            break;
        case WAIT_EVENT_REMOVE_PORT_B:
            // Not the port in use, so nothing changes and nothing wakes.
            SerialOutputPortRemoved(PORT_B);
            break;
        case WAIT_EVENT_NONE:
            if (!woken && !refilled) {
                g_cancelled = true;
            }
            break;
    }

    (void)mutex;
    g_locked = true;
    if (woken) {
        return 0;
    }
    errno = ETIME;
    return -1;
}

bool
usched_is_cancelled(
    _In_ const void* cancellationToken)
{
    (void)cancellationToken;
    return g_cancelled;
}

void
usched_job_yield(void)
{
    g_yieldCount++;
}

int
usched_job_sleep_us(
    _In_ uint64_t microseconds)
{
    CHECK(microseconds > 0);
    CHECK(!g_locked);
    g_sleepCount++;
    if (g_sleepCount >= g_cancelAfterIdle) {
        g_cancelled = true;
    }
    return 0;
}

// reader.c is linked for the reader and the record check. Its console calls
// are never reached from the serial output.

void
ConsoleShowRecord(
    _In_ const OSKernelLogRecord_t* record,
    _In_ const char*                text)
{
    (void)record;
    (void)text;
    CHECK(false);
}

void
ConsoleShowDropped(
    _In_ uint64_t count)
{
    (void)count;
    CHECK(false);
}

void
ConsoleDraw(void)
{
    CHECK(false);
}

// Helpers of the cases.

static void
__Reset(void)
{
    memset(&g_replies[0], 0, sizeof(g_replies));
    memset(&g_ports[0], 0, sizeof(g_ports));
    g_replyCount = 0;
    g_readCount  = 0;
    g_ports[0].DeviceId      = PORT_A;
    g_ports[0].DriverId      = PORT_A_DRIVER;
    g_ports[0].Room          = WIRE_SIZE;
    g_ports[1].DeviceId      = PORT_B;
    g_ports[1].DriverId      = PORT_B_DRIVER;
    g_ports[1].Room          = WIRE_SIZE;
    g_portAtStart            = 1;
    g_deviceSubscribes       = 0;
    g_deviceSubscribeResult  = 0;
    g_portQueries            = 0;
    g_awaitResult            = 0;
    g_abandons               = 0;
    g_waitEventCount         = 0;
    g_waitCount              = 0;
    g_locked                 = false;
    g_cancelled              = false;
    g_cancelAfterIdle        = 1;
    g_sleepCount             = 0;
    g_yieldCount             = 0;
    g_errorCount             = 0;
    g_warningCount           = 0;
    g_lastLog[0]             = '\0';
    CHECK(SerialOutputInitialize() == OS_EOK);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STARTING);
}

/**
 * @brief Adds a record to a reply the way the kernel lays it out. The padding
 * after the text is 'X', so text read past its length shows up on the wire.
 */
static OSKernelLogRecord_t*
__AddRecord(
    _In_ struct __ReadReply* reply,
    _In_ uint64_t            sequence,
    _In_ const char*         text)
{
    OSKernelLogRecord_t* record = (OSKernelLogRecord_t*)&reply->Data[reply->Length];
    uint32_t             length = (uint32_t)strlen(text);
    uint32_t             size   = (uint32_t)((sizeof(OSKernelLogRecord_t) + length + 7) & ~7U);

    CHECK(reply->Length + size <= sizeof(reply->Data));
    memset(record, 'X', size);
    memset(record, 0, sizeof(OSKernelLogRecord_t));
    record->Size      = size;
    record->Length    = (uint16_t)length;
    record->Level     = OSSYSLOGLEVEL_WARNING;
    record->CoreID    = 1;
    record->ThreadID  = 2;
    record->Sequence  = sequence;
    record->Timestamp = (sequence + 1) * NSEC_PER_MSEC;
    memcpy((uint8_t*)record + sizeof(OSKernelLogRecord_t), text, length);
    reply->Length      += size;
    reply->NextSequence = sequence + 1;
    return record;
}

// The first read of the job only looks at the flags.
static void
__AddProbe(
    _In_ uint32_t flags)
{
    g_replies[g_replyCount].Flags = flags;
    g_replyCount++;
}

static void
__AddWaitEvent(
    _In_ enum __WaitEvent event)
{
    CHECK(g_waitEventCount < (int)(sizeof(g_waitEvents) / sizeof(g_waitEvents[0])));
    g_waitEvents[g_waitEventCount++] = event;
}

static void
__CheckWire(
    _In_ const struct __FakePort* port,
    _In_ const char*              expected)
{
    if (port->WireLength != strlen(expected) || memcmp(&port->Wire[0], expected, port->WireLength) != 0) {
        fprintf(stderr, "wire: '%.*s'\nwant: '%s'\n", (int)port->WireLength, &port->Wire[0], expected);
        CHECK(false);
    }
}

// The cases.

static void
__TestFormat(void)
{
    OSKernelLogRecord_t record = { 0 };
    char                line[SERIAL_OUTPUT_LINE_MAX];
    char                text[OSKERNELLOG_MESSAGE_MAX + 1];
    size_t              length;

    // The kernel's format, with CR LF for every LF.
    record.Level     = OSSYSLOGLEVEL_DEBUG;
    record.CoreID    = 3;
    record.ThreadID  = 17;
    record.Timestamp = 1234 * NSEC_PER_MSEC + 999;
    length = SerialOutputFormatRecord(&record, "first\nsecond", &line[0], sizeof(line));
    CHECK(length == strlen("000001234 [debug-3-17] first\r\nsecond\r\n"));
    CHECK(memcmp(&line[0], "000001234 [debug-3-17] first\r\nsecond\r\n", length) == 0);

    // A level outside the table has a name too.
    record.Level = 9;
    length = SerialOutputFormatRecord(&record, "x", &line[0], sizeof(line));
    CHECK(memcmp(&line[0], "000001234 [unknown-3-17] x\r\n", length) == 0);

    // A short buffer cuts the line off, and never ends it with a CR alone.
    length = SerialOutputFormatRecord(&record, "x", &line[0], 5);
    CHECK(length == 5 && memcmp(&line[0], "00000", 5) == 0);
    record.Level = OSSYSLOGLEVEL_ERROR;
    length = SerialOutputFormatRecord(&record, "x", &line[0], strlen("000001234 [error-3-17] x\r"));
    CHECK(length == strlen("000001234 [error-3-17] x"));

    // The longest message, made of LFs only, fits in SERIAL_OUTPUT_LINE_MAX.
    record.CoreID    = 0xFFFFFFFFU;
    record.ThreadID  = 0xFFFFFFFFU;
    record.Timestamp = UINT64_MAX;
    memset(&text[0], '\n', OSKERNELLOG_MESSAGE_MAX);
    text[OSKERNELLOG_MESSAGE_MAX] = '\0';
    length = SerialOutputFormatRecord(&record, &text[0], &line[0], sizeof(line));
    CHECK(length > 2 * (OSKERNELLOG_MESSAGE_MAX + 1));
    CHECK(line[length - 2] == '\r' && line[length - 1] == '\n');
}

static void
__TestKernelConsole(void)
{
    // The kernel prints to serial itself, so the output stays out of the
    // way: no deviced calls and no writes. The read buffer is freed (the
    // leak checker fails the test otherwise).
    __Reset();
    __AddProbe(OSKERNELLOG_READ_SERIAL_CONSOLE);
    SerialOutputJob(NULL, NULL);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_KERNEL_CONSOLE);
    CHECK(g_readCount == 1);
    CHECK(g_deviceSubscribes == 0 && g_portQueries == 0);
    CHECK(g_ports[0].WriteCount == 0);
    CHECK(g_errorCount == 0);

    // A port reported anyway is not used.
    SerialOutputPortFound(PORT_A, PORT_A_DRIVER, SERIAL_ID);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_KERNEL_CONSOLE);
}

static void
__TestBacklog(void)
{
    // Messages logged before the port existed are printed from the oldest
    // one on, and the port's events are subscribed to once.
    __Reset();
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "first");
    __AddRecord(&g_replies[1], 1, "second");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] first\r\n000000002 [warn-1-2] second\r\n");
    CHECK(g_ports[0].Subscribed);
    CHECK(g_readSequences[0] == 0 && g_readSequences[1] == 0 && g_readSequences[2] == 2);

    // A printed batch lets other jobs run before the next read. The job only
    // sleeps once the log has nothing new.
    CHECK(g_yieldCount == 1 && g_sleepCount == 1);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);
    CHECK(g_errorCount == 0 && g_warningCount == 0);
    CHECK(strstr(&g_lastLog[0], "printing the log on serial port 40") != NULL);
}

static void
__TestLatePort(void)
{
    // No port when deviced is asked. A device of another protocol is not
    // taken, the serial port that shows up after it is.
    __Reset();
    g_portAtStart = 0;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "late");
    g_replyCount = 2;
    __AddWaitEvent(WAIT_EVENT_OTHER_PROTOCOL);
    __AddWaitEvent(WAIT_EVENT_PORT_B);
    SerialOutputJob(NULL, NULL);
    CHECK(g_ports[0].WriteCount == 0);
    __CheckWire(&g_ports[1], "000000001 [warn-1-2] late\r\n");
    CHECK(g_readSequences[1] == 0);
    CHECK(g_errorCount == 0);
}

static void
__TestNoPortEver(void)
{
    // The job waits for a port until it is cancelled, without reading.
    __Reset();
    g_portAtStart = 0;
    __AddProbe(0);
    SerialOutputJob(NULL, NULL);
    CHECK(g_readCount == 1);
    CHECK(g_waitCount == 1);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);
    CHECK(g_errorCount == 0);
}

static void
__TestPartialWrites(void)
{
    const char* expected = "000000001 [warn-1-2] a message that is longer than the room\r\n";

    // The driver takes 8 bytes at a time. The rest is written again after
    // each transmit_ready, so every byte arrives once and in order.
    __Reset();
    g_ports[0].Room          = 8;
    g_ports[0].RoomAfterWait = 8;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "a message that is longer than the room");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], expected);
    CHECK(g_waitCount >= (int)(strlen(expected) / 8));
    CHECK(g_warningCount == 0 && g_errorCount == 0);

    // A port whose events cannot be subscribed to still gets every byte. The
    // waits time out instead.
    __Reset();
    g_ports[0].Room            = 8;
    g_ports[0].RoomAfterWait   = 8;
    g_ports[0].SubscribeResult = OS_EBUSY;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "a message that is longer than the room");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], expected);
    CHECK(!g_ports[0].Subscribed);
    CHECK(g_warningCount == 1);
    CHECK(g_errorCount == 0);

    // syslogd may already be on the list of the port.
    __Reset();
    g_ports[0].SubscribeResult = OS_EEXISTS;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "listed");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] listed\r\n");
    CHECK(g_warningCount == 0);
}

static void
__TestDropped(void)
{
    // Messages the kernel dropped before they were read get one line first.
    __Reset();
    __AddProbe(0);
    g_replies[1].Dropped = 3;
    __AddRecord(&g_replies[1], 3, "after");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0],
        "[syslogd] 3 messages were dropped before they were printed\r\n"
        "000000004 [warn-1-2] after\r\n");
}

/**
 * @brief Runs one case where the second write to port A fails in the way
 * set up by configure. Port B shows up afterwards. The message whose write
 * failed is printed again on port B, the one before it is not.
 */
static void
__CheckFailedWrite(
    _In_ void (*configure)(struct __FakePort*))
{
    __Reset();
    g_ports[0].FailWriteAt = 2;
    configure(&g_ports[0]);
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "one");
    __AddRecord(&g_replies[1], 1, "two");
    __AddRecord(&g_replies[2], 1, "two");
    g_replyCount = 3;
    __AddWaitEvent(WAIT_EVENT_PORT_B);
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] one\r\n");
    __CheckWire(&g_ports[1], "000000002 [warn-1-2] two\r\n");
    CHECK(g_readSequences[2] == 1);
    CHECK(g_warningCount == 1);
    CHECK(strstr(&g_lastLog[0], "printing the log on serial port 50") != NULL);
}

static void
__RefuseWrite(
    _In_ struct __FakePort* port)
{
    port->FailStatus = OS_ENOENT;
}

static void
__DropSend(
    _In_ struct __FakePort* port)
{
    port->FailSend = 1;
}

static void
__ClaimTooMuch(
    _In_ struct __FakePort* port)
{
    port->FailStatus   = OS_EOK;
    port->ClaimTooMuch = 1;
}

static void
__TestFailedWrites(void)
{
    __CheckFailedWrite(__RefuseWrite);
    __CheckFailedWrite(__DropSend);
    __CheckFailedWrite(__ClaimTooMuch);

    // An answer that never arrives is abandoned.
    __Reset();
    g_awaitResult = -1;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "lost");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    CHECK(g_abandons >= 1);
    CHECK(g_warningCount >= 1);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);
}

static void
__TestPortRemoved(void)
{
    // Port A goes away while a write waits for room. The output waits for a
    // port again, and prints the whole message on port B.
    __Reset();
    g_ports[0].Room = 4;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "removed");
    __AddRecord(&g_replies[2], 0, "removed");
    g_replyCount = 3;
    __AddWaitEvent(WAIT_EVENT_REMOVE_PORT_A);
    __AddWaitEvent(WAIT_EVENT_PORT_B);
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "0000");
    __CheckWire(&g_ports[1], "000000001 [warn-1-2] removed\r\n");
    CHECK(g_readSequences[2] == 0);
}

static void
__TestOtherPorts(void)
{
    // A port reported before the job waits for one is not taken.
    __Reset();
    g_portAtStart = 0;
    SerialOutputPortFound(PORT_A, PORT_A_DRIVER, SERIAL_ID);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STARTING);
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "early");
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    CHECK(g_ports[0].WriteCount == 0);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);

    // While port A prints, port B is reported. It is not used.
    __Reset();
    g_ports[0].Room          = 4;
    g_ports[0].RoomAfterWait = 64;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "kept");
    g_replyCount = 2;
    __AddWaitEvent(WAIT_EVENT_PORT_B);
    SerialOutputJob(NULL, NULL);
    CHECK(g_ports[1].WriteCount == 0);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] kept\r\n");
    CHECK(strstr(&g_lastLog[0], "serial port 50 is not used") != NULL);

    // While port A prints, port B is removed. Port A is still used.
    __Reset();
    g_ports[0].Room          = 4;
    g_ports[0].RoomAfterWait = 64;
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "still here");
    g_replyCount = 2;
    __AddWaitEvent(WAIT_EVENT_REMOVE_PORT_B);
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] still here\r\n");
    CHECK(g_warningCount == 0);
}

static void
__TestMalformed(void)
{
    OSKernelLogRecord_t* second;

    // A broken record ends the read with an error. The messages before it
    // are printed, and the next read starts after the broken read.
    __Reset();
    __AddProbe(0);
    __AddRecord(&g_replies[1], 0, "good");
    second = __AddRecord(&g_replies[1], 1, "damaged");
    second->Length = OSKERNELLOG_MESSAGE_MAX + 1;
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    __CheckWire(&g_ports[0], "000000001 [warn-1-2] good\r\n");
    CHECK(g_errorCount == 1);
    CHECK(g_readSequences[2] == 2);
}

static void
__TestFailures(void)
{
    // A read that fails ends the job.
    __Reset();
    __AddProbe(0);
    g_replies[1].Status = OS_EPERMISSIONS;
    g_replyCount = 2;
    SerialOutputJob(NULL, NULL);
    CHECK(g_errorCount == 1);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);

    // So does a first read that fails, before deviced is asked.
    __Reset();
    g_replies[0].Status = OS_EPERMISSIONS;
    g_replyCount = 1;
    SerialOutputJob(NULL, NULL);
    CHECK(g_errorCount == 1);
    CHECK(g_deviceSubscribes == 0);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);

    // And deviced events that cannot be subscribed to.
    __Reset();
    g_deviceSubscribeResult = -1;
    __AddProbe(0);
    SerialOutputJob(NULL, NULL);
    CHECK(g_errorCount == 2);
    CHECK(g_portQueries == 0);
    CHECK(g_ports[0].WriteCount == 0);
    CHECK(SerialOutputGetState() == SERIAL_OUTPUT_STOPPED);
}

int
main(void)
{
    __TestFormat();
    __TestKernelConsole();
    __TestBacklog();
    __TestLatePort();
    __TestNoPortEver();
    __TestPartialWrites();
    __TestDropped();
    __TestFailedWrites();
    __TestPortRemoved();
    __TestOtherPorts();
    __TestMalformed();
    __TestFailures();
    printf("syslogd serial output tests passed\n");
    return 0;
}
