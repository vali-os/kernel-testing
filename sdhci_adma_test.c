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
 * Host test for the SDHCI ADMA2 descriptor builder. The real
 * modules/storage/sdhci/adma.c is built against plain memory. Every check
 * reads the encoded table bytes directly, so a wrong byte order, length or
 * attribute fails even when the return code is right. The response field
 * helper from modules/storage/sd/host.h is checked here as well.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sd/host.h"
#include "sdhci/adma.h"

#define PAGE_SIZE      4096U
#define ONE_MIB        (1024U * 1024U)
#define TABLE_ADDRESS  0x80000000ULL
#define MAX_SEGMENTS   600U

// The CPU view of the table. The extra bytes after it catch writes past the end.
#define GUARD_SIZE 64U
static uint8_t           g_memory[PAGE_SIZE + GUARD_SIZE];
static struct DMASegment g_segments[MAX_SEGMENTS];

static const enum SdhciAdmaFormat g_formats[] = { SdhciAdmaFormat32, SdhciAdmaFormat64 };

static void
__InitializeTable(
    _Out_ SdhciAdmaTable_t*    table,
    _In_  enum SdhciAdmaFormat format,
    _In_  size_t               size)
{
    memset(g_memory, 0xA5, sizeof(g_memory));
    assert(SdhciAdmaTableInitialize(table, format, g_memory, size, TABLE_ADDRESS) == OS_EOK);
}

static void
__CheckGuard(void)
{
    unsigned int i;

    for (i = 0; i < GUARD_SIZE; i++) {
        assert(g_memory[PAGE_SIZE + i] == 0xA5);
    }
}

