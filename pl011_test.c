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
 * Host test for the PL011 serial port driver (modules/serial/uart/pl011),
 * linked with the real driver sources: buffer.c, hardware.c, interrupt.c,
 * port.c and service.c. main.c only forwards to port.c and is not linked.
 *
 * QEMU has no RP1, so the port is a model here:
 * - A register window with the PL011 registers the driver uses, a 32-byte
 *   receive FIFO whose entries carry the DR error bits, and a 32-byte
 *   transmit FIFO. FR, MIS and the FIFO flush on a cleared FEN follow the
 *   TRM. Bytes leave the transmit FIFO for the "wire" whenever the driver
 *   busy-waits, unless a case holds them back. The model fails the test when
 *   LCR_H, IBRD or FBRD change while the port is on, or when a byte is
 *   written to a full FIFO or to a port that is off.
 * - deviced (DeviceSetStage, DeviceGetResources, DeviceClockGetRate), the
 *   register mapping, the DeviceInterrupt calls, the event descriptor and
 *   the I/O set, the module lock and the gracht answers and events.
 *
 * Every register write and every outside call is recorded in one event
 * list, in order, so the cases can check what happened and in which order.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/devices/clock.h>
#include <ddk/devices/device.h>
#include <ddk/devices/resources.h>
#include <ddk/interrupts/interrupt.h>
#include <ddk/time.h>
#include <ddk/utils.h>
#include <event.h>
#include <gracht/server.h>
#include <io.h>
#include <ioctl.h>
#include <ioset.h>
#include <os/usched/mutex.h>

#include <ctt_serial_service_server.h>

#include "pl011.h"
#include "registers.h"

#define DEVICE_ID      7U
#define OTHER_DEVICE   8U
#define WINDOW_ID      21U
#define WINDOW_LENGTH  0x100U
#define EVENT_FD       33
#define AIO_HANDLE     5
#define FIFO_DEPTH     32U
#define CLOCK_HZ       50000000ULL
#define MAX_EVENTS     8192U
#define NO_FAILURE     UINT32_MAX

// Divisor of 115200 baud from 50 MHz: 50e6 * 4 / 115200 = 1736.1, so 1736
// 64ths, which is 27 and 8/64.
#define IBRD_115200    27U
#define FBRD_115200    8U
// LCR_H of 8 data bits, no parity, 1 stop bit, FIFOs on.
#define LCR_H_8N1      0x70U
// CR of a running port: UARTEN, TXE, RXE and RTS.
#define CR_RUNNING     0xb01U

enum EventKind {
    EventWrite,
    EventPrepare,
    EventStart,
    EventStop,
    EventResources,
    EventAcquire,
    EventRelease,
    EventClockRate,
    EventInterruptAllocate,
    EventInterruptEnable,
    EventInterruptDestroy,
    EventIosetAdd,
    EventIosetDelete,
    EventClose,
    EventFastWrite,
    EventSignal,
    EventDataReceived,
    EventTransmitReady,
    EventProtocol
};

struct Event {
    enum EventKind Kind;
    uint32_t       Offset;
    uint32_t       Value;
};

static struct Event g_events[MAX_EVENTS];
static uint32_t     g_eventCount;

// The modelled port.
static struct {
    uint32_t Cr;
    uint32_t LcrH;
    uint32_t Ibrd;
    uint32_t Fbrd;
    uint32_t Ifls;
    uint32_t Imsc;
    uint16_t Rx[FIFO_DEPTH];
    uint32_t RxCount;
    uint8_t  Tx[FIFO_DEPTH];
    uint32_t TxCount;
    uint8_t  Wire[16384];
    uint32_t WireCount;
    // Bytes stay in the transmit FIFO and BUSY stays set.
    bool     TxStuck;
    // Received bytes have waited long enough for the RT cause.
    bool     RxTimeout;
} g_uart;

// Fake deviced and the outside calls.
static bool             g_mapped;
static uint32_t         g_windowLength = WINDOW_LENGTH;
static uint32_t         g_windowCount = 1;
static enum DeviceStage g_failStage = (enum DeviceStage)NO_FAILURE;
static oserr_t          g_clockStatus = OS_EOK;
static uint32_t         g_failWriteOffset = NO_FAILURE;
// The next write to this offset fails, and later ones land.
static uint32_t         g_failWriteOnceOffset = NO_FAILURE;
// Bytes that arrive on the wire while the driver busy-waits, one per wait.
static uint32_t         g_stallArrivals;
static uint32_t         g_failInterruptDestroys;
static uint32_t         g_failInterruptEnable;
static uint64_t         g_stalledUs;
static bool             g_interruptEnabled;
static bool             g_locked;
static int              g_eventCounter;
static int              g_serverMarker;
static InterruptResourceTable_t g_table;
gracht_protocol_t       ctt_serial_server_protocol;
static uint32_t         g_protocolRegistrations;

// The last answer sent to a client.
static struct {
    uint32_t               Count;
    oserr_t                Status;
    uint32_t               Written;
    uint8_t                Data[CTT_SERIAL_LIMIT_TRANSFER_BYTES];
    uint32_t               DataCount;
    struct ctt_serial_line Line;
    struct ctt_serial_counters Counters;
} g_reply;

// Clients whose event sends fail, as a bit per connection number.
static uint32_t g_unreachableClients;

static void
__Record(
    _In_ enum EventKind kind,
    _In_ uint32_t       offset,
    _In_ uint32_t       value)
{
    assert(g_eventCount < MAX_EVENTS);
    g_events[g_eventCount++] = (struct Event) { kind, offset, value };
}

