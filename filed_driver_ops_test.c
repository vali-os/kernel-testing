// filed's requests to filesystem drivers (interfaces/driver_ops.c) when a
// request fails: it cannot be sent, the wait for the answer fails, the driver
// answers with an error message, or the answer cannot be read. The old
// wrappers ignored the wait and read the result anyway, so a driver that was
// gone gave a result from uninitialised memory ("failed to cleanup handle
// with code 129").
//
// Part one calls every operation of driver_ops.c with each failure, and checks
// its result, its output values, whether the driver ran the request, the
// abandoned requests and the error log. The driver double asserts that no
// answer is read before a successful wait, or after a failed one.
// Part two makes the requests fail under the VFS code that sends them, and
// checks that the callers close what they opened, release their locks, leave
// the VFS operations they entered and return the error.
//
// Production code compiled here: filesystem.c, storage/utils.c, the VFS, the
// mkdir, open, close, unlink, stat and handle_stat handlers,
// interfaces/driver_ops.c and the libds code. filed_vfs_doubles.inc replaces
// the system services and usched, and filed_module_double.inc the driver.
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef FILED_FILESYSTEM_SOURCE
#define FILED_FILESYSTEM_SOURCE "../services/filed/filesystem.c"
#endif
#include FILED_FILESYSTEM_SOURCE
#include "../services/filed/storage/utils.c"
#include "../services/filed/vfs/private.h"
#include <os/shm.h>
#include <os/grants.h>

#define TEST_MAX_OSHANDLES 8192

static int g_errors;
static int g_warnings;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (level == OSSYSLOGLEVEL_WARNING) {
        g_warnings++;
    }
    if (getenv("FILED_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        fprintf(stderr, "log(%i): %s\n", level, format);
    }
}

#include "filed_vfs_doubles.inc"
#include "filed_module_double.inc"

static const enum __ModuleFailure g_failures[] = {
    MODULE_FAIL_SEND, MODULE_FAIL_WAIT, MODULE_FAIL_STATUS, MODULE_FAIL_READ
};

// Part one. The operations are called directly on an interface, the way the
// VFS calls them, against a filesystem g_fsctx the driver has set up. /f is a
// file with data.
static struct VFSInterface* g_interface;
static void*                g_fsctx;
static int                  g_names;

#define MARKER ((void*)0x5a5a5a5a)

// What one call did: its result, and whether the driver ran it.
struct __CallResult {
    oserr_t Result;
    bool    Ran;
};

// Closes the driver handles of a path, which a request opened in the driver
// but whose answer was lost. filed cannot know them.
static void
__ForgetPath(
    _In_ const char* path)
{
    struct __ModuleFileSystem* fileSystem = __ModuleFileSystem((uintptr_t)g_fsctx);

    for (int i = 0; i < MODULE_MAX_HANDLES; i++) {
        if (g_moduleHandles[i].Used && !strcmp(g_moduleHandles[i].Path, path)) {
            g_moduleHandles[i].Used = false;
            fileSystem->Handles--;
        }
    }
}

// Arms the failure and then runs one call. Calls made before by the case
// itself, to open a file it needs, are not affected.
static void
__Arm(
    _In_ enum __ModuleCall    call,
    _In_ enum __ModuleFailure failure,
    _Out_ int*                callsBefore)
{
    __ModuleFailCall(call, failure, 0);
    *callsBefore = g_moduleCalls[call];
}

static bool
__Ran(
    _In_ enum __ModuleCall call,
    _In_ int               callsBefore)
{
    return g_moduleCalls[call] != callsBefore;
}

static void*
__OpenPath(
    _In_ const char* cpath)
{
    mstring_t* path = mstr_new_u8(cpath);
    void*      data = MARKER;

    assert(path != NULL);
    assert(g_driverOps.Open(g_interface, g_fsctx, path, &data) == OS_EOK);
    mstr_delete(path);
    return data;
}

static void
__ClosePath(
    _In_ void* data)
{
    assert(g_driverOps.Close(g_interface, g_fsctx, data) == OS_EOK);
}

