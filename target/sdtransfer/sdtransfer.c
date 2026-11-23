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
 * SD transfer test. A test program, never part of the normal system: it is
 * built by testing/sdtransfer_image.py and only goes into test images.
 *
 * It runs as a system service, because services are the only programs the
 * system starts by itself during boot. processd starts every service it finds
 * in the initial ramdisk and hands it no arguments other than its API path
 * (services/processd/services/discover.c). The test arguments are therefore
 * built into the program as one text line, TRANSFER_ARGUMENTS, which the image
 * builder sets. The program parses that line like a command line, and every
 * option has a default.
 *
 * What one run does:
 * 1. Wait until a file can be created in the test directory. The directory is
 *    on a disk that filed mounts some time after the services start.
 * 2. For each pass: create the file again (truncated), write it from start to
 *    end in requests of the pass's size, close it, open it again, read it
 *    back in the same request size and compare every sector with the pattern
 *    (pattern.h). The request sizes come from a list, one per pass in turn.
 * 3. Log one result line per direction and pass, and a final PASS or FAIL.
 *
 * The file of the last pass stays on the disk, so the card can be checked on
 * another machine with testing/sdtransfer_check.py.
 *
 * The program talks to filed directly (OSOpenPath and OSTransferFile) with
 * its own shared memory buffer, instead of using read and write. The C
 * library splits every read and write into requests of at most 64 KiB
 * (librt/libc/io/open.c). With its own buffer, a 1 MiB request reaches filed
 * and MFS as one request, and MFS reads and writes it straight from this
 * buffer, so the SD driver gets chunks of up to 1 MiB.
 *
 * Result lines. All start with "sdtransfer: ", and each one stays short,
 * because the system log cuts a message at about 128 characters:
 *   start run 0x<run>, file <path>, <bytes> bytes, <passes> passes
 *   pass <p> write <bytes> bytes in <request> B requests, <s>.<us> s, <m>.<mmm> MiB/s
 *   pass <p> read <bytes> bytes in <request> B requests, <s>.<us> s, <m>.<mmm> MiB/s, <n> mismatches
 *   mismatch pass <p> sector <i> (offset <o>) byte <b>: read <hex> expected <hex>
 *   mismatch pass <p> sector <i> holds marker <ok|bad>, run 0x<r>, pass <p>, index <i>
 *   file <path> left with run 0x<run> pass <p>, <sectors> sectors
 *   result PASS|FAIL, run 0x<run>, <passes> passes, <n> mismatches, <e> errors
 *   pass <p> open refused with <e> for the <write|read>, retry <n>
 * A failed call is logged as "FAIL <what failed>: <error code>". The open
 * retry is a WARNING, see __OpenFile (pass 0 is the wait for the directory).
 * The seconds count only the time spent in OSTransferFile, not the time
 * spent filling and comparing the buffer.
 */

#define __need_minmax
#include <ddk/service.h>
#include <ddk/utils.h>
#include <os/handle.h>
#include <os/services/file.h>
#include <os/shm.h>
#include <os/time.h>
#include <os/usched/job.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pattern.h"

// The test arguments, set by the image builder with -DTRANSFER_ARGUMENTS.
// Options:
//   --directory PATH     Directory for the test file. Default /data.
//   --file-name NAME     Name of the test file. Default sdtransfer.bin.
//   --file-mib N         File size in MiB. Default 64.
//   --request-kib LIST   Request sizes in KiB, comma separated, one per pass
//                        in turn. Default 4,64,1024.
//   --passes N           Number of passes. Default 3.
//   --wait-seconds N     How long to wait for the directory. Default 600.
#ifndef TRANSFER_ARGUMENTS
#define TRANSFER_ARGUMENTS ""
#endif

#define __DEFAULT_DIRECTORY    "/data"
#define __DEFAULT_FILE_NAME    "sdtransfer.bin"
#define __DEFAULT_FILE_MIB     64U
#define __DEFAULT_PASSES       3U
#define __DEFAULT_WAIT_SECONDS 600U

// Limits of the options. A request size is a whole number of sectors and at
// most 16 MiB, which is the size of the program's transfer buffer at most.
// The file size stays below 4 GiB, because the sector index of the pattern
// is a 32-bit word.
#define __MAX_REQUEST_SIZES  8
#define __MAX_REQUEST_BYTES  (16U * 1024U * 1024U)
#define __MAX_FILE_MIB       4095U
#define __MAX_PASSES         1000U
#define __MAX_ARGUMENT_TEXT  256