// Returns the index of the first matching event at or after start, or
// UINT32_MAX. ANY matches every offset or value.
#define ANY UINT32_MAX
static uint32_t
__Find(
    _In_ uint32_t       start,
    _In_ enum EventKind kind,
    _In_ uint32_t       offset,
    _In_ uint32_t       value)
{
    for (uint32_t i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind && (offset == ANY || g_events[i].Offset == offset) &&
            (value == ANY || g_events[i].Value == value)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t
__Count(
    _In_ enum EventKind kind,
    _In_ uint32_t       offset)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind && (offset == ANY || g_events[i].Offset == offset)) {
            count++;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// Port model
// ---------------------------------------------------------------------------

static uint32_t
__RawCauses(void)
{
    uint32_t causes = 0;

    // Half full (receive) and half empty (transmit), the levels the driver
    // selects.
    if (g_uart.RxCount >= FIFO_DEPTH / 2U) {
        causes |= PL011_INT_RX;
    }
    if (g_uart.RxCount != 0 && g_uart.RxTimeout) {
        causes |= PL011_INT_RT;
    }
    if (g_uart.TxCount <= FIFO_DEPTH / 2U) {
        causes |= PL011_INT_TX;
    }
    return causes;
}

static void
__DrainWire(void)
{
    if (g_uart.TxStuck) {
        return;
    }
    assert(g_uart.WireCount + g_uart.TxCount <= sizeof(g_uart.Wire));
    memcpy(&g_uart.Wire[g_uart.WireCount], g_uart.Tx, g_uart.TxCount);
    g_uart.WireCount += g_uart.TxCount;
    g_uart.TxCount = 0;
}

static void
__Arrive(
    _In_ uint16_t entry)
{
    assert(g_uart.RxCount < FIFO_DEPTH);
    g_uart.Rx[g_uart.RxCount++] = entry;
}

static size_t
__ModelRead(
    _In_ uint32_t offset)
{
    uint32_t flags = 0;
    uint16_t entry;

    switch (offset) {
        case PL011_FR:
            if (g_uart.RxCount == 0) {
                flags |= PL011_FR_RXFE;
            }
            if (g_uart.TxCount == FIFO_DEPTH) {
                flags |= PL011_FR_TXFF;
            }
            if (g_uart.TxCount == 0) {
                flags |= PL011_FR_TXFE;
            }
            if (g_uart.TxCount != 0 || g_uart.TxStuck) {
                flags |= PL011_FR_BUSY;
            }
            return flags;
        case PL011_DR:
            assert(g_uart.RxCount != 0);
            entry = g_uart.Rx[0];
            memmove(&g_uart.Rx[0], &g_uart.Rx[1], (g_uart.RxCount - 1U) * sizeof(uint16_t));
            g_uart.RxCount--;
            return entry;
        case PL011_MIS: return __RawCauses() & g_uart.Imsc;
        case PL011_CR: return g_uart.Cr;
        case PL011_LCR_H: return g_uart.LcrH;
        case PL011_IBRD: return g_uart.Ibrd;
        case PL011_FBRD: return g_uart.Fbrd;
        case PL011_IFLS: return g_uart.Ifls;
        case PL011_IMSC: return g_uart.Imsc;
        default:
            assert(!"read of a register the driver does not use");
            return 0;
    }
}

static void
__ModelWrite(
    _In_ uint32_t offset,
    _In_ uint32_t value)
{
    switch (offset) {
        case PL011_DR:
            // A byte written to a port that is off, or to a full FIFO, is lost.
            assert((g_uart.Cr & (PL011_CR_UARTEN | PL011_CR_TXE)) == (PL011_CR_UARTEN | PL011_CR_TXE));
            assert(g_uart.TxCount < FIFO_DEPTH);
            g_uart.Tx[g_uart.TxCount++] = (uint8_t)value;
            break;
        case PL011_LCR_H:
            // The TRM: the port must be off while the line settings change.
            assert((g_uart.Cr & PL011_CR_UARTEN) == 0);
            if ((value & PL011_LCR_H_FEN) == 0) {
                g_uart.RxCount = 0;
                g_uart.TxCount = 0;
            }
            g_uart.LcrH = value;
            break;
        case PL011_IBRD:
            assert((g_uart.Cr & PL011_CR_UARTEN) == 0);
            g_uart.Ibrd = value;
            break;
        case PL011_FBRD:
            assert((g_uart.Cr & PL011_CR_UARTEN) == 0);
            g_uart.Fbrd = value;
            break;
        case PL011_CR: g_uart.Cr = value; break;
        case PL011_IFLS: g_uart.Ifls = value; break;
        case PL011_IMSC: g_uart.Imsc = value; break;
        case PL011_ICR: break;
        default:
            assert(!"write of a register the driver does not use");
    }
}

static void
__ResetModel(void)
{
    memset(&g_uart, 0, sizeof(g_uart));
    // As the firmware might leave it: running, with causes enabled.
    g_uart.Cr = CR_RUNNING;
    g_uart.LcrH = LCR_H_8N1;
    g_uart.Imsc = 0x7f0U;
}

// ---------------------------------------------------------------------------
// libddk, libc and gracht fakes
// ---------------------------------------------------------------------------

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}

size_t
DeviceIoRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    assert(ioSpace->Id == WINDOW_ID && g_mapped && length == 4);
    assert(offset < ioSpace->Access.Memory.Length);
    return __ModelRead((uint32_t)offset);
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(ioSpace->Id == WINDOW_ID && g_mapped && length == 4);
    assert(offset < ioSpace->Access.Memory.Length);
    __Record(EventWrite, (uint32_t)offset, (uint32_t)value);
    if (g_failWriteOffset == offset) {
        return OS_EDEVFAULT;
    }
    if (g_failWriteOnceOffset == offset) {
        g_failWriteOnceOffset = NO_FAILURE;
        return OS_EDEVFAULT;
    }
    __ModelWrite((uint32_t)offset, (uint32_t)value);
    return OS_EOK;
}

oserr_t
DeviceIoAcquire(
    _InOut_ DeviceIo_t* ioSpace)
{
    assert(ioSpace->Id == WINDOW_ID && !g_mapped);
    __Record(EventAcquire, 0, 0);
    g_mapped = true;
    return OS_EOK;
}

oserr_t
DeviceIoRelease(
    _InOut_ DeviceIo_t* ioSpace)
{
    assert(ioSpace->Id == WINDOW_ID && g_mapped);
    __Record(EventRelease, 0, 0);
    g_mapped = false;
    return OS_EOK;
}

oserr_t
DDKStallUs(
    _In_ uint32_t microseconds)
{
    g_stalledUs += microseconds;
    __DrainWire();
    if (g_stallArrivals != 0) {
        g_stallArrivals--;
        __Arrive((uint16_t)('0' + g_stallArrivals % 10U));
    }
    return OS_EOK;
}

oserr_t
DeviceSetStage(
    _In_ uuid_t           device,
    _In_ enum DeviceStage stage)
{
    assert(device == DEVICE_ID);
    __Record(stage == DevicePrepare ? EventPrepare : stage == DeviceStart ? EventStart : EventStop, 0, 0);
    // deviced refuses to stop a device that still holds its interrupt.
    if (stage == DeviceStop) {
        assert(!g_interruptEnabled);
    }
    return stage == g_failStage ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceGetResources(
    _In_  uuid_t                  device,
    _Out_ struct DeviceResources* resources)
{
    assert(device == DEVICE_ID);
    __Record(EventResources, 0, 0);
    memset(resources, 0, sizeof(*resources));
    resources->Count = g_windowCount;
    resources->Registers[0].Id = WINDOW_ID;
    resources->Registers[0].Type = DeviceIoMemoryBased;
    resources->Registers[0].Access.Memory.Length = g_windowLength;
    return OS_EOK;
}

oserr_t
DeviceClockGetRate(
    _In_  uuid_t    device,
    _In_  uint32_t  index,
    _Out_ uint64_t* hz)
{
    assert(device == DEVICE_ID && index == 0);
    __Record(EventClockRate, 0, 0);
    if (g_clockStatus != OS_EOK) {
        return g_clockStatus;
    }
    *hz = CLOCK_HZ;
    return OS_EOK;
}

void
DeviceInterruptInitialize(
    _Out_ DeviceInterrupt_t* interrupt,
    _In_  Device_t*          device)
{
    assert(device->Id == DEVICE_ID);
    memset(interrupt, 0, sizeof(*interrupt));
    interrupt->DeviceId = device->Id;
}

oserr_t
DeviceInterruptSelectSource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index)
{
    assert(index == PL011_INTERRUPT_INDEX);
    interrupt->SourceIndex = index;
    return OS_EOK;
}

oserr_t
DeviceInterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           min,
    _In_    uint32_t           optimal,
    _Out_   uint32_t*          countOut)
{
    assert(min == 1 && optimal == 1);
    __Record(EventInterruptAllocate, 0, 0);
    interrupt->Count = 1;
    *countOut = 1;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetDescriptor(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    int                descriptor)
{
    assert(interrupt->Count == 1 && index == 0 && descriptor == EVENT_FD);
    g_table.HandleResource = (uuid_t)descriptor;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetHandler(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    InterruptHandler_t handler)
{
    assert(interrupt->Count == 1 && index == 0);
    g_table.Handler = handler;
    return OS_EOK;
}

oserr_t
DeviceInterruptAddIoResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    DeviceIo_t*        ioSpace)
{
    assert(interrupt->Count == 1 && index == 0 && ioSpace->Id == WINDOW_ID);
    g_table.IoResources[0] = ioSpace;
    return OS_EOK;
}

oserr_t
DeviceInterruptAddMemoryResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    uintptr_t          address,
    _In_    size_t             length,
    _In_    unsigned int       flags)
{
    assert(interrupt->Count == 1 && index == 0 && length == sizeof(Pl011InterruptShared_t) && flags == 0);
    g_table.MemoryResources[0].Address = address;
    g_table.MemoryResources[0].Length = length;
    return OS_EOK;
}

