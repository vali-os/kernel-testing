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
 * Fake host for the SDHCI engine tests (sdhci_controller_test.c and
 * sdhci_request_test.c). The real engine sources are linked against fakes of
 * every outside call they make:
 *
 * - A register file that models the reset bits, the internal clock
 *   stable bit, the registers a full reset clears, write-one-to-clear
 *   interrupt status, the status enable mask (a write that clears an enable
 *   bit also clears that status bit, as QEMU's host does) and the CMD and
 *   DAT line reset. Tests that latch a change through a script latch it only
 *   while its status enable bit is set. The model also accepts
 *   the 32-bit word forms a BCM2711 host uses (clock, timeout and reset at
 *   0x2C, Auto CMD status and host control 2 at 0x3C). With
 *   g_wordAccessOnly set, any 8 or 16-bit access fails the test. The file
 *   is larger than the standard map, so a variant can use registers above
 *   0x100 in a larger window. They are plain storage.
 * - A second register file for a variant's config window, selected when a
 *   test sets g_configWindowId to the ID of a listed BAR. It takes 32-bit
 *   accesses only, and its accesses are recorded as EventConfigRead and
 *   EventConfigWrite.
 * - DeviceSetStage, DeviceGetResources, DeviceIoAcquire/DeviceIoRelease and
 *   DeviceIoctlEx for a PCI bus device.
 * - DMAOpen, DMAAllocate, the DMABuffer accessors, DMAFree and DMAClose.
 * - The DeviceInterrupt calls, which keep the resource table the kernel
 *   would give the fast handler.
 * - The usched lock, condition and job calls, for one thread.
 * - The engine's timer functions, over a fake clock that only moves when the
 *   engine waits or a test moves it.
 *
 * Every call and every register access is recorded in one event list, in
 * order, with offset, width and value. The test files include this file and
 * check results and that order.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <ddk/devices/bus.h>
#include <ddk/dma.h>
#include <ddk/interrupts/interrupt.h>
#include <ddk/devices/resources.h>
#include <ddk/time.h>
#include <ddk/utils.h>
#include <os/usched/cond.h>
#include <os/usched/job.h>
#include <os/usched/mutex.h>
#include <os/usched/usched.h>

#include "sdhci/registers.h"
#include "sdhci/sdhci.h"
#include "sdhci/variants/standard.h"
#include "sdhci/pci.h"

#define MAX_EVENTS     65536U
#define REGISTER_COUNT 0x300U
#define CONFIG_REGISTER_COUNT 0x200U
#define PAGE_SIZE      4096U
#define FOREVER        UINT32_MAX
#define ANY_VALUE      UINT32_MAX
#define DEVICE_ID      7U
#define BAR_ID_BASE    100U

// Base clock field values for the capability register (in MHz).
#define CAPS_BASE(mhz) ((uint32_t)(mhz) << SDHCI_CAPS_BASE_CLOCK_SHIFT)

enum EventKind {
    EventRead,
    EventWrite,
    EventPrepare,
    EventStart,
    EventStop,
    EventResources,
    EventIoctl,
    EventAcquire,
    EventRelease,
    EventDmaOpen,
    EventDmaAllocate,
    EventDmaFree,
    EventDmaClose,
    EventCardPower,
    // Register accesses made by the fast interrupt handler through its
    // function table, and the event it signals.
    EventFastRead,
    EventFastWrite,
    EventSignal,
    // DeviceInterrupt calls. Offset holds the InterruptCall value.
    EventInterrupt,
    // libddk calls of a platform host, faked by sdhci_platform_test.c. Offset
    // names the supply (1 for vmmc, 2 for vqmmc), Value holds the argument.
    EventSdConfiguration,
    EventSupplyManage,
    EventSupplyState,
    EventSupplyVoltage,
    EventClockRate,
    // 32-bit accesses to the config window. Offset is relative to it.
    EventConfigRead,
    EventConfigWrite
};

// DeviceInterrupt functions, for EventInterrupt and failure injection.
enum InterruptCall {
    InterruptCallNone,
    InterruptCallInitialize,
    InterruptCallAllocate,
    InterruptCallDescriptor,
    InterruptCallHandler,
    InterruptCallIo,
    InterruptCallMemory,
    InterruptCallEnable,
    InterruptCallDestroy,
    // Only a platform device selects a firmware source.
    InterruptCallSelectSource
};

struct Event {
    enum EventKind Kind;
    uint32_t       Offset;
    uint32_t       Width;
    uint32_t       Value;
};

// Points where setup can be made to fail.
enum FailPoint {
    FailNone,
    FailPrepare,
    FailResources,
    FailIoctl,
    FailAcquire,
    FailResetTimeout,
    FailDmaOpen,
    FailDmaOpenCleanup,
    FailAllocateTable,
    FailAllocateControl,
    FailTableSegments,
    FailPointCount,
    // DeviceIoAcquire of the config window fails. Only a variant with a
    // config window reaches it, so it is left out of the points counted by
    // FailPointCount, which the generic setup cases walk.
    FailAcquireConfig
};

struct DMAContext {
    int Open;
};

struct DMABuffer {
    uint8_t           Data[PAGE_SIZE];
    size_t            Size;
    struct DMASegment Segments[2];
    uint32_t          SegmentCount;
};

static struct Event      g_events[MAX_EVENTS];
static uint32_t          g_eventCount;
static uint8_t           g_registers[REGISTER_COUNT];
static char              g_log[65536];
static size_t            g_logLength;
// Lines logged at each OSSYSLOGLEVEL_* level since __Reset.
static uint32_t          g_logLevelLines[OSSYSLOGLEVEL_ERROR + 1];
static uint64_t          g_nowUs;

// Hardware model.
static uint32_t          g_resetBusyReads;
static uint32_t          g_resetBusyLeft;
static uint32_t          g_clockStableReads;
static uint32_t          g_clockStableLeft;
// CMD and DAT line reset: reads that still show the bits set (FOREVER keeps
// them set), and a transfer that stays active after the reset.
static uint32_t          g_lineResetBusyReads;
static uint32_t          g_lineResetLeft;
static bool              g_transferStuck;
// Set for a variant that may only use 32-bit register accesses.
static bool              g_wordAccessOnly;
// Config window model. g_configWindowId is UUID_INVALID unless a test lists
// a BAR as the config window.
static uint8_t           g_configRegisters[CONFIG_REGISTER_COUNT];
static uuid_t            g_configWindowId;
static bool              g_configAcquired;

// Hooks a test can set to script the hardware. g_onRead runs before every
// register read of the engine, g_onWrite after every register write, and
// g_onLineReset when a CMD and DAT line reset finishes.
static void            (*g_onRead)(uint32_t offset);
static void            (*g_onWrite)(uint32_t offset, uint32_t width, uint32_t value);
static void            (*g_onLineReset)(void);
// Runs after every busy wait, for example to finish a fast handler call that
// a test left running.
static void            (*g_onStall)(void);
static uint32_t          g_stalls;
// Status the next busy wait returns, after g_onStall has run. It goes back to
// OS_EOK after that one busy wait, so later busy waits succeed.
static oserr_t           g_failNextStall;

// Interrupt and scheduler model.
static enum InterruptCall g_failInterruptCall;
static InterruptResourceTable_t g_handlerResources;
static bool              g_interruptEnabled;
static uint32_t          g_interruptDestroys;
static uint32_t          g_signals;
static int               g_lockDepth;
static uint32_t          g_notifies;
static usched_task_fn    g_jobEntry;
static void*             g_jobArgument;
static uint32_t          g_joins;
static void            (*g_onConditionWait)(bool timed);
// Counts condition waits with a time limit.
static uint32_t          g_timedWaits;
// When set, the next usched_deadline_after_us fails as if the UTC clock could
// not be read. It is cleared by that call, so later calls succeed.
static bool              g_failNextDeadline;

// Device model.
static BusDevice_t       g_device;
static uint8_t           g_slotInformation;
static bool              g_barListed[6];
static bool              g_prepared;
static bool              g_started;
static bool              g_acquired;
static uuid_t            g_acquiredId;

// DMA model.
static struct DMAContext g_contexts[4];
static uint32_t          g_contextCount;
static uint32_t          g_liveContexts;
static uint32_t          g_liveBuffers;
static uint64_t          g_openLimit;
static uint64_t          g_nextAddress;
static struct DMARequirements g_allocations[4];
static uint32_t          g_allocationCount;

// Scripted failures.
static enum FailPoint    g_failPoint;
static uint32_t          g_failFrees;
static uint32_t          g_failCloses;
static uint32_t          g_failReleases;
static uint32_t          g_failStops;
static uint32_t          g_failStarts;

// The variant and slot __Prepare uses: copies of the standard variant and the
// PCI slot, so a case can change single hooks or flags.
static SdhciVariant_t g_variant;
static SdhciSlot_t    g_slot;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;
    int     written;

    if ((int)level >= 0 && level <= OSSYSLOGLEVEL_ERROR) {
        g_logLevelLines[level]++;
    }
    va_start(arguments, format);
    written = vsnprintf(g_log + g_logLength, sizeof(g_log) - g_logLength, format, arguments);
    va_end(arguments);
    if (written > 0 && g_logLength + (size_t)written + 1 < sizeof(g_log)) {
        g_logLength += (size_t)written;
        g_log[g_logLength++] = '\n';
        g_log[g_logLength] = '\0';
    }
}

