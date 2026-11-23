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
 * Native SMMUv3 self-test: device requests and the checks after them.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * The case functions send every device request through this file. A
 * transfer is a write that must succeed. A probe is a 4-byte write that the
 * SMMU must refuse. Both first fill all observed pages with the fill byte,
 * then send the request, poll the fault queue and compare every observed
 * byte. Each request prints its request record, its "poll" record and its
 * "memory" record, and a probe also prints the fault record it expected.
 * ArmSMMUSelfTestCheckNoFaults runs the same Poll check at the points of the
 * run where no request was sent, after unit start, after connect and during
 * cleanup.
 */

#define __MODULE "SMMUTEST"

#include "native_coordinator.h"
#include <debug.h>
#include <string.h>

// Fill byte for every observed page. It differs from each byte of the device
// pattern (0x78, 0x56, 0x34, 0x12).
#define __FILL_BYTE    0xA5U

// Length of every write that must be refused. QEMU writes the request in
// 4-byte accesses and reports one event for each refused access, so a
// 4-byte probe gives exactly one fault record.
#define __PROBE_LENGTH 4U

// Full-system barrier between CPU accesses to the test pages and device
// requests. The arena may be mapped cached or uncached, so use full-system
// scope.
static inline void
__MemoryBarrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

// Returns the pattern byte the device writes at offset index of a request.
static uint8_t
__PatternByte(
    _In_ uint64_t index)
{
    return (uint8_t)(ARM_SMMU_TEST_DEVICE_PATTERN >> ((index % 4U) * 8U));
}

/*******************************************************************************
 * Fault records
 ******************************************************************************/

// Short names of the fault kinds, used in FAULT records.
static const char*
__FaultKindName(
    _In_ enum DMAIOMMUFaultKind kind)
{
    switch (kind) {
        case DMAIOMMUFaultNoMapping:    return "no-mapping";
        case DMAIOMMUFaultPermission:   return "permission";
        case DMAIOMMUFaultAddressRange: return "address-range";
        case DMAIOMMUFaultOther:        return "other";
        case DMAIOMMUFaultNoRoute:      return "no-route";
        case DMAIOMMUFaultReportsLost:  return "reports-lost";
        default:                        return "unknown";
    }
}

// Direction of the refused access, from the record flags.
static const char*
__FaultDirectionName(
    _In_ uint32_t flags)
{
    uint32_t direction = flags & (DMA_IOMMU_FAULT_READ | DMA_IOMMU_FAULT_WRITE);

    if (direction == DMA_IOMMU_FAULT_WRITE) {
        return "write";
    }
    if (direction == DMA_IOMMU_FAULT_READ) {
        return "read";
    }
    return direction == 0 ? "none" : "read+write";
}

// Prints the one fault record a probe was expected to produce. The record
// names the probe, so the host runner can match it to its request.
static void
__PrintExpectedFault(
    _In_ const char*            probeName,
    _In_ const DMAIOMMUFault_t* fault)
{
    DEBUG("SMMU-SELFTEST FAULT expected %s kind=%s sid=0x%x iova=0x%llx dir=%s code=0x%x",
          probeName, __FaultKindName(fault->Kind), fault->HardwareID, fault->DeviceAddress,
          __FaultDirectionName(fault->Flags), fault->Code);
}

// Prints the first count records in g_armSMMUSelfTest.Faults as unexpected.
// The raw flags are printed, because iova only holds an address when the
// flags include DMA_IOMMU_FAULT_ADDRESS. The FAIL record that follows names
// the step.
static void
__PrintUnexpectedFaults(
    _In_ uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        const DMAIOMMUFault_t* fault = &g_armSMMUSelfTest.Faults[i];

        ERROR("SMMU-SELFTEST FAULT unexpected kind=%s sid=0x%x iova=0x%llx flags=0x%x code=0x%x",
              __FaultKindName(fault->Kind), fault->HardwareID, fault->DeviceAddress,
              fault->Flags, fault->Code);
    }
}