oserr_t
DeviceInterruptEnable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    assert(interrupt->Count == 1 && g_table.Handler != NULL);
    __Record(EventInterruptEnable, 0, 0);
    if (g_failInterruptEnable) {
        g_failInterruptEnable--;
        return OS_EDEVFAULT;
    }
    g_interruptEnabled = true;
    return OS_EOK;
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    __Record(EventInterruptDestroy, 0, 0);
    if (g_failInterruptDestroys) {
        g_failInterruptDestroys--;
        return OS_EDEVFAULT;
    }
    interrupt->Count = 0;
    g_interruptEnabled = false;
    memset(&g_table, 0, sizeof(g_table));
    return OS_EOK;
}

int
eventd(
    _In_ unsigned int initialValue,
    _In_ unsigned int flags)
{
    assert(initialValue == 0 && flags == EVT_RESET_EVENT);
    g_eventCounter = 0;
    return EVENT_FD;
}

int
Pl011TestIoctl(
    _In_ int           iod,
    _In_ unsigned long request,
    ...)
{
    va_list arguments;
    int*    value;

    assert(iod == EVENT_FD);
    va_start(arguments, request);
    value = va_arg(arguments, int*);
    va_end(arguments);
    if (request == FIONREAD) {
        *value = g_eventCounter;
    } else {
        assert(request == FIONBIO && *value == 1);
    }
    return 0;
}

int
Pl011TestRead(
    _In_ int          fd,
    _In_ void*        buffer,
    _In_ unsigned int length)
{
    assert(fd == EVENT_FD && length == sizeof(unsigned int) && g_eventCounter > 0);
    *(unsigned int*)buffer = (unsigned int)g_eventCounter;
    g_eventCounter = 0;
    return (int)length;
}

int
Pl011TestClose(
    _In_ int fd)
{
    assert(fd == EVENT_FD);
    __Record(EventClose, 0, 0);
    return 0;
}

int
ioset_ctrl(
    _In_ int                 setIod,
    _In_ int                 op,
    _In_ int                 iod,
    _In_ struct ioset_event* event)
{
    assert(setIod == AIO_HANDLE && iod == EVENT_FD && event->data.iod == EVENT_FD && event->events == IOSETSYN);
    __Record(op == IOSET_ADD ? EventIosetAdd : EventIosetDelete, 0, 0);
    return 0;
}

gracht_server_t*
__crt_get_module_server(void)
{
    return (gracht_server_t*)&g_serverMarker;
}

gracht_handle_t
gracht_server_get_aio_handle(
    _In_ gracht_server_t* server)
{
    assert(server == (gracht_server_t*)&g_serverMarker);
    return AIO_HANDLE;
}

int
gracht_server_register_protocol(
    _In_ gracht_server_t*   server,
    _In_ gracht_protocol_t* protocol)
{
    assert(server == (gracht_server_t*)&g_serverMarker && protocol == &ctt_serial_server_protocol);
    __Record(EventProtocol, 0, 0);
    g_protocolRegistrations++;
    return 0;
}

// One thread: the lock only checks that it is never taken twice or left
// held.
void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    (void)mutex;
    assert(type == USCHED_MUTEX_PLAIN);
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    assert(!g_locked);
    g_locked = true;
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    assert(g_locked);
    g_locked = false;
}