static void
__Record(
    _In_ enum EventKind kind,
    _In_ uint32_t       offset,
    _In_ uint32_t       width,
    _In_ uint32_t       value)
{
    assert(g_eventCount < MAX_EVENTS);
    g_events[g_eventCount++] = (struct Event) { kind, offset, width, value };
}

// The engine reads only the monotonic clock. The fake clock is that clock.
uint64_t
DDKGetTimeUs(
    _In_ enum OSTimeSource source)
{
    assert(source == OSTimeSource_MONOTONIC);
    return g_nowUs;
}

oserr_t
DDKStallUs(
    _In_ uint32_t microseconds)
{
    oserr_t status;

    g_nowUs += microseconds;
    g_stalls++;
    if (g_onStall != NULL) {
        g_onStall();
    }
    status = g_failNextStall;
    g_failNextStall = OS_EOK;
    return status;
}

// ---------------------------------------------------------------------------
// Register file
// ---------------------------------------------------------------------------

static uint32_t
__LoadRegister(
    _In_ uint32_t offset,
    _In_ uint32_t width)
{
    uint32_t value = 0;
    uint32_t i;

    for (i = 0; i < width; i++) {
        value |= (uint32_t)g_registers[offset + i] << (8U * i);
    }
    return value;
}

static void
__StoreRegister(
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    uint32_t i;

    for (i = 0; i < width; i++) {
        g_registers[offset + i] = (uint8_t)(value >> (8U * i));
    }
}

