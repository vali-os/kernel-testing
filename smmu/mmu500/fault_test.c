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
 * Fault status sorting, fault records and Poll capacity tests.
 * Status bits are written from sections 9.6.15 and 16.5.9 of the SMMU v2
 * specification (IHI 0062D.c), independently of the production register header.
 */
#include "model.h"
#include <stdio.h>
#include <string.h>

#define __FSR_TF     (1U << 1)
#define __FSR_AFF    (1U << 2)
#define __FSR_PF     (1U << 3)
#define __FSR_EF     (1U << 4)
#define __FSR_TLBMCF (1U << 5)
#define __FSR_TLBLKF (1U << 6)
#define __FSR_ASF    (1U << 7)
#define __FSR_UUT    (1U << 8)
#define __FSR_AARCH64 (2U << 9) // FORMAT of a VA64 bank. Present with every recorded fault.
#define __FSR_SS     (1U << 30)
#define __FSR_FAULTS 0xC00001FEU
#define __GFSR_ICF   (1U << 0)
#define __GFSR_USF   (1U << 1)
#define __GFSR_SMCF  (1U << 2)
#define __GFSR_UCBF  (1U << 3)
#define __GFSR_UCIF  (1U << 4)
#define __GFSR_CAF   (1U << 5)
#define __GFSR_EF    (1U << 6)
#define __GFSR_UUT   (1U << 8)
#define __MULTI      (1U << 31)
#define __GFSR       (0x48 / 4)
#define __ABOVE_INPUT (1ULL << 48) // The model unit translates 48-bit input addresses.

typedef struct __DeviceCase {
    bool                   Global;
    uint32_t               Status;
    bool                   Write;
    uint64_t               Address;
    enum DMAIOMMUFaultKind Kind;
    uint32_t               Flags;
} __DeviceCase_t;