static uint32_t
__ExpectedLe32(
    _In_ const uint8_t* bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// Checks one entry byte by byte. Every entry must be a valid transfer entry,
// and only the last one may carry the end attribute.
static void
__CheckEntry(
    _In_ const SdhciAdmaTable_t* table,
    _In_ uint32_t                index,
    _In_ uint64_t                address,
    _In_ uint32_t                length,
    _In_ bool                    last)
{
    const uint8_t*   entry = table->Entries + (size_t)index * SdhciAdmaEntrySize(table->Format);
    uint8_t          attributes = 0x21;
    SdhciAdmaEntry_t decoded;

    if (last) {
        attributes |= 0x02;
    }
    assert(length != 0 && length <= SDHCI_ADMA_ENTRY_MAXIMUM_LENGTH);
    assert(entry[0] == attributes);
    assert(entry[1] == 0);
    assert(entry[2] == (uint8_t)(length & 0xFF));
    assert(entry[3] == (uint8_t)(length >> 8));
    assert(entry[2] != 0 || entry[3] != 0);
    assert(__ExpectedLe32(entry + 4) == (uint32_t)address);
    if (table->Format == SdhciAdmaFormat64) {
        assert(__ExpectedLe32(entry + 8) == (uint32_t)(address >> 32));
    } else {
        assert((address >> 32) == 0);
    }

    // The decoder must agree with the raw bytes.
    assert(SdhciAdmaTableReadEntry(table, index, &decoded) == OS_EOK);
    assert(decoded.Attributes == attributes);
    assert(decoded.LengthField == length);
    assert(decoded.Address == address);
}

// Walks the segments the way the hardware should see them and checks that the
// table holds exactly those bytes, cut at 65,532 bytes.
static void
__CheckTableMatches(
    _In_ const SdhciAdmaTable_t*  table,
    _In_ const struct DMASegment* segments,
    _In_ uint32_t                 segmentCount,
    _In_ uint32_t                 expectedEntries)
{
    uint32_t index = 0;
    uint32_t i;

    assert(table->EntryCount == expectedEntries);
    for (i = 0; i < segmentCount; i++) {
        uint64_t address = segments[i].Address;
        uint64_t remaining = segments[i].Length;

        while (remaining > 0) {
            uint32_t piece = remaining > 65532U ? 65532U : (uint32_t)remaining;

            assert(index < expectedEntries);
            __CheckEntry(table, index, address, piece, index + 1U == expectedEntries);
            index++;
            address += piece;
            remaining -= piece;
        }
    }
    assert(index == expectedEntries);
    __CheckGuard();
}

static void
TestEntrySizesAndCapacity(void)
{
    SdhciAdmaTable_t table;
    unsigned int     i;

    assert(SdhciAdmaEntrySize(SdhciAdmaFormat32) == 8);
    assert(SdhciAdmaEntrySize(SdhciAdmaFormat64) == 12);
    assert(SdhciAdmaEntrySize((enum SdhciAdmaFormat)7) == 0);

    // One page gives 512 and 341 entries.
    __InitializeTable(&table, SdhciAdmaFormat32, PAGE_SIZE);
    assert(table.Capacity == 512 && table.EntryCount == 0 && table.Size == PAGE_SIZE);
    assert(table.DeviceAddress == TABLE_ADDRESS && table.Entries == g_memory);
    for (i = 0; i < PAGE_SIZE; i++) {
        assert(g_memory[i] == 0);
    }
    __CheckGuard();
    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    assert(table.Capacity == 341);

    // A size that is not a multiple of the entry size rounds down.
    __InitializeTable(&table, SdhciAdmaFormat32, 100);
    assert(table.Capacity == 12);
    __InitializeTable(&table, SdhciAdmaFormat64, 100);
    assert(table.Capacity == 8);
    __InitializeTable(&table, SdhciAdmaFormat64, 12);
    assert(table.Capacity == 1);

    // Too small for one entry, NULL pointers and an unknown format.
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, 7, TABLE_ADDRESS) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat64, g_memory, 11, TABLE_ADDRESS) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(NULL, SdhciAdmaFormat32, g_memory, PAGE_SIZE, TABLE_ADDRESS) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, NULL, PAGE_SIZE, TABLE_ADDRESS) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(&table, (enum SdhciAdmaFormat)7, g_memory, PAGE_SIZE, TABLE_ADDRESS) == OS_EINVALPARAMS);

    // Table alignment: 4 bytes for the 32-bit format, 8 for the 64-bit one.
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, PAGE_SIZE, 0x1002) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, PAGE_SIZE, 0x1004) == OS_EOK);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat64, g_memory, PAGE_SIZE, 0x1004) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat64, g_memory, PAGE_SIZE, 0x1008) == OS_EOK);

    // The table address must fit the format, and must not wrap.
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, PAGE_SIZE, 0xFFFFF000ULL) == OS_EOK);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, PAGE_SIZE, 0xFFFFF008ULL) == OS_ENOTSUPPORTED);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat32, g_memory, PAGE_SIZE, 0x100000000ULL) == OS_ENOTSUPPORTED);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat64, g_memory, PAGE_SIZE, 0x100000000ULL) == OS_EOK);
    assert(SdhciAdmaTableInitialize(&table, SdhciAdmaFormat64, g_memory, PAGE_SIZE, UINT64_MAX - 7U) == OS_EOVERFLOW);
    printf("ADMA entry sizes, capacity and table checks passed\n");
}

static void
TestSingleSegments(void)
{
    static const uint32_t lengths[] = { 8, 64, 512 };
    SdhciAdmaTable_t      table;
    unsigned int          f;
    unsigned int          i;

    for (f = 0; f < 2; f++) {
        for (i = 0; i < 3; i++) {
            struct DMASegment segment = { .Address = 0x10000 + i * 0x1000, .Length = lengths[i] };

            __InitializeTable(&table, g_formats[f], PAGE_SIZE);
            assert(SdhciAdmaTableBuild(&table, &segment, 1, lengths[i], lengths[i]) == OS_EOK);
            __CheckTableMatches(&table, &segment, 1, 1);

            // The entry after the last must not be valid.
            assert(table.Entries[SdhciAdmaEntrySize(g_formats[f])] == 0);
        }
    }
    printf("ADMA single segment transfers passed\n");
}