// Makes an SHM buffer and a transfer of it, full or of length bytes.
static void
__MakeTransfer(
    _In_  size_t         length,
    _Out_ OSHandle_t*    shm,
    _Out_ SHMTransfer_t* transfer)
{
    GrantTokenOptions_t options = {
        .ResourceType = GrantTokenResourceTypeSHM,
        .TargetType = GrantTokenTargetTypeAny,
        .TargetGlobalHandleID = UUID_INVALID,
        .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
        .Data.SHM = { .Offset = 0, .Length = 4096 }
    };

    assert(SHMCreate(&(SHM_t) { .Size = 4096 }, shm) == OS_EOK);
    memset(transfer, 0, sizeof(*transfer));
    assert(OSGrantTokenCreate(shm->ID, &options, &transfer->Token) == OS_EOK);
    if (length == 0) {
        transfer->RangeMode = SHMTransferRangeFull;
    } else {
        transfer->RangeMode = SHMTransferRangeSubrange;
        transfer->RequestedLength = (uint32_t)length;
    }
}

static void
__DestroyTransfer(
    _In_ OSHandle_t*    shm,
    _In_ SHMTransfer_t* transfer)
{
    assert(OSGrantTokenRevoke(transfer->Token.ControlHandleID) == OS_EOK);
    OSHandleDestroy(shm);
}

static struct __CallResult
__CallSetup(
    _In_ enum __ModuleFailure failure)
{
    struct VFSStorageParameters parameters = { 0 };
    struct __CallResult         call;
    void*                       data = MARKER;
    int                         before;

    __Arm(MODULE_CALL_SETUP, failure, &before);
    call.Result = g_driverOps.Initialize(g_interface, &parameters, &data);
    call.Ran = __Ran(MODULE_CALL_SETUP, before);
    if (call.Result == OS_EOK) {
        assert(data != MARKER && data != NULL);
        assert(g_driverOps.Destroy(g_interface, data, 0) == OS_EOK);
    } else {
        assert(data == MARKER);
    }

    // A setup the driver ran, but whose answer was lost, leaves its
    // filesystem in the driver. filed cannot know it.
    if (call.Ran && call.Result != OS_EOK) {
        for (int i = 0; i < MODULE_MAX_FILESYSTEMS; i++) {
            if (g_moduleFileSystems[i].Used && (void*)(uintptr_t)(i + 1) != g_fsctx) {
                g_moduleFileSystems[i].Used = false;
            }
        }
    }
    return call;
}

static struct __CallResult
__CallDestroy(
    _In_ enum __ModuleFailure failure)
{
    struct VFSStorageParameters parameters = { 0 };
    struct __CallResult         call;
    void*                       data;
    int                         before;

    assert(g_driverOps.Initialize(g_interface, &parameters, &data) == OS_EOK);
    __Arm(MODULE_CALL_DESTROY, failure, &before);
    call.Result = g_driverOps.Destroy(g_interface, data, 0);
    call.Ran = __Ran(MODULE_CALL_DESTROY, before);
    if (!call.Ran) {
        assert(g_driverOps.Destroy(g_interface, data, 0) == OS_EOK);
    }
    return call;
}

static struct __CallResult
__CallStat(
    _In_ enum __ModuleFailure failure)
{
    struct VFSStatFS    stats = { .Label = MARKER };
    struct __CallResult call;
    int                 before;

    __Arm(MODULE_CALL_FSSTAT, failure, &before);
    call.Result = g_driverOps.Stat(g_interface, g_fsctx, &stats);
    call.Ran = __Ran(MODULE_CALL_FSSTAT, before);
    if (call.Result == OS_EOK) {
        assert(stats.Label != MARKER && stats.Label != NULL && stats.BlockSize == 512);
        mstr_delete(stats.Label);
    } else {
        assert(stats.Label == MARKER);
    }
    return call;
}

static struct __CallResult
__CallOpen(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    mstring_t*          path = mstr_new_u8("/f");
    void*               data = MARKER;
    int                 before;

    __Arm(MODULE_CALL_OPEN, failure, &before);
    call.Result = g_driverOps.Open(g_interface, g_fsctx, path, &data);
    call.Ran = __Ran(MODULE_CALL_OPEN, before);
    mstr_delete(path);
    if (call.Result == OS_EOK) {
        assert(data != MARKER);
        __ClosePath(data);
    } else {
        assert(data == MARKER);
        __ForgetPath("/f");
    }
    return call;
}

