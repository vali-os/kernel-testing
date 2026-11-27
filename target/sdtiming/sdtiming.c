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
 * SD timing test. A test program, never part of the normal system: it is
 * built by testing/sdtransfer_image.py --program sdtiming and only goes into
 * test images.
 *
 * A 4 KiB read through a file takes about 1.45 ms on the Raspberry Pi 4,
 * while the data itself moves at about 18 MiB/s. The read passes the program,
 * filed, the MFS module process, the SD driver process and the card, and
 * comes back the same way. This program times parts of that path one by one
 * (the probes below). When two probes differ in one part of the path, the
 * difference of their times is the cost of that part.
 *
 * It runs as a system service, like testing/target/sdtransfer: processd
 * starts every service in the initial ramdisk and hands it no command line.
 * The arguments are built into the program as one text line,
 * TIMING_ARGUMENTS, which the image builder sets.
 *
 * What one run does:
 * 1. Wait until the test file can be created in the test directory, which
 *    is on a disk that filed mounts some time after the services start.
 * 2. Write the test file in 1 MiB requests. It stays open, for reading and
 *    writing, until the end of the run.
 * 3. Ask filed which disk holds the test file (sys_file_ststat_path). The
 *    answer names the storage driver and the disk's device ID, which the
 *    driver probes send their reads to.
 * 4. Run the chosen probes in the order below. Each probe makes a number of
 *    calls and reads the monotonic clock right before and right after every
 *    single call. All samples are kept, and one line per probe reports the
 *    minimum, median, 90th percentile, maximum and mean (statistics.h).
 *
 * The minimum is the cost of the path when nothing has to wait. The gap from
 * the minimum to the median shows waiting, for a free core or for a thread
 * to wake up. Every sample also holds one clock read, which the clock probe
 * measures.
 *
 * The probes. Only the two file write probes write to the card.
 *   clock              Two clock reads back to back, with no call between.
 *                      This is the cost of one clock read.
 *   filed-call         OSGetFilePosition on the test file. filed answers it
 *                      itself (sys_file_get_position_invocation in
 *                      services/filed/api.c). It checks the handle with one
 *                      Syscall_HandleValidate and reads the position from its
 *                      handle table. No grant token, no filesystem module and
 *                      no storage take part, so this is one IPC round trip to
 *                      filed. Every file transfer pays the same handle check.
 *   grant              OSGrantTokenCreate and OSGrantTokenRevoke for 4 KiB of
 *                      the program's buffer, the same token OSTransferFile and
 *                      StorageTransfer create for each request.
 *   driver-read-1      StorageTransfer of 1, 8 and 128 sectors straight to the
 *   driver-read-8      SD driver: one IPC round trip to the driver, one grant
 *   driver-read-128    token, the driver's work and the card. Sequential from
 *                      --first-sector, back to it after --span-mib, and never
 *                      beyond the end of the disk. The driver's transfer
 *                      handler does not check who sends, and a read does not
 *                      disturb the mounted filesystem. No write is ever sent
 *                      to the driver.
 *   file-read-4k       OSTransferFile reads of 4 KiB and 64 KiB from the test
 *   file-read-64k      file, sequential, back to the start at the end of the
 *                      file. The whole path.
 *   memfs-read-4k      OSTransferFile reads of 4 KiB from an empty file in
 *                      filed's root filesystem (MEMFS_FILE_PATH). See below.
 *   file-overwrite-4k  OSTransferFile writes of 4 KiB over the test file,
 *                      sequential, back to the start at the end. The file
 *                      does not grow.
 *   file-append-4k     OSTransferFile writes of 4 KiB that grow a new file,
 *                      APPEND_FILE_NAME in the test directory. The file is
 *                      deleted at the end of the probe.
 *
 * memfs-read-4k takes the place of a read from the initial ramdisk. filed
 * does not serve the ramdisk: no /initfs exists in its file tree (services/
 * filed/scope.c creates the root as a memfs with only /storage in it).
 * processd and deviced map the ramdisk and read /initfs paths from it
 * themselves (services/processd/pe/cache.c, services/deviced/core/initrd.c),
 * so OSOpenPath of an /initfs path from a service cannot succeed. filed's
 * root memfs is served inside filed. A read there runs filed's transfer
 * handler, imports the grant token and maps the buffer (services/filed/vfs/
 * memfs.c, __MemFSRead), with no module and no card behind it. memfs cannot
 * be written (__MemFSWrite returns OS_ENOTSUPPORTED), so the file is empty
 * and each read copies 0 bytes. The probe is one IPC round trip to filed plus
 * the token and the mapping, and leaves out the copy of the data.
 *
 * Result lines. All start with "sdtiming: ". A probe line stays below 128
 * characters while its values stay below 100 ms and its maximum below 10 s,
 * and the other lines are shorter. A longer probe line is still logged
 * whole, because the system log keeps up to OSKERNELLOG_MESSAGE_MAX (255)
 * characters of a message.
 *   arguments "<line>"
 *   waiting up to <s> s for <path> (<e>)
 *   <path> can be created after <s> s
 *   file <path>, <bytes> bytes
 *   start driver <id> device <id>, <count> sectors of <size> B, <calls> calls, <appends> appends
 *   probe <name> <n> calls, us min <a> median <b> p90 <c> max <d> mean <e>
 *   probe <name> skipped, <reason>
 *   result PASS|FAIL, <n> probes, <s> skipped, <e> errors
 * The values of a probe line are microseconds with three decimals. The start
 * line shows driver 0 and device 0 when filed did not name the disk, and the
 * driver probes are then skipped. A failed
 * call is logged as "FAIL <what failed>: <error code>". A failed call inside
 * a probe names the probe and the call number, counts as an error and ends
 * that probe, which then has no probe line. The run goes on with the next
 * probe. An open that filed refuses right after a close of the same file is
 * tried again, as in sdtransfer, and each try gives a WARNING line
 * "open of <path> refused with <e>, retry <n>". A delete that filed refuses
 * because it still counts the closed handle gives "delete of <path> refused
 * with <e>, retry <n>".
 *
 * The program talks to filed and the driver with its own SHM buffer, so the
 * C library does not split or copy the requests.
 */

