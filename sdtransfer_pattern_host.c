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
 * Host helper of testing/sdtransfer_check_test.py. It runs the target
 * program's pattern code (testing/target/sdtransfer/pattern.c) on the host,
 * so the test can compare its sectors byte for byte with the host checker's
 * own copy of the layout.
 *
 * Usage:
 *   sdtransfer_pattern_host sectors <run> <pass> <first> <count> <total> <request>
 *       Writes the pattern sectors first to first + count - 1 of a file with
 *       total sectors to standard output. Numbers may be decimal or 0x hex.
 *   sdtransfer_pattern_host crc <text>
 *       Prints the CRC-32 of text in hex.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pattern.h"

static uint32_t
__Number(
    _In_ const char* text)
{
    char*         end;
    unsigned long value = strtoul(text, &end, 0);

    if (end == text || *end != '\0' || value > 0xFFFFFFFFUL) {
        fprintf(stderr, "bad number: %s\n", text);
        exit(2);
    }
    return (uint32_t)value;
}

int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    TransferPatternSector_t sector;
    uint8_t                 data[TRANSFER_PATTERN_SECTOR_SIZE];
    uint32_t                first;
    uint32_t                count;
    uint32_t                i;

    if (argc == 3 && !strcmp(argv[1], "crc")) {
        printf("%08x\n", TransferPatternChecksum((const uint8_t*)argv[2], strlen(argv[2])));
        return 0;
    }
    if (argc != 8 || strcmp(argv[1], "sectors")) {
        fprintf(stderr, "usage: %s sectors <run> <pass> <first> <count> <total> <request> | crc <text>\n", argv[0]);
        return 2;
    }

    sector.RunId = __Number(argv[2]);
    sector.Pass = __Number(argv[3]);
    first = __Number(argv[4]);
    count = __Number(argv[5]);
    sector.SectorCount = __Number(argv[6]);
    sector.RequestBytes = __Number(argv[7]);
    for (i = 0; i < count; i++) {
        sector.SectorIndex = first + i;
        TransferPatternFill(&sector, data);
        if (fwrite(data, 1, sizeof(data), stdout) != sizeof(data)) {
            fprintf(stderr, "writing sector %u failed\n", first + i);
            return 1;
        }
    }
    return 0;
}
