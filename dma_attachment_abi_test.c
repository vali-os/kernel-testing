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

// The shared structures are passed by pointer even on the 32-bit syscall ABI.
#include <stddef.h>
#include <ddk/dma/attachment.h>

_Static_assert(sizeof(OSDmaAttachmentParameters_t) == 1584, "registration size");
_Static_assert(offsetof(OSDmaAttachmentParameters_t, RequesterGroup) == 16, "requester offset");
_Static_assert(offsetof(OSDmaAttachmentParameters_t, ResetGroup) == 24, "reset offset");
_Static_assert(offsetof(OSDmaAttachmentParameters_t, AddressLimit) == 32, "limit offset");
_Static_assert(offsetof(OSDmaAttachmentParameters_t, Ranges) == 48, "range offset");
_Static_assert(sizeof(OSDmaAttachmentId_t) == 8, "attachment ID size");
_Static_assert(sizeof(OSDmaAttachmentEvent_t) == 32, "event size");
_Static_assert(offsetof(OSDmaAttachmentEvent_t, Reasons) == 24, "reason offset");
_Static_assert(offsetof(OSDmaAttachmentEvent_t, Reserved) == 28, "reserved offset");

_Static_assert(sizeof(OSDmaBindParameters_t) == 24, "binding request size");
_Static_assert(offsetof(OSDmaBindParameters_t, Principal) == 8, "principal offset");
_Static_assert(offsetof(OSDmaBindParameters_t, RequestId) == 16, "request offset");
_Static_assert(sizeof(OSDmaBindingResult_t) == 16, "binding result size");
_Static_assert(offsetof(OSDmaBindingResult_t, Reserved) == 4, "binding reserved offset");
_Static_assert(offsetof(OSDmaBindingResult_t, Generation) == 8, "generation offset");

_Static_assert(sizeof(OSDmaBoundOpenParameters_t) == 16, "bound open size");
_Static_assert(offsetof(OSDmaBoundOpenParameters_t, AddressLimit) == 8, "bound limit offset");
_Static_assert(sizeof(OSDmaStopTicket_t) == 16, "stop ticket size");
_Static_assert(offsetof(OSDmaStopTicket_t, Epoch) == 8, "stop epoch offset");