#define __need_minmax
#include <ddk/service.h>
#include <ddk/storage-transfer.h>
#include <ddk/utils.h>
#include <gracht/link/vali.h>
#include <internal/_utils.h>
#include <os/grants.h>
#include <os/handle.h>
#include <os/services/file.h>
#include <os/services/lookup.h>
#include <os/services/process.h>
#include <os/shm.h>
#include <os/time.h>
#include <os/usched/job.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys_file_service_client.h>

#include "statistics.h"

// The test arguments, set by the image builder with -DTIMING_ARGUMENTS.
// Options:
//   --directory PATH     Directory for the test file. Default /data.
//   --file-name NAME     Name of the test file. Default sdtiming.bin.
//   --file-mib N         Size of the test file in MiB. Default 8.
//   --calls N            Calls of every probe except file-append-4k.
//                        Default 2000.
//   --append-calls N     Calls of file-append-4k. Default 500.
//   --first-sector N     First disk sector of the driver reads. Default 2048.
//   --span-mib N         The driver reads go back to the first sector after
//                        this many MiB. Default 64.
//   --wait-seconds N     How long to wait for the directory. Default 600.
//   --probes LIST        Probes to run, comma separated. A name is a probe
//                        name or "driver-read" or "file-read" for all probes
//                        of that kind. Default all probes.
#ifndef TIMING_ARGUMENTS
#define TIMING_ARGUMENTS ""
#endif

#define __DEFAULT_DIRECTORY    "/data"
#define __DEFAULT_FILE_NAME    "sdtiming.bin"
#define __DEFAULT_FILE_MIB     8U
#define __DEFAULT_CALLS        2000U
#define __DEFAULT_APPEND_CALLS 500U
#define __DEFAULT_FIRST_SECTOR 2048U
#define __DEFAULT_SPAN_MIB     64U
#define __DEFAULT_WAIT_SECONDS 600U

// The file that file-append-4k grows, in the test directory.
#define APPEND_FILE_NAME "sdtiming-grow.bin"

// The empty file of memfs-read-4k, in filed's root filesystem.
#define MEMFS_FILE_PATH "/sdtiming-memfs.bin"

// Limits of the options. The append limit keeps the grown file at or below
// 200 MiB.
#define __MAX_FILE_MIB       1024U
#define __MAX_CALLS          100000U
#define __MAX_APPEND_CALLS   50000U
#define __MAX_SPAN_MIB       65536U
#define __MAX_ARGUMENT_TEXT  256

// The test file is written in requests of this size, and the transfer buffer
// has this size. It also limits a driver read to 1 MiB.
#define __BUFFER_BYTES (1024U * 1024U)

// An open refused because filed has not yet closed the previous handle is
// tried again this often, this far apart. The same holds for a delete that
// filed refuses with OS_EBUSY for the same reason. 50 tries of 100 ms give
// filed 5 seconds.
#define __RETRY_LIMIT    50U
#define __RETRY_DELAY_MS 100U

// The wait for the test directory polls once a second.
#define __WAIT_POLL_MS 1000U

// Room for one probe line, also for values of 100 s (140 characters with
// the prefix, see the host test testing/sdtiming_stats_test.py).
#define __LINE_CAPACITY 160

#define __NSEC_PER_SEC  1000000000ULL
#define __BYTES_PER_MIB (1024ULL * 1024ULL)

// What a probe measures. The kind decides the setup, the call and the
// cleanup of the probe.
enum __ProbeKind {
    __ProbeKindClock,
    __ProbeKindFiledCall,
    __ProbeKindGrant,
    __ProbeKindDriverRead,
    __ProbeKindFileRead,
    __ProbeKindMemfsRead,
    __ProbeKindFileOverwrite,
    __ProbeKindFileAppend
};

/**
 * @brief One probe of the list. Group is the name that selects every probe
 * of the same kind with --probes. Size is the request size in bytes for the
 * file probes and the grant, and the number of sectors for a driver read.
 */
typedef struct TimingProbe {
    const char*      Name;
    const char*      Group;
    enum __ProbeKind Kind;
    uint32_t         Size;
} TimingProbe_t;

#define __PROBE_COUNT 11

static const TimingProbe_t g_probes[__PROBE_COUNT] = {
    { "clock",             "clock",             __ProbeKindClock,         0 },
    { "filed-call",        "filed-call",        __ProbeKindFiledCall,     0 },
    { "grant",             "grant",             __ProbeKindGrant,         4096 },
    { "driver-read-1",     "driver-read",       __ProbeKindDriverRead,    1 },
    { "driver-read-8",     "driver-read",       __ProbeKindDriverRead,    8 },
    { "driver-read-128",   "driver-read",       __ProbeKindDriverRead,    128 },
    { "file-read-4k",      "file-read",         __ProbeKindFileRead,      4096 },
    { "file-read-64k",     "file-read",         __ProbeKindFileRead,      65536 },
    { "memfs-read-4k",     "memfs-read-4k",     __ProbeKindMemfsRead,     4096 },
    { "file-overwrite-4k", "file-overwrite-4k", __ProbeKindFileOverwrite, 4096 },
    { "file-append-4k",    "file-append-4k",    __ProbeKindFileAppend,    4096 }
};

