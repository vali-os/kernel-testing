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
 */

#ifndef __DMA_IOMMU_SIM_H__
#define __DMA_IOMMU_SIM_H__

#include "../kernel/memory/dma/backend.h"

// Host-only translation model. Never linked into the kernel.
typedef struct DMAIOMMUSim DMAIOMMUSim_t;

/** Immutable native limits and the number of command slots allocated at creation. */
typedef struct DMAIOMMUSimConfig {
    DMAIOMMUUnitConfig_t Unit;
    uint32_t            QueueCapacity;
} DMAIOMMUSimConfig_t;

enum DMAIOMMUSimFailurePoint {
    DMAIOMMUSimFailureNone,
    DMAIOMMUSimFailureInstallEntry,
    DMAIOMMUSimFailureRemoveEntry,
    DMAIOMMUSimFailureSubmit,
    DMAIOMMUSimFailureComplete
};

/** One copied, one-shot failure script and the unit's command wait controls. */
typedef struct DMAIOMMUSimControl {
    enum DMAIOMMUSimFailurePoint FailurePoint;
    uint64_t                    AfterSteps;
    oserr_t                     FailureStatus;
    bool                        FailUnit;
    uint64_t                    CompletionDelayNs;
    bool                        HoldCompletions;
} DMAIOMMUSimControl_t;

/** Live resources and cumulative command counts. TablePages includes candidates. */
typedef struct DMAIOMMUSimStats {
    uint64_t Spaces;
    uint64_t Mappings;
    uint64_t TablePages;
    uint64_t CachedTranslations;
    uint64_t PendingCommands;
    uint64_t SubmittedCommands;
    uint64_t CompletedCommands;
} DMAIOMMUSimStats_t;

/**
 * @brief Create a host model and its real native unit wrapper.
 * Copies config. QueueCapacity must be nonzero. Outputs must initially be NULL
 * and stay unchanged on failure. Uses native validation/allocation errors.
 */
oserr_t
DMAIOMMUSimCreate(
    _In_    const DMAIOMMUSimConfig_t* config,
    _InOut_ DMAIOMMUSim_t**            simOut);

/** The simulator's creator reference. Do not destroy it separately. */
DMAIOMMUUnit_t*
DMAIOMMUSimUnit(
    _In_ DMAIOMMUSim_t* sim);

/**
 * @brief Destroy after all callers stop. Busy/error keeps the pointer intact.
 * Calls native unit Destroy first. NULL is a successful no-op. No force-free
 * path exists for mappings pinned by a terminal unit failure.
 */
oserr_t
DMAIOMMUSimDestroy(
    _InOut_ DMAIOMMUSim_t** sim);

/**
 * @brief Copy a unit-wide script. Matching steps fail once after AfterSteps.
 * FailureStatus must be non-OS_EOK for an enabled failure. Delay applies to new
 * commands, HoldCompletions to queued ones too. Controls never cancel a token.
 * Returns OS_EINVALPARAMS for invalid inputs, otherwise OS_EOK.
 */
oserr_t
DMAIOMMUSimSetControl(
    _In_ DMAIOMMUSim_t*              sim,
    _In_ const DMAIOMMUSimControl_t* control);

/**
 * @brief Advance virtual time and process eligible commands in queue order.
 * A held command stops processing. OS_EOK still means time advanced. Overflow
 * returns OS_EOVERFLOW without changing time. A scripted error preserves the
 * failing command and returns its status. Callbacks use this same clock model.
 */
oserr_t
DMAIOMMUSimAdvance(
    _In_ DMAIOMMUSim_t* sim,
    _In_ uint64_t       nanoseconds);

/** Copy counters under the model lock. Both pointers must be valid. */
void
DMAIOMMUSimSnapshot(
    _In_  DMAIOMMUSim_t*      sim,
    _Out_ DMAIOMMUSimStats_t* statsOut);

/**
 * @brief Probe simulated device access, independently of core mapping state.
 *
 * Hold a native space reference. The space must belong to sim. Nonzero length
 * must fit the 64-bit address range. Access direction requests read/write/both.
 * Returned physical spans are merged when adjacent. Caller supplies storage.
 *
 * @return OS_EOK, OS_ENOENT for missing translation, OS_EPERMISSIONS for denied
 *         access, OS_EINVALPARAMS for invalid inputs, or OS_EOVERFLOW when a
 *         result is unrepresentable. OS_EBUFFER writes only the required count.
 *         NULL spans with zero capacity are an OS_EBUFFER count query. Other
 *         errors leave outputs unchanged. Probing can fill translation caches
 *         even if an error prevents output. Both passes hold the model lock.
 */
oserr_t
DMAIOMMUSimAccess(
    _In_  DMAIOMMUSim_t*              sim,
    _In_  const struct DMAAddressSpace* space,
    _In_  uint64_t                     address,
    _In_  uint64_t                     length,
    _In_  enum DMADirection            access,
    _Out_ SHMSG_t*                     spansOut,
    _In_  uint32_t                     capacity,
    _Out_ uint32_t*                    countOut);

// Forward declaration keeps standalone simulator builds independent of bindings.
typedef struct DMAManager DMAManager_t;

/**
 * @brief Probe a unit-local requester through the native software domain registry.
 * Hold sim and manager alive. Stop probes before destroying their domain.
 * Uses DMAIOMMUSimAccess output/error rules. Unknown requesters return OS_ENOENT.
 * Closing domains return OS_EBUSY. Neither means hardware was disconnected.
 * This probe deliberately ignores principal and software unit-health gates.
 */
oserr_t
DMAIOMMUSimAccessRequester(
    _In_  DMAIOMMUSim_t*    sim,
    _In_  DMAManager_t*     manager,
    _In_  uint32_t          requester,
    _In_  uint64_t          address,
    _In_  uint64_t          length,
    _In_  enum DMADirection access,
    _Out_ SHMSG_t*          spansOut,
    _In_  uint32_t          capacity,
    _Out_ uint32_t*         countOut);

#endif //!__DMA_IOMMU_SIM_H__