// How many mismatching sectors each pass logs in detail. Every mismatch is
// counted, but the log would drown in a pass where everything is wrong.
#define __MISMATCH_LOG_LIMIT 8

// Bytes shown from the first differing byte on, in a mismatch line.
#define __MISMATCH_SHOW_BYTES 8

// An open refused because filed has not yet closed the previous handle is
// tried again this often, this far apart (see __OpenFile). 50 tries of 100 ms
// give filed 5 seconds.
#define __OPEN_RETRY_LIMIT    50U
#define __OPEN_RETRY_DELAY_MS 100U

// The wait for the test directory polls once a second.
#define __WAIT_POLL_MS 1000U

#define __NSEC_PER_SEC 1000000000ULL
#define __BYTES_PER_MIB (1024ULL * 1024ULL)

/**
 * @brief The test options after parsing TRANSFER_ARGUMENTS.
 */
typedef struct TransferOptions {
    char     Directory[128];
    char     FileName[64];
    uint64_t FileBytes;
    uint32_t RequestBytes[__MAX_REQUEST_SIZES];
    unsigned RequestSizeCount;
    unsigned Passes;
    unsigned WaitSeconds;
} TransferOptions_t;

/**
 * @brief Everything one run of the test uses. Buffer and Access are set up
 * once and used by every pass. Expected holds the pattern of one request,
 * which the read check compares the buffer with.
 */
typedef struct TransferTest {
    TransferOptions_t       Options;
    char                    Path[200];
    uint32_t                RunId;
    uint32_t                SectorCount;
    size_t                  BufferBytes;
    OSHandle_t              Buffer;
    struct SHMAccessSession Access;
    uint8_t*                Expected;
    uint64_t                Mismatches;
    unsigned                Errors;
} TransferTest_t;

// Direction of one pass step, used to pick the open flags and the log text.
enum __TransferDirection {
    __TransferDirectionWrite,
    __TransferDirectionRead
};

// Reads the monotonic clock in nanoseconds since boot.
static oserr_t
__Now(
    _Out_ uint64_t* nanosecondsOut)
{
    OSTimestamp_t now;
    oserr_t       oserr;

    oserr = OSGetTime(OSTimeSource_MONOTONIC, &now);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL reading the monotonic clock: %u", oserr);
        return oserr;
    }
    *nanosecondsOut = ((uint64_t)now.Seconds * __NSEC_PER_SEC) + (uint64_t)now.Nanoseconds;
    return OS_EOK;
}

// Sleeps the calling job for the given number of milliseconds.
static void
__Sleep(
    _In_ unsigned milliseconds)
{
    struct timespec until;

    if (timespec_get(&until, TIME_UTC) != TIME_UTC) {
        ERROR("sdtransfer: reading the UTC clock for a sleep failed");
        return;
    }
    until.tv_sec += milliseconds / 1000U;
    until.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (until.tv_nsec >= (long)__NSEC_PER_SEC) {
        until.tv_sec++;
        until.tv_nsec -= (long)__NSEC_PER_SEC;
    }
    if (usched_job_sleep(&until) != 0) {
        ERROR("sdtransfer: sleeping %u ms failed", milliseconds);
    }
}

// Reads a decimal number of at least minimum and at most maximum. Returns 0
// on success.
static int
__ParseNumber(
    _In_  const char* text,
    _In_  unsigned    minimum,
    _In_  unsigned    maximum,
    _Out_ unsigned*   valueOut)
{
    unsigned long value;
    char*         end;

    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) {
        return -1;
    }
    *valueOut = (unsigned)value;
    return 0;
}

// Reads the comma separated list of request sizes in KiB. Each size must be
// a whole number of sectors (KiB always is) and at most __MAX_REQUEST_BYTES.
static int
__ParseRequestSizes(
    _In_  char*              text,
    _Out_ TransferOptions_t* options)
{
    unsigned count = 0;
    unsigned kib;
    char*    item;
    char*    next;

    item = text;
    while (item != NULL) {
        next = strchr(item, ',');
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        if (count == __MAX_REQUEST_SIZES ||
            __ParseNumber(item, 1, __MAX_REQUEST_BYTES / 1024U, &kib)) {
            return -1;
        }
        options->RequestBytes[count] = kib * 1024U;
        count++;
        item = next;
    }
    options->RequestSizeCount = count;
    return 0;
}