static void
TestFragmented(void)
{
    struct DMASegment segments[] = {
        { .Address = 0x3000, .Length = 100 },
        { .Address = 0x8F9C, .Length = 924 },
        { .Address = 0x20000, .Length = 1024 }
    };
    SdhciAdmaTable_t table;
    unsigned int     f;

    for (f = 0; f < 2; f++) {
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, segments, 3, 2048, 512) == OS_EOK);
        __CheckTableMatches(&table, segments, 3, 3);
    }
    printf("ADMA fragmented transfer passed\n");
}

static void
TestFullyFragmentedMegabyte(void)
{
    SdhciAdmaTable_t table;
    unsigned int     f;
    uint32_t         i;

    for (f = 0; f < 2; f++) {
        // 256 pages, none of them next to each other.
        for (i = 0; i < 256; i++) {
            g_segments[i].Address = 0x40000000ULL + (uint64_t)i * 0x3000;
            g_segments[i].Length = PAGE_SIZE;
        }
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, g_segments, 256, ONE_MIB, 512) == OS_EOK);
        __CheckTableMatches(&table, g_segments, 256, 256);

        // The same chunk starting 512 bytes into a page needs one more entry.
        g_segments[0].Address += 0x200;
        g_segments[0].Length = PAGE_SIZE - 0x200;
        g_segments[256].Address = 0x40000000ULL + 256ULL * 0x3000;
        g_segments[256].Length = 0x200;
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, g_segments, 257, ONE_MIB, 512) == OS_EOK);
        __CheckTableMatches(&table, g_segments, 257, 257);
    }
    printf("ADMA fully fragmented 1 MiB chunk passed\n");
}

static void
TestSplitting(void)
{
    SdhciAdmaTable_t  table;
    struct DMASegment segment;
    unsigned int      f;
    uint32_t          i;

    for (f = 0; f < 2; f++) {
        // Exactly the largest piece: one entry.
        segment = (struct DMASegment) { .Address = 0x100000, .Length = 65532 };
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, &segment, 1, 65532, 4) == OS_EOK);
        __CheckEntry(&table, 0, 0x100000, 65532, true);
        assert(table.EntryCount == 1);

        // 65,536 bytes must not use the zero length encoding.
        segment.Length = 65536;
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, &segment, 1, 65536, 512) == OS_EOK);
        assert(table.EntryCount == 2);
        __CheckEntry(&table, 0, 0x100000, 65532, false);
        __CheckEntry(&table, 1, 0x100000 + 65532, 4, true);

        segment.Length = 65540;
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, &segment, 1, 65540, 4) == OS_EOK);
        assert(table.EntryCount == 2);
        __CheckEntry(&table, 0, 0x100000, 65532, false);
        __CheckEntry(&table, 1, 0x100000 + 65532, 8, true);

        // 1 MiB contiguous: 16 full pieces and 64 bytes left over.
        segment.Length = ONE_MIB;
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);
        assert(SdhciAdmaTableBuild(&table, &segment, 1, ONE_MIB, 512) == OS_EOK);
        assert(table.EntryCount == 17);
        for (i = 0; i < 16; i++) {
            __CheckEntry(&table, i, 0x100000 + (uint64_t)i * 65532, 65532, false);
        }
        __CheckEntry(&table, 16, 0x100000 + 16ULL * 65532, 64, true);
        __CheckGuard();
    }
    printf("ADMA splitting at 65,532 bytes passed\n");
}