/**
 * @brief The test options after parsing TIMING_ARGUMENTS. Bit i of
 * ProbeMask selects g_probes[i].
 */
typedef struct TimingOptions {
    char     Directory[128];
    char     FileName[64];
    uint64_t FileBytes;
    unsigned Calls;
    unsigned AppendCalls;
    unsigned FirstSector;
    unsigned SpanMib;
    unsigned WaitSeconds;
    uint32_t ProbeMask;
} TimingOptions_t;

/**
 * @brief The disk behind the test file, as filed reports it. DriverID is
 * UUID_INVALID until filed has answered, and stays so for a disk without a
 * storage driver.
 */
typedef struct TimingDisk {
    uuid_t   DriverID;
    uuid_t   DeviceID;
    uint32_t SectorSize;
    uint64_t SectorCount;
} TimingDisk_t;

/**
 * @brief Where a probe is in its file or on the disk. File is the probe's
 * own file (memfs-read-4k and file-append-4k), UUID_INVALID otherwise.
 * Position is the next file byte or disk sector. A probe goes back to First
 * when the next request would pass End.
 */
typedef struct TimingCursor {
    OSHandle_t File;
    uint64_t   First;
    uint64_t   Position;
    uint64_t   End;
} TimingCursor_t;

/**
 * @brief Everything one run uses. File is the test file, open for reading
 * and writing from the first create to the end of the run. Samples holds
 * one sample per call of the running probe.
 */
typedef struct TimingTest {
    TimingOptions_t         Options;
    char                    Path[200];
    char                    AppendPath[200];
    OSHandle_t              File;
    OSHandle_t              Buffer;
    struct SHMAccessSession Access;
    TimingDisk_t            Disk;
    uint64_t*               Samples;
    unsigned                Reported;
    unsigned                Skipped;
    unsigned                Errors;
} TimingTest_t;

// How the setup of a probe ended.
enum __ProbeSetup {
    __ProbeSetupReady,
    __ProbeSetupSkipped,
    __ProbeSetupFailed
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
        ERROR("sdtiming: FAIL reading the monotonic clock: %u", oserr);
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
        ERROR("sdtiming: reading the UTC clock for a sleep failed");
        return;
    }
    until.tv_sec += milliseconds / 1000U;
    until.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (until.tv_nsec >= (long)__NSEC_PER_SEC) {
        until.tv_sec++;
        until.tv_nsec -= (long)__NSEC_PER_SEC;
    }
    if (usched_job_sleep(&until) != 0) {
        ERROR("sdtiming: sleeping %u ms failed", milliseconds);
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

// Reads the comma separated list of probe names into the probe mask. A name
// selects the probe of that name and every probe of the group of that name.
// A name that selects nothing is an error.
static int
__ParseProbes(
    _In_  char*            text,
    _Out_ TimingOptions_t* options)
{
    uint32_t mask = 0;
    uint32_t matched;
    unsigned i;
    char*    item;
    char*    next;

    item = text;
    while (item != NULL) {
        next = strchr(item, ',');
        if (next != NULL) {
            *next = '\0';
            next++;
        }
        matched = 0;
        for (i = 0; i < __PROBE_COUNT; i++) {
            if (!strcmp(item, g_probes[i].Name) || !strcmp(item, g_probes[i].Group)) {
                matched |= 1U << i;
            }
        }
        if (matched == 0) {
            ERROR("sdtiming: FAIL unknown probe %s", item);
            return -1;
        }
        mask |= matched;
        item = next;
    }
    options->ProbeMask = mask;
    return 0;
}

// Splits arguments at spaces and fills options. Options that are not given
// keep their defaults. Returns 0 on success, and logs the bad option on
// failure.
static int
__ParseArguments(
    _In_  const char*      arguments,
    _Out_ TimingOptions_t* options)
{
    char     text[__MAX_ARGUMENT_TEXT];
    char*    tokens[32];
    unsigned tokenCount = 0;
    unsigned fileMib = __DEFAULT_FILE_MIB;
    unsigned i;
    char*    cursor;
    int      status;

    memset(options, 0, sizeof(TimingOptions_t));
    strcpy(options->Directory, __DEFAULT_DIRECTORY);
    strcpy(options->FileName, __DEFAULT_FILE_NAME);
    options->Calls = __DEFAULT_CALLS;
    options->AppendCalls = __DEFAULT_APPEND_CALLS;
    options->FirstSector = __DEFAULT_FIRST_SECTOR;
    options->SpanMib = __DEFAULT_SPAN_MIB;
    options->WaitSeconds = __DEFAULT_WAIT_SECONDS;
    options->ProbeMask = (1U << __PROBE_COUNT) - 1U;

    if (strlen(arguments) >= sizeof(text)) {
        ERROR("sdtiming: FAIL argument line is longer than %u characters", (unsigned)sizeof(text) - 1);
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
            ERROR("sdtiming: FAIL argument line has more than %u words", tokenCount);
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
            ERROR("sdtiming: FAIL option %s has no value", tokens[i]);
            return -1;
        }
        if (!strcmp(tokens[i], "--directory")) {
            status = __CopyText(options->Directory, sizeof(options->Directory), tokens[i + 1]);
        } else if (!strcmp(tokens[i], "--file-name")) {
            status = __CopyText(options->FileName, sizeof(options->FileName), tokens[i + 1]);
        } else if (!strcmp(tokens[i], "--file-mib")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_FILE_MIB, &fileMib);
        } else if (!strcmp(tokens[i], "--calls")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_CALLS, &options->Calls);
        } else if (!strcmp(tokens[i], "--append-calls")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_APPEND_CALLS, &options->AppendCalls);
        } else if (!strcmp(tokens[i], "--first-sector")) {
            status = __ParseNumber(tokens[i + 1], 0, 0xFFFFFFFFU, &options->FirstSector);
        } else if (!strcmp(tokens[i], "--span-mib")) {
            status = __ParseNumber(tokens[i + 1], 1, __MAX_SPAN_MIB, &options->SpanMib);
        } else if (!strcmp(tokens[i], "--wait-seconds")) {
            status = __ParseNumber(tokens[i + 1], 0, 86400, &options->WaitSeconds);
        } else if (!strcmp(tokens[i], "--probes")) {
            status = __ParseProbes(tokens[i + 1], options);
        } else {
            ERROR("sdtiming: FAIL unknown option %s", tokens[i]);
            return -1;
        }
        if (status) {
            ERROR("sdtiming: FAIL bad value for %s: %s", tokens[i], tokens[i + 1]);
            return -1;
        }
    }
    options->FileBytes = (uint64_t)fileMib * __BYTES_PER_MIB;
    return 0;
}

