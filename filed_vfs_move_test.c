// Moves and copies files with VFSNodeMove (vfs/handlers/move.c) through the
// real VFS, the real driver interface (interfaces/driver_ops.c) and two
// mounted disks, plus the memfs root scope. Every case checks the result, the
// files the driver has afterwards, what a lookup finds, and that the move left
// no node lock, no VFS operation (VFSEnter) and no driver handle behind.
//
// The old __MoveLocal never advanced its loop over the parents of the source,
// so any move of a file in a directory hung filed's job for ever. The runner
// builds the old handler as a mutation and expects a timeout.
//
// Production code compiled here: filesystem.c, storage/utils.c, vfs/vfs.c,
// vfs/utils.c, vfs/bind.c, vfs/mount.c, vfs/memfs.c, the mkdir, open, close,
// unlink, handle_stat and move handlers, interfaces/driver_ops.c and the libds
// code. filed_vfs_doubles.inc replaces the system services and usched, and
// filed_module_double.inc the filesystem driver process behind driver_ops.c.
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../services/filed/filesystem.c"
#include "../services/filed/storage/utils.c"
#include "../services/filed/vfs/private.h"
#include <os/shm.h>
#include <os/grants.h>

#define TEST_MAX_OSHANDLES 8192
#define TEST_THREAD_MOVES  25

static int g_errors;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (getenv("FILED_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        fprintf(stderr, "log(%i): %s\n", level, format);
    }
}

#include "filed_vfs_doubles.inc"
#include "filed_module_double.inc"

static struct __TestDisk g_diskA = { .Serial = "sa", .Label = "disk-a" };
static struct __TestDisk g_diskB = { .Serial = "sb", .Label = "disk-b" };
// The VFS handles that stay open: the mount directories, and files a case
// keeps open.
static int               g_expectedHandles;

// Nothing the move took is still held: no node lock, no VFS operation, no
// driver handle and no VFS handle other than g_expectedHandles. errors is the
// number of error logs the case expects.
static void
__ExpectIdle(
    _In_ int errors)
{
    __ExpectUnlocked(g_rootScope->Root);
    __ExpectNoOperations(g_rootScope);
    __ExpectNoOperations(g_diskA.VFS);
    __ExpectNoOperations(g_diskB.VFS);
    assert(__ModuleOpenHandles() == 0);
    assert(__OpenHandles() == g_expectedHandles);
    assert(g_errors == errors);
    g_errors = 0;
}

static oserr_t
__Move(
    _In_ const char* from,
    _In_ const char* to,
    _In_ bool        copy)
{
    return VFSNodeMove(g_rootScope, from, to, copy);
}

// The first case, as the old handler hung in its loop over the parents of the
// source here.
static void
__TestMoveBetweenDirectories(void)
{
    __CreateDirectory(&g_diskA, "/dir");
    __CreateDirectory(&g_diskA, "/other");
    __CreateFile(&g_diskA, "/dir/one", "first file");
    assert(__Move(__On(&g_diskA, "/dir/one"), __On(&g_diskA, "/other/three"), false) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/one", NULL);
    __ExpectFile(&g_diskA, "/other/three", "first file");

    // Into a parent directory, and to the root of the filesystem.
    assert(__Move(__On(&g_diskA, "/other/three"), __On(&g_diskA, "/three"), false) == OS_EOK);
    __ExpectFile(&g_diskA, "/other/three", NULL);
    __ExpectFile(&g_diskA, "/three", "first file");
    assert(__Move(__On(&g_diskA, "/three"), __On(&g_diskA, "/dir/three"), false) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/three", "first file");
    __ExpectIdle(0);
    puts("move between directories passed");
}

static void
__TestMoveInOneDirectory(void)
{
    assert(__Move(__On(&g_diskA, "/dir/three"), __On(&g_diskA, "/dir/four"), false) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/three", NULL);
    __ExpectFile(&g_diskA, "/dir/four", "first file");
    __ExpectIdle(0);
    puts("move inside one directory passed");
}

static void
__TestMoveOntoExistingName(void)
{
    struct __ModuleEntry* entry;

    __CreateFile(&g_diskA, "/dir/x", "x data");
    __CreateFile(&g_diskA, "/dir/y", "y data");
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskA, "/dir/y"), false) == OS_EEXISTS);
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskA, "/dir/x"), false) == OS_EEXISTS);
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskA, "/dir"), false) == OS_EEXISTS);

    // A name that is only on the disk, in a directory filed has not loaded
    // yet. A new directory node is loaded on its first lookup.
    __CreateDirectory(&g_diskA, "/fresh");
    pthread_mutex_lock(&g_moduleLock);
    entry = __ModuleAdd(__DriverOf(&g_diskA), "/fresh/z", FILE_FLAG_FILE, FILE_PERMISSION_READ);
    entry->Size = 1;
    entry->Data[0] = 'z';
    pthread_mutex_unlock(&g_moduleLock);
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskA, "/fresh/z"), false) == OS_EEXISTS);

    // And across filesystems.
    __CreateDirectory(&g_diskB, "/in");
    __CreateFile(&g_diskB, "/in/x", "b data");
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskB, "/in/x"), false) == OS_EEXISTS);

    __ExpectFile(&g_diskA, "/dir/x", "x data");
    __ExpectFile(&g_diskA, "/dir/y", "y data");
    __ExpectFile(&g_diskA, "/fresh/z", "z");
    __ExpectFile(&g_diskB, "/in/x", "b data");
    __ExpectIdle(0);
    puts("move onto an existing name passed");
}

