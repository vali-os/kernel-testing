/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
 * Test stand-in for librt/libddk/include/internal/_storage.h, used by
 * sd_storage_test.c. The real __StorageAttachTransfer is a static inline
 * function that talks to the kernel. Here it is an ordinary function that
 * the test defines, so the test can record and script it. The runner puts
 * this directory first on the include path.
 */

#ifndef __DDK_INTERNAL_STORAGE_H__
#define __DDK_INTERNAL_STORAGE_H__

#include <os/osdefs.h>
#include <os/types/handle.h>

struct sys_shm_transfer;

oserr_t
__StorageAttachTransfer(
    _In_  const struct sys_shm_transfer* description,
    _In_  size_t                         sectorSize,
    _In_  size_t                         count,
    _In_  bool                           write,
    _Out_ OSHandle_t*                    buffer,
    _Out_ size_t*                        offsetOut);

#endif //!__DDK_INTERNAL_STORAGE_H__