// Copies text into a fixed buffer. Returns 0 when it fits.
static int
__CopyText(
    _Out_ char*       target,
    _In_  size_t      capacity,
    _In_  const char* text)
{
    size_t length = strlen(text);

    if (length == 0 || length >= capacity) {
        return -1;
    }
    memcpy(target, text, length + 1);
    return 0;
}

// Splits arguments at spaces and fills options. Options that are not given
// keep their defaults. Returns 0 on success, and logs the bad option on
// failure.
static int
__ParseArguments(
    _In_  const char*        arguments,
    _Out_ TransferOptions_t* options)
{
    char     text[__MAX_ARGUMENT_TEXT];
    char*    tokens[32];
    unsigned tokenCount = 0;
    unsigned fileMib = __DEFAULT_FILE_MIB;
    unsigned i;
    char*    cursor;
    int      status;

    memset(options, 0, sizeof(TransferOptions_t));
    strcpy(options->Directory, __DEFAULT_DIRECTORY);
    strcpy(options->FileName, __DEFAULT_FILE_NAME);
    options->RequestBytes[0] = 4U * 1024U;
    options->RequestBytes[1] = 64U * 1024U;
    options->RequestBytes[2] = 1024U * 1024U;
    options->RequestSizeCount = 3;
    options->Passes = __DEFAULT_PASSES;
    options->WaitSeconds = __DEFAULT_WAIT_SECONDS;

    if (strlen(arguments) >= sizeof(text)) {
        ERROR("sdtransfer: FAIL argument line is longer than %u characters", (unsigned)sizeof(text) - 1);
        return -1;
    }
    strcpy(text, arguments);

    // Cut the line into words. Each word ends at a space or the end.
    cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            *cursor = '\0';
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (tokenCount == sizeof(tokens) / sizeof(tokens[0])) {
            ERROR("sdtransfer: FAIL argument line has more than %u words", tokenCount);
            return -1;
        }
        tokens[tokenCount++] = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }
    }

    // Every option takes exactly one value.
    for (i = 0; i < tokenCount; i += 2) {
        if (i + 1 >= tokenCount) {
            ERROR("sdtransfer: FAIL option %s has no value", tokens[i]);
            return -1;
        }
        if (!strcmp(tokens[i], "--directory")) {
            status = __CopyText(options->Directory, sizeof(options->Directory), tokens[i + 1]);
        } else if (!strcmp(tokens[i], "--file-name")) {
            status = __CopyText(options->FileName, sizeof(options->FileName), tokens[i + 1]);
        } else if (!strcmp(tokens[i], "--file-mib")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_FILE_MIB, &fileMib);
        } else if (!strcmp(tokens[i], "--request-kib")) {
            status = __ParseRequestSizes(tokens[i + 1], options);
        } else if (!strcmp(tokens[i], "--passes")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_PASSES, &options->Passes);
        } else if (!strcmp(tokens[i], "--wait-seconds")) {
            status = __ParseNumber(tokens[i + 1], 0, 86400, &options->WaitSeconds);
        } else {
            ERROR("sdtransfer: FAIL unknown option %s", tokens[i]);
            return -1;
        }
        if (status) {
            ERROR("sdtransfer: FAIL bad value for %s: %s", tokens[i], tokens[i + 1]);
            return -1;
        }
    }
    options->FileBytes = (uint64_t)fileMib * __BYTES_PER_MIB;
    return 0;
}

// Prints up to __MISMATCH_SHOW_BYTES bytes as hex into text.
static void
__FormatBytes(
    _Out_ char*          text,
    _In_  const uint8_t* bytes,
    _In_  size_t         count)
{
    size_t i;

    for (i = 0; i < count && i < __MISMATCH_SHOW_BYTES; i++) {
        snprintf(&text[i * 2], 3, "%02x", bytes[i]);
    }
    text[i * 2] = '\0';
}