// The port list of libds takes its own lock around every change. One thread
// never contends for it.
void
spinlock_init(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

// The answers and events of the serial contract.
static void
__Reply(
    _In_ oserr_t status)
{
    g_reply.Count++;
    g_reply.Status = status;
}

int ctt_serial_get_line_response(struct gracht_message* message, oserr_t result, struct ctt_serial_line* line)
{
    (void)message;
    __Reply(result);
    g_reply.Line = *line;
    return 0;
}

int ctt_serial_set_line_response(struct gracht_message* message, oserr_t result)
{
    (void)message;
    __Reply(result);
    return 0;
}

int ctt_serial_write_response(struct gracht_message* message, oserr_t result, uint32_t written)
{
    (void)message;
    __Reply(result);
    g_reply.Written = written;
    return 0;
}

int ctt_serial_read_response(struct gracht_message* message, oserr_t result, uint8_t* data, uint32_t data_count)
{
    (void)message;
    __Reply(result);
    assert(data_count <= sizeof(g_reply.Data));
    memcpy(g_reply.Data, data, data_count);
    g_reply.DataCount = data_count;
    return 0;
}

int ctt_serial_get_counters_response(struct gracht_message* message, oserr_t result,
                                     struct ctt_serial_counters* counters)
{
    (void)message;
    __Reply(result);
    g_reply.Counters = *counters;
    return 0;
}

int ctt_serial_subscribe_port_response(struct gracht_message* message, oserr_t result)
{
    (void)message;
    __Reply(result);
    return 0;
}

int ctt_serial_unsubscribe_port_response(struct gracht_message* message, oserr_t result)
{
    (void)message;
    __Reply(result);
    return 0;
}

int ctt_serial_event_data_received_single(gracht_server_t* server, const gracht_conn_t client,
                                          const uuid_t deviceId, const uint32_t available)
{
    assert(server == (gracht_server_t*)&g_serverMarker && deviceId == DEVICE_ID);
    __Record(EventDataReceived, (uint32_t)client, available);
    return (g_unreachableClients & (1U << client)) ? -1 : 0;
}

int ctt_serial_event_transmit_ready_single(gracht_server_t* server, const gracht_conn_t client,
                                           const uuid_t deviceId)
{
    assert(server == (gracht_server_t*)&g_serverMarker && deviceId == DEVICE_ID);
    __Record(EventTransmitReady, (uint32_t)client, 0);
    return (g_unreachableClients & (1U << client)) ? -1 : 0;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static struct gracht_message*
__Message(
    _In_ gracht_conn_t client)
{
    static struct gracht_message message;

    memset(&message, 0, sizeof(message));
    message.client = client;
    return &message;
}

// The kernel's side of an interrupt: run the fast handler with the function
// table, and raise the event counter when it signals.
static size_t
__FastRead(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    assert(io->Id == WINDOW_ID && length == 4);
    return __ModelRead((uint32_t)offset);
}

static oserr_t
__FastWrite(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    assert(io->Id == WINDOW_ID && length == 4);
    __Record(EventFastWrite, (uint32_t)offset, (uint32_t)value);
    __ModelWrite((uint32_t)offset, (uint32_t)value);
    return OS_EOK;
}

static oserr_t
__FastSignal(
    _In_ uuid_t handle)
{
    assert(handle == (uuid_t)EVENT_FD);
    __Record(EventSignal, 0, 0);
    if (g_eventCounter != 0) {
        return OS_EOK;
    }
    g_eventCounter = 1;
    return OS_EINCOMPLETE;
}

static InterruptFunctionTable_t g_kernel = {
    .ReadIoSpace = __FastRead,
    .WriteIoSpace = __FastWrite,
    .EventSignal = __FastSignal
};

// Runs the fast handler if the port raises its line, and then the module's
// event handling, as the module's main loop would.
static irqstatus_t
__Interrupt(void)
{
    irqstatus_t status;

    assert(g_interruptEnabled && g_table.Handler != NULL);
    status = g_table.Handler(&g_kernel, &g_table);
    if (status == IRQSTATUS_HANDLED) {
        assert(Pl011PortsHandleEvent(EVENT_FD) == OS_EOK);
    }
    return status;
}

static void
__ResetCase(void)
{
    __ResetModel();
    memset(g_events, 0, sizeof(g_events));
    g_eventCount = 0;
    g_mapped = false;
    g_windowLength = WINDOW_LENGTH;
    g_windowCount = 1;
    g_failStage = (enum DeviceStage)NO_FAILURE;
    g_clockStatus = OS_EOK;
    g_failWriteOffset = NO_FAILURE;
    g_failWriteOnceOffset = NO_FAILURE;
    g_stallArrivals = 0;
    g_failInterruptDestroys = 0;
    g_failInterruptEnable = 0;
    g_stalledUs = 0;
    g_unreachableClients = 0;
    memset(&g_reply, 0, sizeof(g_reply));
}

static Pl011Hardware_t
__Hardware(
    _In_ DeviceIo_t* window)
{
    Pl011Hardware_t hardware = { .Registers = window, .ClockRate = CLOCK_HZ };

    memset(window, 0, sizeof(*window));
    window->Id = WINDOW_ID;
    window->Type = DeviceIoMemoryBased;
    window->Access.Memory.Length = WINDOW_LENGTH;
    g_mapped = true;
    return hardware;
}

// ---------------------------------------------------------------------------
// Hardware layer
// ---------------------------------------------------------------------------

static void
__TestDivisor(void)
{
    uint32_t integer = 0xdead;
    uint32_t fraction = 0xbeef;

    assert(Pl011Divisor(CLOCK_HZ, 115200, &integer, &fraction) == OS_EOK);
    assert(integer == IBRD_115200 && fraction == FBRD_115200);

    // 48 MHz at 115200: 1666.67 64ths rounds up to 1667, which is 26 3/64.
    // Cutting the fraction off would give 26 2/64.
    assert(Pl011Divisor(48000000ULL, 115200, &integer, &fraction) == OS_EOK);
    assert(integer == 26 && fraction == 3);

    // The fastest rate is clock / 16, a divisor of exactly 1.
    assert(Pl011Divisor(CLOCK_HZ, 3125000, &integer, &fraction) == OS_EOK);
    assert(integer == 1 && fraction == 0);

    // Too fast and too slow for the clock, and zero. Outputs stay unchanged.
    integer = 0xdead;
    fraction = 0xbeef;
    assert(Pl011Divisor(CLOCK_HZ, 3200000, &integer, &fraction) == OS_ENOTSUPPORTED);
    assert(Pl011Divisor(CLOCK_HZ, 47, &integer, &fraction) == OS_ENOTSUPPORTED);
    assert(Pl011Divisor(CLOCK_HZ, 0, &integer, &fraction) == OS_EINVALPARAMS);
    assert(integer == 0xdead && fraction == 0xbeef);

    // The largest whole part is allowed only without a fraction: 65535 * 16
    // * 64 / 4 Hz is exactly divisor 65535 at 64 baud.
    assert(Pl011Divisor(65535ULL * 16U * 64U, 64, &integer, &fraction) == OS_EOK);
    assert(integer == PL011_IBRD_MAX && fraction == 0);
    assert(Pl011Divisor(65535ULL * 16U * 64U + 16U * 64U / 2U, 64, &integer, &fraction) == OS_ENOTSUPPORTED);
    printf("PL011 divisor: passed\n");
}

static void
__TestLineControl(void)
{
    Pl011Line_t line;
    uint32_t    value = 0x1234;

    Pl011LineDefault(&line);
    assert(line.BaudRate == 115200 && line.DataBits == 8 && line.Parity == PL011_PARITY_NONE);
    assert(line.StopBits == 1 && line.FlowControl == PL011_FLOW_CONTROL_NONE);
    assert(Pl011LineControl(&line, &value) == OS_EOK && value == LCR_H_8N1);

    line.DataBits = 7;
    line.Parity = PL011_PARITY_EVEN;
    line.StopBits = 2;
    assert(Pl011LineControl(&line, &value) == OS_EOK);
    assert(value == (PL011_LCR_H_FEN | (2U << 5) | PL011_LCR_H_PEN | PL011_LCR_H_EPS | PL011_LCR_H_STP2));

    line.DataBits = 5;
    line.Parity = PL011_PARITY_ODD;
    line.StopBits = 1;
    assert(Pl011LineControl(&line, &value) == OS_EOK);
    assert(value == (PL011_LCR_H_FEN | PL011_LCR_H_PEN));

    value = 0x1234;
    line.DataBits = 4;
    assert(Pl011LineControl(&line, &value) == OS_EINVALPARAMS);
    line.DataBits = 9;
    assert(Pl011LineControl(&line, &value) == OS_EINVALPARAMS);
    line.DataBits = 8;
    line.StopBits = 3;
    assert(Pl011LineControl(&line, &value) == OS_EINVALPARAMS);
    line.StopBits = 1;
    line.Parity = (enum Pl011Parity)7;
    assert(Pl011LineControl(&line, &value) == OS_EINVALPARAMS);
    line.Parity = PL011_PARITY_NONE;
    line.FlowControl = PL011_FLOW_CONTROL_RTS_CTS;
    assert(Pl011LineControl(&line, &value) == OS_ENOTSUPPORTED);
    line.FlowControl = (enum Pl011FlowControl)9;
    assert(Pl011LineControl(&line, &value) == OS_EINVALPARAMS);
    assert(value == 0x1234);
    printf("PL011 line control: passed\n");
}

static void
__TestStart(void)
{
    DeviceIo_t      window;
    Pl011Hardware_t hardware;
    Pl011Line_t     line;
    uint32_t        mask;
    uint32_t        off;
    uint32_t        lcrFlush;
    uint32_t        ibrd;
    uint32_t        fbrd;
    uint32_t        lcr;
    uint32_t        enable;

    __ResetCase();
    hardware = __Hardware(&window);
    Pl011LineDefault(&line);
    assert(Pl011HardwareStart(&hardware, &line) == OS_EOK);

    // Mask, turn off and clear, then program the line with the FIFOs
    // emptied, the divisor before the final LCR_H, then turn on and unmask.
    mask = __Find(0, EventWrite, PL011_IMSC, 0);
    off = __Find(mask, EventWrite, PL011_CR, 0);
    assert(mask == 0 && off != UINT32_MAX);
    assert(__Find(off, EventWrite, PL011_ICR, PL011_ICR_ALL) != UINT32_MAX);
    lcrFlush = __Find(off, EventWrite, PL011_LCR_H, LCR_H_8N1 & ~PL011_LCR_H_FEN);
    ibrd = __Find(lcrFlush, EventWrite, PL011_IBRD, IBRD_115200);
    fbrd = __Find(lcrFlush, EventWrite, PL011_FBRD, FBRD_115200);
    lcr = __Find(lcrFlush + 1, EventWrite, PL011_LCR_H, LCR_H_8N1);
    enable = __Find(off + 1, EventWrite, PL011_CR, CR_RUNNING);
    assert(lcrFlush != UINT32_MAX && ibrd != UINT32_MAX && fbrd != UINT32_MAX && lcr != UINT32_MAX);
    assert(ibrd < lcr && fbrd < lcr && lcr < enable && enable != UINT32_MAX);
    assert(g_uart.Ifls == (PL011_IFLS_HALF | (PL011_IFLS_HALF << PL011_IFLS_RX_SHIFT)));
    assert(g_uart.Imsc == (PL011_INT_RX | PL011_INT_RT) && hardware.Mask == g_uart.Imsc);
    assert(g_events[g_eventCount - 1].Offset == PL011_IMSC);
    assert(hardware.Line.BaudRate == 115200);

    // Settings the clock cannot reach write nothing.
    __ResetCase();
    hardware = __Hardware(&window);
    line.BaudRate = 4000000;
    assert(Pl011HardwareStart(&hardware, &line) == OS_ENOTSUPPORTED);
    assert(g_eventCount == 0);

    // A failed write stops the start.
    __ResetCase();
    hardware = __Hardware(&window);
    Pl011LineDefault(&line);
    g_failWriteOffset = PL011_IBRD;
    assert(Pl011HardwareStart(&hardware, &line) == OS_EDEVFAULT);
    assert(__Find(0, EventWrite, PL011_CR, CR_RUNNING) == UINT32_MAX);
    printf("PL011 start: passed\n");
}

static void
__TestStop(void)
{
    DeviceIo_t      window;
    Pl011Hardware_t hardware;

    // Every write is tried even when the first one fails, and the first
    // error is the result.
    __ResetCase();
    hardware = __Hardware(&window);
    g_failWriteOffset = PL011_IMSC;
    assert(Pl011HardwareStop(&hardware) == OS_EDEVFAULT);
    assert(__Find(0, EventWrite, PL011_CR, 0) != UINT32_MAX);
    assert(__Find(0, EventWrite, PL011_ICR, PL011_ICR_ALL) != UINT32_MAX);
    assert(g_uart.Cr == 0);
    printf("PL011 stop: passed\n");
}

static void
__TestReceive(void)
{
    static Pl011Buffer_t buffer;
    DeviceIo_t           window;
    Pl011Hardware_t      hardware;
    uint8_t              data[8];

    __ResetCase();
    hardware = __Hardware(&window);
    memset(&buffer, 0, sizeof(buffer));
    __Arrive('A');
    __Arrive('B' | PL011_DR_FE);
    __Arrive('C' | PL011_DR_PE);
    __Arrive(0 | PL011_DR_BE | PL011_DR_FE);
    __Arrive('D' | PL011_DR_OE);
    assert(Pl011HardwareReceive(&hardware, &buffer) == 4);
    assert(g_uart.RxCount == 0);
    assert(Pl011BufferRead(&buffer, data, sizeof(data)) == 4 && !memcmp(data, "ABCD", 4));

    // The break is counted on its own: its framing bit is part of the break.
    assert(hardware.Counters.Received == 4 && hardware.Counters.Breaks == 1);
    assert(hardware.Counters.FramingErrors == 1 && hardware.Counters.ParityErrors == 1);
    assert(hardware.Counters.Overruns == 1 && hardware.Counters.Dropped == 0);

    // A full queue drops bytes but still empties the FIFO.
    for (uint32_t i = 0; i < PL011_BUFFER_SIZE - 1U; i++) {
        assert(Pl011BufferPush(&buffer, 'x') == OS_EOK);
    }
    __Arrive('1');
    __Arrive('2');
    __Arrive('3');
    assert(Pl011HardwareReceive(&hardware, &buffer) == 1);
    assert(g_uart.RxCount == 0 && hardware.Counters.Dropped == 2 && hardware.Counters.Received == 5);
    assert(Pl011BufferSpace(&buffer) == 0 && Pl011BufferPush(&buffer, 'y') == OS_EBUFFER);
    printf("PL011 receive: passed\n");
}

static void
__TestTransmit(void)
{
    static Pl011Buffer_t buffer;
    DeviceIo_t           window;
    Pl011Hardware_t      hardware;
    Pl011Line_t          line;
    uint8_t              data[100];

    __ResetCase();
    hardware = __Hardware(&window);
    Pl011LineDefault(&line);
    assert(Pl011HardwareStart(&hardware, &line) == OS_EOK);
    memset(&buffer, 0, sizeof(buffer));
    for (uint32_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7U);
    }
    assert(Pl011BufferWrite(&buffer, data, sizeof(data)) == sizeof(data));

    // The FIFO fills, and the transmit cause is wanted while bytes are left.
    assert(Pl011HardwareTransmit(&hardware, &buffer) == OS_EOK);
    assert(g_uart.TxCount == FIFO_DEPTH && Pl011BufferCount(&buffer) == sizeof(data) - FIFO_DEPTH);
    assert(hardware.Mask & PL011_INT_TX);
    while (Pl011BufferCount(&buffer) != 0) {
        __DrainWire();
        assert(Pl011HardwareTransmit(&hardware, &buffer) == OS_EOK);
    }
    assert(!(hardware.Mask & PL011_INT_TX));
    __DrainWire();
    assert(g_uart.WireCount == sizeof(data) && !memcmp(g_uart.Wire, data, sizeof(data)));
    assert(hardware.Counters.Sent == sizeof(data));

    // A failed write keeps the byte in the queue.
    assert(Pl011BufferWrite(&buffer, data, 3) == 3);
    g_failWriteOffset = PL011_DR;
    assert(Pl011HardwareTransmit(&hardware, &buffer) == OS_EDEVFAULT);
    assert(Pl011BufferCount(&buffer) == 3);
    printf("PL011 transmit: passed\n");
}

static void
__TestSetLine(void)
{
    static Pl011Buffer_t receive;
    DeviceIo_t           window;
    Pl011Hardware_t      hardware;
    Pl011Line_t          line;
    Pl011Line_t          wanted;
    uint32_t             first;
    uint32_t             off;
    uint8_t              data[4];

    __ResetCase();
    hardware = __Hardware(&window);
    Pl011LineDefault(&line);
    assert(Pl011HardwareStart(&hardware, &line) == OS_EOK);
    memset(&receive, 0, sizeof(receive));

    // Settings the port cannot use write nothing and keep the old ones.
    first = g_eventCount;
    wanted = line;
    wanted.DataBits = 9;
    assert(Pl011HardwareSetLine(&hardware, &wanted, &receive) == OS_EINVALPARAMS);
    wanted = line;
    wanted.BaudRate = 10;
    assert(Pl011HardwareSetLine(&hardware, &wanted, &receive) == OS_ENOTSUPPORTED);
    assert(g_eventCount == first && hardware.Line.BaudRate == 115200);

    // A transmit FIFO that never empties gives a timeout, with the port
    // left running and the old settings in place.
    // Bytes keep arriving during the wait, more than the FIFO holds, and
    // none may be lost.
    g_uart.Tx[0] = 'z';
    g_uart.TxCount = 1;
    g_uart.TxStuck = true;
    g_stallArrivals = 2U * FIFO_DEPTH;
    wanted = line;
    wanted.BaudRate = 9600;
    assert(Pl011HardwareSetLine(&hardware, &wanted, &receive) == OS_ETIMEOUT);
    assert(g_eventCount == first && g_uart.Cr == CR_RUNNING && hardware.Line.BaudRate == 115200);
    assert(g_stallArrivals == 0 && Pl011BufferCount(&receive) + g_uart.RxCount == 2U * FIFO_DEPTH);
    assert(hardware.Counters.Dropped == 0);
    while (Pl011BufferCount(&receive) != 0) {
        Pl011BufferDrop(&receive);
    }
    g_uart.RxCount = 0;
    // At 115200 baud, 33 bytes of 12 bits take 3.4 ms. Twice that plus the
    // margin is the time allowed.
    assert(g_stalledUs >= 7000 && g_stalledUs <= 8000);

    // A change with bytes in both FIFOs: the transmit FIFO is sent first, the
    // receive FIFO is taken before the port turns off, then the new line.
    g_uart.TxStuck = false;
    g_stalledUs = 0;
    __Arrive('h');
    __Arrive('i');
    wanted.DataBits = 7;
    wanted.Parity = PL011_PARITY_EVEN;
    assert(Pl011HardwareSetLine(&hardware, &wanted, &receive) == OS_EOK);
    assert(g_uart.WireCount == 1 && g_uart.Wire[0] == 'z');
    assert(Pl011BufferRead(&receive, data, sizeof(data)) == 2 && !memcmp(data, "hi", 2));
    off = __Find(first, EventWrite, PL011_CR, 0);
    assert(off != UINT32_MAX && __Find(off, EventWrite, PL011_CR, CR_RUNNING) != UINT32_MAX);
    assert(g_uart.Ibrd == 325 && g_uart.Fbrd == 33);
    assert(g_uart.LcrH == (PL011_LCR_H_FEN | (2U << 5) | PL011_LCR_H_PEN | PL011_LCR_H_EPS));
    assert(hardware.Line.BaudRate == 9600 && hardware.Line.DataBits == 7);

    // A write that fails with the port off puts the old settings back and
    // turns the port on again.
    wanted.BaudRate = 115200;
    g_failWriteOnceOffset = PL011_FBRD;
    assert(Pl011HardwareSetLine(&hardware, &wanted, &receive) == OS_EDEVFAULT);
    assert(g_uart.Cr == CR_RUNNING && g_uart.Ibrd == 325 && g_uart.Fbrd == 33);
    assert(g_uart.LcrH == (PL011_LCR_H_FEN | (2U << 5) | PL011_LCR_H_PEN | PL011_LCR_H_EPS));
    assert(hardware.Line.BaudRate == 9600);
    printf("PL011 line change: passed\n");
}

// ---------------------------------------------------------------------------
// Fast handler, port setup and teardown
// ---------------------------------------------------------------------------

static Pl011Port_t*
__Register(void)
{
    Pl011Port_t* port;

    Pl011PortRegister(DEVICE_ID);
    assert(!g_locked);
    port = Pl011PortLock(DEVICE_ID);
    Pl011PortUnlock();
    return port;
}

static void
__Unregister(void)
{
    assert(Pl011PortUnregister(DEVICE_ID) == OS_EOK);
    assert(!g_mapped && !g_interruptEnabled && !g_locked);
}

static void
__TestSetup(void)
{
    Pl011Port_t* port;
    uint32_t     prepare;
    uint32_t     clock;
    uint32_t     enable;
    uint32_t     start;
    uint32_t     running;
    uint32_t     destroy;
    uint32_t     stop;

    __ResetCase();
    port = __Register();
    assert(port != NULL && port->State == PL011_PORT_READY);

    // Prepare comes before any register access, and the interrupt is only
    // enabled with the port masked and off.
    prepare = __Find(0, EventPrepare, 0, 0);
    clock = __Find(0, EventClockRate, 0, 0);
    enable = __Find(0, EventInterruptEnable, 0, 0);
    start = __Find(0, EventStart, 0, 0);
    running = __Find(0, EventWrite, PL011_CR, CR_RUNNING);
    assert(prepare == 0 && __Find(0, EventWrite, ANY, ANY) > clock);
    assert(__Find(0, EventWrite, PL011_IMSC, 0) < enable && __Find(0, EventWrite, PL011_CR, 0) < enable);
    assert(enable < start && start < running && running < __Find(0, EventProtocol, 0, 0));
    assert(__Find(0, EventIosetAdd, 0, 0) < __Find(0, EventInterruptAllocate, 0, 0));
    assert(port->Hardware.ClockRate == CLOCK_HZ && g_uart.Ibrd == IBRD_115200);

    // Teardown: off, interrupt gone, DeviceStop, unmap, in that order.
    g_eventCount = 0;
    __Unregister();
    destroy = __Find(0, EventInterruptDestroy, 0, 0);
    stop = __Find(0, EventStop, 0, 0);
    assert(__Find(0, EventWrite, PL011_IMSC, 0) < destroy && __Find(0, EventWrite, PL011_CR, 0) < destroy);
    assert(destroy < __Find(0, EventIosetDelete, 0, 0) && __Find(0, EventIosetDelete, 0, 0) < stop);
    assert(__Find(0, EventClose, 0, 0) < stop && stop < __Find(0, EventRelease, 0, 0));
    assert(Pl011PortUnregister(DEVICE_ID) == OS_ENOENT);
    assert(Pl011PortLock(DEVICE_ID) == NULL);
    Pl011PortUnlock();

    // The serial protocol is registered with the first port only.
    __ResetCase();
    assert(__Register() != NULL);
    assert(__Count(EventProtocol, ANY) == 0 && g_protocolRegistrations == 1);

    // A second registration of the same device changes nothing.
    g_eventCount = 0;
    Pl011PortRegister(DEVICE_ID);
    assert(__Count(EventPrepare, ANY) == 0);
    __Unregister();
    printf("PL011 setup and teardown: passed\n");
}

static void
__TestSetupFailures(void)
{
    // A failed prepare is followed by one DeviceStop, so deviced can finish
    // undoing its part. The port is freed even when that stop fails.
    __ResetCase();
    g_failStage = DevicePrepare;
    assert(__Register() == NULL);
    assert(__Count(EventStop, ANY) == 1 && __Count(EventAcquire, ANY) == 0);
    assert(Pl011PortUnregister(DEVICE_ID) == OS_ENOENT);

    // A short window is refused before it is mapped, and the prepare undone.
    __ResetCase();
    g_windowLength = PL011_WINDOW_MINIMUM_LENGTH - 4U;
    assert(__Register() == NULL);
    assert(__Count(EventAcquire, ANY) == 0 && __Count(EventStop, ANY) == 1);
    __ResetCase();
    g_windowCount = 0;
    assert(__Register() == NULL);
    assert(__Count(EventAcquire, ANY) == 0 && __Count(EventStop, ANY) == 1);

    // Without a clock rate the mapped port is turned off, stopped and unmapped.
    __ResetCase();
    g_clockStatus = OS_ENOTSUPPORTED;
    assert(__Register() == NULL);
    assert(__Find(0, EventWrite, PL011_CR, 0) < __Find(0, EventStop, 0, 0));
    assert(__Count(EventRelease, ANY) == 1 && !g_mapped);
    assert(Pl011PortUnregister(DEVICE_ID) == OS_ENOENT);

    // A failed interrupt enable tears the partial interrupt down before
    // DeviceStop.
    __ResetCase();
    g_failInterruptEnable = 1;
    assert(__Register() == NULL);
    assert(__Find(0, EventInterruptDestroy, 0, 0) < __Find(0, EventStop, 0, 0));
    assert(__Count(EventClose, ANY) == 1 && !g_mapped);

    // A failed DeviceStart turns the port off again.
    __ResetCase();
    g_failStage = DeviceStart;
    assert(__Register() == NULL);
    assert(__Count(EventStop, ANY) == 1 && !g_mapped && !g_interruptEnabled);
    printf("PL011 setup failures: passed\n");
}

static void
__TestTeardownRetry(void)
{
    // An interrupt that cannot be removed keeps the port listed, mapped and
    // not serving. The next attempt goes on from the interrupt removal,
    // without turning the port off twice, and never stops the device first.
    __ResetCase();
    assert(__Register() != NULL);
    g_eventCount = 0;
    g_failInterruptDestroys = 1;
    assert(Pl011PortUnregister(DEVICE_ID) == OS_EDEVFAULT);
    assert(g_mapped && g_interruptEnabled && __Count(EventStop, ANY) == 0);
    assert(Pl011PortLock(DEVICE_ID) == NULL);
    Pl011PortUnlock();
    g_eventCount = 0;
    __Unregister();
    assert(__Count(EventWrite, PL011_CR) == 0 && __Count(EventInterruptDestroy, ANY) == 1);

    // A failed DeviceStop is tried again on the next attempt, and the
    // interrupt is not destroyed twice.
    __ResetCase();
    assert(__Register() != NULL);
    g_failStage = DeviceStop;
    assert(Pl011PortUnregister(DEVICE_ID) == OS_EDEVFAULT);
    g_failStage = (enum DeviceStage)NO_FAILURE;
    g_eventCount = 0;
    __Unregister();
    assert(__Count(EventInterruptDestroy, ANY) == 0 && __Count(EventStop, ANY) == 1);

    // Unload makes one attempt per port.
    __ResetCase();
    assert(__Register() != NULL);
    Pl011PortsReleaseAll();
    assert(!g_mapped && Pl011PortUnregister(DEVICE_ID) == OS_ENOENT);
    printf("PL011 teardown retry: passed\n");
}

static void
__TestFastInterrupt(void)
{
    Pl011InterruptShared_t* shared;

    __ResetCase();
    assert(__Register() != NULL);
    shared = (Pl011InterruptShared_t*)g_table.MemoryResources[0].Address;

    // Nothing active: not ours, and nothing written.
    g_eventCount = 0;
    assert(g_table.Handler(&g_kernel, &g_table) == IRQSTATUS_NOT_HANDLED);
    assert(g_eventCount == 0);

    // Bytes waiting with the timeout: every cause is masked, the causes are
    // written down, and the event is signaled.
    __Arrive('q');
    g_uart.RxTimeout = true;
    assert(g_table.Handler(&g_kernel, &g_table) == IRQSTATUS_HANDLED);
    assert(g_uart.Imsc == 0 && __Find(0, EventFastWrite, PL011_IMSC, 0) == 0);
    assert(atomic_load(&shared->Causes) == PL011_INT_RT && g_eventCounter == 1);
    assert(__Find(0, EventSignal, 0, 0) != UINT32_MAX);

    // The module takes the counter and the causes, empties the FIFO and
    // turns the receive causes back on.
    assert(Pl011PortsHandleEvent(EVENT_FD) == OS_EOK);
    assert(g_eventCounter == 0 && atomic_load(&shared->Causes) == 0);
    assert(g_uart.RxCount == 0 && g_uart.Imsc == (PL011_INT_RX | PL011_INT_RT));
    assert(Pl011PortsHandleEvent(EVENT_FD + 1) == OS_ENOENT);
    __Unregister();

    // An event that arrives after the interrupt is gone is not ours.
    assert(Pl011PortsHandleEvent(EVENT_FD) == OS_ENOENT);
    printf("PL011 fast interrupt: passed\n");
}

// ---------------------------------------------------------------------------
// Serial contract
// ---------------------------------------------------------------------------

static void
__TestWriteAndTransmitReady(void)
{
    static uint8_t data[CTT_SERIAL_LIMIT_TRANSFER_BYTES + 1];
    Pl011Port_t*   port;
    uint32_t       sent = 0;

    __ResetCase();
    port = __Register();
    for (uint32_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i + 1U);
    }
    ctt_serial_subscribe_port_invocation(__Message(3), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && port->SubscriberCount == 1);

    // A write starts sending at once: the FIFO is full and the transmit
    // cause is on for the rest.
    ctt_serial_write_invocation(__Message(3), DEVICE_ID, data, 100);
    assert(g_reply.Status == OS_EOK && g_reply.Written == 100);
    assert(g_uart.TxCount == FIFO_DEPTH && (g_uart.Imsc & PL011_INT_TX));
    while (Pl011BufferCount(&port->Transmit) != 0) {
        __DrainWire();
        assert(__Interrupt() == IRQSTATUS_HANDLED);
    }
    __DrainWire();
    assert(g_uart.WireCount == 100 && !memcmp(g_uart.Wire, data, 100));
    assert(!(g_uart.Imsc & PL011_INT_TX));
    // Every byte fit, so no transmit_ready was due.
    assert(__Count(EventTransmitReady, ANY) == 0);

    // A write larger than the free space takes what fits, and the client
    // hears once the queue has been sent.
    g_uart.TxStuck = true;
    for (uint32_t i = 0; i < 4; i++) {
        ctt_serial_write_invocation(__Message(3), DEVICE_ID, data, CTT_SERIAL_LIMIT_TRANSFER_BYTES);
        sent += g_reply.Written;
    }
    ctt_serial_write_invocation(__Message(3), DEVICE_ID, data, 64);
    assert(g_reply.Status == OS_EOK && g_reply.Written == PL011_BUFFER_SIZE + FIFO_DEPTH - sent);
    assert(port->TransmitNotice == PL011_TRANSMIT_NOTICE_WANTED);
    g_uart.TxStuck = false;
    while (Pl011BufferCount(&port->Transmit) != 0) {
        assert(__Count(EventTransmitReady, ANY) == 0);
        __DrainWire();
        assert(__Interrupt() == IRQSTATUS_HANDLED);
    }
    assert(__Count(EventTransmitReady, 3) == 1 && port->TransmitNotice == PL011_TRANSMIT_NOTICE_NONE);

    // Too many bytes in one call, and an unknown device.
    g_reply.Written = 77;
    ctt_serial_write_invocation(__Message(3), DEVICE_ID, data, sizeof(data));
    assert(g_reply.Status == OS_EINVALPARAMS && g_reply.Written == 0);
    ctt_serial_write_invocation(__Message(3), OTHER_DEVICE, data, 1);
    assert(g_reply.Status == OS_ENOENT && !g_locked);
    __Unregister();
    printf("PL011 write and transmit_ready: passed\n");
}