// What a full reset clears: host control 1, power, block gap, wakeup, clock,
// timeout, the interrupt registers and host control 2.
static void
__HardwareReset(void)
{
    memset(&g_registers[SDHCI_HOST_CONTROL1], 0, SDHCI_SOFTWARE_RESET - SDHCI_HOST_CONTROL1);
    memset(&g_registers[SDHCI_INTERRUPT_STATUS], 0, 12);
    __StoreRegister(SDHCI_HOST_CONTROL2, 2, 0);
}

static void
__CheckAccess(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    // The engine may touch registers only through the acquired window, and
    // the standard variant only at the natural alignment of each width.
    assert(g_prepared);
    assert(g_acquired);
    assert(ioSpace->Id == g_acquiredId);
    assert(length == 1 || length == 2 || length == 4);
    assert(!g_wordAccessOnly || length == 4);
    assert(offset % length == 0);
    assert(offset + length <= REGISTER_COUNT);
    assert(offset + length <= ioSpace->Access.Memory.Length);
}

static bool
__IsConfigWindow(
    _In_ const DeviceIo_t* ioSpace)
{
    return g_configWindowId != UUID_INVALID && ioSpace->Id == g_configWindowId;
}

static void
__CheckConfigAccess(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    // The config window takes aligned 32-bit accesses inside its length.
    assert(g_prepared);
    assert(g_configAcquired);
    assert(length == 4);
    assert(offset % 4 == 0);
    assert(offset + length <= CONFIG_REGISTER_COUNT);
    assert(offset + length <= ioSpace->Access.Memory.Length);
}

static uint32_t
__LoadConfig(
    _In_ uint32_t offset)
{
    uint32_t value;

    memcpy(&value, &g_configRegisters[offset], sizeof(value));
    return value;
}

static void
__StoreConfig(
    _In_ uint32_t offset,
    _In_ uint32_t value)
{
    memcpy(&g_configRegisters[offset], &value, sizeof(value));
}

#define __LINE_RESETS (SDHCI_RESET_COMMAND | SDHCI_RESET_DATA)

// A finished CMD and DAT line reset ends every transfer, unless the test
// keeps one stuck.
static uint32_t
__ReadLineReset(
    _In_ uint32_t value)
{
    if (g_lineResetLeft == FOREVER) {
        return value;
    }
    if (g_lineResetLeft > 0) {
        g_lineResetLeft--;
        return value;
    }
    value &= ~__LINE_RESETS;
    g_registers[SDHCI_SOFTWARE_RESET] = (uint8_t)value;
    if (!g_transferStuck) {
        __StoreRegister(SDHCI_PRESENT_STATE, 4, __LoadRegister(SDHCI_PRESENT_STATE, 4) &
            ~(SDHCI_PRESENT_COMMAND_INHIBIT | SDHCI_PRESENT_DATA_INHIBIT | SDHCI_PRESENT_DATA_ACTIVE |
              SDHCI_PRESENT_READ_ACTIVE | SDHCI_PRESENT_WRITE_ACTIVE));
    }
    if (g_onLineReset != NULL) {
        g_onLineReset();
    }
    return value;
}

// Interrupt status is write-one-to-clear in both its 16-bit halves and as
// one 32-bit register.
static bool
__StoreStatusWrite(
    _In_ uint32_t offset,
    _In_ uint32_t width,
    _In_ uint32_t value)
{
    if (offset < SDHCI_INTERRUPT_STATUS || offset >= SDHCI_INTERRUPT_ENABLE) {
        return false;
    }
    __StoreRegister(offset, width, __LoadRegister(offset, width) & ~value);
    return true;
}