// Opens a file. Closing a file only releases the program's handle. filed
// closes its own side later, when it learns that the handle is gone
// (librt/libos/services/file.c, OSOpenPath). Until then filed still counts
// the old handle, so an open right after a close of the same file can be
// refused with OS_EPERMISSIONS. The open is then tried again, up to
// __RETRY_LIMIT times, __RETRY_DELAY_MS apart.
static oserr_t
__OpenFile(
    _In_  const char*  path,
    _In_  unsigned int flags,
    _Out_ OSHandle_t*  handleOut)
{
    unsigned attempt;
    oserr_t  oserr;

    for (attempt = 1; ; attempt++) {
        oserr = OSOpenPath(path, flags, FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, handleOut);
        if (oserr != OS_EPERMISSIONS || attempt > __RETRY_LIMIT) {
            return oserr;
        }
        WARNING("sdtiming: open of %s refused with %u, retry %u", path, oserr, attempt);
        __Sleep(__RETRY_DELAY_MS);
    }
}

// Closes a file the program opened and marks the handle as closed. Returns
// the close result and logs a failure.
static oserr_t
__CloseFile(
    _In_    const char* path,
    _InOut_ OSHandle_t* handle)
{
    oserr_t oserr;

    oserr = OSHandleRelease(handle);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL closing %s: %u", path, oserr);
    }
    handle->ID = UUID_INVALID;
    return oserr;
}

// Deletes a file the program has closed. filed refuses the delete with
// OS_EBUSY while it still counts the closed handle (see __OpenFile), so a
// refused delete is tried again in the same way.
static oserr_t
__DeleteFile(
    _In_ const char* path)
{
    unsigned attempt;
    oserr_t  oserr;

    for (attempt = 1; ; attempt++) {
        oserr = OSUnlinkPath(path);
        if (oserr != OS_EBUSY || attempt > __RETRY_LIMIT) {
            break;
        }
        WARNING("sdtiming: delete of %s refused with %u, retry %u", path, oserr, attempt);
        __Sleep(__RETRY_DELAY_MS);
    }
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL deleting %s: %u", path, oserr);
    }
    return oserr;
}

// Sets the position of the test file to its start. step names the part of
// the run (the setup or a probe) for the log.
static oserr_t
__Rewind(
    _In_ const char* step,
    _In_ uuid_t      file)
{
    UInteger64_t position = { .QuadPart = 0 };
    oserr_t      oserr;

    oserr = OSSeekFile(file, &position);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL seeking to the start of the test file in %s: %u", step, oserr);
    }
    return oserr;
}

// Waits until the test file can be created in the test directory, and keeps
// it open. filed mounts the disk some time after the services start.
static oserr_t
__WaitForDirectory(
    _In_ TimingTest_t* test)
{
    unsigned int flags = __FILE_CREATE | __FILE_TRUNCATE | __FILE_BINARY | __FILE_READ_ACCESS | __FILE_WRITE_ACCESS;
    unsigned     waited;
    oserr_t      oserr;

    for (waited = 0; ; waited++) {
        oserr = __OpenFile(test->Path, flags, &test->File);
        if (oserr == OS_EOK) {
            break;
        }
        if (waited == 0) {
            NOTICE("sdtiming: waiting up to %u s for %s (%u)", test->Options.WaitSeconds, test->Path, oserr);
        }
        if (waited >= test->Options.WaitSeconds) {
            ERROR("sdtiming: FAIL %s could not be created within %u s: %u",
                  test->Path, test->Options.WaitSeconds, oserr);
            test->File.ID = UUID_INVALID;
            return oserr;
        }
        __Sleep(__WAIT_POLL_MS);
    }
    NOTICE("sdtiming: %s can be created after %u s", test->Path, waited);
    return OS_EOK;
}