static void
__TestReadAndDataReceived(void)
{
    Pl011Port_t* port;

    __ResetCase();
    port = __Register();

    // Bytes that arrive before anyone listens are announced on subscribe.
    __Arrive('a');
    g_uart.RxTimeout = true;
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(__Count(EventDataReceived, ANY) == 0);
    ctt_serial_subscribe_port_invocation(__Message(4), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && __Find(0, EventDataReceived, 4, 1) != UINT32_MAX);

    // More bytes before a read: no new event.
    __Arrive('b');
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(__Count(EventDataReceived, ANY) == 1);

    // A read that leaves bytes behind does not make the next arrival news.
    ctt_serial_read_invocation(__Message(4), DEVICE_ID, 1);
    assert(g_reply.Status == OS_EOK && g_reply.DataCount == 1 && g_reply.Data[0] == 'a');
    __Arrive('c');
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(__Count(EventDataReceived, ANY) == 1);

    // A read that empties the queue does.
    ctt_serial_read_invocation(__Message(4), DEVICE_ID, 100);
    assert(g_reply.DataCount == 2 && !memcmp(g_reply.Data, "bc", 2));
    assert(port->ReceiveNotice == PL011_RECEIVE_NOTICE_DUE);
    __Arrive('d');
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(__Count(EventDataReceived, ANY) == 2);

    // A read is capped at the transfer limit, and an empty queue gives no
    // bytes.
    ctt_serial_read_invocation(__Message(4), DEVICE_ID, 100000);
    assert(g_reply.Status == OS_EOK && g_reply.DataCount == 1);
    ctt_serial_read_invocation(__Message(4), DEVICE_ID, 10);
    assert(g_reply.Status == OS_EOK && g_reply.DataCount == 0);
    ctt_serial_read_invocation(__Message(4), OTHER_DEVICE, 10);
    assert(g_reply.Status == OS_ENOENT);

    // The counters reach the client.
    ctt_serial_get_counters_invocation(__Message(4), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && g_reply.Counters.received_bytes == 4);
    __Unregister();
    printf("PL011 read and data_received: passed\n");
}