static uint32_t
__ReadModel(
    _In_ uint32_t offset,
    _In_ uint32_t length)
{
    uint32_t value;

    // A 32-bit read of the word at 0x2C reads clock control, timeout control
    // and software reset one by one, so the reset and clock model below
    // behaves the same for a host that only reads whole words.
    if (offset == SDHCI_CLOCK_CONTROL && length == 4) {
        return __ReadModel(SDHCI_CLOCK_CONTROL, 2) |
            (__ReadModel(SDHCI_TIMEOUT_CONTROL, 1) << 16) |
            (__ReadModel(SDHCI_SOFTWARE_RESET, 1) << 24);
    }

    value = __LoadRegister(offset, length);
    if (offset == SDHCI_SOFTWARE_RESET && length == 1 && (value & __LINE_RESETS) != 0) {
        value = __ReadLineReset(value);
    }
    if (offset == SDHCI_SOFTWARE_RESET && length == 1 && (value & SDHCI_RESET_ALL) != 0) {
        if (g_resetBusyLeft == 0) {
            g_registers[SDHCI_SOFTWARE_RESET] = 0;
            value = 0;
        } else if (g_resetBusyLeft != FOREVER) {
            g_resetBusyLeft--;
        }
    }
    if (offset == SDHCI_CLOCK_CONTROL && length == 2 && (value & SDHCI_CLOCK_INTERNAL_ENABLE) != 0) {
        if (g_clockStableLeft == 0) {
            value |= SDHCI_CLOCK_INTERNAL_STABLE;
        } else if (g_clockStableLeft != FOREVER) {
            g_clockStableLeft--;
        }
    }
    return value;
}

size_t
DeviceIoRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    uint32_t value;

    if (__IsConfigWindow(ioSpace)) {
        __CheckConfigAccess(ioSpace, offset, length);
        value = __LoadConfig((uint32_t)offset);
        __Record(EventConfigRead, (uint32_t)offset, 4, value);
        return value;
    }

    __CheckAccess(ioSpace, offset, length);
    if (g_onRead != NULL) {
        g_onRead((uint32_t)offset);
    }
    value = __ReadModel((uint32_t)offset, (uint32_t)length);
    __Record(EventRead, (uint32_t)offset, (uint32_t)length, value);
    return value;
}

static void
__WriteSoftwareReset(
    _In_ uint32_t value)
{
    if ((value & SDHCI_RESET_ALL) != 0) {
        __HardwareReset();
        g_resetBusyLeft = g_resetBusyReads;
    }
    if ((value & __LINE_RESETS) != 0) {
        g_lineResetLeft = g_lineResetBusyReads;
    }
    g_registers[SDHCI_SOFTWARE_RESET] = (uint8_t)value;
}

// A 32-bit write of the word at 0x2C. Writing clock control with the value
// it already has does not restart the internal clock, which matters because
// a word-only host rewrites it with every timeout or reset write. The reset
// byte is applied last, so a full reset clears what the same write stored.
static void
__WriteClockWord(
    _In_ uint32_t value)
{
    uint32_t clock = value & 0xFFFFU & ~SDHCI_CLOCK_INTERNAL_STABLE;

    if (clock != __LoadRegister(SDHCI_CLOCK_CONTROL, 2)) {
        __StoreRegister(SDHCI_CLOCK_CONTROL, 2, clock);
        g_clockStableLeft = g_clockStableReads;
    }
    g_registers[SDHCI_TIMEOUT_CONTROL] = (uint8_t)(value >> 16);
    __WriteSoftwareReset(value >> 24);
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    if (__IsConfigWindow(ioSpace)) {
        __CheckConfigAccess(ioSpace, offset, length);
        __Record(EventConfigWrite, (uint32_t)offset, 4, (uint32_t)value);
        __StoreConfig((uint32_t)offset, (uint32_t)value);
        return OS_EOK;
    }

    __CheckAccess(ioSpace, offset, length);
    __Record(EventWrite, (uint32_t)offset, (uint32_t)length, (uint32_t)value);

    // Read-only registers must never be written. Above the capabilities only
    // the ADMA system address is writable, up to the end of the standard
    // map. A 32-bit write at 0x3C carries host control 2 in its upper half,
    // and the host ignores the read-only Auto CMD status in the lower half.
    assert(offset != SDHCI_PRESENT_STATE);
    assert(offset < SDHCI_CAPABILITIES || offset == SDHCI_ADMA_ADDRESS || offset == SDHCI_ADMA_ADDRESS_HIGH ||
        offset >= SDHCI_REGISTER_WINDOW_SIZE);
    if (offset == SDHCI_AUTO_CMD_STATUS) {
        assert(length == 4);
        __StoreRegister(SDHCI_HOST_CONTROL2, 2, (uint32_t)value >> 16);
        return OS_EOK;
    }
    if (offset == SDHCI_CLOCK_CONTROL && length == 4) {
        __WriteClockWord((uint32_t)value);
        return OS_EOK;
    }
    if (offset == SDHCI_SOFTWARE_RESET) {
        assert(length == 1);
        __WriteSoftwareReset((uint32_t)value);
        return OS_EOK;
    }
    if (__StoreStatusWrite((uint32_t)offset, (uint32_t)length, (uint32_t)value)) {
        return OS_EOK;
    }
    if (offset == SDHCI_CLOCK_CONTROL) {
        assert(length == 2);
        __StoreRegister((uint32_t)offset, 2, (uint32_t)value & ~SDHCI_CLOCK_INTERNAL_STABLE);
        g_clockStableLeft = g_clockStableReads;
        return OS_EOK;
    }
    __StoreRegister((uint32_t)offset, (uint32_t)length, (uint32_t)value);

    // A status bit whose enable bit is now clear is masked: the host drops it.
    if (offset >= SDHCI_INTERRUPT_ENABLE && offset < SDHCI_INTERRUPT_SIGNAL) {
        __StoreRegister(SDHCI_INTERRUPT_STATUS, 4,
            __LoadRegister(SDHCI_INTERRUPT_STATUS, 4) & __LoadRegister(SDHCI_INTERRUPT_ENABLE, 4));
    }
    if (g_onWrite != NULL) {
        g_onWrite((uint32_t)offset, (uint32_t)length, (uint32_t)value);
    }
    return OS_EOK;
}