static void
__TestMoveOfMissingSource(void)
{
    assert(__Move(__On(&g_diskA, "/dir/missing"), __On(&g_diskA, "/dir/new"), false) == OS_ENOENT);
    assert(__Move(__On(&g_diskA, "/nodir/missing"), __On(&g_diskA, "/dir/new"), false) == OS_ENOENT);
    assert(__Move(__On(&g_diskA, "/dir/missing"), __On(&g_diskB, "/in/new"), false) == OS_ENOENT);

    // A missing target directory, and the root, which cannot be moved.
    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskA, "/nodir/x"), false) == OS_ENOENT);
    assert(__Move("/", "/elsewhere", false) == OS_EPERMISSIONS);
    __ExpectFile(&g_diskA, "/dir/new", NULL);
    __ExpectFile(&g_diskB, "/in/new", NULL);
    __ExpectFile(&g_diskA, "/dir/x", "x data");
    __ExpectIdle(0);
    puts("move of a missing source passed");
}

static void
__TestMoveAcrossFileSystems(void)
{
    struct VFSStat stat;
    uuid_t         handle;

    assert(__Move(__On(&g_diskA, "/dir/x"), __On(&g_diskB, "/in/moved"), false) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/x", NULL);
    __ExpectFile(&g_diskB, "/in/moved", "x data");
    assert(VFSNodeStat(g_rootScope, __On(&g_diskB, "/in/moved"), 1, &stat) == OS_EOK);
    assert(stat.Size == strlen("x data") && FILE_FLAG_TYPE(stat.Flags) == FILE_FLAG_FILE);

    // memfs cannot write files, so a copy to the root scope fails. The empty
    // file the copy created there is removed again, and the source stays.
    assert(__Move(__On(&g_diskB, "/in/moved"), "/moved", false) == OS_ENOTSUPPORTED);
    assert(__Lookup("/moved") == OS_ENOENT);
    __ExpectFile(&g_diskB, "/in/moved", "x data");
    __ExpectIdle(1);

    // An empty memfs file can be moved to a disk, and back to disk A.
    assert(VFSNodeOpen(g_rootScope, "/memfile", __FILE_CREATE | __FILE_READ_ACCESS | __FILE_WRITE_ACCESS,
                       FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, &handle) == OS_EOK);
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    assert(__Move("/memfile", __On(&g_diskA, "/dir/empty"), false) == OS_EOK);
    assert(__Lookup("/memfile") == OS_ENOENT);
    __ExpectFile(&g_diskA, "/dir/empty", "");
    assert(__Move(__On(&g_diskB, "/in/moved"), __On(&g_diskA, "/dir/back"), false) == OS_EOK);
    __ExpectFile(&g_diskB, "/in/moved", NULL);
    __ExpectFile(&g_diskA, "/dir/back", "x data");

    // A move within memfs, which does not support it.
    assert(VFSNodeOpen(g_rootScope, "/tmpfile", __FILE_CREATE | __FILE_READ_ACCESS | __FILE_WRITE_ACCESS,
                       FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, &handle) == OS_EOK);
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    assert(__Move("/tmpfile", "/tmpfile2", false) == OS_ENOTSUPPORTED);
    assert(__Lookup("/tmpfile") == OS_EOK && __Lookup("/tmpfile2") == OS_ENOENT);
    assert(VFSNodeUnlink(g_rootScope, "/tmpfile") == OS_EOK);
    __ExpectIdle(0);
    puts("move across two filesystems passed");
}

static void
__TestCopy(void)
{
    assert(__Move(__On(&g_diskA, "/dir/y"), __On(&g_diskA, "/other/y"), true) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/y", "y data");
    __ExpectFile(&g_diskA, "/other/y", "y data");
    assert(__Move(__On(&g_diskA, "/dir/y"), __On(&g_diskB, "/in/y"), true) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/y", "y data");
    __ExpectFile(&g_diskB, "/in/y", "y data");
    __ExpectIdle(0);
    puts("copy within and across filesystems passed");
}

// A move destroys the source node, so an open file or a directory is refused.
// A copy of an open file is fine.
static void
__TestMoveOfBusySource(void)
{
    uuid_t handle;

    assert(VFSNodeOpen(g_rootScope, __On(&g_diskA, "/dir/y"), __FILE_READ_ACCESS | __FILE_READ_SHARE, 0, &handle) == OS_EOK);
    g_expectedHandles++;
    assert(__Move(__On(&g_diskA, "/dir/y"), __On(&g_diskA, "/dir/y2"), false) == OS_EBUSY);
    assert(__Move(__On(&g_diskA, "/dir/y"), __On(&g_diskB, "/in/y2"), false) == OS_EBUSY);
    assert(__Move(__On(&g_diskA, "/dir/y"), __On(&g_diskA, "/dir/y2"), true) == OS_EOK);
    __ExpectFile(&g_diskA, "/dir/y2", "y data");
    assert(__ModuleOpenHandles() == 1);
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    g_expectedHandles--;

    assert(__Move(__On(&g_diskA, "/other"), __On(&g_diskA, "/dir/other"), false) == OS_EISDIR);
    assert(__Move(__On(&g_diskA, ""), __On(&g_diskB, "/in/whole"), false) == OS_EPERMISSIONS);
    __ExpectFile(&g_diskA, "/dir/y", "y data");
    __ExpectIdle(0);
    puts("move of an open file or a directory passed");
}

// A failed copy to another filesystem leaves the source in place and removes
// the incomplete file. A move within one filesystem that the driver never ran
// changes nothing. The driver logs one error for each failed request, and the
// copy one more.
static void
__TestFailedMove(void)
{
    __ModuleFailCall(MODULE_CALL_WRITE, MODULE_FAIL_STATUS, 0);
    assert(__Move(__On(&g_diskA, "/dir/back"), __On(&g_diskB, "/in/half"), false) == OS_EUNKNOWN);
    __ExpectFile(&g_diskA, "/dir/back", "x data");
    __ExpectFile(&g_diskB, "/in/half", NULL);
    __ExpectIdle(2);

    __ModuleFailCall(MODULE_CALL_MOVE, MODULE_FAIL_WAIT, 0);
    assert(__Move(__On(&g_diskA, "/dir/back"), __On(&g_diskA, "/dir/front"), false) == OS_EUNKNOWN);
    __ExpectFile(&g_diskA, "/dir/back", "x data");
    __ExpectFile(&g_diskA, "/dir/front", NULL);
    __ExpectIdle(1);

    __ModuleFailCall(MODULE_CALL_MOVE, MODULE_FAIL_SEND, 0);
    assert(__Move(__On(&g_diskA, "/dir/back"), __On(&g_diskA, "/dir/front"), false) == OS_EUNKNOWN);
    __ExpectFile(&g_diskA, "/dir/back", "x data");
    __ExpectFile(&g_diskA, "/dir/front", NULL);
    __ExpectIdle(1);
    puts("failed moves passed");
}

// Two jobs move files between the same two directories in opposite
// directions. Taking both directory locks at once would deadlock them.
struct __MoveThread {
    const char* From;
    const char* To;
    const char* Name;
};

static void*
__MoveThread(
    _In_ void* argument)
{
    struct __MoveThread* request = argument;
    char                 from[64];
    char                 to[64];

    for (int i = 0; i < TEST_THREAD_MOVES; i++) {
        snprintf(from, sizeof(from), "%s/%s", (i % 2) ? request->To : request->From, request->Name);
        snprintf(to, sizeof(to), "%s/%s", (i % 2) ? request->From : request->To, request->Name);
        assert(__Move(__On(&g_diskA, from), __On(&g_diskA, to), false) == OS_EOK);
    }
    return NULL;
}

static void
__TestOppositeMoves(void)
{
    struct __MoveThread first = { .From = "/left", .To = "/right", .Name = "f1" };
    struct __MoveThread second = { .From = "/right", .To = "/left", .Name = "f2" };
    pthread_t           threads[2];

    __CreateDirectory(&g_diskA, "/left");
    __CreateDirectory(&g_diskA, "/right");
    __CreateFile(&g_diskA, "/left/f1", "one");
    __CreateFile(&g_diskA, "/right/f2", "two");
    assert(pthread_create(&threads[0], NULL, __MoveThread, &first) == 0);
    assert(pthread_create(&threads[1], NULL, __MoveThread, &second) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);

    // An odd number of moves ends each file in the other directory.
    __ExpectFile(&g_diskA, "/right/f1", "one");
    __ExpectFile(&g_diskA, "/left/f2", "two");
    __ExpectFile(&g_diskA, "/left/f1", NULL);
    __ExpectFile(&g_diskA, "/right/f2", NULL);
    __ExpectIdle(0);
    puts("moves in opposite directions passed");
}

int
main(void)
{
    // Keep the case lines when an assert ends the test.
    setvbuf(stdout, NULL, _IONBF, 0);
    __CreateRootScope();
    __RegisterDisk(&g_diskA);
    __RegisterDisk(&g_diskB);
    g_expectedHandles = __OpenHandles();
    __ExpectIdle(0);

    __TestMoveBetweenDirectories();
    __TestMoveInOneDirectory();
    __TestMoveOntoExistingName();
    __TestMoveOfMissingSource();
    __TestMoveAcrossFileSystems();
    __TestCopy();
    __TestMoveOfBusySource();
    __TestFailedMove();
    __TestOppositeMoves();

    __UnregisterDisk(&g_diskA);
    __UnregisterDisk(&g_diskB);
    assert(__ModuleOpenHandles() == 0 && __OpenHandles() == 0);
    puts("filed moves passed");
    return 0;
}