static void
__TestSubscribers(void)
{
    Pl011Port_t* port;

    __ResetCase();
    port = __Register();
    for (gracht_conn_t client = 1; client <= CTT_SERIAL_LIMIT_SUBSCRIBERS; client++) {
        ctt_serial_subscribe_port_invocation(__Message(client), DEVICE_ID);
        assert(g_reply.Status == OS_EOK);
    }
    ctt_serial_subscribe_port_invocation(__Message(2), DEVICE_ID);
    assert(g_reply.Status == OS_EEXISTS);
    ctt_serial_subscribe_port_invocation(__Message(9), DEVICE_ID);
    assert(g_reply.Status == OS_EBUSY);

    // A client that cannot be reached is dropped, and the others still hear
    // about the bytes in the same pass.
    g_unreachableClients = 1U << 2;
    __Arrive('z');
    g_uart.RxTimeout = true;
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(port->SubscriberCount == CTT_SERIAL_LIMIT_SUBSCRIBERS - 1U);
    for (gracht_conn_t client = 1; client <= CTT_SERIAL_LIMIT_SUBSCRIBERS; client++) {
        assert(__Count(EventDataReceived, (uint32_t)client) == 1);
    }
    ctt_serial_unsubscribe_port_invocation(__Message(2), DEVICE_ID);
    assert(g_reply.Status == OS_ENOENT);
    ctt_serial_unsubscribe_port_invocation(__Message(1), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && port->SubscriberCount == CTT_SERIAL_LIMIT_SUBSCRIBERS - 2U);
    ctt_serial_subscribe_port_invocation(__Message(9), DEVICE_ID);
    assert(g_reply.Status == OS_EOK);
    __Unregister();

    // When every subscriber is gone the notice stays due, and the next
    // client hears about the waiting bytes.
    __ResetCase();
    port = __Register();
    g_unreachableClients = 1U << 5;
    ctt_serial_subscribe_port_invocation(__Message(5), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && port->SubscriberCount == 1);
    __Arrive('k');
    g_uart.RxTimeout = true;
    assert(__Interrupt() == IRQSTATUS_HANDLED);
    assert(port->SubscriberCount == 0 && port->ReceiveNotice == PL011_RECEIVE_NOTICE_DUE);
    ctt_serial_subscribe_port_invocation(__Message(6), DEVICE_ID);
    assert(__Find(0, EventDataReceived, 6, 1) != UINT32_MAX);

    // A client that subscribes after the others were told hears about the
    // waiting bytes once. The others are not told again.
    ctt_serial_unsubscribe_port_invocation(__Message(6), DEVICE_ID);
    ctt_serial_subscribe_port_invocation(__Message(7), DEVICE_ID);
    assert(__Count(EventDataReceived, 7) == 1 && __Count(EventDataReceived, 6) == 1);
    ctt_serial_subscribe_port_invocation(__Message(8), DEVICE_ID);
    assert(__Count(EventDataReceived, 8) == 1 && __Count(EventDataReceived, 7) == 1);
    __Unregister();
    printf("PL011 subscribers: passed\n");
}