// ---------------------------------------------------------------------------
// Device calls
// ---------------------------------------------------------------------------

oserr_t
DeviceSetStage(
    _In_ uuid_t           device,
    _In_ enum DeviceStage stage)
{
    assert(device == DEVICE_ID);
    if (stage == DevicePrepare) {
        __Record(EventPrepare, 0, 0, 0);
        if (g_failPoint == FailPrepare) {
            return OS_EPERMISSIONS;
        }
        g_prepared = true;
        return OS_EOK;
    }
    if (stage == DeviceStart) {
        __Record(EventStart, 0, 0, 0);
        assert(g_prepared);
        if (g_failStarts > 0) {
            g_failStarts--;
            return OS_EDEVFAULT;
        }
        g_started = true;
        return OS_EOK;
    }

    __Record(EventStop, 0, 0, 0);
    assert(g_prepared);
    if (g_failStops > 0) {
        g_failStops--;
        return OS_EDEVFAULT;
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
    uint32_t bar;

    assert(device == DEVICE_ID);
    __Record(EventResources, 0, 0, 0);
    if (g_failPoint == FailResources) {
        return OS_EPROTOCOL;
    }

    // deviced lists the published BARs packed together, without their index.
    memset(resources, 0, sizeof(*resources));
    for (bar = 0; bar < 6; bar++) {
        if (g_barListed[bar]) {
            resources->Registers[resources->Count++] = g_device.IoSpaces[bar];
        }
    }
    return OS_EOK;
}

oserr_t
DeviceIoctlEx(
    _In_    uuid_t       deviceId,
    _In_    int          direction,
    _In_    unsigned int offset,
    _InOut_ size_t*      value,
    _In_    size_t       width)
{
    assert(deviceId == DEVICE_ID);
    assert(direction == (int)DEVICEMANAGER_IOCTL_EXT_READ);
    __Record(EventIoctl, offset, (uint32_t)width, 0);
    assert(offset == SDHCI_PCI_SLOT_INFO && width == 1);
    if (g_failPoint == FailIoctl) {
        return OS_EPROTOCOL;
    }
    *value = g_slotInformation;
    return OS_EOK;
}

oserr_t
DeviceIoAcquire(
    _In_ DeviceIo_t* ioSpace)
{
    assert(g_prepared);
    __Record(EventAcquire, 0, 0, ioSpace->Id);
    if (__IsConfigWindow(ioSpace)) {
        assert(!g_configAcquired);
        if (g_failPoint == FailAcquireConfig) {
            return OS_EPERMISSIONS;
        }
        g_configAcquired = true;
        return OS_EOK;
    }
    assert(!g_acquired);
    if (g_failPoint == FailAcquire) {
        return OS_EPERMISSIONS;
    }
    g_acquired = true;
    g_acquiredId = ioSpace->Id;
    return OS_EOK;
}

oserr_t
DeviceIoRelease(
    _In_ DeviceIo_t* ioSpace)
{
    if (__IsConfigWindow(ioSpace)) {
        assert(g_configAcquired);
        // The config window goes back before the host window.
        assert(g_acquired);
    } else {
        assert(g_acquired);
        assert(ioSpace->Id == g_acquiredId);
        assert(!g_configAcquired);
    }
    __Record(EventRelease, 0, 0, ioSpace->Id);
    if (g_failReleases > 0) {
        g_failReleases--;
        return OS_EDEVFAULT;
    }
    if (__IsConfigWindow(ioSpace)) {
        g_configAcquired = false;
        return OS_EOK;
    }
    g_acquired = false;
    return OS_EOK;
}

// ---------------------------------------------------------------------------
// DMA calls
// ---------------------------------------------------------------------------

oserr_t
DMAOpen(
    _In_    uuid_t              device,
    _In_    uint64_t            addressLimit,
    _InOut_ struct DMAContext** contextOut)
{
    assert(device == DEVICE_ID);
    assert(*contextOut == NULL);
    assert(g_contextCount < 4);
    __Record(EventDmaOpen, 0, 0, 0);
    g_openLimit = addressLimit;
    if (g_failPoint == FailDmaOpen) {
        return OS_EPERMISSIONS;
    }

    g_contexts[g_contextCount].Open = 1;
    *contextOut = &g_contexts[g_contextCount++];
    g_liveContexts++;
    if (g_failPoint == FailDmaOpenCleanup) {
        // The reply was lost: the caller gets a cleanup-only context.
        return OS_EPROTOCOL;
    }
    return OS_EOK;
}

oserr_t
DMAClose(
    _InOut_ struct DMAContext** context)
{
    __Record(EventDmaClose, 0, 0, 0);
    assert(*context != NULL && (*context)->Open);
    if (g_liveBuffers != 0) {
        return OS_EBUSY;
    }
    if (g_failCloses > 0) {
        g_failCloses--;
        return OS_EDEVFAULT;
    }
    (*context)->Open = 0;
    *context = NULL;
    g_liveContexts--;
    return OS_EOK;
}

oserr_t
DMAAllocate(
    _In_    struct DMAContext*            context,
    _In_    const struct DMARequirements* requirements,
    _InOut_ struct DMABuffer**            bufferOut)
{
    struct DMABuffer* buffer;

    assert(context != NULL && context->Open);
    assert(*bufferOut == NULL);
    assert(g_allocationCount < 4);
    g_allocations[g_allocationCount++] = *requirements;
    __Record(EventDmaAllocate, 0, (uint32_t)requirements->Length, 0);
    if ((g_failPoint == FailAllocateTable && g_allocationCount == 1) ||
        (g_failPoint == FailAllocateControl && g_allocationCount == 2)) {
        return OS_EOOM;
    }

    buffer = calloc(1, sizeof(*buffer));
    assert(buffer != NULL);
    buffer->Size = PAGE_SIZE;
    buffer->SegmentCount = 1;
    buffer->Segments[0] = (struct DMASegment) { g_nextAddress, PAGE_SIZE };
    if (g_failPoint == FailTableSegments && g_allocationCount == 1) {
        buffer->SegmentCount = 2;
        buffer->Segments[0].Length = PAGE_SIZE / 2;
        buffer->Segments[1] = (struct DMASegment) { g_nextAddress + 0x10000, PAGE_SIZE / 2 };
    }
    g_nextAddress += 2 * PAGE_SIZE;
    g_liveBuffers++;
    *bufferOut = buffer;
    return OS_EOK;
}

void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    return (void*)buffer->Data;
}

