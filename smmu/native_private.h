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
 * Native SMMUv3 self-test: QEMU iommu-testdev adapter.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST, or into the host test.
 *
 * QEMU's iommu-testdev is a PCI test endpoint with one 4 KiB memory BAR. The
 * kernel writes a device address (IOVA), a physical address, a length and a
 * set of bus attributes into its registers, arms it, then reads its trigger
 * register. During that read QEMU writes a fixed 32-bit pattern to the IOVA
 * through the SMMU, then reads the physical address directly and compares the
 * two. The result register is 0 when the pattern reached the expected
 * physical memory. Otherwise it names the failure, such as a write that the
 * SMMU refused.
 *
 * The adapter below only knows about PCI configuration and this register
 * protocol. All SMMU work stays in the coordinator files (native.c and the
 * native_*.c files listed there).
 */

#ifndef __ARM_SMMU_NATIVE_PRIVATE_H__
#define __ARM_SMMU_NATIVE_PRIVATE_H__

#include <os/osdefs.h>

// The QEMU fixture places exactly two test endpoints behind the SMMU.
#define ARM_SMMU_TEST_DEVICE_COUNT 2U

// Size of the single memory BAR of iommu-testdev (BAR0_SIZE in QEMU).
#define ARM_SMMU_TEST_DEVICE_BAR_SIZE 0x1000U

// Largest request the coordinator sends. QEMU accepts larger lengths, but
// a short request keeps each transfer inside the allocated test pages.
#define ARM_SMMU_TEST_DEVICE_MAX_LENGTH 256U

// Fixed data the device writes, repeated in little-endian order over the
// whole request (ITD_DMA_WRITE_VAL in QEMU).
#define ARM_SMMU_TEST_DEVICE_PATTERN 0x12345678U

// Values of the result register (ITD_REG_DMA_RESULT) after a trigger read.
// 0 means the write was accepted and the physical readback matched.
#define ARM_SMMU_TEST_RESULT_SUCCESS   0x00000000U
#define ARM_SMMU_TEST_RESULT_IDLE      0xFFFFFFFFU // Disarmed, no request pending.
#define ARM_SMMU_TEST_RESULT_BUSY      0xFFFFFFFEU // Armed, waiting for the trigger read.
#define ARM_SMMU_TEST_RESULT_BAD_LEN   0xDEAD0001U // Length was zero.
#define ARM_SMMU_TEST_RESULT_TX_FAIL   0xDEAD0002U // The SMMU refused the device write.
#define ARM_SMMU_TEST_RESULT_RD_FAIL   0xDEAD0003U // The physical readback failed.
#define ARM_SMMU_TEST_RESULT_MISMATCH  0xDEAD0004U // Readback did not hold the pattern.
#define ARM_SMMU_TEST_RESULT_NOT_ARMED 0xDEAD0005U // Trigger read without arming.
#define ARM_SMMU_TEST_RESULT_BAD_ATTRS 0xDEAD0006U // Secure bit and security space disagree.

/**
 * How far an adapter record has been set up.
 * - Empty: zeroed record. Nothing is known about the device.
 * - Identified: segment and requester ID are recorded. Configuration writes
 *   may have started, so Disable must still clear the command register.
 * - Ready: BAR0 is assigned and mapped, memory decode is on, bus mastering is
 *   off and the device was checked to be disarmed with its reset values.
 */
enum ArmSMMUTestDeviceState {
    ArmSMMUTestDeviceEmpty,
    ArmSMMUTestDeviceIdentified,
    ArmSMMUTestDeviceReady
};

/**
 * @brief One QEMU iommu-testdev endpoint used by the native SMMU test.
 *
 * Segment and RequesterId identify the PCI function (bus << 8 | device << 3 |
 * function). The SMMU stream ID is resolved separately through firmware
 * discovery and is never assumed to equal RequesterId. BarBusAddress is the
 * value programmed into BAR0, BarPhysicalAddress is where the CPU sees the
 * same bytes, and Registers is the kernel mapping of that 4 KiB range. The
 * records live in static storage so they stay valid after a failed test.
 */