// Fills the transfer buffer once with a byte pattern, so the file writes
// put defined data on the card. The content is never checked.
static oserr_t
__FillBuffer(
    _In_ TimingTest_t* test)
{
    uint8_t* buffer = SHMBuffer(&test->Buffer);
    size_t   i;
    oserr_t  oserr;

    // The CPU writes the buffer only inside an access session, so it never
    // overlaps a device transfer of the same memory.
    oserr = SHMAccessBeginCpu(&test->Access, SHMCpuAccessWrite, SHMWaitUntilAvailable, NULL);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL starting CPU write access to the buffer: %u", oserr);
        return oserr;
    }
    for (i = 0; i < __BUFFER_BYTES; i++) {
        buffer[i] = (uint8_t)((i * 31U) ^ (i >> 9));
    }
    oserr = SHMAccessEndCpu(&test->Access);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL ending CPU write access to the buffer: %u", oserr);
    }
    return oserr;
}

// Writes the whole test file in requests of __BUFFER_BYTES and goes back to
// its start.
static oserr_t
__WriteTestFile(
    _In_ TimingTest_t* test)
{
    uint64_t offset;
    size_t   length;
    size_t   transferred;
    oserr_t  oserr;

    for (offset = 0; offset < test->Options.FileBytes; offset += length) {
        length = (size_t)MIN((uint64_t)__BUFFER_BYTES, test->Options.FileBytes - offset);
        oserr = OSTransferFile(
            test->File.ID,
            test->Buffer.ID,
            0,
            true,
            length,
            &transferred
        );
        if (oserr != OS_EOK) {
            ERROR("sdtiming: FAIL writing the test file at offset %llu: %u", offset, oserr);
            return oserr;
        }
        if (transferred != length) {
            ERROR("sdtiming: FAIL writing the test file at offset %llu moved %u of %u bytes",
                  offset, (unsigned)transferred, (unsigned)length);
            return OS_EINCOMPLETE;
        }
    }
    NOTICE("sdtiming: file %s, %llu bytes", test->Path, test->Options.FileBytes);
    return __Rewind("the setup", test->File.ID);
}

// Asks filed which disk holds the test file. OSStorageDescriptor_t, which
// the public GetStorageInformationFromPath fills, has no driver or device
// ID, so the program sends the filed call itself.
static oserr_t
__ReadDisk(
    _In_ TimingTest_t* test)
{
    struct vali_link_message   msg = VALI_MSG_INIT_HANDLE(OSServiceGetFile());
    struct sys_disk_descriptor descriptor;
    oserr_t                    oserr;
    int                        status;

    status = sys_file_ststat_path(
        GetGrachtClient(),
        &msg.base,
        OSProcessCurrentID(),
        test->Path,
        1
    );
    if (status != 0) {
        ERROR("sdtiming: FAIL sending the disk query for %s: %i", test->Path, status);
        return OS_EPROTOCOL;
    }
    status = gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);
    if (status != 0) {
        ERROR("sdtiming: FAIL waiting for the disk query of %s: %i", test->Path, status);
        return OS_EPROTOCOL;
    }
    status = sys_file_ststat_path_result(GetGrachtClient(), &msg.base, &oserr, &descriptor);
    if (status != 0) {
        ERROR("sdtiming: FAIL reading the answer of the disk query for %s: %i", test->Path, status);
        return OS_EPROTOCOL;
    }
    if (oserr == OS_EOK) {
        test->Disk.DriverID = descriptor.driver_id;
        test->Disk.DeviceID = descriptor.device_id;
        test->Disk.SectorSize = descriptor.geometry.sector_size;
        test->Disk.SectorCount = descriptor.geometry.sectors_total;
    }

    // The answer holds the disk's model and serial as allocated strings.
    sys_disk_descriptor_destroy(&descriptor);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL disk query for %s: %u", test->Path, oserr);
    }
    return oserr;
}

// Sets up the driver reads of one probe: the sector range that the reads go
// through, from --first-sector over --span-mib, cut at the end of the disk.
static enum __ProbeSetup
__BeginDriverRead(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe,
    _In_ TimingCursor_t*      cursor)
{
    uint64_t spanSectors;

    if (test->Disk.DriverID == UUID_INVALID || test->Disk.SectorSize == 0) {
        NOTICE("sdtiming: probe %s skipped, no storage driver known for %s", probe->Name, test->Path);
        return __ProbeSetupSkipped;
    }
    if ((uint64_t)probe->Size * test->Disk.SectorSize > __BUFFER_BYTES) {
        NOTICE("sdtiming: probe %s skipped, %u sectors of %u B do not fit the buffer",
               probe->Name, probe->Size, test->Disk.SectorSize);
        return __ProbeSetupSkipped;
    }

    spanSectors = ((uint64_t)test->Options.SpanMib * __BYTES_PER_MIB) / test->Disk.SectorSize;
    cursor->First = test->Options.FirstSector;
    cursor->Position = cursor->First;
    cursor->End = MIN(cursor->First + spanSectors, test->Disk.SectorCount);
    if (cursor->First >= cursor->End || cursor->End - cursor->First < probe->Size) {
        NOTICE("sdtiming: probe %s skipped, sector %llu is too close to the disk end (%llu)",
               probe->Name, cursor->First, test->Disk.SectorCount);
        return __ProbeSetupSkipped;
    }
    return __ProbeSetupReady;
}

