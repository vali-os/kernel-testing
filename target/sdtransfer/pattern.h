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
 * Sector pattern of the SD transfer test. This header is the one place the
 * layout is described. The target program (sdtransfer.c) writes it, and the
 * host checker (testing/sdtransfer_check.py) reads the TRANSFER_PATTERN_
 * values below straight from this file, so both always use the same layout.
 *
 * Every 512-byte sector of the test file is one pattern sector. A pattern
 * sector can be found and checked on the raw card without reading the file
 * system, because it says by itself which file sector it is. All numbers are
 * 32-bit little endian.
 *
 * Layout of one sector (byte offsets):
 *   0    Marker word 0, TRANSFER_PATTERN_MARKER_0 ("VSDT").
 *   4    Marker word 1, TRANSFER_PATTERN_MARKER_1 ("RAN1"). The 1 is the
 *        layout version.
 *   8    Run identifier. The program picks a new one at each start and logs
 *        it, so sectors of earlier runs on the same card can be told apart.
 *   12   Pass number, counted from 1. Each pass writes the whole file again,
 *        and the file system may put the new data in other buckets. Sectors
 *        of an earlier pass can then remain in free space, and the pass
 *        number tells them apart from the sectors of the last pass.
 *   16   Sector index within the file, counted from 0.
 *   20   Number of sectors in the file.
 *   24   Request size in bytes that the pass used to write the file. For the
 *        record only, the checker does not need it.
 *   28   Fill: 120 words. The first word is the seed, every next word is the
 *        xorshift32 step of the word before it. The seed is
 *        run ^ (pass * 0x9E3779B9) ^ (index * 0x85EBCA6B), computed modulo
 *        2^32, and 1 if that is 0. Every sector has other fill, so a sector
 *        read from or written to the wrong place does not match.
 *   508  Checksum: CRC-32 over bytes 0 to 507. This is the common CRC-32 of
 *        zlib and Ethernet (reflected polynomial 0xEDB88320, start value
 *        0xFFFFFFFF, result inverted).
 *
 * xorshift32 step, in this order:
 *   x ^= x << 13
 *   x ^= x >> 17
 *   x ^= x << 5
 */

#ifndef __TRANSFER_PATTERN_H__
#define __TRANSFER_PATTERN_H__

#include <stddef.h>
#include <stdint.h>

// The host test builds pattern.c with the host compiler, which has no SAL
// annotations. The Vali headers define them.
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif

#define TRANSFER_PATTERN_SECTOR_SIZE    512
#define TRANSFER_PATTERN_MARKER_0        0x54445356
#define TRANSFER_PATTERN_MARKER_1        0x314E4152
#define TRANSFER_PATTERN_OFFSET_MARKER_0 0
#define TRANSFER_PATTERN_OFFSET_MARKER_1 4
#define TRANSFER_PATTERN_OFFSET_RUN      8
#define TRANSFER_PATTERN_OFFSET_PASS     12
#define TRANSFER_PATTERN_OFFSET_INDEX    16
#define TRANSFER_PATTERN_OFFSET_COUNT    20
#define TRANSFER_PATTERN_OFFSET_REQUEST  24
#define TRANSFER_PATTERN_OFFSET_FILL     28
#define TRANSFER_PATTERN_FILL_WORDS      120
#define TRANSFER_PATTERN_OFFSET_CHECKSUM 508
#define TRANSFER_PATTERN_SEED_PASS       0x9E3779B9
#define TRANSFER_PATTERN_SEED_INDEX      0x85EBCA6B
#define TRANSFER_PATTERN_CRC_POLYNOMIAL  0xEDB88320

/**
 * @brief The values that make one pattern sector. Everything else in the
 * sector follows from them.
 */
typedef struct TransferPatternSector {
    uint32_t RunId;
    uint32_t Pass;
    uint32_t SectorIndex;
    uint32_t SectorCount;
    uint32_t RequestBytes;
} TransferPatternSector_t;

/**
 * @brief Writes one pattern sector.
 *
 * @param sector Values of the sector.
 * @param data Receives the sector. Must be writable for
 *             TRANSFER_PATTERN_SECTOR_SIZE bytes. Needs no alignment.
 */
void
TransferPatternFill(
    _In_  const TransferPatternSector_t* sector,
    _Out_ uint8_t*                       data);

/**
 * @brief Computes the CRC-32 that the pattern uses for its checksum.
 *
 * @param data Bytes to check. Must be readable for length bytes.
 * @param length Number of bytes.
 * @return The CRC-32 of the bytes.
 */
uint32_t
TransferPatternChecksum(
    _In_ const uint8_t* data,
    _In_ size_t         length);

#endif //!__TRANSFER_PATTERN_H__