typedef struct ArmSMMUTestDevice {
    enum ArmSMMUTestDeviceState State;
    uint16_t                    Segment;
    uint16_t                    RequesterId;
    uint64_t                    BarBusAddress;
    uint64_t                    BarPhysicalAddress;
    uintptr_t                   Registers;
} ArmSMMUTestDevice_t;

/**
 * @brief Finds both test endpoints and makes them ready for requests.
 *
 * Call once from thread context after the ACPI PCI root list exists. Pass two
 * zeroed records. The flow is:
 * 1. Check the fixed topology: one ACPI PCI root, the QEMU host bridge at
 *    function 00.0 and iommu-testdev at 01.0 and 02.0 of its first bus. Any
 *    other function or any bridge fails the call. Both identities are recorded
 *    (Identified) before the first configuration write.
 * 2. Turn off memory decode and bus mastering on both endpoints, then probe
 *    every BAR. BAR0 must be a 4 KiB 32-bit nonprefetchable memory BAR and the
 *    other five must be absent.
 * 3. Pick two 4 KiB bus ranges from the single nonprefetchable memory window
 *    below 4 GiB, translate them to CPU addresses, program BAR0, map the range
 *    as Device memory and enable memory decode.
 * 4. Check the register reset values and disarm each device (Ready).
 * 5. Read the slot, BAR0 and command register of each endpoint again and
 *    print two "SMMU-SELFTEST STEP endpoint" records with what was verified.
 * Bus mastering stays off and no request is started.
 *
 * @param devices Two zeroed records. Receives both endpoints.
 * @return OS_EOK when both records are Ready. OS_EINVALPARAMS if a record was
 *         not zeroed, OS_ENOENT if an expected device is missing,
 *         OS_ENOTSUPPORTED for another topology, BAR layout or window list,
 *         OS_EDEVFAULT for a failed configuration access or readback, and
 *         OS_EOOM if a BAR cannot be mapped. On failure each record keeps the
 *         state it reached, so ArmSMMUTestDeviceDisable can clean it up.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUTestDevicesPrepare(
    _InOut_ ArmSMMUTestDevice_t devices[2]);

/**
 * @brief Runs one device write and returns the device's result code.
 *
 * The caller (the single test worker) must have reset the memory it observes
 * and finished its CPU barrier first. In order, this writes the IOVA, the
 * expected physical address, the length and NonSecure bus attributes, reading
 * each register back. It then enables bus mastering, arms the doorbell and
 * checks RESULT=BUSY, reads the trigger register once, reads the result,
 * disarms and checks RESULT=IDLE, and clears bus mastering. All device
 * registers use 32-bit accesses and the command register uses 16-bit accesses.
 *
 * @param device                  Ready record.
 * @param deviceAddress           Four-byte aligned IOVA the device writes.
 * @param expectedPhysicalAddress Physical address QEMU reads back afterwards.
 * @param length                  Nonzero multiple of 4, at most 256 bytes.
 * @param resultOut               Receives the result register value.
 * @return OS_EOK when the protocol finished and no request stays armed. The
 *         result may still describe a refused write. OS_EINVALPARAMS for a
 *         bad argument or a record that is not Ready. OS_EDEVFAULT if a
 *         register readback failed. On failure resultOut is unchanged, the
 *         record is kept as is and the caller must call Disable.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUTestDeviceWrite(
    _In_  ArmSMMUTestDevice_t* device,
    _In_  uint64_t             deviceAddress,
    _In_  uint64_t             expectedPhysicalAddress,
    _In_  uint32_t             length,
    _Out_ uint32_t*            resultOut);

/**
 * @brief Disarms the device and turns off memory decode and bus mastering.
 *
 * An Empty record needs no work. A Ready record is disarmed first and must
 * read back RESULT=IDLE. For Ready and Identified records the command
 * register bits are then cleared and checked, and the record becomes
 * Identified. The BAR mapping stays, as Arm64MapDevice mappings live for the
 * whole boot. The firmware command register value is not restored.
 *
 * @param device Record to disable.
 * @return OS_EOK on success. OS_EINVALPARAMS for NULL. OS_EDEVFAULT if a
 *         readback failed. The record is unchanged on failure.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUTestDeviceDisable(
    _InOut_ ArmSMMUTestDevice_t* device);

#endif //!__ARM_SMMU_NATIVE_PRIVATE_H__
