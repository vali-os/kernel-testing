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
 * Host boundaries shared by the SMMUv3 and MMU-500 register models.
 */
#ifndef __ARM_SMMU_TEST_MODEL_SUPPORT_H__
#define __ARM_SMMU_TEST_MODEL_SUPPORT_H__

#include <os/osdefs.h>

/**
 * @brief Resolve a complete physical range in a live fixture allocation.
 * The fixture supplies the CPU view of control tables, queues and data pages.
 * Assert if any byte falls outside that allocation or the allocation is gone.
 * This does not allocate memory or advance modeled hardware work.
 */
void*
SMMUTestPhysical(
    _In_ uint64_t physicalAddress,
    _In_ size_t   length);

/**
 * @brief Assert that the mutex for this register mapping is held.
 * The register model calls this for mutable MMIO and ordering barriers.
 * Read-only capability probes may run before the unit mutex exists.
 */
void
SMMUTestAssertUnitLocked(
    _In_ uintptr_t registers);

#endif //!__ARM_SMMU_TEST_MODEL_SUPPORT_H__
