// A disk is unregistered while another request still works on its
// filesystem. The teardown (VFSFileSystemUnmount, VFSFileSystemDisconnectInterface
// and FileSystemDestroy, in the order __StorageUnmount and VFSStorageDelete use)
// must wait for that request, and new lookups into the filesystem must fail
// with OS_ENOENT while it waits. Without the wait, the request writes into
// nodes VFSDestroy has freed, which ASan reports.
//
// Production code compiled here: filesystem.c, storage/utils.c, vfs/vfs.c,
// vfs/utils.c, vfs/bind.c, vfs/mount.c, vfs/memfs.c (the root scope), the
// mkdir, open, close, unlink and handle_stat handlers, and the libds
// hashtable, guid and mstring code. filed_vfs_doubles.inc replaces usched
// locks, the root scope, the VFS handle table, kernel services and mstr_fmt.
// This test adds:
// - The partition filesystem driver (interfaces/). An empty directory tree
//   that records its open driver handles. Create can be held at a gate,
//   which stands in for a driver that waits for a storage driver whose
//   card was removed. The gate releases the call with the result the test
//   chooses, like the storage driver failing its queued requests.
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

#define TEST_WAIT_SECONDS 5

static int g_errors;
static int g_bindRemovals;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (strstr(format, "VFSNodeDestroy removed the bind") == format) {
        g_bindRemovals++;
    }
    if (getenv("FILED_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        printf("log(%i): %s\n", level, format);
    }
}

#include "filed_vfs_doubles.inc"

// The gate holds one driver Create until the test releases it. Armed names
// the call to hold, and Path the name of the new entry. The held call returns
// Result.
enum __GateCall {
    GATE_NONE,
    GATE_CREATE
};

enum __GateState {
    GATE_IDLE,
    GATE_HOLDING,
    GATE_RELEASED
};

struct __Gate {
    pthread_mutex_t  Lock;
    pthread_cond_t   Changed;
    enum __GateCall  Armed;
    char             Path[64];
    enum __GateState State;
    oserr_t          Result;
};

static struct __Gate g_gate = {
    .Lock = PTHREAD_MUTEX_INITIALIZER,
    .Changed = PTHREAD_COND_INITIALIZER
};

static void
__ArmGate(
    _In_ enum __GateCall call,
    _In_ const char*     path)
{
    pthread_mutex_lock(&g_gate.Lock);
    g_gate.Armed = call;
    snprintf(g_gate.Path, sizeof(g_gate.Path), "%s", path);
    g_gate.State = GATE_IDLE;
    pthread_mutex_unlock(&g_gate.Lock);
}

// Holds the calling driver call if the gate is armed for it. Returns OS_EOK
// for a call that was not held, or the result the test released it with.
static oserr_t
__PassGate(
    _In_ enum __GateCall call,
    _In_ mstring_t*      path)
{
    char*   cpath = mstr_u8(path);
    oserr_t result = OS_EOK;

    assert(cpath != NULL);
    pthread_mutex_lock(&g_gate.Lock);
    if (g_gate.Armed == call && !strcmp(g_gate.Path, cpath)) {
        g_gate.Armed = GATE_NONE;
        g_gate.State = GATE_HOLDING;
        pthread_cond_broadcast(&g_gate.Changed);
        while (g_gate.State != GATE_RELEASED) {
            pthread_cond_wait(&g_gate.Changed, &g_gate.Lock);
        }
        result = g_gate.Result;
    }
    pthread_mutex_unlock(&g_gate.Lock);
    free(cpath);
    return result;
}

static void
__WaitForGate(void)
{
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += TEST_WAIT_SECONDS;
    pthread_mutex_lock(&g_gate.Lock);
    while (g_gate.State != GATE_HOLDING) {
        assert(pthread_cond_timedwait(&g_gate.Changed, &g_gate.Lock, &deadline) == 0);
    }
    pthread_mutex_unlock(&g_gate.Lock);
}

static void
__ReleaseGate(
    _In_ oserr_t result)
{
    pthread_mutex_lock(&g_gate.Lock);
    assert(g_gate.State == GATE_HOLDING);
    g_gate.Result = result;
    g_gate.State = GATE_RELEASED;
    pthread_cond_broadcast(&g_gate.Changed);
    pthread_mutex_unlock(&g_gate.Lock);
}

// The partition driver. Each Open and Create returns a driver handle that
// must be closed before Destroy, which is what MFS needs as well. The driver
// stays empty: Read returns no directory entries.
static int  g_driverHandles;
static int  g_driverDestroys;
static bool g_driverDestroyed;
static int  g_callsAfterDestroy;
static int  g_createsDone;