static void
TestAddressLimits(void)
{
    SdhciAdmaTable_t  table;
    struct DMASegment segments[2];

    // Pi 4 device addresses start at 0xc0000000 and must pass unchanged.
    segments[0] = (struct DMASegment) { .Address = 0xC0000000ULL, .Length = 512 };
    segments[1] = (struct DMASegment) { .Address = 0xC0001000ULL, .Length = 512 };
    __InitializeTable(&table, SdhciAdmaFormat32, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, segments, 2, 1024, 512) == OS_EOK);
    __CheckTableMatches(&table, segments, 2, 2);
    assert(table.Entries[7] == 0xC0 && table.Entries[6] == 0 && table.Entries[15] == 0xC0);

    // A segment ending exactly at 4 GiB fits the 32-bit format.
    segments[0] = (struct DMASegment) { .Address = 0xFFFFFE00ULL, .Length = 512 };
    __InitializeTable(&table, SdhciAdmaFormat32, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, segments, 1, 512, 512) == OS_EOK);
    __CheckEntry(&table, 0, 0xFFFFFE00ULL, 512, true);

    // Crossing 4 GiB, or starting above it, does not.
    segments[0] = (struct DMASegment) { .Address = 0xFFFFFE00ULL, .Length = 1024 };
    assert(SdhciAdmaTableBuild(&table, segments, 1, 1024, 512) == OS_ENOTSUPPORTED);
    assert(table.EntryCount == 0);
    segments[0] = (struct DMASegment) { .Address = 0x100000000ULL, .Length = 512 };
    assert(SdhciAdmaTableBuild(&table, segments, 1, 512, 512) == OS_ENOTSUPPORTED);
    segments[0] = (struct DMASegment) { .Address = 0x1000, .Length = 512 };
    segments[1] = (struct DMASegment) { .Address = 0x1FFFFFE00ULL, .Length = 512 };
    assert(SdhciAdmaTableBuild(&table, segments, 2, 1024, 512) == OS_ENOTSUPPORTED);

    // The 64-bit format takes addresses above 4 GiB and stores the high word.
    segments[0] = (struct DMASegment) { .Address = 0x123456780ULL, .Length = 512 };
    segments[1] = (struct DMASegment) { .Address = 0xFFFFFFFFFFFFFE00ULL, .Length = 512 };
    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, segments, 2, 1024, 512) == OS_EOK);
    __CheckTableMatches(&table, segments, 2, 2);
    assert(table.Entries[8] == 0x01 && table.Entries[9] == 0 && table.Entries[10] == 0 && table.Entries[11] == 0);
    assert(table.Entries[20] == 0xFF && table.Entries[23] == 0xFF);
    printf("ADMA 4 GiB limits passed\n");
}

// Checks that a failed build cleared the first entry, set EntryCount to zero
// and left every other entry of the earlier build alone.
static void
__CheckFailedBuild(
    _In_ SdhciAdmaTable_t*        table,
    _In_ const struct DMASegment* segments,
    _In_ uint32_t                 segmentCount,
    _In_ uint64_t                 length,
    _In_ uint32_t                 blockSize,
    _In_ oserr_t                  expected)
{
    struct DMASegment earlier[2] = {
        { .Address = 0x5000, .Length = 512 },
        { .Address = 0x7000, .Length = 512 }
    };
    size_t            entrySize = SdhciAdmaEntrySize(table->Format);
    unsigned int      i;

    assert(SdhciAdmaTableBuild(table, earlier, 2, 1024, 512) == OS_EOK);
    assert(SdhciAdmaTableBuild(table, segments, segmentCount, length, blockSize) == expected);
    assert(table->EntryCount == 0);
    for (i = 0; i < entrySize; i++) {
        assert(table->Entries[i] == 0);
    }
    __CheckEntry(table, 1, 0x7000, 512, true);
}