static void
__TestLineRequests(void)
{
    struct ctt_serial_line line = {
        .baud_rate = 9600,
        .data_bits = 7,
        .parity = CTT_SERIAL_PARITY_ODD,
        .stop_bits = CTT_SERIAL_STOP_BITS_TWO,
        .flow_control = CTT_SERIAL_FLOW_CONTROL_NONE
    };

    __ResetCase();
    assert(__Register() != NULL);
    ctt_serial_get_line_invocation(__Message(1), DEVICE_ID);
    assert(g_reply.Status == OS_EOK && g_reply.Line.baud_rate == 115200 && g_reply.Line.data_bits == 8);
    assert(g_reply.Line.parity == CTT_SERIAL_PARITY_NONE && g_reply.Line.stop_bits == CTT_SERIAL_STOP_BITS_ONE);

    ctt_serial_set_line_invocation(__Message(1), DEVICE_ID, &line);
    assert(g_reply.Status == OS_EOK);
    assert(g_uart.LcrH == (PL011_LCR_H_FEN | (2U << 5) | PL011_LCR_H_PEN | PL011_LCR_H_STP2));
    ctt_serial_get_line_invocation(__Message(1), DEVICE_ID);
    assert(g_reply.Line.baud_rate == 9600 && g_reply.Line.data_bits == 7);
    assert(g_reply.Line.parity == CTT_SERIAL_PARITY_ODD && g_reply.Line.stop_bits == CTT_SERIAL_STOP_BITS_TWO);

    // Flow control is refused, and so are values the contract does not
    // define. The settings in use stay.
    line.flow_control = CTT_SERIAL_FLOW_CONTROL_RTS_CTS;
    ctt_serial_set_line_invocation(__Message(1), DEVICE_ID, &line);
    assert(g_reply.Status == OS_ENOTSUPPORTED);
    line.flow_control = CTT_SERIAL_FLOW_CONTROL_NONE;
    line.parity = (enum ctt_serial_parity)5;
    ctt_serial_set_line_invocation(__Message(1), DEVICE_ID, &line);
    assert(g_reply.Status == OS_EINVALPARAMS);
    line.parity = CTT_SERIAL_PARITY_NONE;
    line.stop_bits = (enum ctt_serial_stop_bits)3;
    ctt_serial_set_line_invocation(__Message(1), DEVICE_ID, &line);
    assert(g_reply.Status == OS_EINVALPARAMS);
    ctt_serial_get_line_invocation(__Message(1), DEVICE_ID);
    assert(g_reply.Line.baud_rate == 9600 && g_reply.Line.parity == CTT_SERIAL_PARITY_ODD);
    ctt_serial_get_line_invocation(__Message(1), OTHER_DEVICE);
    assert(g_reply.Status == OS_ENOENT && !g_locked);
    __Unregister();
    printf("PL011 line requests: passed\n");
}

int
main(void)
{
    Pl011PortsInitialize();
    __TestDivisor();
    __TestLineControl();
    __TestStart();
    __TestStop();
    __TestReceive();
    __TestTransmit();
    __TestSetLine();
    __TestSetup();
    __TestSetupFailures();
    __TestTeardownRetry();
    __TestFastInterrupt();
    __TestWriteAndTransmitReady();
    __TestReadAndDataReceived();
    __TestSubscribers();
    __TestLineRequests();
    printf("PL011 driver: all cases passed\n");
    return 0;
}