// Sets up one probe before its first call. A probe that cannot run here is
// logged as skipped. A failed call is logged as FAIL.
static enum __ProbeSetup
__BeginProbe(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe,
    _In_ TimingCursor_t*      cursor)
{
    unsigned int appendFlags = __FILE_CREATE | __FILE_TRUNCATE | __FILE_BINARY | __FILE_READ_ACCESS |
                               __FILE_WRITE_ACCESS;
    unsigned int memfsFlags = __FILE_CREATE | __FILE_BINARY | __FILE_READ_ACCESS;
    oserr_t      oserr;

    memset(cursor, 0, sizeof(TimingCursor_t));
    cursor->File.ID = UUID_INVALID;

    switch (probe->Kind) {
        case __ProbeKindDriverRead:
            return __BeginDriverRead(test, probe, cursor);

        case __ProbeKindFileRead:
        case __ProbeKindFileOverwrite:
            if (probe->Size > test->Options.FileBytes) {
                NOTICE("sdtiming: probe %s skipped, the test file is smaller than %u B", probe->Name, probe->Size);
                return __ProbeSetupSkipped;
            }
            cursor->End = test->Options.FileBytes;
            oserr = __Rewind(probe->Name, test->File.ID);
            if (oserr != OS_EOK) {
                return __ProbeSetupFailed;
            }
            return __ProbeSetupReady;

        case __ProbeKindMemfsRead:
            oserr = __OpenFile(MEMFS_FILE_PATH, memfsFlags, &cursor->File);
            if (oserr != OS_EOK) {
                NOTICE("sdtiming: probe %s skipped, creating %s failed: %u", probe->Name, MEMFS_FILE_PATH, oserr);
                cursor->File.ID = UUID_INVALID;
                return __ProbeSetupSkipped;
            }
            return __ProbeSetupReady;

        case __ProbeKindFileAppend:
            oserr = __OpenFile(test->AppendPath, appendFlags, &cursor->File);
            if (oserr != OS_EOK) {
                ERROR("sdtiming: FAIL probe %s creating %s: %u", probe->Name, test->AppendPath, oserr);
                cursor->File.ID = UUID_INVALID;
                return __ProbeSetupFailed;
            }
            return __ProbeSetupReady;

        default:
            return __ProbeSetupReady;
    }
}

// Closes and deletes the probe's own file. Returns the first failure.
static oserr_t
__EndProbe(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe,
    _In_ TimingCursor_t*      cursor)
{
    const char* path;
    oserr_t     oserr;

    if (cursor->File.ID == UUID_INVALID) {
        return OS_EOK;
    }
    path = probe->Kind == __ProbeKindMemfsRead ? MEMFS_FILE_PATH : test->AppendPath;
    oserr = __CloseFile(path, &cursor->File);
    if (oserr != OS_EOK) {
        return oserr;
    }
    return __DeleteFile(path);
}

// The work between two calls, outside the timed part: a probe that would
// read or write past the end of its range goes back to the first byte or
// sector.
static oserr_t
__PrepareCall(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe,
    _In_ TimingCursor_t*      cursor)
{
    if (probe->Kind == __ProbeKindDriverRead) {
        if (cursor->Position + probe->Size > cursor->End) {
            cursor->Position = cursor->First;
        }
        return OS_EOK;
    }
    if (probe->Kind == __ProbeKindFileRead || probe->Kind == __ProbeKindFileOverwrite) {
        if (cursor->Position + probe->Size > cursor->End) {
            cursor->Position = 0;
            return __Rewind(probe->Name, test->File.ID);
        }
    }
    return OS_EOK;
}

// Creates and revokes one grant token for the first probe->Size bytes of
// the buffer, with the options OSTransferFile and StorageTransfer use.
static oserr_t
__CallGrant(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe,
    _In_ unsigned             call)
{
    GrantToken_t token;
    oserr_t      oserr;

    oserr = OSGrantTokenCreate(
        test->Buffer.ID,
        &(GrantTokenOptions_t) {
            .ResourceType = GrantTokenResourceTypeSHM,
            .TargetType = GrantTokenTargetTypeAny,
            .TargetGlobalHandleID = UUID_INVALID,
            .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
            .Data.SHM = {
                .Offset = 0,
                .Length = probe->Size
            }
        },
        &token
    );
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL probe %s call %u creating the token: %u", probe->Name, call, oserr);
        return oserr;
    }
    oserr = OSGrantTokenRevoke(token.ControlHandleID);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL probe %s call %u revoking the token: %u", probe->Name, call, oserr);
    }
    return oserr;
}

// Reads probe->Size sectors at the cursor straight from the storage driver.
static oserr_t
__CallDriverRead(
    _In_ TimingTest_t*         test,
    _In_ const TimingProbe_t*  probe,
    _In_ const TimingCursor_t* cursor,
    _In_ unsigned              call)
{
    UInteger64_t sector = { .QuadPart = cursor->Position };
    size_t       completed;
    oserr_t      oserr;

    oserr = StorageTransfer(
        test->Disk.DriverID,
        test->Disk.DeviceID,
        test->Buffer.ID,
        0,
        &sector,
        test->Disk.SectorSize,
        probe->Size,
        false,
        &completed
    );
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL probe %s call %u reading sector %llu: %u, %u sectors moved",
              probe->Name, call, cursor->Position, oserr, (unsigned)completed);
        return oserr;
    }
    if (completed != probe->Size) {
        ERROR("sdtiming: FAIL probe %s call %u at sector %llu moved %u of %u sectors",
              probe->Name, call, cursor->Position, (unsigned)completed, probe->Size);
        return OS_EINCOMPLETE;
    }
    return OS_EOK;
}

