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
 * Native SMMUv3 self-test entry point.
 * Only builds with VALI_ARM64_SMMU_SELFTEST enabled include this header. The
 * test drives the real SMMUv3 backend against QEMU's iommu-testdev and prints
 * one terminal "SMMU-SELFTEST PASS" or "SMMU-SELFTEST FAIL" record. Such a
 * build never starts userspace.
 */

#ifndef __ARM_SMMU_NATIVE_H__
#define __ARM_SMMU_NATIVE_H__

#include <os/osdefs.h>

/**
 * @brief Queues the self-test worker thread.
 *
 * Call once from InitializeMachine after platform, ACPI and SMP setup and
 * before SchedulerEnable. This only creates a kernel thread. All hardware
 * access happens later in that thread, once the scheduler runs it.
 *
 * @return OS_EOK when the thread was created, or the ThreadCreate error.
 *         The caller must stop the boot on failure.
 */
KERNELABI oserr_t KERNELAPI
ArmSMMUSelfTestStart(void);

#endif //!__ARM_SMMU_NATIVE_H__