// Reads a little endian word of a sector that was read back.
static uint32_t
__LoadWord(
    _In_ const uint8_t* data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

// Logs one sector that does not hold what was written: where the first
// difference is, and what the sector holds instead. The second line helps
// to tell a misplaced sector (a valid pattern sector with another index or
// pass) from damaged data.
static void
__LogMismatch(
    _In_ unsigned       pass,
    _In_ uint32_t       sectorIndex,
    _In_ const uint8_t* readData,
    _In_ const uint8_t* expected)
{
    char   readText[(__MISMATCH_SHOW_BYTES * 2) + 1];
    char   expectedText[(__MISMATCH_SHOW_BYTES * 2) + 1];
    size_t first = 0;
    size_t shown;
    int    markerValid;

    while (first < TRANSFER_PATTERN_SECTOR_SIZE && readData[first] == expected[first]) {
        first++;
    }
    shown = TRANSFER_PATTERN_SECTOR_SIZE - first;
    __FormatBytes(readText, &readData[first], shown);
    __FormatBytes(expectedText, &expected[first], shown);
    ERROR("sdtransfer: mismatch pass %u sector %u (offset %llu) byte %u: read %s expected %s",
          pass, sectorIndex, (unsigned long long)sectorIndex * TRANSFER_PATTERN_SECTOR_SIZE,
          (unsigned)first, readText, expectedText);

    markerValid = __LoadWord(&readData[TRANSFER_PATTERN_OFFSET_MARKER_0]) == TRANSFER_PATTERN_MARKER_0 &&
                  __LoadWord(&readData[TRANSFER_PATTERN_OFFSET_MARKER_1]) == TRANSFER_PATTERN_MARKER_1;
    ERROR("sdtransfer: mismatch pass %u sector %u holds marker %s, run 0x%08x, pass %u, index %u",
          pass, sectorIndex, markerValid ? "ok" : "bad",
          __LoadWord(&readData[TRANSFER_PATTERN_OFFSET_RUN]),
          __LoadWord(&readData[TRANSFER_PATTERN_OFFSET_PASS]),
          __LoadWord(&readData[TRANSFER_PATTERN_OFFSET_INDEX]));
}

// Writes the pattern sectors of one request into target. firstSector is the
// file sector index of the first sector, length a whole number of sectors.
static void
__FillRequest(
    _In_  const TransferTest_t* test,
    _In_  unsigned              pass,
    _In_  uint32_t              requestBytes,
    _In_  uint32_t              firstSector,
    _In_  size_t                length,
    _Out_ uint8_t*              target)
{
    TransferPatternSector_t sector;
    size_t                  offset;

    sector.RunId = test->RunId;
    sector.Pass = pass;
    sector.SectorCount = test->SectorCount;
    sector.RequestBytes = requestBytes;
    for (offset = 0; offset < length; offset += TRANSFER_PATTERN_SECTOR_SIZE) {
        sector.SectorIndex = firstSector + (uint32_t)(offset / TRANSFER_PATTERN_SECTOR_SIZE);
        TransferPatternFill(&sector, &target[offset]);
    }
}

// Opens the test file. A write pass creates it again, empty. A read pass
// opens the file the write pass just closed.
//
// Closing a file only releases the program's handle. filed closes its own
// side later, when it learns that the handle is gone (librt/libos/services/
// file.c, OSOpenPath). Until then filed still counts the old handle, and
// both opens here ask for exclusive access, so an open right after a close
// can be refused with OS_EPERMISSIONS. The open is then tried again, up to
// __OPEN_RETRY_LIMIT times, __OPEN_RETRY_DELAY_MS apart. Each retry is logged
// with "open refused", so the runner can count them.
static oserr_t
__OpenFile(
    _In_  TransferTest_t*          test,
    _In_  unsigned                 pass,
    _In_  enum __TransferDirection direction,
    _Out_ OSHandle_t*              handleOut)
{
    unsigned int flags;
    unsigned     attempt;
    oserr_t      oserr;

    if (direction == __TransferDirectionWrite) {
        flags = __FILE_CREATE | __FILE_TRUNCATE | __FILE_BINARY | __FILE_READ_ACCESS | __FILE_WRITE_ACCESS;
    } else {
        flags = __FILE_BINARY | __FILE_READ_ACCESS;
    }

    for (attempt = 1; ; attempt++) {
        oserr = OSOpenPath(test->Path, flags, FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, handleOut);
        if (oserr != OS_EPERMISSIONS || attempt > __OPEN_RETRY_LIMIT) {
            return oserr;
        }
        WARNING("sdtransfer: pass %u open refused with %u for the %s, retry %u",
                pass, oserr, direction == __TransferDirectionWrite ? "write" : "read", attempt);
        __Sleep(__OPEN_RETRY_DELAY_MS);
    }
}

// Closes the test file and logs a failure. Returns the close result.
static oserr_t
__CloseFile(
    _In_    unsigned    pass,
    _In_    const char* step,
    _InOut_ OSHandle_t* handle)
{
    oserr_t oserr;

    oserr = OSHandleRelease(handle);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL pass %u closing the file after the %s: %u", pass, step, oserr);
    }
    return oserr;
}

