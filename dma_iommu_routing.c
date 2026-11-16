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

#include "dma_iommu_sim.h"
#include "../kernel/memory/dma/attachment_private.h"

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
    _Out_ uint32_t*         countOut)
{
    struct DMAAddressSpace* space = NULL;
    DMAIOMMUUnit_t*         unit;
    oserr_t                 status = OS_ENOENT;

    if (sim == NULL || manager == NULL || countOut == NULL ||
        (spansOut == NULL && capacity != 0) || length == 0 || length - 1 > UINT64_MAX - address ||
        (access != DMAToDevice && access != DMAFromDevice && access != DMABidirectional)) {
        return OS_EINVALPARAMS;
    }
    unit = DMAIOMMUSimUnit(sim);
    MutexLock(&manager->Lock);
    foreach (node, &manager->Domains) {
        DMATranslatedDomain_t* domain = node->value;

        if (domain->Unit != unit) {
            continue;
        }
        for (uint32_t i = 0; i < domain->RequesterCount; i++) {
            if (domain->Requesters[i] != requester) {
                continue;
            }
            // Closing reserves the identity but cannot supply a space ref.
            // Neither this state nor a failed unit proves physical disconnect.
            if (domain->State != DMATranslatedDomainReady) {
                status = OS_EBUSY;
                goto unlock;
            }
            status = DMAAddressSpaceAcquire(domain->AddressSpace);
            if (status == OS_EOK) {
                space = domain->AddressSpace;
            }
            goto unlock;
        }
    }
unlock:
    MutexUnlock(&manager->Lock);
    if (status != OS_EOK) {
        return status;
    }
    // A requester has no driver principal or software admission gate. The
    // model's tables/cache determine access even after terminal unit failure.
    status = DMAIOMMUSimAccess(sim, space, address, length, access, spansOut, capacity, countOut);
    DMAAddressSpaceRelease(&space);
    return status;
}