static void
__DriverCall(void)
{
    pthread_mutex_lock(&g_gate.Lock);
    if (g_driverDestroyed) {
        g_callsAfterDestroy++;
    }
    pthread_mutex_unlock(&g_gate.Lock);
}

static oserr_t
__TestInitialize(
    _In_  struct VFSInterface*         interface,
    _In_  struct VFSStorageParameters* storageParameters,
    _Out_ void**                       instanceData)
{
    mstring_t* label = mstr_new_u8("vali-data");

    assert(label != NULL);
    *instanceData = label;
    pthread_mutex_lock(&g_gate.Lock);
    g_driverDestroyed = false;
    pthread_mutex_unlock(&g_gate.Lock);
    return OS_EOK;
}

static oserr_t
__TestDestroy(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ unsigned int         unmountFlags)
{
    pthread_mutex_lock(&g_gate.Lock);
    assert(g_driverHandles == 0);
    g_driverDestroyed = true;
    g_driverDestroys++;
    pthread_mutex_unlock(&g_gate.Lock);
    mstr_delete(instanceData);
    return OS_EOK;
}

static oserr_t
__TestStat(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ struct VFSStatFS*    stat)
{
    __DriverCall();
    stat->Label = instanceData;
    return OS_EOK;
}

static void*
__NewDriverHandle(void)
{
    pthread_mutex_lock(&g_gate.Lock);
    g_driverHandles++;
    pthread_mutex_unlock(&g_gate.Lock);
    return malloc(1);
}

static oserr_t
__TestOpen(
    _In_      struct VFSInterface* interface,
    _In_      void*                instanceData,
    _In_      mstring_t*           path,
    _Out_Opt_ void**               dataOut)
{
    __DriverCall();
    *dataOut = __NewDriverHandle();
    return OS_EOK;
}

static oserr_t
__TestCreate(
    _In_  struct VFSInterface* interface,
    _In_  void*                instanceData,
    _In_  void*                data,
    _In_  mstring_t*           name,
    _In_  uint32_t             owner,
    _In_  uint32_t             flags,
    _In_  uint32_t             permissions,
    _Out_ void**               dataOut)
{
    oserr_t oserr;

    __DriverCall();
    oserr = __PassGate(GATE_CREATE, name);
    pthread_mutex_lock(&g_gate.Lock);
    g_createsDone++;
    pthread_mutex_unlock(&g_gate.Lock);
    if (oserr != OS_EOK) {
        return oserr;
    }
    *dataOut = __NewDriverHandle();
    return OS_EOK;
}

static oserr_t
__TestClose(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ void*                data)
{
    __DriverCall();
    pthread_mutex_lock(&g_gate.Lock);
    assert(g_driverHandles > 0);
    g_driverHandles--;
    pthread_mutex_unlock(&g_gate.Lock);
    free(data);
    return OS_EOK;
}

static oserr_t
__TestRead(
    _In_  struct VFSInterface* interface,
    _In_  void*                instanceData,
    _In_  void*                data,
    _In_  const SHMTransfer_t* transfer,
    _Out_ size_t*              read)
{
    __DriverCall();
    *read = 0;
    return OS_EOK;
}

static struct VFSOperations g_driverOperations = {
    .Initialize = __TestInitialize,
    .Destroy = __TestDestroy,
    .Stat = __TestStat,
    .Open = __TestOpen,
    .Create = __TestCreate,
    .Close = __TestClose,
    .Read = __TestRead,
};

// A registered disk with one partition, connected and mounted like
// VFSStorageRegisterAndSetupPartition does it: the default mount at
// /storage/test-disk/vali-data and the bind at /data.
struct __TestDisk {
    struct VFSStorage* Storage;
    FileSystem_t*      FileSystem;
    struct VFS*        VFS;
};

static void
__RegisterDisk(
    _Out_ struct __TestDisk* disk)
{
    UInteger64_t sector = { .QuadPart = 2048 };
    guid_t       guid = GUID_EMPTY;
    int          events = g_events;

    disk->Storage = calloc(1, sizeof(struct VFSStorage));
    assert(disk->Storage != NULL);
    disk->Storage->Protocol.StorageType = VFSSTORAGE_TYPE_DEVICE;
    strcpy(&disk->Storage->Stats.Serial[0], "test-disk");
    __MakeDirectory("/storage/test-disk");

    disk->FileSystem = FileSystemNew(
        disk->Storage, 0, &sector,
        VFSIdentifierAllocate(disk->Storage), &guid
    );
    assert(disk->FileSystem != NULL);
    assert(VFSFileSystemConnectInterface(
        disk->FileSystem,
        VFSInterfaceNew(UUID_INVALID, &g_driverOperations)) == OS_EOK);
    assert(VFSFileSystemMount(disk->FileSystem, NULL) == OS_EOK);
    assert(g_events == events + 1);
    disk->VFS = disk->FileSystem->VFS;
}