static void
TestRejections(void)
{
    SdhciAdmaTable_t  table;
    struct DMASegment segments[2];
    unsigned int      f;
    uint32_t          i;

    for (f = 0; f < 2; f++) {
        __InitializeTable(&table, g_formats[f], PAGE_SIZE);

        // Unaligned address and unaligned length.
        segments[0] = (struct DMASegment) { .Address = 0x1002, .Length = 512 };
        __CheckFailedBuild(&table, segments, 1, 512, 512, OS_EINVALPARAMS);
        segments[0] = (struct DMASegment) { .Address = 0x1000, .Length = 510 };
        segments[1] = (struct DMASegment) { .Address = 0x2000, .Length = 2 };
        __CheckFailedBuild(&table, segments, 2, 512, 512, OS_EINVALPARAMS);

        // A zero-length segment, even when the total still adds up.
        segments[0] = (struct DMASegment) { .Address = 0x1000, .Length = 0 };
        segments[1] = (struct DMASegment) { .Address = 0x2000, .Length = 512 };
        __CheckFailedBuild(&table, segments, 2, 512, 512, OS_EINVALPARAMS);

        // Total length that differs from the stated length, both ways.
        segments[0] = (struct DMASegment) { .Address = 0x1000, .Length = 512 };
        __CheckFailedBuild(&table, segments, 1, 1024, 512, OS_EINVALPARAMS);
        segments[0].Length = 1024;
        __CheckFailedBuild(&table, segments, 1, 512, 512, OS_EINVALPARAMS);

        // Length not a multiple of the block size, zero length, zero block size.
        segments[0].Length = 516;
        __CheckFailedBuild(&table, segments, 1, 516, 512, OS_EINVALPARAMS);
        __CheckFailedBuild(&table, segments, 1, 0, 512, OS_EINVALPARAMS);
        __CheckFailedBuild(&table, segments, 1, 516, 0, OS_EINVALPARAMS);

        // No segments at all.
        __CheckFailedBuild(&table, segments, 0, 512, 512, OS_EINVALPARAMS);
        __CheckFailedBuild(&table, NULL, 1, 512, 512, OS_EINVALPARAMS);
        assert(SdhciAdmaTableBuild(NULL, segments, 1, 512, 512) == OS_EINVALPARAMS);
    }

    // A segment that runs past the top of the 64-bit address space.
    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    segments[0] = (struct DMASegment) { .Address = UINT64_MAX - 511U, .Length = 1024 };
    __CheckFailedBuild(&table, segments, 1, 1024, 512, OS_EOVERFLOW);

    // Two segments whose lengths add up past UINT64_MAX.
    segments[0] = (struct DMASegment) { .Address = 0, .Length = 0x8000000000000000ULL };
    segments[1] = (struct DMASegment) { .Address = 0, .Length = 0x8000000000000000ULL };
    __CheckFailedBuild(&table, segments, 2, 512, 512, OS_EOVERFLOW);

    // Table exhaustion by one entry. 512 entries fit a 32-bit page, 513 do not.
    for (i = 0; i < 513; i++) {
        g_segments[i].Address = 0x1000000ULL + (uint64_t)i * 0x1000;
        g_segments[i].Length = 512;
    }
    __InitializeTable(&table, SdhciAdmaFormat32, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, g_segments, 512, 512U * 512U, 512) == OS_EOK);
    __CheckTableMatches(&table, g_segments, 512, 512);
    __CheckFailedBuild(&table, g_segments, 513, 513U * 512U, 512, OS_EBUFFER);
    __CheckGuard();

    // 341 entries fit a 64-bit page, 342 do not.
    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, g_segments, 341, 341U * 512U, 512) == OS_EOK);
    __CheckTableMatches(&table, g_segments, 341, 341);
    __CheckFailedBuild(&table, g_segments, 342, 342U * 512U, 512, OS_EBUFFER);
    __CheckGuard();

    // Exhaustion caused by splitting: three pieces in a two-entry table.
    __InitializeTable(&table, SdhciAdmaFormat32, 16);
    segments[0] = (struct DMASegment) { .Address = 0x100000, .Length = 65532U * 2U + 4U };
    __CheckFailedBuild(&table, segments, 1, 65532U * 2U + 4U, 4, OS_EBUFFER);
    segments[0].Length = 65532U * 2U;
    assert(SdhciAdmaTableBuild(&table, segments, 1, 65532U * 2U, 4) == OS_EOK);
    assert(table.EntryCount == 2);
    printf("ADMA rejections passed\n");
}