// Reads the pending fault records into g_armSMMUSelfTest.Faults. Call without
// the unit mutex. OS_EOK means the unit is still healthy, also when records
// were returned (see SMMUv3UnitPoll).
static oserr_t
__ReadFaults(
    _Out_ uint32_t* countOut)
{
    oserr_t status;

    *countOut = 0;
    status = SMMUv3UnitPoll(
        g_armSMMUSelfTest.Unit,
        &g_armSMMUSelfTest.Faults[0],
        ARM_SMMU_SELFTEST_FAULT_CAPACITY,
        countOut
    );
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: SMMUv3UnitPoll failed: %u, %u records", status, *countOut);
    }
    return status;
}

// Reads every pending fault record outside of a request. None is expected,
// so each record is printed and fails the test.
oserr_t
ArmSMMUSelfTestCheckNoFaults(void)
{
    uint32_t count = 0;
    oserr_t  status;

    status = __ReadFaults(&count);
    __PrintUnexpectedFaults(count);
    if (status != OS_EOK) {
        return status;
    }
    return count == 0 ? OS_EOK : OS_EDEVFAULT;
}

// Polls right after a request and prints the "<name> poll" record with the
// Poll status and the number of records, which the host runner checks.
static oserr_t
__PollRequest(
    _In_  const char* name,
    _Out_ uint32_t*   countOut)
{
    oserr_t status;

    status = __ReadFaults(countOut);
    DEBUG("SMMU-SELFTEST STEP %s poll status=%u records=%u", name, status, *countOut);
    return status;
}

// Builds the record Poll must return for a probe. The two translation
// events carry the input address and the access direction (section 7.3 of
// the SMMUv3 specification), so their record has both. C_BAD_STREAMID has
// neither, so a NoRoute record carries only the stream ID.
static void
__ExpectedFault(
    _In_  const ArmSMMUTestProbe_t* probe,
    _In_  uint64_t                  iova,
    _Out_ DMAIOMMUFault_t*          expectedOut)
{
    memset(expectedOut, 0, sizeof(*expectedOut));
    expectedOut->HardwareID = g_armSMMUSelfTest.Domains[probe->Domain].HardwareID;
    switch (probe->Outcome) {
        case ArmSMMUTestProbeNoMapping:
            expectedOut->Kind = DMAIOMMUFaultNoMapping;
            expectedOut->Code = SMMUV3_EVENT_F_TRANSLATION;
            break;
        case ArmSMMUTestProbePermission:
            expectedOut->Kind = DMAIOMMUFaultPermission;
            expectedOut->Code = SMMUV3_EVENT_F_PERMISSION;
            break;
        default:
            expectedOut->Kind = DMAIOMMUFaultNoRoute;
            expectedOut->Code = SMMUV3_EVENT_C_BAD_STREAMID;
            return;
    }
    expectedOut->Flags = DMA_IOMMU_FAULT_ADDRESS | DMA_IOMMU_FAULT_WRITE;
    expectedOut->DeviceAddress = iova;
}

// Checks the records Poll returned for a probe. The two fault cases need
// exactly one record equal to the expected one. The abort cases accept no
// record, or one NoRoute record for the probing stream ID and nothing else.
// The accepted record is printed as expected. Anything else is printed as
// unexpected and fails.
static oserr_t
__CheckProbeFaults(
    _In_ const ArmSMMUTestProbe_t* probe,
    _In_ uint64_t                  iova,
    _In_ uint32_t                  count)
{
    const DMAIOMMUFault_t* fault = &g_armSMMUSelfTest.Faults[0];
    DMAIOMMUFault_t        expected;

    if (count == 0 && probe->Outcome == ArmSMMUTestProbeAbort) {
        return OS_EOK;
    }

    __ExpectedFault(probe, iova, &expected);
    if (count != 1) {
        __PrintUnexpectedFaults(count);
        ERROR("SMMU-SELFTEST: %s: Poll returned %u fault records, expected one %s record",
              probe->Name, count, __FaultKindName(expected.Kind));
        return OS_EDEVFAULT;
    }

    if (fault->Kind != expected.Kind || fault->HardwareID != expected.HardwareID ||
        fault->Flags != expected.Flags || fault->Code != expected.Code ||
        fault->DeviceAddress != expected.DeviceAddress) {
        __PrintUnexpectedFaults(count);
        ERROR("SMMU-SELFTEST: %s: expected kind=%s sid=0x%x iova=0x%llx flags=0x%x code=0x%x",
              probe->Name, __FaultKindName(expected.Kind), expected.HardwareID,
              expected.DeviceAddress, expected.Flags, expected.Code);
        return OS_EDEVFAULT;
    }
    __PrintExpectedFault(probe->Name, fault);
    return OS_EOK;
}