static struct __CallResult
__CallCreate(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               directory = __OpenPath("/");
    mstring_t*          name = mstr_fmt("c%i", g_names++);
    void*               data = MARKER;
    int                 before;

    __Arm(MODULE_CALL_CREATE, failure, &before);
    call.Result = g_driverOps.Create(g_interface, g_fsctx, directory, name,
                                     0, FILE_FLAG_FILE, FILE_PERMISSION_READ, &data);
    call.Ran = __Ran(MODULE_CALL_CREATE, before);
    if (call.Result == OS_EOK) {
        assert(data != MARKER);
        __ClosePath(data);
    } else {
        char* cname = mstr_u8(name);
        char  path[32];

        assert(data == MARKER);
        snprintf(path, sizeof(path), "/%s", cname);
        __ForgetPath(path);
        free(cname);
    }
    mstr_delete(name);
    __ClosePath(directory);
    return call;
}

static struct __CallResult
__CallClose(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               data = __OpenPath("/f");
    int                 before;

    __Arm(MODULE_CALL_CLOSE, failure, &before);
    call.Result = g_driverOps.Close(g_interface, g_fsctx, data);
    call.Ran = __Ran(MODULE_CALL_CLOSE, before);
    if (!call.Ran) {
        __ClosePath(data);
    }
    return call;
}

static struct __CallResult
__CallLink(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               directory = __OpenPath("/");
    mstring_t*          name = mstr_new_u8("link");
    mstring_t*          target = mstr_new_u8("/f");
    int                 before;

    __Arm(MODULE_CALL_LINK, failure, &before);
    call.Result = g_driverOps.Link(g_interface, g_fsctx, directory, name, target, 1);
    call.Ran = __Ran(MODULE_CALL_LINK, before);
    mstr_delete(name);
    mstr_delete(target);
    __ClosePath(directory);
    return call;
}

static struct __CallResult
__CallUnlink(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    mstring_t*          path = mstr_fmt("/u%i", g_names++);
    char*               cpath = mstr_u8(path);
    int                 before;

    __ModuleAdd(__ModuleFileSystem((uintptr_t)g_fsctx), cpath, FILE_FLAG_FILE, 0);
    __Arm(MODULE_CALL_UNLINK, failure, &before);
    call.Result = g_driverOps.Unlink(g_interface, g_fsctx, path);
    call.Ran = __Ran(MODULE_CALL_UNLINK, before);
    assert((__ModuleFind(__ModuleFileSystem((uintptr_t)g_fsctx), cpath) == NULL) == call.Ran);
    free(cpath);
    mstr_delete(path);
    return call;
}

static struct __CallResult
__CallReadLink(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    mstring_t*          path = mstr_new_u8("/f");
    mstring_t*          link = MARKER;
    int                 before;

    __Arm(MODULE_CALL_READLINK, failure, &before);
    call.Result = g_driverOps.ReadLink(g_interface, g_fsctx, path, &link);
    call.Ran = __Ran(MODULE_CALL_READLINK, before);
    mstr_delete(path);
    if (call.Result == OS_EOK) {
        char* text = mstr_u8(link);
        assert(text != NULL && !strcmp(text, "/f.target"));
        free(text);
        mstr_delete(link);
    } else {
        assert(link == MARKER);
    }
    return call;
}

static struct __CallResult
__CallMove(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    mstring_t*          from = mstr_fmt("/m%i", g_names);
    mstring_t*          to = mstr_fmt("/m%ib", g_names++);
    char*               cfrom = mstr_u8(from);
    int                 before;

    __ModuleAdd(__ModuleFileSystem((uintptr_t)g_fsctx), cfrom, FILE_FLAG_FILE, 0);
    __Arm(MODULE_CALL_MOVE, failure, &before);
    call.Result = g_driverOps.Move(g_interface, g_fsctx, from, to, 0);
    call.Ran = __Ran(MODULE_CALL_MOVE, before);
    free(cfrom);
    mstr_delete(from);
    mstr_delete(to);
    return call;
}