// Computes MiB/s with three decimals as a whole number (MiB/s * 1000), so no
// floating point is needed.
static uint64_t
__MibPerSecondMilli(
    _In_ uint64_t bytes,
    _In_ uint64_t nanoseconds)
{
    if (nanoseconds == 0) {
        return 0;
    }
    return ((bytes * 1000ULL) / __BYTES_PER_MIB) * __NSEC_PER_SEC / nanoseconds;
}

// Logs the result line of one direction of a pass.
static void
__LogPassResult(
    _In_ unsigned                 pass,
    _In_ enum __TransferDirection direction,
    _In_ uint64_t                 bytes,
    _In_ uint32_t                 requestBytes,
    _In_ uint64_t                 nanoseconds,
    _In_ uint64_t                 mismatches)
{
    uint64_t milli = __MibPerSecondMilli(bytes, nanoseconds);

    if (direction == __TransferDirectionWrite) {
        NOTICE("sdtransfer: pass %u write %llu bytes in %u B requests, %llu.%06llu s, %llu.%03llu MiB/s",
               pass, bytes, requestBytes, nanoseconds / __NSEC_PER_SEC,
               (nanoseconds % __NSEC_PER_SEC) / 1000ULL, milli / 1000ULL, milli % 1000ULL);
    } else {
        NOTICE("sdtransfer: pass %u read %llu bytes in %u B requests, %llu.%06llu s, %llu.%03llu MiB/s, %llu mismatches",
               pass, bytes, requestBytes, nanoseconds / __NSEC_PER_SEC,
               (nanoseconds % __NSEC_PER_SEC) / 1000ULL, milli / 1000ULL, milli % 1000ULL, mismatches);
    }
}

// Moves one request between the buffer and the file at the file's current
// position, and adds the time OSTransferFile took to nanoseconds.
static oserr_t
__TransferRequest(
    _In_    TransferTest_t*          test,
    _In_    unsigned                 pass,
    _In_    enum __TransferDirection direction,
    _In_    uuid_t                   file,
    _In_    uint64_t                 offset,
    _In_    size_t                   length,
    _InOut_ uint64_t*                nanoseconds)
{
    const char* name = direction == __TransferDirectionWrite ? "write" : "read";
    uint64_t    started;
    uint64_t    ended;
    size_t      transferred = 0;
    oserr_t     oserr;

    oserr = __Now(&started);
    if (oserr != OS_EOK) {
        return oserr;
    }
    oserr = OSTransferFile(
        file,
        test->Buffer.ID,
        0,
        direction == __TransferDirectionWrite,
        length,
        &transferred
    );
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL pass %u %s of %u bytes at offset %llu: %u, %u bytes moved",
              pass, name, (unsigned)length, offset, oserr, (unsigned)transferred);
        return oserr;
    }
    if (transferred != length) {
        ERROR("sdtransfer: FAIL pass %u %s at offset %llu moved %u of %u bytes",
              pass, name, offset, (unsigned)transferred, (unsigned)length);
        return OS_EINCOMPLETE;
    }
    oserr = __Now(&ended);
    if (oserr != OS_EOK) {
        return oserr;
    }
    *nanoseconds += ended - started;
    return OS_EOK;
}