// The teardown sys_storage_unregister_invocation runs for a forced removal:
// __StorageUnmount, then VFSStorageDelete and __StorageRemoveDirectory.
static bool g_teardownDone;

static void*
__TeardownThread(
    _In_ void* argument)
{
    struct __TestDisk* disk = argument;

    assert(VFSFileSystemUnmount(disk->FileSystem, 1) == OS_EOK);
    assert(VFSFileSystemDisconnectInterface(disk->FileSystem, 1) == OS_EOK);
    FileSystemDestroy(disk->FileSystem);
    assert(VFSNodeUnlink(g_rootScope, "/storage/test-disk") == OS_EOK);
    free(disk->Storage);

    pthread_mutex_lock(&g_gate.Lock);
    g_teardownDone = true;
    pthread_mutex_unlock(&g_gate.Lock);
    return NULL;
}

static bool
__TeardownDone(void)
{
    bool done;

    pthread_mutex_lock(&g_gate.Lock);
    done = g_teardownDone;
    pthread_mutex_unlock(&g_gate.Lock);
    return done;
}

// Waits until the teardown has marked the VFS as closing and waits for the
// given number of operations. The VFS is still allocated then, because the
// teardown cannot finish while those operations are inside it.
static void
__WaitForClosing(
    _In_ struct VFS*  vfs,
    _In_ unsigned int operations)
{
    for (int i = 0; i < TEST_WAIT_SECONDS * 1000; i++) {
        enum VFSState state;
        unsigned int  count;

        usched_mtx_lock(&vfs->StateLock);
        state = vfs->State;
        count = vfs->Operations;
        usched_mtx_unlock(&vfs->StateLock);
        if (state == VFS_STATE_CLOSING) {
            assert(count == operations);
            return;
        }
        assert(!__TeardownDone());
        nanosleep(&(struct timespec) { .tv_nsec = 1000000 }, NULL);
    }
    abort();
}

// A served style mkdir that runs on its own thread.
struct __MkdirRequest {
    const char* Path;
    uuid_t      Handle;
    oserr_t     Result;
};

static void*
__MkdirThread(
    _In_ void* argument)
{
    struct __MkdirRequest* request = argument;
    mstring_t*             path = mstr_path_new_u8(request->Path);

    assert(path != NULL);
    request->Result = VFSNodeMkdir(g_rootScope, path, FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, &request->Handle);
    mstr_delete(path);
    return NULL;
}

static void
__ResetCounters(void)
{
    g_teardownDone = false;
    g_driverDestroys = 0;
    g_callsAfterDestroy = 0;
    g_createsDone = 0;
}

// served creates /data/served/apps while the card is removed. The create is
// held in the driver while the disk is unregistered. The teardown must wait
// for it, and only then destroy the VFS and the driver. createResult is what
// the driver returns once released: OS_ENOENT is what the storage driver
// gives for a removed card, OS_EOK a create that finished before the removal.
static void
__TestCreateDuringTeardown(
    _In_ oserr_t createResult)
{
    struct __MkdirRequest request = { .Path = "/data/served", .Handle = UUID_INVALID };
    struct __TestDisk     disk;
    struct VFSNode*       node;
    mstring_t*            path;
    pthread_t             requestThread;
    pthread_t             teardownThread;

    __ResetCounters();
    __RegisterDisk(&disk);
    __ArmGate(GATE_CREATE, "served");
    assert(pthread_create(&requestThread, NULL, __MkdirThread, &request) == 0);
    __WaitForGate();

    // The create holds one operation in the VFS. The teardown unmounts, marks
    // the VFS as closing and waits for that operation.
    assert(pthread_create(&teardownThread, NULL, __TeardownThread, &disk) == 0);
    __WaitForClosing(disk.VFS, 1);
    assert(g_driverDestroys == 0);

    // /data is gone, and a new lookup does not reach the filesystem.
    path = mstr_path_new_u8("/data/served");
    assert(path != NULL);
    assert(VFSNodeGet(g_rootScope, path, 1, &node) == OS_ENOENT);
    mstr_delete(path);

    __ReleaseGate(createResult);
    assert(pthread_join(requestThread, NULL) == 0);
    assert(pthread_join(teardownThread, NULL) == 0);

    // A create that succeeded also opened its handle, which the teardown
    // closed. Either way the driver was destroyed once, after the create, and
    // with all its handles closed.
    assert(request.Result == createResult);
    assert(g_createsDone == 1 && g_driverDestroys == 1 && g_callsAfterDestroy == 0);
    assert(g_driverHandles == 0 && __OpenHandles() == 0);
    assert(VFSNodeHandleGet(request.Handle, &(struct VFSNodeHandle*) { NULL }) == OS_ENOENT);
    assert(g_errors == 0);
}

