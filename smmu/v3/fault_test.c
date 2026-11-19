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
 * Event queue sorting, fault records, overflow and Poll capacity tests.
 * Event record bits are written from section 7.3 of the SMMUv3 specification
 * (IHI 0070 G.b), independently of the production register header.
 */
#include "model.h"
#include <stdio.h>
#include <string.h>

#define __EVT_STALL (1ULL << 31) // Word 1: Stall in events 0x10..0x13, R permission in event 0x05.
#define __EVT_READ  (1ULL << 35) // Word 1: RnW, 1 for a read.
#define __EVT_S2    (1ULL << 39) // Word 1: S2, the fault happened at stage 2.
#define __EVTQ_PROD (0x100A8 / 4)
#define __EVTQ_CONS (0x100AC / 4)
#define __OVERFLOW  (1U << 31)
#define __MAX_QUEUE 256U

typedef struct __DeviceCase {
    uint32_t               Number;
    uint64_t               Word1;
    enum DMAIOMMUFaultKind Kind;
    uint32_t               Flags;
} __DeviceCase_t;

// Every device fault row of the F5 table. Word 2 is always non-zero, so a
// record must only carry the address for event numbers that define one.
static const __DeviceCase_t g_deviceCases[] = {
    { 0x10, __EVT_READ, DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { 0x10, 0, DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { 0x11, __EVT_READ, DMAIOMMUFaultAddressRange, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { 0x13, 0, DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    // Stage 2 permission faults are the normal case for a stage 2 only unit.
    { 0x13, __EVT_READ | __EVT_S2, DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { 0x01, __EVT_READ, DMAIOMMUFaultOther, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    // Bit 31 is the requested read permission here, not Stall. It has no RnW.
    { 0x05, __EVT_STALL, DMAIOMMUFaultOther, DMA_IOMMU_FAULT_ADDRESS },
    { 0x07, 0, DMAIOMMUFaultOther, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { 0x06, 0, DMAIOMMUFaultOther, 0 },
    { 0x08, 0, DMAIOMMUFaultOther, 0 },
    { 0x02, 0, DMAIOMMUFaultNoRoute, 0 }
};

typedef struct __FailureCase {
    uint32_t Number;
    uint64_t Word1;
} __FailureCase_t;

// Every unit failure row of the F5 table, unknown and implementation defined
// numbers, stalled faults and an F_ADDR_SIZE raised by a stage 2 table entry.
// F_ACCESS (0x12) fails the unit with and without Stall, because the kernel
// sets the access flag in every page table entry it writes.
static const __FailureCase_t g_failureCases[] = {
    { 0x03, 0 }, { 0x04, 0 }, { 0x09, 0 }, { 0x0A, 0 }, { 0x0B, 0 },
    { 0x12, 0 }, { 0x20, 0 }, { 0x21, 0 }, { 0x24, 0 }, { 0x25, 0 },
    { 0x00, 0 }, { 0x0C, 0 }, { 0x14, 0 }, { 0xE0, 0 }, { 0xFF, 0 },
    { 0x10, __EVT_STALL }, { 0x11, __EVT_STALL }, { 0x12, __EVT_STALL }, { 0x13, __EVT_STALL },
    { 0x11, __EVT_S2 }
};

static unsigned int g_failureCase;

static void __Inject(uint32_t sid, uint32_t number, uint64_t word1, uint64_t address)
{
    const uint64_t words[4] = { number | ((uint64_t)sid << 32), word1, address, 0 };

    SMMUv3ModelEventRecord(words);
}

static void __AssertHealthy(SMMUv3Unit_t* unit)
{
    assert(DMAIOMMUUnitCheck(unit->Common.Backend) == OS_EOK);
    assert(!unit->Common.CompletionUnreliable);
    assert(g_smmuv3Model.Registers[__EVTQ_CONS] == unit->EventConsumer);
}

static void __AssertFault(const DMAIOMMUFault_t* fault, enum DMAIOMMUFaultKind kind, uint32_t flags,
                          uint32_t sid, uint32_t code, uint64_t address)
{
    assert(fault->Kind == kind && fault->Flags == flags);
    assert(fault->HardwareID == sid && fault->Code == code);
    assert(fault->DeviceAddress == ((flags & DMA_IOMMU_FAULT_ADDRESS) != 0 ? address : 0));
}

// Device faults become records with the decoded fields. Nothing is logged,
// the unit stays healthy and destruction does not pin control memory.
static void __DeviceFaults(void)
{
    const unsigned int count = sizeof(g_deviceCases) / sizeof(g_deviceCases[0]);
    DMAIOMMUFault_t faults[32];
    SMMUv3Unit_t* unit;
    uint32_t written = 77;
    unsigned int i;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    for (i = 0; i < count; i++) {
        __Inject(100 + i, g_deviceCases[i].Number, g_deviceCases[i].Word1, 0x10000 + i * 0x1000);
    }
    assert(SMMUv3UnitPoll(unit, faults, 32, &written) == OS_EOK && written == count);
    for (i = 0; i < count; i++) {
        __AssertFault(&faults[i], g_deviceCases[i].Kind, g_deviceCases[i].Flags, 100 + i,
                      g_deviceCases[i].Number, 0x10000 + i * 0x1000);
    }
    assert(unit->EventConsumer == count && g_lastError[0] == '\0');
    __AssertHealthy(unit);
    assert(SMMUv3UnitPoll(unit, faults, 32, &written) == OS_EOK && written == 0);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
    SMMUv3ModelEnd();
}

// A device fault queued before the failing event is still returned with the
// failure status. The log names the failing event number and Stream ID.
static void __UnitFailure(void)
{
    const __FailureCase_t* entry = &g_failureCases[g_failureCase];
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t written = 77;
    char expected[64];

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    __Inject(5, 0x10, __EVT_READ, 0x3000);
    __Inject(9, entry->Number, entry->Word1, 0x4000);
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    __AssertFault(&faults[0], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ,
                  5, 0x10, 0x3000);
    assert(unit->Common.CompletionUnreliable && unit->EventConsumer == 2);
    assert(snprintf(expected, sizeof(expected), "event %x, SID 9,", entry->Number) > 0);
    assert(strstr(g_firstError, "__FailForEvent: ") == g_firstError && strstr(g_firstError, expected) != NULL);
    written = 77;
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 0);
    g_smmuv3Model.Clock = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_ECANCELLED && unit != NULL && g_unmaps == 0);
}

// An active global error fails the unit. Queued device faults are still read
// and returned so their diagnostics are not lost.
static void __GlobalErrorWithEvent(void)
{
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t written = 77;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    g_smmuv3Model.Registers[0x60 / 4] = 1;
    __Inject(3, 0x13, 0, 0x5000);
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    __AssertFault(&faults[0], DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                  3, 0x13, 0x5000);
    assert(unit->Common.CompletionUnreliable && unit->EventConsumer == 1);
    g_smmuv3Model.Clock = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_ECANCELLED && g_unmaps == 0);
}

// A queue that reports more records than it holds cannot be trusted. Nothing
// is read and the unit fails.
static void __BrokenQueuePointers(void)
{
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t written = 77;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    g_smmuv3Model.Registers[__EVTQ_PROD] = (1U << unit->EventBits) + 1;
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 0);
    assert(unit->Common.CompletionUnreliable && unit->EventConsumer == 0);
    assert(strstr(g_firstError, "SMMUv3FaultDrain: event queue pointers") == g_firstError);
    g_smmuv3Model.Clock = false;
    assert(SMMUv3UnitDestroy(&unit) == OS_ECANCELLED && g_unmaps == 0);
}

// A unit failed through the generic layer keeps reliable completion. Poll
// still returns its records with the failure status.
static void __AlreadyFailedUnit(void)
{
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t written = 77;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    DMAIOMMUUnitFail(unit->Common.Backend, OS_EDEVFAULT);
    __Inject(4, 0x10, 0, 0x6000);
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    __AssertFault(&faults[0], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                  4, 0x10, 0x6000);
    assert(!unit->Common.CompletionUnreliable);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK && unit == NULL);
    SMMUv3ModelEnd();
}

// Records that do not fit stay in the queue and come back in order on the
// next calls. Each call advances EVTQ_CONS by exactly the records it read.
// The fault array is larger than capacity so an overrun fails an assertion.
static void __CapacityLimit(void)
{
    DMAIOMMUFault_t faults[8];
    SMMUv3Unit_t* unit;
    uint32_t written;
    uint32_t sid = 0;
    uint32_t expected[] = { 2, 2, 1, 0 };
    unsigned int i;
    unsigned int j;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    for (i = 0; i < 5; i++) __Inject(i, 0x10, 0, 0x1000 * (i + 1));
    for (i = 0; i < 4; i++) {
        written = 77;
        assert(SMMUv3UnitPoll(unit, faults, 2, &written) == OS_EOK && written == expected[i]);
        for (j = 0; j < written; j++, sid++) {
            __AssertFault(&faults[j], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                          sid, 0x10, 0x1000 * (sid + 1));
        }
        assert(unit->EventConsumer == sid);
        __AssertHealthy(unit);
    }
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

static void __FillQueue(uint32_t first, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) __Inject(first + i, 0x10, 0, 0x1000);
}

// Hardware drops records once the queue is full and toggles OVFLG. Poll
// acknowledges it, returns one ReportsLost record first and keeps the unit
// healthy. A second overflow toggles OVFLG back and is reported again.
static void __Overflow(void)
{
    DMAIOMMUFault_t faults[__MAX_QUEUE + 4];
    SMMUv3Unit_t* unit;
    uint32_t queue;
    uint32_t producer;
    uint32_t written;
    uint32_t i;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    queue = 1U << unit->EventBits;
    assert(queue <= __MAX_QUEUE);
    __FillQueue(0, queue + 3);
    producer = g_smmuv3Model.Registers[__EVTQ_PROD];
    assert(g_smmuv3Model.DroppedEvents == 3 && (producer & __OVERFLOW) != 0);
    written = 77;
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 4);
    __AssertFault(&faults[0], DMAIOMMUFaultReportsLost, 0, 0, producer, 0);
    for (i = 1; i < 4; i++) assert(faults[i].Kind == DMAIOMMUFaultNoMapping && faults[i].HardwareID == i - 1);
    assert(g_smmuv3Model.Registers[__EVTQ_CONS] == (__OVERFLOW | 3));
    __AssertHealthy(unit);

    // The rest of the queue follows without another lost-reports record.
    assert(SMMUv3UnitPoll(unit, faults, __MAX_QUEUE + 4, &written) == OS_EOK && written == queue - 3);
    for (i = 0; i < written; i++) assert(faults[i].Kind == DMAIOMMUFaultNoMapping && faults[i].HardwareID == i + 3);
    assert(SMMUv3UnitPoll(unit, faults, __MAX_QUEUE + 4, &written) == OS_EOK && written == 0);

    __FillQueue(1000, queue + 1);
    producer = g_smmuv3Model.Registers[__EVTQ_PROD];
    assert(g_smmuv3Model.DroppedEvents == 4 && (producer & __OVERFLOW) == 0);
    // Capacity equals the number of queued records. The lost-reports record
    // takes one slot, so the last queued record waits for the next call.
    assert(SMMUv3UnitPoll(unit, faults, queue, &written) == OS_EOK && written == queue);
    __AssertFault(&faults[0], DMAIOMMUFaultReportsLost, 0, 0, producer, 0);
    assert(faults[1].HardwareID == 1000 && faults[queue - 1].HardwareID == 1000 + queue - 2);
    assert(SMMUv3UnitPoll(unit, faults, __MAX_QUEUE + 4, &written) == OS_EOK && written == 1);
    assert(faults[0].Kind == DMAIOMMUFaultNoMapping && faults[0].HardwareID == 1000 + queue - 1);
    __AssertHealthy(unit);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

// With capacity 1 an overflow fills the only slot. The overflow is still
// acknowledged, and no queued record is read until the next call.
static void __OverflowAtCapacity(void)
{
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t queue;
    uint32_t written = 77;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    queue = 1U << unit->EventBits;
    __FillQueue(0, queue + 1);
    assert(SMMUv3UnitPoll(unit, faults, 1, &written) == OS_EOK && written == 1);
    assert(faults[0].Kind == DMAIOMMUFaultReportsLost);
    assert(unit->EventConsumer == __OVERFLOW && g_smmuv3Model.Registers[__EVTQ_CONS] == __OVERFLOW);
    assert(SMMUv3UnitPoll(unit, faults, 1, &written) == OS_EOK && written == 1);
    assert(faults[0].Kind == DMAIOMMUFaultNoMapping && faults[0].HardwareID == 0);
    __AssertHealthy(unit);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

// Invalid arguments read nothing and still clear countOut when it exists.
static void __InvalidArguments(void)
{
    DMAIOMMUFault_t faults[4];
    SMMUv3Unit_t* unit;
    uint32_t written;

    SMMUv3ModelBegin(false);
    unit = SMMUv3ModelStart();
    __Inject(1, 0x10, 0, 0x1000);
    written = 77;
    assert(SMMUv3UnitPoll(NULL, faults, 4, &written) == OS_EINVALPARAMS && written == 0);
    written = 77;
    assert(SMMUv3UnitPoll(unit, NULL, 4, &written) == OS_EINVALPARAMS && written == 0);
    written = 77;
    assert(SMMUv3UnitPoll(unit, faults, 0, &written) == OS_EINVALPARAMS && written == 0);
    assert(SMMUv3UnitPoll(unit, faults, 4, NULL) == OS_EINVALPARAMS);
    assert(unit->EventConsumer == 0);
    assert(SMMUv3UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 1);
    assert(SMMUv3UnitDestroy(&unit) == OS_EOK);
    SMMUv3ModelEnd();
}

void SMMUv3FaultTests(void)
{
    __DeviceFaults();
    for (g_failureCase = 0; g_failureCase < sizeof(g_failureCases) / sizeof(g_failureCases[0]); g_failureCase++) {
        SMMUv3ModelPinned(__UnitFailure);
    }
    SMMUv3ModelPinned(__GlobalErrorWithEvent);
    SMMUv3ModelPinned(__BrokenQueuePointers);
    __AlreadyFailedUnit();
    __CapacityLimit();
    __Overflow();
    __OverflowAtCapacity();
    __InvalidArguments();
}
