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
 * Sector pattern of the SD transfer test. The layout is described in
 * pattern.h. This file builds both for the target program and, in the host
 * test, with the host compiler.
 */

#include "pattern.h"

// Stores value as four bytes, least significant first, so the layout does
// not depend on the byte order or alignment rules of the CPU.
static void
__StoreWord(
    _Out_ uint8_t* data,
    _In_  uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFF);
    data[1] = (uint8_t)((value >> 8) & 0xFF);
    data[2] = (uint8_t)((value >> 16) & 0xFF);
    data[3] = (uint8_t)((value >> 24) & 0xFF);
}

// One xorshift32 step (pattern.h). Never gives 0 for a value that is not 0.
static uint32_t
__NextFill(
    _In_ uint32_t value)
{
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

uint32_t
TransferPatternChecksum(
    _In_ const uint8_t* data,
    _In_ size_t         length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t   i;
    int      bit;

    // Bitwise CRC-32. A table would be faster, but the checksum covers only
    // the pattern sectors, and this keeps the code short and easy to compare
    // with the description in pattern.h.
    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ TRANSFER_PATTERN_CRC_POLYNOMIAL;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

void
TransferPatternFill(
    _In_  const TransferPatternSector_t* sector,
    _Out_ uint8_t*                       data)
{
    uint32_t fill;
    int      i;

    __StoreWord(data + TRANSFER_PATTERN_OFFSET_MARKER_0, TRANSFER_PATTERN_MARKER_0);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_MARKER_1, TRANSFER_PATTERN_MARKER_1);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_RUN, sector->RunId);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_PASS, sector->Pass);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_INDEX, sector->SectorIndex);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_COUNT, sector->SectorCount);
    __StoreWord(data + TRANSFER_PATTERN_OFFSET_REQUEST, sector->RequestBytes);

    // The seed mixes all three numbers that name the sector. The products
    // wrap around at 2^32, which unsigned arithmetic in C does by itself.
    fill = sector->RunId
        ^ (sector->Pass * (uint32_t)TRANSFER_PATTERN_SEED_PASS)
        ^ (sector->SectorIndex * (uint32_t)TRANSFER_PATTERN_SEED_INDEX);
    if (fill == 0) {
        fill = 1;
    }
    for (i = 0; i < TRANSFER_PATTERN_FILL_WORDS; i++) {
        __StoreWord(data + TRANSFER_PATTERN_OFFSET_FILL + (i * 4), fill);
        fill = __NextFill(fill);
    }

    __StoreWord(
        data + TRANSFER_PATTERN_OFFSET_CHECKSUM,
        TransferPatternChecksum(data, TRANSFER_PATTERN_OFFSET_CHECKSUM)
    );
}