// A lookup that reaches the filesystem through a bind made with VFSNodeBind
// (a sys_mount bind of /data/served at /srv). The teardown does not know this
// bind. A lookup through /srv while the teardown waits fails with OS_ENOENT,
// and the teardown removes the bind before the source node is freed.
static void
__TestBindIntoClosingFileSystem(void)
{
    struct __MkdirRequest request = { .Path = "/srv/apps", .Handle = UUID_INVALID };
    struct __TestDisk     disk;
    struct VFSNode*       node;
    mstring_t*            path;
    uuid_t                served;
    uuid_t                srv;
    pthread_t             requestThread;
    pthread_t             teardownThread;

    __ResetCounters();
    g_bindRemovals = 0;
    __RegisterDisk(&disk);

    path = mstr_path_new_u8("/data/served");
    assert(path != NULL);
    assert(VFSNodeMkdir(g_rootScope, path, FILE_PERMISSION_READ, &served) == OS_EOK);
    mstr_delete(path);
    path = mstr_path_new_u8("/srv");
    assert(path != NULL);
    assert(VFSNodeMkdir(g_rootScope, path, FILE_PERMISSION_READ, &srv) == OS_EOK);
    assert(VFSNodeBind(g_rootScope, served, srv) == OS_EOK);

    // A create through the bind holds an operation in the disk's VFS.
    __ArmGate(GATE_CREATE, "apps");
    assert(pthread_create(&requestThread, NULL, __MkdirThread, &request) == 0);
    __WaitForGate();
    assert(pthread_create(&teardownThread, NULL, __TeardownThread, &disk) == 0);
    __WaitForClosing(disk.VFS, 1);

    // The bind still points into the closing VFS, and the lookup stops there.
    assert(VFSNodeGet(g_rootScope, path, 1, &node) == OS_ENOENT);

    __ReleaseGate(OS_ENOENT);
    assert(pthread_join(requestThread, NULL) == 0);
    assert(pthread_join(teardownThread, NULL) == 0);
    assert(request.Result == OS_ENOENT);
    assert(g_driverDestroys == 1 && g_callsAfterDestroy == 0 && g_driverHandles == 0);
    assert(g_bindRemovals == 1);

    // /srv is a plain directory of the root scope again. The handle on the
    // bind source was closed by the teardown, the one on /srv is still open.
    assert(VFSNodeGet(g_rootScope, path, 1, &node) == OS_EOK);
    assert(__NodeIsRegular(node) && node->FileSystem == g_rootScope);
    assert(node->Mounts.element_count == 0);
    VFSNodePut(node);
    mstr_delete(path);
    assert(VFSNodeClose(g_rootScope, srv) == OS_EOK);
    assert(VFSNodeUnlink(g_rootScope, "/srv") == OS_EOK);
    assert(__OpenHandles() == 0);
    assert(g_errors == 0);
}

// A lookup into a VFS that is already closing is refused before it touches a
// node, and does not change the count of operations the teardown waits for.
static void
__TestEnterClosingFileSystem(void)
{
    struct __MkdirRequest request = { .Path = "/data/served", .Handle = UUID_INVALID };
    struct __TestDisk     disk;
    pthread_t             requestThread;
    pthread_t             teardownThread;

    __ResetCounters();
    __RegisterDisk(&disk);
    __ArmGate(GATE_CREATE, "served");
    assert(pthread_create(&requestThread, NULL, __MkdirThread, &request) == 0);
    __WaitForGate();
    assert(pthread_create(&teardownThread, NULL, __TeardownThread, &disk) == 0);
    __WaitForClosing(disk.VFS, 1);

    assert(VFSEnter(disk.VFS) == OS_ENOENT);
    __WaitForClosing(disk.VFS, 1);

    __ReleaseGate(OS_ENOENT);
    assert(pthread_join(requestThread, NULL) == 0);
    assert(pthread_join(teardownThread, NULL) == 0);
    assert(request.Result == OS_ENOENT && g_driverDestroys == 1);
    assert(g_errors == 0);
}

int
main(void)
{
    __CreateRootScope();

    __TestCreateDuringTeardown(OS_ENOENT);
    __TestCreateDuringTeardown(OS_EOK);
    __TestBindIntoClosingFileSystem();
    __TestEnterClosingFileSystem();

    // Every disk was torn down completely.
    assert(__OpenHandles() == 0 && g_driverHandles == 0);
    puts("filed teardown with requests in flight passed");
    return 0;
}