size_t
DMABufferSize(
    _In_ const struct DMABuffer* buffer)
{
    return buffer->Size;
}

const struct DMASegment*
DMABufferSegments(
    _In_  const struct DMABuffer* buffer,
    _Out_ uint32_t*               countOut)
{
    *countOut = buffer->SegmentCount;
    return buffer->Segments;
}

oserr_t
DMAFree(
    _InOut_ struct DMABuffer** buffer)
{
    __Record(EventDmaFree, 0, 0, 0);
    assert(*buffer != NULL);
    if (g_failFrees > 0) {
        g_failFrees--;
        return OS_EDEVFAULT;
    }
    free(*buffer);
    *buffer = NULL;
    g_liveBuffers--;
    return OS_EOK;
}

// ---------------------------------------------------------------------------
// DeviceInterrupt calls. They keep the resource table the kernel would hand
// to the fast handler, so a test can call the handler directly.
// ---------------------------------------------------------------------------

static oserr_t
__InterruptCall(
    _In_ enum InterruptCall call)
{
    __Record(EventInterrupt, (uint32_t)call, 0, 0);
    return g_failInterruptCall == call ? OS_EDEVFAULT : OS_EOK;
}

void
DeviceInterruptInitialize(
    _Out_ DeviceInterrupt_t* interrupt,
    _In_  Device_t*          device)
{
    // The device is the PCI bus device of these fakes, or a platform device
    // of a test that uses the same device ID. IsPci is set the way libddk
    // sets it, from the bus type of a bus device.
    assert(device->Id == DEVICE_ID);
    memset(interrupt, 0, sizeof(*interrupt));
    interrupt->DeviceId = device->Id;
    if (device->Length == sizeof(BusDevice_t)) {
        interrupt->IsPci = ((const BusDevice_t*)device)->Type == BusTypePci;
    }
    (void)__InterruptCall(InterruptCallInitialize);
}

oserr_t
DeviceInterruptSelectSource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index)
{
    // libddk refuses a source selection for a PCI device.
    assert(!interrupt->IsPci);
    interrupt->SourceIndex = index;
    __Record(EventInterrupt, (uint32_t)InterruptCallSelectSource, 0, index);
    return g_failInterruptCall == InterruptCallSelectSource ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceInterruptAllocate(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           min,
    _In_    uint32_t           optimal,
    _Out_   uint32_t*          countOut)
{
    // One vector on a line that may be shared.
    assert(min == 1 && optimal == 1);
    assert((interrupt->Flags & INTERRUPT_EXCLUSIVE) == 0);
    *countOut = 0;
    if (__InterruptCall(InterruptCallAllocate) != OS_EOK) {
        return OS_EDEVFAULT;
    }
    *countOut = 1;
    interrupt->Count = 1;
    return OS_EOK;
}

oserr_t
DeviceInterruptSetDescriptor(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    int                descriptor)
{
    assert(interrupt->Count == 1 && index == 0);
    g_handlerResources.HandleResource = (uuid_t)descriptor;
    return __InterruptCall(InterruptCallDescriptor);
}

oserr_t
DeviceInterruptSetHandler(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    InterruptHandler_t handler)
{
    assert(interrupt->Count == 1 && index == 0);
    g_handlerResources.Handler = handler;
    return __InterruptCall(InterruptCallHandler);
}

oserr_t
DeviceInterruptAddIoResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    DeviceIo_t*        ioSpace)
{
    assert(interrupt->Count == 1 && index == 0);
    assert(g_handlerResources.IoResources[0] == NULL);
    g_handlerResources.IoResources[0] = ioSpace;
    return __InterruptCall(InterruptCallIo);
}