// Write half of a pass: creates the file and writes every sector of it.
static oserr_t
__WriteFile(
    _In_ TransferTest_t* test,
    _In_ unsigned        pass,
    _In_ uint32_t        requestBytes)
{
    OSHandle_t file;
    uint64_t   offset;
    uint64_t   nanoseconds = 0;
    size_t     length;
    oserr_t    oserr;
    oserr_t    closeStatus;
    uint8_t*   buffer = SHMBuffer(&test->Buffer);

    oserr = __OpenFile(test, pass, __TransferDirectionWrite, &file);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL pass %u creating %s: %u", pass, test->Path, oserr);
        return oserr;
    }

    for (offset = 0; offset < test->Options.FileBytes; offset += length) {
        length = (size_t)MIN((uint64_t)requestBytes, test->Options.FileBytes - offset);

        // The CPU writes the buffer only inside an access session, so it
        // never overlaps a device transfer of the same memory.
        oserr = SHMAccessBeginCpu(&test->Access, SHMCpuAccessWrite, SHMWaitUntilAvailable, NULL);
        if (oserr != OS_EOK) {
            ERROR("sdtransfer: FAIL pass %u starting CPU write access to the buffer: %u", pass, oserr);
            break;
        }
        __FillRequest(test, pass, requestBytes,
                      (uint32_t)(offset / TRANSFER_PATTERN_SECTOR_SIZE), length, buffer);
        oserr = SHMAccessEndCpu(&test->Access);
        if (oserr != OS_EOK) {
            ERROR("sdtransfer: FAIL pass %u ending CPU write access to the buffer: %u", pass, oserr);
            break;
        }

        oserr = __TransferRequest(test, pass, __TransferDirectionWrite, file.ID, offset, length, &nanoseconds);
        if (oserr != OS_EOK) {
            break;
        }
    }

    closeStatus = __CloseFile(pass, "write", &file);
    if (oserr != OS_EOK) {
        return oserr;
    }
    if (closeStatus != OS_EOK) {
        return closeStatus;
    }
    __LogPassResult(pass, __TransferDirectionWrite, test->Options.FileBytes, requestBytes, nanoseconds, 0);
    return OS_EOK;
}

// Compares one request that was read back with the pattern, sector by
// sector. Returns the number of sectors that differ and logs the first
// __MISMATCH_LOG_LIMIT of the pass.
static uint64_t
__CheckRequest(
    _In_    TransferTest_t* test,
    _In_    unsigned        pass,
    _In_    uint32_t        requestBytes,
    _In_    uint32_t        firstSector,
    _In_    size_t          length,
    _In_    const uint8_t*  readData,
    _InOut_ uint64_t*       passMismatches)
{
    uint64_t mismatches = 0;
    size_t   offset;

    __FillRequest(test, pass, requestBytes, firstSector, length, test->Expected);
    for (offset = 0; offset < length; offset += TRANSFER_PATTERN_SECTOR_SIZE) {
        if (memcmp(&readData[offset], &test->Expected[offset], TRANSFER_PATTERN_SECTOR_SIZE) == 0) {
            continue;
        }
        if (*passMismatches + mismatches < __MISMATCH_LOG_LIMIT) {
            __LogMismatch(pass, firstSector + (uint32_t)(offset / TRANSFER_PATTERN_SECTOR_SIZE),
                          &readData[offset], &test->Expected[offset]);
        }
        mismatches++;
    }
    *passMismatches += mismatches;
    return mismatches;
}