// Every device fault row of the F5 MMU-500 table. Each case also has a
// nonzero address, so a record must only carry it where the table says so.
static const __DeviceCase_t g_deviceCases[] = {
    { false, __FSR_TF, false, 0x11000, DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { false, __FSR_TF, true, 0x12000, DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { false, __FSR_PF, true, 0x13000, DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { false, __FSR_PF, false, 0x14000, DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    // An address size fault on an address above the input range was raised
    // by the address the device sent.
    { false, __FSR_ASF, false, __ABOVE_INPUT + 0x1000, DMAIOMMUFaultAddressRange,
      DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { false, __FSR_UUT, true, 0x15000, DMAIOMMUFaultOther, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    // MULTI next to a device fault returns the same record.
    { false, __FSR_TF | __MULTI, false, 0x16000, DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { false, __FSR_PF | __MULTI, true, 0x17000, DMAIOMMUFaultPermission, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { true, __GFSR_USF, true, 0x18000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { true, __GFSR_USF, false, 0x19000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    // GFAR is not defined for an unsupported transaction.
    { true, __GFSR_UUT, true, 0x1A000, DMAIOMMUFaultNoRoute, 0 },
    { true, __GFSR_USF | __MULTI, false, 0x1B000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { true, __GFSR_UUT | __MULTI, false, 0x1C000, DMAIOMMUFaultNoRoute, 0 },
    // An invalid context fault comes from a device that sent DMA while its
    // route was set to fault (FC.4). GFAR and GFSYNR0 describe the access.
    { true, __GFSR_ICF, true, 0x1D000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE },
    { true, __GFSR_ICF, false, 0x1E000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    { true, __GFSR_ICF | __MULTI, false, 0x1F000, DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ },
    // With two global device bits, the record keeps the fields of the one
    // with the most detail.
    { true, __GFSR_ICF | __GFSR_UUT, true, 0x20000, DMAIOMMUFaultNoRoute,
      DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE }
};

typedef struct __FailureCase {
    bool        Global;
    uint32_t    Status;
    uint64_t    Address;
    const char* Reason;
} __FailureCase_t;

#define __KERNEL_CONTEXT "context fault from kernel tables or the unit itself"
#define __KERNEL_GLOBAL  "global fault from kernel routes, unit registers or an external abort"

// Every unit failure row of the F5 MMU-500 table, including a device fault
// bit next to a failure bit, MULTI on its own and an address size fault on an
// address inside the input range.
static const __FailureCase_t g_failureCases[] = {
    { false, __FSR_AFF, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_AFF | __FSR_TF, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_EF, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_EF | __MULTI, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_TLBMCF, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_TLBLKF, 0x1000, __KERNEL_CONTEXT },
    { false, __FSR_SS | __FSR_TF, 0x1000, "stalled transaction" },
    { false, __FSR_SS | __FSR_PF | __MULTI, 0x1000, "stalled transaction" },
    { false, __MULTI, 0x1000, "context faults lost without a recorded fault" },
    { false, __FSR_ASF, 0x1000, "page table entry points outside the output address size" },
    { false, __FSR_ASF | __FSR_TF, 0x1000, "page table entry points outside the output address size" },
    { true, __GFSR_ICF | __GFSR_SMCF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_ICF | __GFSR_EF | __MULTI, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_SMCF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_UCBF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_UCIF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_CAF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_EF, 0x1000, __KERNEL_GLOBAL },
    { true, __GFSR_USF | __GFSR_CAF, 0x1000, __KERNEL_GLOBAL },
    { true, __MULTI, 0x1000, "global faults lost without a recorded fault" }
};

static unsigned int g_failureCase;

static uint32_t __BankStatus(uint32_t bank)
{
    return g_smmu500Model.Registers[(SMMU500ModelBank(bank) + 0x58) / 4];
}

static void __AssertHealthy(SMMU500Unit_t* unit)
{
    assert(DMAIOMMUUnitCheck(unit->Common.Backend) == OS_EOK);
    assert(!unit->Common.CompletionUnreliable);
    assert(g_lastError[0] == '\0');
}

static void __AssertFault(const DMAIOMMUFault_t* fault, enum DMAIOMMUFaultKind kind, uint32_t flags,
                          uint32_t sid, uint32_t code, uint64_t address)
{
    assert(fault->Kind == kind && fault->Flags == flags);
    assert(fault->HardwareID == sid && fault->Code == code);
    assert(fault->DeviceAddress == ((flags & DMA_IOMMU_FAULT_ADDRESS) != 0 ? address : 0));
}

// Device faults become records with the decoded fields. Nothing is logged,
// the unit stays healthy, and exactly the reported status is cleared.
static void __DeviceFaults(void)
{
    const unsigned int count = sizeof(g_deviceCases) / sizeof(g_deviceCases[0]);
    const __DeviceCase_t* entry;
    DMAIOMMUFault_t faults[8];
    SMMU500Unit_t* unit;
    uint32_t written;
    uint32_t status;
    uint32_t bank;
    unsigned int i;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    for (i = 0; i < count; i++) {
        entry = &g_deviceCases[i];
        bank = i % 4;
        status = entry->Global ? entry->Status : entry->Status | __FSR_AARCH64;
        if (entry->Global) SMMU500ModelGlobalFault(status, 100 + i, entry->Address, entry->Write);
        else SMMU500ModelContextFault(bank, status, 100 + i, entry->Address, entry->Write);
        written = 77;
        assert(SMMU500UnitPoll(unit, faults, 8, &written) == OS_EOK && written == 1);
        __AssertFault(&faults[0], entry->Kind, entry->Flags, 100 + i, status, entry->Address);
        assert(g_smmu500Model.Registers[__GFSR] == 0 && (__BankStatus(bank) & __FSR_FAULTS) == 0);
        __AssertHealthy(unit);
    }
    assert(SMMU500UnitPoll(unit, faults, 8, &written) == OS_EOK && written == 0);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK && unit == NULL);
    SMMU500ModelEnd();
}

// A MULTI that arrives between the status read and the clear belongs to the
// fault just reported. It is cleared with it and the unit stays healthy.
static void __MultiAfterRead(void)
{
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit;
    uint32_t written;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    SMMU500ModelGlobalFault(__GFSR_USF, 3, 0x1000, false);
    g_smmu500Model.GlobalRace = __MULTI;
    SMMU500ModelContextFault(1, __FSR_TF | __FSR_AARCH64, 4, 0x2000, false);
    g_smmu500Model.BankRace = __MULTI;
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 2);
    assert(faults[0].Kind == DMAIOMMUFaultNoRoute && faults[0].HardwareID == 3);
    assert(faults[1].Kind == DMAIOMMUFaultNoMapping && faults[1].HardwareID == 4);
    assert(g_smmu500Model.Registers[__GFSR] == 0 && (__BankStatus(1) & __FSR_FAULTS) == 0);
    __AssertHealthy(unit);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// One global record and one record per bank, global first, banks in index order.
static void __SeveralSources(void)
{
    DMAIOMMUFault_t faults[8];
    SMMU500Unit_t* unit;
    uint32_t written;
    uint32_t bank;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    SMMU500ModelGlobalFault(__GFSR_USF, 50, 0x5000, true);
    // A second global fault only sets MULTI and keeps the first one's details.
    SMMU500ModelGlobalFault(__GFSR_UUT, 51, 0x6000, false);
    for (bank = 0; bank < 4; bank++) {
        SMMU500ModelContextFault(bank, __FSR_TF | __FSR_AARCH64, 10 + bank, 0x1000 * (bank + 1), false);
    }
    assert(SMMU500UnitPoll(unit, faults, 8, &written) == OS_EOK && written == 5);
    __AssertFault(&faults[0], DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                  50, __GFSR_USF | __MULTI, 0x5000);
    for (bank = 0; bank < 4; bank++) {
        __AssertFault(&faults[bank + 1], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ,
                      10 + bank, __FSR_TF | __FSR_AARCH64, 0x1000 * (bank + 1));
        assert((__BankStatus(bank) & __FSR_FAULTS) == 0);
    }
    __AssertHealthy(unit);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// Banks past the capacity are not read. Their status stays latched and later
// calls return them in the same order. The fault array is larger than
// capacity and filled with a pattern, so a write past capacity is caught.
static void __CapacityLimit(void)
{
    DMAIOMMUFault_t faults[8];
    SMMU500Unit_t* unit;
    uint32_t written;
    uint32_t bank;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    SMMU500ModelGlobalFault(__GFSR_USF, 50, 0x5000, false);
    for (bank = 0; bank < 4; bank++) {
        SMMU500ModelContextFault(bank, __FSR_PF | __FSR_AARCH64, 10 + bank, 0x1000, true);
    }
    memset(faults, 0xEE, sizeof(faults));
    assert(SMMU500UnitPoll(unit, faults, 2, &written) == OS_EOK && written == 2);
    assert(faults[0].HardwareID == 50 && faults[1].HardwareID == 10);
    assert(faults[2].HardwareID == 0xEEEEEEEEU);
    assert((__BankStatus(0) & __FSR_FAULTS) == 0);
    for (bank = 1; bank < 4; bank++) assert(__BankStatus(bank) == (__FSR_PF | __FSR_AARCH64));
    assert(SMMU500UnitPoll(unit, faults, 2, &written) == OS_EOK && written == 2);
    assert(faults[0].HardwareID == 11 && faults[1].HardwareID == 12);
    assert(__BankStatus(3) == (__FSR_PF | __FSR_AARCH64));
    assert(SMMU500UnitPoll(unit, faults, 2, &written) == OS_EOK && written == 1);
    assert(faults[0].HardwareID == 13 && faults[0].Kind == DMAIOMMUFaultPermission);
    assert(SMMU500UnitPoll(unit, faults, 2, &written) == OS_EOK && written == 0);

    // With capacity 1 the global record fills the call, and no bank is read.
    SMMU500ModelGlobalFault(__GFSR_USF, 60, 0x5000, false);
    SMMU500ModelContextFault(2, __FSR_TF | __FSR_AARCH64, 61, 0x2000, false);
    assert(SMMU500UnitPoll(unit, faults, 1, &written) == OS_EOK && written == 1);
    assert(faults[0].HardwareID == 60 && __BankStatus(2) == (__FSR_TF | __FSR_AARCH64));
    assert(SMMU500UnitPoll(unit, faults, 1, &written) == OS_EOK && written == 1);
    assert(faults[0].HardwareID == 61 && (__BankStatus(2) & __FSR_FAULTS) == 0);
    __AssertHealthy(unit);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// A device fault reported in the same call as a unit failure is still
// returned. The log names the reason, the context bank and the Stream ID.
// The failed status is cleared for the bits that were read. A MULTI that
// arrives after the read stays latched, and SS stays because it is read-only.
static void __UnitFailure(void)
{
    const __FailureCase_t* entry = &g_failureCases[g_failureCase];
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit;
    uint32_t written = 77;
    uint32_t remaining;
    char expected[160];

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    remaining = (entry->Status & __MULTI) != 0 ? 0 : __MULTI;
    if (entry->Global) {
        SMMU500ModelGlobalFault(entry->Status, 9, entry->Address, false);
        g_smmu500Model.GlobalRace = __MULTI;
        SMMU500ModelContextFault(0, __FSR_TF | __FSR_AARCH64, 5, 0x3000, false);
        assert(snprintf(expected, sizeof(expected), "__FailForGlobalFault: %s at 100000, SID 9,", entry->Reason) > 0);
    } else {
        SMMU500ModelGlobalFault(__GFSR_USF, 5, 0x3000, false);
        SMMU500ModelContextFault(0, entry->Status | __FSR_AARCH64, 9, entry->Address, false);
        g_smmu500Model.BankRace = __MULTI;
        assert(snprintf(expected, sizeof(expected), "__FailForContextFault: %s at 100000, bank 0, SID 9,",
                        entry->Reason) > 0);
    }
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    assert(faults[0].HardwareID == 5 && faults[0].DeviceAddress == 0x3000);
    assert(unit->Common.CompletionUnreliable);
    assert(strstr(g_firstError, expected) == g_firstError);
    if (entry->Global) {
        assert(g_smmu500Model.Registers[__GFSR] == remaining);
    } else {
        assert((__BankStatus(0) & __FSR_FAULTS) == ((entry->Status & __FSR_SS) | remaining));
    }
    written = 77;
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 0);
    g_smmu500Model.Clock = false;
    assert(SMMU500UnitDestroy(&unit) == OS_ECANCELLED && unit != NULL && g_unmaps == 0);
}

// Checks run while the kernel changes the unit never clear device faults,
// so no report is lost before Poll reads it.
static void __ChecksKeepDeviceFaults(void)
{
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit;
    SMMU500Space_t* space = NULL;
    ArmSMMUDeadline_t deadline;
    SMMU500Sync_t sync = {.Scope = SMMU500SyncAll};
    uint32_t written;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SpaceCreate(unit, (void**)&space) == OS_EOK);
    assert(ArmSMMUDeadlineBegin(ARM_SMMU_TIMEOUT_NS, &deadline) == OS_EOK);
    assert(SMMU500ContextEnable(space, &deadline) == OS_EOK);
    SMMU500ModelGlobalFault(__GFSR_USF, 7, 0x7000, false);
    SMMU500ModelContextFault(space->Bank, __FSR_TF | __FSR_AARCH64, 8, 0x8000, true);
    assert(SMMU500SyncRun(unit, &sync, &deadline) == OS_EOK);
    assert(SMMU500FaultCheckAll(unit) == OS_EOK);
    assert(SMMU500ContextDisable(space, &deadline) == OS_EOK);
    assert(g_smmu500Model.Registers[__GFSR] == __GFSR_USF);
    assert(__BankStatus(space->Bank) == (__FSR_TF | __FSR_AARCH64));
    SMMU500SpaceDestroy(unit, space);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    __AssertHealthy(unit);
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 2);
    __AssertFault(&faults[0], DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ,
                  7, __GFSR_USF, 0x7000);
    __AssertFault(&faults[1], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                  8, __FSR_TF | __FSR_AARCH64, 0x8000);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// Closing a route writes S2CR to fault before it clears SMR. A device that
// sends DMA between the two writes raises an invalid context fault. The
// checked writes must leave it latched and keep the unit healthy, and Poll
// then returns it as a NoRoute record.
static void __InvalidContextWhileClosingRoute(void)
{
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit;
    uint32_t written;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    SMMU500ModelGlobalFault(__GFSR_ICF, 12, 0x2A000, true);
    DMAIOMMUUnitLock(unit->Common.Backend);
    assert(SMMU500SlotSetFault(unit, 0) == OS_EOK);
    DMAIOMMUUnitUnlock(unit->Common.Backend);
    assert(g_smmu500Model.Registers[__GFSR] == __GFSR_ICF);
    __AssertHealthy(unit);
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 1);
    __AssertFault(&faults[0], DMAIOMMUFaultNoRoute, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE,
                  12, __GFSR_ICF, 0x2A000);
    assert(g_smmu500Model.Registers[__GFSR] == 0);
    __AssertHealthy(unit);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// Device faults left by firmware do not stop startup. The first Poll
// returns them.
static void __FirmwareDeviceFaults(void)
{
    ArmSMMUDescription_t description = SMMU500ModelDescription();
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit = NULL;
    uint32_t written;

    SMMU500ModelBegin(true);
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    SMMU500ModelGlobalFault(__GFSR_UUT, 1, 0, false);
    SMMU500ModelContextFault(3, __FSR_TF | __FSR_AARCH64, 2, 0x9000, false);
    assert(SMMU500UnitStart(unit) == OS_EOK);
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 2);
    assert(faults[0].HardwareID == 1 && faults[1].HardwareID == 2);
    __AssertHealthy(unit);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

// A unit failed through the generic layer keeps reliable completion. Poll
// still returns its records with the failure status.
static void __AlreadyFailedUnit(void)
{
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit;
    uint32_t written = 77;

    SMMU500ModelBegin(true);
    unit = SMMU500ModelStart();
    DMAIOMMUUnitFail(unit->Common.Backend, OS_EDEVFAULT);
    SMMU500ModelContextFault(2, __FSR_TF | __FSR_AARCH64, 4, 0x6000, false);
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    __AssertFault(&faults[0], DMAIOMMUFaultNoMapping, DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_READ,
                  4, __FSR_TF | __FSR_AARCH64, 0x6000);
    assert(!unit->Common.CompletionUnreliable && (__BankStatus(2) & __FSR_FAULTS) == 0);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK && unit == NULL);
    SMMU500ModelEnd();
}

// After a failed start Poll still reads the status. Records written in that
// call come back with the failure status.
static void __PollAfterFailedStart(void)
{
    ArmSMMUDescription_t description = SMMU500ModelDescription();
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit = NULL;
    uint32_t written = 77;

    SMMU500ModelBegin(true);
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    g_smmu500Model.LockACR = true;
    assert(SMMU500UnitStart(unit) == OS_ECANCELLED);
    SMMU500ModelContextFault(1, __FSR_PF | __FSR_AARCH64, 6, 0x4000, true);
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_ECANCELLED && written == 1);
    assert(faults[0].Kind == DMAIOMMUFaultPermission && faults[0].HardwareID == 6);
}

// Invalid arguments read nothing and still clear countOut when it exists. A
// unit that has not started returns OS_EBUSY with no records.
static void __InvalidArguments(void)
{
    ArmSMMUDescription_t description = SMMU500ModelDescription();
    DMAIOMMUFault_t faults[4];
    SMMU500Unit_t* unit = NULL;
    uint32_t written;

    SMMU500ModelBegin(true);
    assert(SMMU500UnitCreate(&description, &unit) == OS_EOK);
    written = 77;
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EBUSY && written == 0);
    assert(SMMU500UnitStart(unit) == OS_EOK);
    SMMU500ModelContextFault(0, __FSR_TF | __FSR_AARCH64, 1, 0x1000, false);
    written = 77;
    assert(SMMU500UnitPoll(NULL, faults, 4, &written) == OS_EINVALPARAMS && written == 0);
    written = 77;
    assert(SMMU500UnitPoll(unit, NULL, 4, &written) == OS_EINVALPARAMS && written == 0);
    written = 77;
    assert(SMMU500UnitPoll(unit, faults, 0, &written) == OS_EINVALPARAMS && written == 0);
    assert(SMMU500UnitPoll(unit, faults, 4, NULL) == OS_EINVALPARAMS);
    assert(__BankStatus(0) == (__FSR_TF | __FSR_AARCH64));
    assert(SMMU500UnitPoll(unit, faults, 4, &written) == OS_EOK && written == 1);
    assert(SMMU500UnitDestroy(&unit) == OS_EOK);
    SMMU500ModelEnd();
}

void SMMU500FaultTests(void)
{
    __DeviceFaults();
    __MultiAfterRead();
    __SeveralSources();
    __CapacityLimit();
    for (g_failureCase = 0; g_failureCase < sizeof(g_failureCases) / sizeof(g_failureCases[0]); g_failureCase++) {
        SMMU500ModelPinned(__UnitFailure);
    }
    __ChecksKeepDeviceFaults();
    __InvalidContextWhileClosingRoute();
    __FirmwareDeviceFaults();
    __AlreadyFailedUnit();
    SMMU500ModelPinned(__PollAfterFailedStart);
    __InvalidArguments();
}