// Moves probe->Size bytes between the buffer and a file at the file's
// position. A memfs read may move fewer bytes, because that file is empty.
static oserr_t
__CallFileTransfer(
    _In_ TimingTest_t*         test,
    _In_ const TimingProbe_t*  probe,
    _In_ const TimingCursor_t* cursor,
    _In_ unsigned              call)
{
    uuid_t  file = cursor->File.ID != UUID_INVALID ? cursor->File.ID : test->File.ID;
    bool    write = probe->Kind == __ProbeKindFileOverwrite || probe->Kind == __ProbeKindFileAppend;
    size_t  transferred = 0;
    oserr_t oserr;

    oserr = OSTransferFile(
        file,
        test->Buffer.ID,
        0,
        write,
        probe->Size,
        &transferred
    );
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL probe %s call %u at offset %llu: %u, %u bytes moved",
              probe->Name, call, cursor->Position, oserr, (unsigned)transferred);
        return oserr;
    }
    if (transferred != probe->Size && probe->Kind != __ProbeKindMemfsRead) {
        ERROR("sdtiming: FAIL probe %s call %u at offset %llu moved %u of %u bytes",
              probe->Name, call, cursor->Position, (unsigned)transferred, probe->Size);
        return OS_EINCOMPLETE;
    }
    return OS_EOK;
}

// The timed call of a probe. Logs a failure with the probe name and the call
// number.
static oserr_t
__MakeCall(
    _In_ TimingTest_t*         test,
    _In_ const TimingProbe_t*  probe,
    _In_ const TimingCursor_t* cursor,
    _In_ unsigned              call)
{
    UInteger64_t position;
    oserr_t      oserr;

    switch (probe->Kind) {
        case __ProbeKindClock:
            return OS_EOK;

        case __ProbeKindFiledCall:
            oserr = OSGetFilePosition(test->File.ID, &position);
            if (oserr != OS_EOK) {
                ERROR("sdtiming: FAIL probe %s call %u: %u", probe->Name, call, oserr);
            }
            return oserr;

        case __ProbeKindGrant:
            return __CallGrant(test, probe, call);

        case __ProbeKindDriverRead:
            return __CallDriverRead(test, probe, cursor, call);

        default:
            return __CallFileTransfer(test, probe, cursor, call);
    }
}

// Runs the calls of one probe and logs its line. A failed call ends the
// probe and counts as an error.
static void
__RunProbe(
    _In_ TimingTest_t*        test,
    _In_ const TimingProbe_t* probe)
{
    TimingCursor_t    cursor;
    TimingSummary_t   summary;
    enum __ProbeSetup setup;
    char              line[__LINE_CAPACITY];
    unsigned          calls;
    unsigned          call;
    uint64_t          started;
    uint64_t          ended;
    oserr_t           oserr = OS_EOK;
    oserr_t           endStatus;

    setup = __BeginProbe(test, probe, &cursor);
    if (setup == __ProbeSetupSkipped) {
        test->Skipped++;
        return;
    }
    if (setup == __ProbeSetupFailed) {
        test->Errors++;
        return;
    }

    // Each sample is the time from right before to right after one call.
    // The rewind of a probe that reached the end of its range is done
    // before the first clock read, so it is never part of a sample.
    calls = probe->Kind == __ProbeKindFileAppend ? test->Options.AppendCalls : test->Options.Calls;
    for (call = 0; call < calls; call++) {
        oserr = __PrepareCall(test, probe, &cursor);
        if (oserr != OS_EOK) {
            break;
        }
        oserr = __Now(&started);
        if (oserr != OS_EOK) {
            break;
        }
        oserr = __MakeCall(test, probe, &cursor, call);
        if (oserr != OS_EOK) {
            break;
        }
        oserr = __Now(&ended);
        if (oserr != OS_EOK) {
            break;
        }
        test->Samples[call] = ended - started;
        cursor.Position += probe->Size;
    }

    endStatus = __EndProbe(test, probe, &cursor);
    if (oserr != OS_EOK || endStatus != OS_EOK) {
        test->Errors++;
        return;
    }

    if (TimingSummarize(test->Samples, calls, &summary)) {
        ERROR("sdtiming: FAIL probe %s summing up %u samples", probe->Name, calls);
        test->Errors++;
        return;
    }
    if (TimingFormatProbeLine(line, sizeof(line), probe->Name, &summary) < 0) {
        ERROR("sdtiming: FAIL probe %s line does not fit %u characters", probe->Name, (unsigned)sizeof(line) - 1);
        test->Errors++;
        return;
    }
    NOTICE("sdtiming: %s", line);
    test->Reported++;
}

// Sets up the transfer buffer, its CPU access session and the sample array.
static oserr_t
__CreateBuffers(
    _In_ TimingTest_t* test)
{
    unsigned samples = MAX(test->Options.Calls, test->Options.AppendCalls);
    oserr_t  oserr;

    oserr = SHMCreate(
        &(SHM_t) {
            .Flags = SHM_COMMIT,
            .Access = SHM_ACCESS_READ | SHM_ACCESS_WRITE,
            .Size = __BUFFER_BYTES
        },
        &test->Buffer
    );
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL creating a %u byte transfer buffer: %u", __BUFFER_BYTES, oserr);
        return oserr;
    }

    oserr = SHMAccessOpen(&test->Buffer, 0, __BUFFER_BYTES, &test->Access);
    if (oserr != OS_EOK) {
        ERROR("sdtiming: FAIL opening the CPU access session of the buffer: %u", oserr);
        return oserr;
    }

    test->Samples = malloc(sizeof(uint64_t) * samples);
    if (test->Samples == NULL) {
        ERROR("sdtiming: FAIL allocating room for %u samples", samples);
        return OS_EOOM;
    }
    return OS_EOK;
}