oserr_t
DeviceInterruptAddMemoryResource(
    _InOut_ DeviceInterrupt_t* interrupt,
    _In_    uint32_t           index,
    _In_    uintptr_t          address,
    _In_    size_t             length,
    _In_    unsigned int       flags)
{
    assert(interrupt->Count == 1 && index == 0);
    assert(g_handlerResources.MemoryResources[0].Address == 0);
    g_handlerResources.MemoryResources[0].Address = address;
    g_handlerResources.MemoryResources[0].Length = length;
    g_handlerResources.MemoryResources[0].Flags = flags;
    return __InterruptCall(InterruptCallMemory);
}

oserr_t
DeviceInterruptEnable(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    assert(interrupt->Count == 1 && g_handlerResources.Handler != NULL);
    if (__InterruptCall(InterruptCallEnable) != OS_EOK) {
        return OS_EDEVFAULT;
    }
    g_interruptEnabled = true;
    return OS_EOK;
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    g_interruptDestroys++;
    if (__InterruptCall(InterruptCallDestroy) != OS_EOK) {
        return OS_EDEVFAULT;
    }
    interrupt->Count = 0;
    g_interruptEnabled = false;
    memset(&g_handlerResources, 0, sizeof(g_handlerResources));
    return OS_EOK;
}

// ---------------------------------------------------------------------------
// Scheduler calls. There is one thread: the lock only checks that it is
// never taken twice or held across a work function, and a wait returns at
// once after moving the fake clock.
// ---------------------------------------------------------------------------

int
usched_deadline_after_us(
    _In_  uint64_t         microseconds,
    _Out_ struct timespec* until)
{
    // The fake UTC clock is the fake monotonic clock.
    uint64_t untilUs = g_nowUs + microseconds;

    if (g_failNextDeadline) {
        g_failNextDeadline = false;
        errno = EIO;
        return -1;
    }

    until->tv_sec = (time_t)(untilUs / 1000000ULL);
    until->tv_nsec = (long)((untilUs % 1000000ULL) * 1000ULL);
    return 0;
}

void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->type = type;
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    assert(g_lockDepth == 0);
    g_lockDepth++;
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
    assert(g_lockDepth == 1);
    g_lockDepth--;
}

void
usched_cnd_init(
    _In_ struct usched_cnd* condition)
{
    memset(condition, 0, sizeof(*condition));
}

void
usched_cnd_notify_one(
    _In_ struct usched_cnd* condition)
{
    (void)condition;
    g_notifies++;
}

void
usched_cnd_wait(
    _In_ struct usched_cnd* condition,
    _In_ struct usched_mtx* mutex)
{
    (void)condition;
    (void)mutex;
    assert(g_lockDepth == 1);
    assert(g_onConditionWait != NULL);
    g_onConditionWait(false);
}

int
usched_cnd_timedwait(
    _In_ struct usched_cnd*             condition,
    _In_ struct usched_mtx*             mutex,
    _In_ const struct timespec *restrict until)
{
    uint64_t untilUs = (uint64_t)until->tv_sec * 1000000ULL + (uint64_t)until->tv_nsec / 1000ULL;

    (void)condition;
    (void)mutex;
    assert(g_lockDepth == 1);
    g_timedWaits++;
    if (g_onConditionWait != NULL) {
        g_onConditionWait(true);
    }
    if (untilUs > g_nowUs) {
        g_nowUs = untilUs;
    }
    return -1;
}

uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void*          argument)
{
    g_jobEntry = entry;
    g_jobArgument = argument;
    return 42;
}