// Read half of a pass: opens the file again, checks its size, reads every
// sector and compares it with the pattern.
static oserr_t
__ReadFile(
    _In_ TransferTest_t* test,
    _In_ unsigned        pass,
    _In_ uint32_t        requestBytes)
{
    OSHandle_t   file;
    UInteger64_t size;
    uint64_t     offset;
    uint64_t     nanoseconds = 0;
    uint64_t     mismatches = 0;
    size_t       length;
    oserr_t      oserr;
    oserr_t      closeStatus;
    uint8_t*     buffer = SHMBuffer(&test->Buffer);

    oserr = __OpenFile(test, pass, __TransferDirectionRead, &file);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL pass %u opening %s again: %u", pass, test->Path, oserr);
        return oserr;
    }

    // A file of the wrong size would make the per-sector check meaningless.
    oserr = OSGetFileSize(file.ID, &size);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL pass %u reading the file size: %u", pass, oserr);
    } else if (size.QuadPart != test->Options.FileBytes) {
        ERROR("sdtransfer: FAIL pass %u file size is %llu bytes, wrote %llu",
              pass, size.QuadPart, test->Options.FileBytes);
        oserr = OS_EINCOMPLETE;
    }

    for (offset = 0; oserr == OS_EOK && offset < test->Options.FileBytes; offset += length) {
        length = (size_t)MIN((uint64_t)requestBytes, test->Options.FileBytes - offset);
        oserr = __TransferRequest(test, pass, __TransferDirectionRead, file.ID, offset, length, &nanoseconds);
        if (oserr != OS_EOK) {
            break;
        }

        oserr = SHMAccessBeginCpu(&test->Access, SHMCpuAccessRead, SHMWaitUntilAvailable, NULL);
        if (oserr != OS_EOK) {
            ERROR("sdtransfer: FAIL pass %u starting CPU read access to the buffer: %u", pass, oserr);
            break;
        }
        __CheckRequest(test, pass, requestBytes, (uint32_t)(offset / TRANSFER_PATTERN_SECTOR_SIZE),
                       length, buffer, &mismatches);
        oserr = SHMAccessEndCpu(&test->Access);
        if (oserr != OS_EOK) {
            ERROR("sdtransfer: FAIL pass %u ending CPU read access to the buffer: %u", pass, oserr);
            break;
        }
    }

    closeStatus = __CloseFile(pass, "read", &file);
    test->Mismatches += mismatches;
    if (oserr != OS_EOK) {
        return oserr;
    }
    if (closeStatus != OS_EOK) {
        return closeStatus;
    }
    __LogPassResult(pass, __TransferDirectionRead, test->Options.FileBytes, requestBytes, nanoseconds, mismatches);
    return OS_EOK;
}

// Waits until the test file can be created in the test directory. filed
// mounts the disk some time after the services start.
static oserr_t
__WaitForDirectory(
    _In_ TransferTest_t* test)
{
    OSHandle_t file;
    oserr_t    oserr;
    unsigned   waited;

    for (waited = 0; ; waited++) {
        oserr = __OpenFile(test, 0, __TransferDirectionWrite, &file);
        if (oserr == OS_EOK) {
            break;
        }
        if (waited == 0) {
            NOTICE("sdtransfer: waiting up to %u s for %s (%u)", test->Options.WaitSeconds, test->Path, oserr);
        }
        if (waited >= test->Options.WaitSeconds) {
            ERROR("sdtransfer: FAIL %s could not be created within %u s: %u",
                  test->Path, test->Options.WaitSeconds, oserr);
            return oserr;
        }
        __Sleep(__WAIT_POLL_MS);
    }

    oserr = OSHandleRelease(&file);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL closing %s after the first create: %u", test->Path, oserr);
        return oserr;
    }
    NOTICE("sdtransfer: %s can be created after %u s", test->Path, waited);
    return OS_EOK;
}

// Sets up the transfer buffer, its CPU access session and the buffer for the
// expected pattern. The buffer holds the largest request of the run.
static oserr_t
__CreateBuffers(
    _In_ TransferTest_t* test)
{
    oserr_t  oserr;
    unsigned i;

    test->BufferBytes = 0;
    for (i = 0; i < test->Options.RequestSizeCount; i++) {
        test->BufferBytes = MAX(test->BufferBytes, (size_t)test->Options.RequestBytes[i]);
    }

    oserr = SHMCreate(
        &(SHM_t) {
            .Flags = SHM_COMMIT,
            .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE,
            .Size = test->BufferBytes
        },
        &test->Buffer
    );
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL creating a %u byte transfer buffer: %u", (unsigned)test->BufferBytes, oserr);
        return oserr;
    }

    oserr = SHMAccessOpen(&test->Buffer, 0, test->BufferBytes, &test->Access);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: FAIL opening the CPU access session of the buffer: %u", oserr);
        return oserr;
    }

    test->Expected = malloc(test->BufferBytes);
    if (test->Expected == NULL) {
        ERROR("sdtransfer: FAIL allocating %u bytes for the expected pattern", (unsigned)test->BufferBytes);
        return OS_EOOM;
    }
    return OS_EOK;
}

// Releases what __CreateBuffers set up. Safe on a partly set up test.
static void
__DestroyBuffers(
    _In_ TransferTest_t* test)
{
    oserr_t oserr;

    free(test->Expected);
    test->Expected = NULL;

    oserr = SHMAccessClose(&test->Access);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: closing the CPU access session failed: %u", oserr);
    }
    oserr = SHMRelease(&test->Buffer);
    if (oserr != OS_EOK) {
        ERROR("sdtransfer: releasing the transfer buffer failed: %u", oserr);
    }
}