static struct __CallResult
__CallRead(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               data = __OpenPath("/f");
    OSHandle_t          shm;
    SHMTransfer_t       transfer;
    size_t              read = 99;
    int                 before;

    __MakeTransfer(0, &shm, &transfer);
    __Arm(MODULE_CALL_READ, failure, &before);
    call.Result = g_driverOps.Read(g_interface, g_fsctx, data, &transfer, &read);
    call.Ran = __Ran(MODULE_CALL_READ, before);
    if (call.Result == OS_EOK) {
        assert(read == 5 && !memcmp(SHMBuffer(&shm), "hello", 5));
    } else {
        assert(read == 0);
    }
    __DestroyTransfer(&shm, &transfer);
    __ClosePath(data);
    return call;
}

static struct __CallResult
__CallWrite(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               data = __OpenPath("/f");
    OSHandle_t          shm;
    SHMTransfer_t       transfer;
    size_t              written = 99;
    int                 before;

    __MakeTransfer(5, &shm, &transfer);
    memcpy(SHMBuffer(&shm), "hello", 5);
    __Arm(MODULE_CALL_WRITE, failure, &before);
    call.Result = g_driverOps.Write(g_interface, g_fsctx, data, &transfer, &written);
    call.Ran = __Ran(MODULE_CALL_WRITE, before);
    assert(written == (call.Result == OS_EOK ? 5 : 0));
    __DestroyTransfer(&shm, &transfer);
    __ClosePath(data);
    return call;
}

static struct __CallResult
__CallTruncate(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               data = __OpenPath("/f");
    int                 before;

    __Arm(MODULE_CALL_TRUNCATE, failure, &before);
    call.Result = g_driverOps.Truncate(g_interface, g_fsctx, data, 5);
    call.Ran = __Ran(MODULE_CALL_TRUNCATE, before);
    __ClosePath(data);
    return call;
}

static struct __CallResult
__CallSeek(
    _In_ enum __ModuleFailure failure)
{
    struct __CallResult call;
    void*               data = __OpenPath("/f");
    uint64_t            position = 99;
    int                 before;

    __Arm(MODULE_CALL_SEEK, failure, &before);
    call.Result = g_driverOps.Seek(g_interface, g_fsctx, data, 2, &position);
    call.Ran = __Ran(MODULE_CALL_SEEK, before);
    assert(position == (call.Result == OS_EOK ? 2 : 99));

    // The driver refuses a position past the end, and its answer has no
    // position to use.
    if (failure == MODULE_FAIL_NONE) {
        position = 99;
        assert(g_driverOps.Seek(g_interface, g_fsctx, data, 1000, &position) == OS_EINVALPARAMS);
        assert(position == 99);
    }
    __ClosePath(data);
    return call;
}

static struct __CallResult (*g_calls[MODULE_CALL_COUNT])(enum __ModuleFailure) = {
    [MODULE_CALL_SETUP]    = __CallSetup,
    [MODULE_CALL_DESTROY]  = __CallDestroy,
    [MODULE_CALL_FSSTAT]   = __CallStat,
    [MODULE_CALL_OPEN]     = __CallOpen,
    [MODULE_CALL_CREATE]   = __CallCreate,
    [MODULE_CALL_CLOSE]    = __CallClose,
    [MODULE_CALL_LINK]     = __CallLink,
    [MODULE_CALL_UNLINK]   = __CallUnlink,
    [MODULE_CALL_READLINK] = __CallReadLink,
    [MODULE_CALL_MOVE]     = __CallMove,
    [MODULE_CALL_READ]     = __CallRead,
    [MODULE_CALL_WRITE]    = __CallWrite,
    [MODULE_CALL_TRUNCATE] = __CallTruncate,
    [MODULE_CALL_SEEK]     = __CallSeek,
};

// The result the driver gives for a call that it ran without a failure.
static oserr_t
__DriverResult(
    _In_ enum __ModuleCall call)
{
    return call == MODULE_CALL_LINK ? OS_ENOTSUPPORTED : OS_EOK;
}