// Releases what __CreateBuffers set up. Safe on a partly set up test.
static void
__DestroyBuffers(
    _In_ TimingTest_t* test)
{
    oserr_t oserr;

    free(test->Samples);
    test->Samples = NULL;

    if (test->Access.Handle != UUID_INVALID) {
        oserr = SHMAccessClose(&test->Access);
        if (oserr != OS_EOK) {
            ERROR("sdtiming: closing the CPU access session failed: %u", oserr);
        }
    }
    if (test->Buffer.ID != UUID_INVALID) {
        oserr = SHMRelease(&test->Buffer);
        if (oserr != OS_EOK) {
            ERROR("sdtiming: releasing the transfer buffer failed: %u", oserr);
        }
    }
}

// Builds the paths of the test file and of the file that file-append-4k
// grows. Returns 0 when both fit.
static int
__BuildPaths(
    _In_ TimingTest_t* test)
{
    int length;

    length = snprintf(test->Path, sizeof(test->Path), "%s/%s", test->Options.Directory, test->Options.FileName);
    if (length < 0 || (size_t)length >= sizeof(test->Path)) {
        ERROR("sdtiming: FAIL test file path is longer than %u characters", (unsigned)sizeof(test->Path) - 1);
        return -1;
    }
    length = snprintf(test->AppendPath, sizeof(test->AppendPath), "%s/%s", test->Options.Directory, APPEND_FILE_NAME);
    if (length < 0 || (size_t)length >= sizeof(test->AppendPath)) {
        ERROR("sdtiming: FAIL append file path is longer than %u characters",
              (unsigned)sizeof(test->AppendPath) - 1);
        return -1;
    }
    if (!strcmp(test->Path, test->AppendPath)) {
        ERROR("sdtiming: FAIL the test file must not be named %s", APPEND_FILE_NAME);
        return -1;
    }
    return 0;
}

// Prepares the run: the buffer, the test file and the disk. A failure here
// ends the run, except the disk query, whose failure only makes the driver
// probes skip.
static oserr_t
__Setup(
    _In_ TimingTest_t* test)
{
    oserr_t oserr;

    oserr = __CreateBuffers(test);
    if (oserr != OS_EOK) {
        return oserr;
    }
    oserr = __FillBuffer(test);
    if (oserr != OS_EOK) {
        return oserr;
    }
    oserr = __WaitForDirectory(test);
    if (oserr != OS_EOK) {
        return oserr;
    }
    oserr = __WriteTestFile(test);
    if (oserr != OS_EOK) {
        return oserr;
    }

    oserr = __ReadDisk(test);
    if (oserr != OS_EOK) {
        test->Errors++;
    }
    return OS_EOK;
}

// The test job. The order matters: the arguments decide the sample array,
// the test file must exist before the probes, and the probes run in the
// order of g_probes.
static void
__TimingMain(
    _In_ void* argument,
    _In_ void* cancellationToken)
{
    static TimingTest_t test;
    unsigned            i;
    oserr_t             oserr;
    _CRT_UNUSED(argument);
    _CRT_UNUSED(cancellationToken);

    memset(&test, 0, sizeof(test));
    test.File.ID = UUID_INVALID;
    test.Buffer.ID = UUID_INVALID;
    test.Access.Handle = UUID_INVALID;
    test.Disk.DriverID = UUID_INVALID;
    test.Disk.DeviceID = UUID_INVALID;

    NOTICE("sdtiming: arguments \"%s\"", TIMING_ARGUMENTS);
    if (__ParseArguments(TIMING_ARGUMENTS, &test.Options) || __BuildPaths(&test)) {
        test.Errors++;
        goto result;
    }

    oserr = __Setup(&test);
    if (oserr != OS_EOK) {
        test.Errors++;
        goto cleanup;
    }

    NOTICE("sdtiming: start driver %u device %u, %llu sectors of %u B, %u calls, %u appends",
           test.Disk.DriverID, test.Disk.DeviceID, test.Disk.SectorCount, test.Disk.SectorSize,
           test.Options.Calls, test.Options.AppendCalls);
    for (i = 0; i < __PROBE_COUNT; i++) {
        if (test.Options.ProbeMask & (1U << i)) {
            __RunProbe(&test, &g_probes[i]);
        }
    }

cleanup:
    if (test.File.ID != UUID_INVALID && __CloseFile(test.Path, &test.File) != OS_EOK) {
        test.Errors++;
    }
    __DestroyBuffers(&test);

result:
    if (test.Errors == 0) {
        NOTICE("sdtiming: result PASS, %u probes, %u skipped, 0 errors", test.Reported, test.Skipped);
    } else {
        ERROR("sdtiming: result FAIL, %u probes, %u skipped, %u errors", test.Reported, test.Skipped, test.Errors);
    }
}

// The sys_file client in libddk, which __ReadDisk uses, calls this for
// filed's storage_ready event. The program does not subscribe to filed's
// events, so it is never called and does nothing.
void
sys_file_event_storage_ready_invocation(
    _In_ gracht_client_t* client,
    _In_ const char*      path)
{
    _CRT_UNUSED(client);
    _CRT_UNUSED(path);
}

void
ServiceInitialize(
    _In_ struct ServiceStartupOptions* startupOptions)
{
    _CRT_UNUSED(startupOptions);

    // ServiceInitialize must return quickly, so the test runs in its own job.
    if (usched_job_queue(__TimingMain, NULL) == UUID_INVALID) {
        ERROR("sdtiming: FAIL queueing the test job");
    }
}