static void
TestByteOrder(void)
{
    static const uint8_t expected[12] = {
        0x23, 0x00, 0x34, 0x12, 0xEC, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01
    };
    struct DMASegment segment = { .Address = 0x0123456789ABCDECULL, .Length = 0x1234 };
    SdhciAdmaTable_t  table;
    unsigned int      i;

    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, &segment, 1, 0x1234, 4) == OS_EOK);
    for (i = 0; i < 12; i++) {
        assert(table.Entries[i] == expected[i]);
    }

    segment.Address = 0x89ABCDECULL;
    __InitializeTable(&table, SdhciAdmaFormat32, PAGE_SIZE);
    assert(SdhciAdmaTableBuild(&table, &segment, 1, 0x1234, 4) == OS_EOK);
    for (i = 0; i < 8; i++) {
        assert(table.Entries[i] == expected[i]);
    }
    // The next 32-bit entry starts right after, and is still clear.
    for (i = 8; i < 16; i++) {
        assert(table.Entries[i] == 0);
    }
    printf("ADMA little-endian encoding passed\n");
}

static void
TestFindEntry(void)
{
    SdhciAdmaTable_t table;
    SdhciAdmaEntry_t entry;
    uint32_t         index;

    __InitializeTable(&table, SdhciAdmaFormat64, PAGE_SIZE);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS, &index) == OS_EOK && index == 0);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS + 3 * 12, &index) == OS_EOK && index == 3);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS + 340 * 12, &index) == OS_EOK && index == 340);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS + 341 * 12, &index) == OS_ENOENT);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS + 8, &index) == OS_ENOENT);
    assert(SdhciAdmaTableFindEntry(&table, TABLE_ADDRESS - 12, &index) == OS_ENOENT);
    assert(SdhciAdmaTableFindEntry(NULL, TABLE_ADDRESS, &index) == OS_EINVALPARAMS);
    assert(SdhciAdmaTableReadEntry(&table, 340, &entry) == OS_EOK);
    assert(entry.Attributes == 0 && entry.LengthField == 0 && entry.Address == 0);
    assert(SdhciAdmaTableReadEntry(&table, 341, &entry) == OS_EINVALPARAMS);
    printf("ADMA entry lookup passed\n");
}

// Bit by bit reference for SdResponseBits.
static uint32_t
__ReferenceBits(
    _In_ const uint32_t response[4],
    _In_ unsigned int   start,
    _In_ unsigned int   width)
{
    uint32_t     value = 0;
    unsigned int i;

    for (i = 0; i < width; i++) {
        unsigned int bit = start + i;
        uint32_t     word = response[3U - bit / 32U];

        value |= ((word >> (bit % 32U)) & 1U) << i;
    }
    return value;
}

static void
TestResponseHelpers(void)
{
    static const uint32_t response[4] = { 0x12345678, 0x9ABCDEF0, 0x0FEDCBA9, 0x87654300 };
    unsigned int          start;
    unsigned int          width;

    assert(SdResponseBits(response, 96, 32) == 0x12345678);
    assert(SdResponseBits(response, 126, 2) == 0);
    assert(SdResponseBits(response, 8, 8) == 0x43);
    assert(SdResponseBits(response, 88, 16) == 0x789A);
    for (width = 1; width <= 32; width++) {
        for (start = 0; start + width <= 128; start++) {
            assert(SdResponseBits(response, start, width) == __ReferenceBits(response, start, width));
        }
    }

    assert(!SdResponseHasCrc(SdResponseNone) && !SdResponseHasCrc(SdResponseR3));
    assert(SdResponseHasCrc(SdResponseR1) && SdResponseHasCrc(SdResponseR2) && SdResponseHasCrc(SdResponseR7));
    assert(SdResponseHasIndex(SdResponseR1b) && SdResponseHasIndex(SdResponseR6));
    assert(!SdResponseHasIndex(SdResponseR2) && !SdResponseHasIndex(SdResponseR3));
    assert(SdResponseHasBusy(SdResponseR1b) && !SdResponseHasBusy(SdResponseR1));
    assert(SdResponseIsLong(SdResponseR2) && !SdResponseIsLong(SdResponseR7));
    printf("SD response helpers passed\n");
}

int
main(void)
{
    TestEntrySizesAndCapacity();
    TestSingleSegments();
    TestFragmented();
    TestFullyFragmentedMegabyte();
    TestSplitting();
    TestAddressLimits();
    TestRejections();
    TestByteOrder();
    TestFindEntry();
    TestResponseHelpers();
    printf("SDHCI ADMA tests passed\n");
    return 0;
}