static void
__TestEveryOperation(void)
{
    struct VFSStorageParameters parameters = { 0 };
    struct __ModuleEntry*       file;

    g_interface = VFSInterfaceNew(7, &g_driverOps);
    assert(g_driverOps.Initialize(g_interface, &parameters, &g_fsctx) == OS_EOK);
    file = __ModuleAdd(__ModuleFileSystem((uintptr_t)g_fsctx), "/f", FILE_FLAG_FILE, FILE_PERMISSION_READ);
    memcpy(file->Data, "hello", 5);
    file->Size = 5;

    for (int i = 0; i < MODULE_CALL_COUNT; i++) {
        struct __CallResult call;

        // Without a failure the call gives the driver's result.
        g_errors = 0;
        g_moduleAbandons = 0;
        call = g_calls[i](MODULE_FAIL_NONE);
        assert(call.Result == __DriverResult(i) && call.Ran);
        assert(g_errors == 0 && g_moduleAbandons == 0);

        for (size_t j = 0; j < sizeof(g_failures) / sizeof(g_failures[0]); j++) {
            enum __ModuleFailure failure = g_failures[j];
            oserr_t              expected = failure == MODULE_FAIL_READ ? OS_EPROTOCOL : OS_EUNKNOWN;
            int                  abandons = (failure == MODULE_FAIL_WAIT || failure == MODULE_FAIL_STATUS) ? 1 : 0;

            g_errors = 0;
            g_moduleAbandons = 0;
            call = g_calls[i](failure);
            if (call.Result != expected || call.Ran != (failure == MODULE_FAIL_READ) ||
                g_errors != 1 || g_moduleAbandons != abandons) {
                fprintf(stderr, "%s with a failed %s: result %u, ran %i, errors %i, abandons %i\n",
                        g_moduleCallNames[i], g_moduleFailureNames[failure], call.Result,
                        call.Ran, g_errors, g_moduleAbandons);
                abort();
            }
        }
        assert(__ModuleFileSystem((uintptr_t)g_fsctx)->Handles == 0);
        printf("%s: sent, not sent, wait failed, error answer and unreadable answer passed\n",
               g_moduleCallNames[i]);
    }

    assert(g_driverOps.Destroy(g_interface, g_fsctx, 0) == OS_EOK);
    VFSInterfaceDelete(g_interface);
    g_errors = 0;
}

// Part two: the callers in the VFS.
static struct __TestDisk g_disk = { .Serial = "sd", .Label = "disk" };
static int               g_expectedHandles;

// Closes driver handles that a failed request left open in the driver. They
// are lost to filed, as the answer that named them was not read, or the close
// of them was never run. In filed they last until the driver goes away.
static void
__ForgetDriverHandles(
    _In_ int count)
{
    struct __ModuleFileSystem* fileSystem = __DriverOf(&g_disk);

    assert(__ModuleOpenHandles() == count);
    for (int i = 0; i < MODULE_MAX_HANDLES; i++) {
        if (g_moduleHandles[i].Used && g_moduleHandles[i].FileSystem == fileSystem) {
            g_moduleHandles[i].Used = false;
            fileSystem->Handles--;
        }
    }
}

// The request left nothing behind: no node lock, no VFS operation, no driver
// handle and no VFS handle the case did not keep. errors and warnings are the
// logs the case expects.
static void
__ExpectIdle(
    _In_ int errors,
    _In_ int warnings)
{
    __ExpectUnlocked(g_rootScope->Root);
    __ExpectNoOperations(g_rootScope);
    __ExpectNoOperations(g_disk.VFS);
    assert(__ModuleOpenHandles() == 0);
    assert(__OpenHandles() == g_expectedHandles);
    if (g_errors != errors || g_warnings != warnings) {
        fprintf(stderr, "expected %i errors and %i warnings, got %i and %i\n",
                errors, warnings, g_errors, g_warnings);
        abort();
    }
    g_errors = 0;
    g_warnings = 0;
}

static oserr_t
__ExpectedResult(
    _In_ enum __ModuleFailure failure)
{
    return failure == MODULE_FAIL_READ ? OS_EPROTOCOL : OS_EUNKNOWN;
}

