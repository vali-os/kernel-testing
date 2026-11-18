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
 * Shared access counters and fault tracking for one SHM region. Enforces
 * multiple-reader / single-writer rules across CPU and device sessions,
 * tracks stopped device transfers, and gates region reopening until cache
 * recovery completes.
 */

#include <os/types/dma_region.h>
#include <ddk/dma_region.h>
#include <stddef.h>

_Static_assert(offsetof(OSDmaRegionSession_t, Handle) == 0, "session handle ABI");
_Static_assert(offsetof(OSDmaRegionSession_t, Identity) == 4, "session identity ABI");
_Static_assert(offsetof(OSDmaRegionSession_t, TokenCapacity) == 16, "session capacity ABI");
#if __SIZEOF_POINTER__ == 4
_Static_assert(offsetof(OSDmaRegionSession_t, Offset) == 20, "i386 offset ABI");
_Static_assert(sizeof(OSDmaRegionSession_t) == 28, "i386 session ABI");
#else
_Static_assert(offsetof(OSDmaRegionSession_t, Offset) == 24, "64-bit offset ABI");
_Static_assert(sizeof(OSDmaRegionSession_t) == 40, "64-bit session ABI");
#endif