int
usched_job_join(
    _In_  uuid_t jobID,
    _Out_ int*   exitCode)
{
    // The real usched_job_join always stores the exit code and crashes on a
    // NULL pointer, which the first QEMU teardown showed.
    assert(exitCode != NULL);
    assert(jobID == 42);
    *exitCode = 0;
    g_joins++;
    return 0;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static oserr_t
__RecordCardPower(
    _In_ SdhciController_t* controller,
    _In_ enum SdBusPower    power)
{
    (void)controller;
    __Record(EventCardPower, 0, 0, (uint32_t)power);
    return OS_EOK;
}

// Default host: version 3.00, 200 MHz base clock, ADMA2, high speed, 3.3 V,
// 512-byte blocks, removable slot, one slot in BAR 0.
static void
__Reset(void)
{
    uint32_t bar;

    memset(g_events, 0, sizeof(g_events));
    g_eventCount = 0;
    memset(g_registers, 0, sizeof(g_registers));
    g_log[0] = '\0';
    g_logLength = 0;
    memset(g_logLevelLines, 0, sizeof(g_logLevelLines));
    g_nowUs = 1000;
    g_resetBusyReads = 3;
    g_resetBusyLeft = 0;
    g_clockStableReads = 2;
    g_clockStableLeft = 0;
    g_lineResetBusyReads = 2;
    g_lineResetLeft = 0;
    g_transferStuck = false;
    g_wordAccessOnly = false;
    memset(g_configRegisters, 0, sizeof(g_configRegisters));
    g_configWindowId = UUID_INVALID;
    g_configAcquired = false;
    g_onRead = NULL;
    g_onWrite = NULL;
    g_onLineReset = NULL;
    g_onStall = NULL;
    g_stalls = 0;
    g_failNextStall = OS_EOK;

    g_failInterruptCall = InterruptCallNone;
    memset(&g_handlerResources, 0, sizeof(g_handlerResources));
    g_interruptEnabled = false;
    g_interruptDestroys = 0;
    g_signals = 0;
    g_lockDepth = 0;
    g_notifies = 0;
    g_jobEntry = NULL;
    g_jobArgument = NULL;
    g_joins = 0;
    g_onConditionWait = NULL;
    g_timedWaits = 0;
    g_failNextDeadline = false;

    memset(&g_device, 0, sizeof(g_device));
    g_device.Base.Id = DEVICE_ID;
    g_device.Base.Length = sizeof(BusDevice_t);
    g_device.Type = BusTypePci;
    for (bar = 0; bar < 6; bar++) {
        g_device.IoSpaces[bar].Id = UUID_INVALID;
        g_barListed[bar] = false;
    }
    g_slotInformation = 0;
    g_prepared = false;
    g_started = false;
    g_acquired = false;
    g_acquiredId = UUID_INVALID;

    memset(g_contexts, 0, sizeof(g_contexts));
    g_contextCount = 0;
    g_liveContexts = 0;
    g_liveBuffers = 0;
    g_openLimit = 0;
    g_nextAddress = 0x10000000ULL;
    g_allocationCount = 0;

    g_failPoint = FailNone;
    g_failFrees = 0;
    g_failCloses = 0;
    g_failReleases = 0;
    g_failStops = 0;
    g_failStarts = 0;

    g_variant = g_sdhciStandardVariant;
    g_slot = g_sdhciPciSlot;
    g_slot.SetCardPower = __RecordCardPower;
}

static void
__AddBar(
    _In_ uint32_t bar,
    _In_ size_t   length,
    _In_ bool     listed)
{
    g_device.IoSpaces[bar].Id = BAR_ID_BASE + bar;
    g_device.IoSpaces[bar].Type = DeviceIoMemoryBased;
    g_device.IoSpaces[bar].Access.Memory.PhysicalBase = 0xFE000000U + bar * 0x1000U;
    g_device.IoSpaces[bar].Access.Memory.Length = length;
    g_barListed[bar] = listed;
}

static void
__SetHost(
    _In_ uint16_t version,
    _In_ uint32_t capabilities)
{
    __StoreRegister(SDHCI_HOST_VERSION, 2, version);
    __StoreRegister(SDHCI_CAPABILITIES, 4, capabilities);
}

static uint32_t
__DefaultCapabilities(void)
{
    return CAPS_BASE(200) | SDHCI_CAPS_ADMA2 | SDHCI_CAPS_HIGH_SPEED | SDHCI_CAPS_VOLTAGE_330;
}

static void
__DefaultHost(void)
{
    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, __DefaultCapabilities());
}

static int
__Find(
    _In_ enum EventKind kind,
    _In_ uint32_t       offset,
    _In_ uint32_t       value,
    _In_ uint32_t       start)
{
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind != kind) {
            continue;
        }
        if ((kind == EventRead || kind == EventWrite || kind == EventFastRead || kind == EventFastWrite) &&
            g_events[i].Offset != offset) {
            continue;
        }
        if (value != ANY_VALUE && g_events[i].Value != value) {
            continue;
        }
        return (int)i;
    }
    return -1;
}

static uint32_t
__CountWrites(
    _In_ uint32_t offset,
    _In_ uint32_t start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == EventWrite && g_events[i].Offset == offset) {
            count++;
        }
    }
    return count;
}

static void
__Prepare(
    _Out_ SdhciController_t* controller)
{
    memset(controller, 0, sizeof(*controller));
    assert(SdhciControllerPrepare(controller, &g_device.Base, &g_variant, &g_slot) == OS_EOK);
}

static void
__DestroyAll(
    _InOut_ SdhciController_t* controller)
{
    assert(SdhciControllerDestroy(controller) == OS_EOK);
    assert(g_liveBuffers == 0);
    assert(g_liveContexts == 0);
    assert(!g_acquired);
    assert(!g_configAcquired);
    assert(!g_prepared);
}

static SdBusSettings_t
__Settings(
    _In_ uint32_t         clockHz,
    _In_ enum SdBusWidth  width,
    _In_ enum SdBusTiming timing,
    _In_ enum SdBusPower  power)
{
    SdBusSettings_t settings = { clockHz, width, timing, power, SdBusDeviceSd };
    return settings;
}