// A lookup that loads a directory: the open, the read and the close of the
// directory fail in turn.
static void
__TestLoadDirectory(
    _In_ enum __ModuleFailure failure)
{
    char directory[16];
    char file[32];

    snprintf(directory, sizeof(directory), "/ld%i", g_names++);
    snprintf(file, sizeof(file), "%s/f", directory);
    __CreateDirectory(&g_disk, directory);
    pthread_mutex_lock(&g_moduleLock);
    __ModuleAdd(__DriverOf(&g_disk), file, FILE_FLAG_FILE, FILE_PERMISSION_READ);
    pthread_mutex_unlock(&g_moduleLock);
    __ExpectIdle(0, 0);

    // The directory stays unloaded, so the next lookup tries again.
    __ModuleFailCall(MODULE_CALL_OPEN, failure, 0);
    assert(__Lookup(__On(&g_disk, file)) == __ExpectedResult(failure));
    if (failure == MODULE_FAIL_READ) {
        __ForgetDriverHandles(1);
    }
    __ExpectIdle(1, 0);

    __ModuleFailCall(MODULE_CALL_READ, failure, 0);
    assert(__Lookup(__On(&g_disk, file)) == __ExpectedResult(failure));
    __ExpectIdle(1, 0);

    // A failed close of the directory is only a warning. The load is done.
    __ModuleFailCall(MODULE_CALL_CLOSE, failure, 0);
    assert(__Lookup(__On(&g_disk, file)) == OS_EOK);
    if (failure != MODULE_FAIL_READ) {
        __ForgetDriverHandles(1);
    }
    __ExpectIdle(1, 1);
}

// VFSNodeOpen of an existing file, and of a new one (VFSNodeCreateChild).
static void
__TestOpen(
    _In_ enum __ModuleFailure failure)
{
    char   path[16];
    uuid_t handle = UUID_INVALID;

    snprintf(path, sizeof(path), "/o%i", g_names++);
    __CreateFile(&g_disk, path, "data");
    __ExpectIdle(0, 0);

    // The resource for the handle is destroyed again.
    __ModuleFailCall(MODULE_CALL_OPEN, failure, 0);
    assert(VFSNodeOpen(g_rootScope, __On(&g_disk, path), __FILE_READ_ACCESS, 0, &handle) == __ExpectedResult(failure));
    assert(handle == UUID_INVALID);
    if (failure == MODULE_FAIL_READ) {
        __ForgetDriverHandles(1);
    }
    __ExpectIdle(1, 0);

    // A create fails when the directory cannot be opened, or the file not
    // created. Neither leaves a node.
    snprintf(path, sizeof(path), "/n%i", g_names++);
    __ModuleFailCall(MODULE_CALL_OPEN, failure, 0);
    assert(VFSNodeOpen(g_rootScope, __On(&g_disk, path), __FILE_CREATE | __FILE_READ_ACCESS,
                       FILE_PERMISSION_READ, &handle) == __ExpectedResult(failure));
    if (failure == MODULE_FAIL_READ) {
        __ForgetDriverHandles(1);
    }
    __ExpectIdle(1, 0);

    __ModuleFailCall(MODULE_CALL_CREATE, failure, 0);
    assert(VFSNodeOpen(g_rootScope, __On(&g_disk, path), __FILE_CREATE | __FILE_READ_ACCESS,
                       FILE_PERMISSION_READ, &handle) == __ExpectedResult(failure));
    assert(handle == UUID_INVALID);
    if (failure == MODULE_FAIL_READ) {
        // The driver created the file, but its answer was lost.
        __ForgetDriverHandles(1);
        pthread_mutex_lock(&g_moduleLock);
        __ModuleFind(__DriverOf(&g_disk), path)->Used = false;
        pthread_mutex_unlock(&g_moduleLock);
    }
    __ExpectIdle(1, 0);
    assert(__Lookup(__On(&g_disk, path)) == OS_ENOENT);
}

// VFSNodeClose always removes the handle. A failed close is a warning.
static void
__TestClose(
    _In_ enum __ModuleFailure failure)
{
    char   path[16];
    uuid_t handle;

    snprintf(path, sizeof(path), "/c%i", g_names++);
    __CreateFile(&g_disk, path, "data");
    assert(VFSNodeOpen(g_rootScope, __On(&g_disk, path), __FILE_READ_ACCESS, 0, &handle) == OS_EOK);
    __ModuleFailCall(MODULE_CALL_CLOSE, failure, 0);
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    assert(VFSNodeHandleGet(handle, &(struct VFSNodeHandle*) { NULL }) == OS_ENOENT);
    if (failure != MODULE_FAIL_READ) {
        __ForgetDriverHandles(1);
    }
    __ExpectIdle(1, 1);
}