/*******************************************************************************
 * Transfers and observations
 ******************************************************************************/

// Fills every observed block with __FILL_BYTE, then finishes the barrier so
// the device cannot see older contents.
static void
__ResetPages(void)
{
    for (uint32_t i = 0; i < ArmSMMUTestBlockCount; i++) {
        ArmSMMUMemoryBlock_t* memory = &g_armSMMUSelfTest.Blocks[i].Memory;

        memset(memory->VirtualAddress, __FILL_BYTE, memory->Length);
    }
    __MemoryBarrier();
}

// Compares every byte of one block. Bytes in the target range of written
// must hold the pattern. All other bytes must still hold the fill byte.
// written is NULL after a refused write, when no byte may have changed.
static oserr_t
__CheckBlock(
    _In_ enum ArmSMMUTestBlockIndex   index,
    _In_ const char*                  name,
    _In_ const ArmSMMUTestTransfer_t* written)
{
    const ArmSMMUTestBlock_t* block = &g_armSMMUSelfTest.Blocks[index];
    const uint8_t*            bytes = (const uint8_t*)block->Memory.VirtualAddress;
    bool                      hasTarget = written != NULL && written->Block == index;
    uint64_t                  start = hasTarget ? written->BlockOffset : 0;
    uint64_t                  end = hasTarget ? written->BlockOffset + written->Length : 0;
    uint32_t                  mismatches = 0;
    uint64_t                  first = 0;
    uint8_t                   expected;

    for (uint64_t offset = 0; offset < block->Memory.Length; offset++) {
        if (offset >= start && offset < end) {
            expected = __PatternByte(offset - start);
        } else {
            expected = __FILL_BYTE;
        }

        if (bytes[offset] != expected) {
            if (mismatches == 0) {
                first = offset;
            }
            mismatches++;
        }
    }

    if (mismatches != 0) {
        ERROR("SMMU-SELFTEST: %s: block %s has %u wrong bytes, first at 0x%llx is 0x%02x",
              name, block->Name, mismatches, first, bytes[first]);
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

// Checks every observed block after a request and prints the "<name> memory"
// record. It names the range that holds the pattern (or "none" after a
// refused write) and confirms that the guard and every other byte kept the
// fill byte.
static oserr_t
__CheckPages(
    _In_ const char*                  name,
    _In_ const ArmSMMUTestTransfer_t* written)
{
    oserr_t status;

    for (uint32_t i = 0; i < ArmSMMUTestBlockCount; i++) {
        status = __CheckBlock((enum ArmSMMUTestBlockIndex)i, name, written);
        if (status != OS_EOK) {
            return status;
        }
    }

    if (written == NULL) {
        DEBUG("SMMU-SELFTEST STEP %s memory written=none guard=unchanged other-bytes=unchanged", name);
        return OS_EOK;
    }
    DEBUG("SMMU-SELFTEST STEP %s memory written=%s+0x%llx len=%u guard=unchanged other-bytes=unchanged",
          name, g_armSMMUSelfTest.Blocks[written->Block].Name, written->BlockOffset, written->Length);
    return OS_EOK;
}

// Prints the request record: who sent it, the IOVA, the physical readback
// address the device was given and the device result.
static void
__PrintRequest(
    _In_ const char*                name,
    _In_ const ArmSMMUTestDomain_t* domain,
    _In_ uint64_t                   iova,
    _In_ uint64_t                   physical,
    _In_ uint32_t                   length,
    _In_ uint32_t                   result)
{
    DEBUG("SMMU-SELFTEST STEP %s rid=%04x sid=0x%x iova=0x%llx pa=0x%llx len=%u result=0x%x",
          name, domain->Device->RequesterId, domain->HardwareID, iova, physical, length, result);
}

// Runs one device write that must succeed and checks the result, the fault
// queue and every observed byte. A pass shows that the write was translated
// to the selected data page, that the guard at the identity address and
// every byte outside the request kept the fill byte, and that no fault was
// reported.
oserr_t
ArmSMMUSelfTestTransfer(
    _In_ const ArmSMMUTestTransfer_t* transfer)
{
    ArmSMMUTestDomain_t* domain = &g_armSMMUSelfTest.Domains[transfer->Domain];
    ArmSMMUTestBlock_t*  block = &g_armSMMUSelfTest.Blocks[transfer->Block];
    uint64_t             iova = __ArmSMMUSelfTestGuardBase() + transfer->IovaOffset;
    uint64_t             physical = block->Memory.PhysicalAddress + transfer->BlockOffset;
    uint32_t             result = ARM_SMMU_TEST_RESULT_IDLE;
    uint32_t             count = 0;
    oserr_t              status;

    __ArmSMMUSelfTestStep(transfer->Name);
    __ResetPages();
    status = ArmSMMUTestDeviceWrite(domain->Device, iova, physical, transfer->Length, &result);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: %s: device protocol failed: %u", transfer->Name, status);
        return status;
    }
    __MemoryBarrier();
    __PrintRequest(transfer->Name, domain, iova, physical, transfer->Length, result);

    // Poll before judging the result, so that a refused write also prints
    // the fault record that explains it.
    status = __PollRequest(transfer->Name, &count);
    __PrintUnexpectedFaults(count);
    if (status != OS_EOK) {
        return status;
    }

    if (result != ARM_SMMU_TEST_RESULT_SUCCESS) {
        ERROR("SMMU-SELFTEST: %s: device result 0x%x, expected 0", transfer->Name, result);
        return OS_EDEVFAULT;
    }

    if (count != 0) {
        ERROR("SMMU-SELFTEST: %s: %u fault records after a successful write", transfer->Name, count);
        return OS_EDEVFAULT;
    }
    return __CheckPages(transfer->Name, transfer);
}

// Runs one write that the SMMU must refuse, then checks the device result,
// the fault records and every observed byte. The device gets the IOVA itself
// as the physical readback address: if the SMMU let the write through
// untranslated, the readback would match and the result would be 0. Only the
// transaction failure result (the SMMU refused the write) is accepted. Every
// other nonzero result is a fixture failure and fails the case. Afterwards
// no observed byte may have changed.
oserr_t
ArmSMMUSelfTestProbe(
    _In_ const ArmSMMUTestProbe_t* probe)
{
    ArmSMMUTestDomain_t* domain = &g_armSMMUSelfTest.Domains[probe->Domain];
    uint64_t             iova = __ArmSMMUSelfTestGuardBase() + probe->IovaOffset;
    uint32_t             result = ARM_SMMU_TEST_RESULT_IDLE;
    uint32_t             count = 0;
    oserr_t              status;

    __ArmSMMUSelfTestStep(probe->Name);
    __ResetPages();
    status = ArmSMMUTestDeviceWrite(domain->Device, iova, iova, __PROBE_LENGTH, &result);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: %s: device protocol failed: %u", probe->Name, status);
        return status;
    }
    __MemoryBarrier();
    __PrintRequest(probe->Name, domain, iova, iova, __PROBE_LENGTH, result);

    status = __PollRequest(probe->Name, &count);
    if (status != OS_EOK) {
        __PrintUnexpectedFaults(count);
        return status;
    }

    if (result != ARM_SMMU_TEST_RESULT_TX_FAIL) {
        __PrintUnexpectedFaults(count);
        ERROR("SMMU-SELFTEST: %s: device result 0x%x, expected 0x%x (write refused by the SMMU)",
              probe->Name, result, ARM_SMMU_TEST_RESULT_TX_FAIL);

        // The case fails either way. The page check logs which block the
        // write reached, if it reached one, so its status adds nothing here.
        (void)__CheckPages(probe->Name, NULL);
        return OS_EDEVFAULT;
    }

    status = __CheckProbeFaults(probe, iova, count);
    if (status != OS_EOK) {
        return status;
    }
    return __CheckPages(probe->Name, NULL);
}