// Picks the run identifier from the monotonic clock. It only has to differ
// between runs on the same card, and it must not be 0.
static oserr_t
__PickRunId(
    _In_ TransferTest_t* test)
{
    uint64_t now;
    oserr_t  oserr;

    oserr = __Now(&now);
    if (oserr != OS_EOK) {
        return oserr;
    }
    test->RunId = (uint32_t)(now ^ (now >> 32) ^ ((now / 1000ULL) << 7));
    if (test->RunId == 0) {
        test->RunId = 1;
    }
    return OS_EOK;
}

// Runs the passes. Returns after the first failed call, since every later
// pass would fail in the same way. A pass with mismatches does not stop the
// run, so the log shows whether other request sizes are hit too.
static void
__RunPasses(
    _In_ TransferTest_t* test)
{
    unsigned pass;
    uint32_t requestBytes;
    oserr_t  oserr;

    for (pass = 1; pass <= test->Options.Passes; pass++) {
        requestBytes = test->Options.RequestBytes[(pass - 1) % test->Options.RequestSizeCount];
        oserr = __WriteFile(test, pass, requestBytes);
        if (oserr != OS_EOK) {
            test->Errors++;
            return;
        }
        oserr = __ReadFile(test, pass, requestBytes);
        if (oserr != OS_EOK) {
            test->Errors++;
            return;
        }
    }
    NOTICE("sdtransfer: file %s left with run 0x%08x pass %u, %u sectors",
           test->Path, test->RunId, test->Options.Passes, test->SectorCount);
}

// The test job. The order matters: the arguments decide the buffer size, and
// the directory must exist before the first pass.
static void
__TransferMain(
    _In_ void* argument,
    _In_ void* cancellationToken)
{
    static TransferTest_t test;
    int                   length;
    oserr_t               oserr;
    _CRT_UNUSED(argument);
    _CRT_UNUSED(cancellationToken);

    memset(&test, 0, sizeof(test));
    test.Buffer.ID = UUID_INVALID;
    test.Access.Handle = UUID_INVALID;

    NOTICE("sdtransfer: arguments \"%s\"", TRANSFER_ARGUMENTS);
    if (__ParseArguments(TRANSFER_ARGUMENTS, &test.Options)) {
        test.Errors++;
        goto result;
    }

    length = snprintf(test.Path, sizeof(test.Path), "%s/%s", test.Options.Directory, test.Options.FileName);
    if (length < 0 || (size_t)length >= sizeof(test.Path)) {
        ERROR("sdtransfer: FAIL test file path is longer than %u characters", (unsigned)sizeof(test.Path) - 1);
        test.Errors++;
        goto result;
    }
    test.SectorCount = (uint32_t)(test.Options.FileBytes / TRANSFER_PATTERN_SECTOR_SIZE);

    oserr = __PickRunId(&test);
    if (oserr != OS_EOK) {
        test.Errors++;
        goto result;
    }

    oserr = __CreateBuffers(&test);
    if (oserr != OS_EOK) {
        test.Errors++;
        goto cleanup;
    }

    oserr = __WaitForDirectory(&test);
    if (oserr != OS_EOK) {
        test.Errors++;
        goto cleanup;
    }

    NOTICE("sdtransfer: start run 0x%08x, file %s, %llu bytes, %u passes",
           test.RunId, test.Path, test.Options.FileBytes, test.Options.Passes);
    __RunPasses(&test);

cleanup:
    __DestroyBuffers(&test);

result:
    if (test.Errors == 0 && test.Mismatches == 0) {
        NOTICE("sdtransfer: result PASS, run 0x%08x, %u passes, 0 mismatches, 0 errors",
               test.RunId, test.Options.Passes);
    } else {
        ERROR("sdtransfer: result FAIL, run 0x%08x, %u passes, %llu mismatches, %u errors",
              test.RunId, test.Options.Passes, test.Mismatches, test.Errors);
    }
}

void
ServiceInitialize(
    _In_ struct ServiceStartupOptions* startupOptions)
{
    _CRT_UNUSED(startupOptions);

    // ServiceInitialize must return quickly, so the test runs in its own job.
    if (usched_job_queue(__TransferMain, NULL) == UUID_INVALID) {
        ERROR("sdtransfer: FAIL queueing the test job");
    }
}