// Truncate, stat of the filesystem and unlink return the error, and change
// nothing.
static void
__TestHandleOperations(
    _In_ enum __ModuleFailure failure)
{
    struct VFSStatFS stats = { .Label = MARKER };
    UInteger64_t     size = { .QuadPart = 2 };
    struct VFSStat   stat;
    char             path[16];
    uuid_t           handle;

    snprintf(path, sizeof(path), "/h%i", g_names++);
    __CreateFile(&g_disk, path, "data");
    assert(VFSNodeOpen(g_rootScope, __On(&g_disk, path), __FILE_READ_ACCESS | __FILE_WRITE_ACCESS, 0, &handle) == OS_EOK);
    g_expectedHandles++;
    assert(VFSNodeStatHandle(handle, &stat) == OS_EOK);

    __ModuleFailCall(MODULE_CALL_TRUNCATE, failure, 0);
    assert(VFSNodeSetSize(handle, &size) == __ExpectedResult(failure));
    assert(VFSNodeStatHandle(handle, &(struct VFSStat) { 0 }) == OS_EOK);
    __ModuleFailCall(MODULE_CALL_FSSTAT, failure, 0);
    assert(VFSNodeStatFsHandle(handle, &stats) == __ExpectedResult(failure));
    assert(stats.Label == MARKER);
    __ModuleFailCall(MODULE_CALL_FSSTAT, failure, 0);
    assert(VFSNodeStatFs(g_rootScope, __On(&g_disk, path), 1, &stats) == __ExpectedResult(failure));
    assert(stats.Label == MARKER);

    // An open file cannot be unlinked, so it is closed first.
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    g_expectedHandles--;
    __ExpectIdle(3, 0);

    __ModuleFailCall(MODULE_CALL_UNLINK, failure, 0);
    assert(VFSNodeUnlink(g_rootScope, __On(&g_disk, path)) == __ExpectedResult(failure));
    if (failure == MODULE_FAIL_READ) {
        // The driver removed the file, but the answer was lost, so filed
        // keeps the node.
        pthread_mutex_lock(&g_moduleLock);
        assert(__ModuleFind(__DriverOf(&g_disk), path) == NULL);
        pthread_mutex_unlock(&g_moduleLock);
        assert(__Lookup(__On(&g_disk, path)) == OS_EOK);
    } else {
        __ExpectFile(&g_disk, path, "data");
    }

    // __DeleteNode logs the failed unlink as well.
    __ExpectIdle(2, 0);
}

// Connecting a filesystem whose setup fails leaves it without an interface.
// A failed stat gives the default label. A VFS that cannot be made after the
// setup destroys the driver's filesystem again. A failed destroy is a
// warning, and the interface is released anyway.
static void
__TestConnect(
    _In_ enum __ModuleFailure failure)
{
    struct VFSStorage    storage = { .Protocol.StorageType = VFSSTORAGE_TYPE_DEVICE };
    struct VFSInterface* interface;
    FileSystem_t*        fileSystem;
    UInteger64_t         sector = { .QuadPart = 2048 };
    guid_t               guid = GUID_EMPTY;
    int                  setups = g_moduleCalls[MODULE_CALL_SETUP];
    int                  destroys = g_moduleCalls[MODULE_CALL_DESTROY];

    strcpy(&storage.Stats.Serial[0], "sc");
    __MakeDirectory("/storage/sc");
    fileSystem = FileSystemNew(&storage, 0, &sector, 1, &guid);
    assert(fileSystem != NULL);

    interface = VFSInterfaceNew(1, &g_driverOps);
    __ModuleFailCall(MODULE_CALL_SETUP, failure, 0);
    assert(VFSFileSystemConnectInterface(fileSystem, interface) == __ExpectedResult(failure));
    assert(fileSystem->State == FileSystemState_NO_INTERFACE && fileSystem->VFS == NULL);
    if (failure == MODULE_FAIL_READ) {
        // The driver set up a filesystem that filed does not know.
        assert(g_moduleCalls[MODULE_CALL_SETUP] == setups + 1);
        for (int i = 0; i < MODULE_MAX_FILESYSTEMS; i++) {
            if (g_moduleFileSystems[i].Used && &g_moduleFileSystems[i] != __DriverOf(&g_disk)) {
                g_moduleFileSystems[i].Used = false;
            }
        }
    }
    __ExpectIdle(2, 0);

    // The setup succeeds, but VFSNew fails: the driver's filesystem is
    // destroyed again.
    setups = g_moduleCalls[MODULE_CALL_SETUP];
    destroys = g_moduleCalls[MODULE_CALL_DESTROY];
    g_shmCreateFailures = 1;
    assert(VFSFileSystemConnectInterface(fileSystem, interface) == OS_EOOM);
    assert(g_moduleCalls[MODULE_CALL_SETUP] == setups + 1);
    assert(g_moduleCalls[MODULE_CALL_DESTROY] == destroys + 1);
    assert(fileSystem->State == FileSystemState_NO_INTERFACE && fileSystem->Data == NULL);
    __ExpectIdle(1, 0);

    // The label cannot be read, so the mount uses p0.
    __ModuleFailCall(MODULE_CALL_FSSTAT, failure, 0);
    assert(VFSFileSystemConnectInterface(fileSystem, interface) == OS_EOK);
    assert(__Lookup("/storage/sc/p0") == OS_EOK);
    g_expectedHandles++;
    __ExpectIdle(1, 0);

    // A failed destroy when the filesystem is disconnected.
    assert(VFSFileSystemUnmount(fileSystem, 1) == OS_EOK);
    g_expectedHandles--;
    destroys = g_moduleCalls[MODULE_CALL_DESTROY];
    __ModuleFailCall(MODULE_CALL_DESTROY, failure, 0);
    assert(VFSFileSystemDisconnectInterface(fileSystem, 1) == __ExpectedResult(failure));
    assert(fileSystem->State == FileSystemState_NO_INTERFACE && fileSystem->Interface == NULL);
    if (failure != MODULE_FAIL_READ) {
        // The driver never destroyed its filesystem.
        assert(g_moduleCalls[MODULE_CALL_DESTROY] == destroys);
        for (int i = 0; i < MODULE_MAX_FILESYSTEMS; i++) {
            if (g_moduleFileSystems[i].Used && &g_moduleFileSystems[i] != __DriverOf(&g_disk)) {
                g_moduleFileSystems[i].Used = false;
            }
        }
    }
    __ExpectIdle(1, 1);

    // A failed destroy when a connected filesystem is destroyed.
    interface = VFSInterfaceNew(1, &g_driverOps);
    assert(VFSFileSystemConnectInterface(fileSystem, interface) == OS_EOK);
    assert(VFSFileSystemUnmount(fileSystem, 1) == OS_EOK);
    __ModuleFailCall(MODULE_CALL_DESTROY, failure, 0);
    FileSystemDestroy(fileSystem);
    for (int i = 0; i < MODULE_MAX_FILESYSTEMS; i++) {
        if (g_moduleFileSystems[i].Used && &g_moduleFileSystems[i] != __DriverOf(&g_disk)) {
            g_moduleFileSystems[i].Used = false;
        }
    }
    assert(VFSNodeUnlink(g_rootScope, "/storage/sc") == OS_EOK);
    __ExpectIdle(1, 1);
}

int
main(void)
{
    // Keep the case lines when an assert ends the test.
    setvbuf(stdout, NULL, _IONBF, 0);
    __TestEveryOperation();

    __CreateRootScope();
    __RegisterDisk(&g_disk);
    g_expectedHandles = __OpenHandles();
    __ExpectIdle(0, 0);
    for (size_t i = 0; i < sizeof(g_failures) / sizeof(g_failures[0]); i++) {
        __TestLoadDirectory(g_failures[i]);
        __TestOpen(g_failures[i]);
        __TestClose(g_failures[i]);
        __TestHandleOperations(g_failures[i]);
        __TestConnect(g_failures[i]);
        printf("callers with a failed %s passed\n", g_moduleFailureNames[g_failures[i]]);
    }
    __UnregisterDisk(&g_disk);
    assert(__ModuleOpenHandles() == 0 && __OpenHandles() == 0);
    puts("filed driver requests passed");
    return 0;
}
