// The MFS mount lock (FileSystemMFS_t::Lock in modules/filesystems/mfs/mfs.h).
//
// filed sends each request to MFS as its own job (librt/libgracht, one usched
// job per message). A job lets other jobs run each time it waits for the
// disk. One mount has one transfer buffer, so a second operation that runs
// while the first one waits can replace the data the first one is about to
// write. On AMD64 this lost a new directory record: FsCreate had added the
// record to the root directory bucket in the transfer buffer, FsOpen read the
// same bucket into the buffer again, and the create then wrote the old
// directory back.
//
// The test reuses the in-memory partition and the storage doubles of
// mfs_create_test.c, and links the real MFS sources: main.c, records.c,
// bucket_map.c, utilities.c, file_operations.c and directory_operations.c.
//
// Jobs are modelled with threads, of which only one runs at a time. A thread
// runs only while it holds the turn (g_turn). The main thread is job 1. When
// job 1 makes a chosen disk transfer, the storage double starts job 2 and
// gives it the turn, the way the scheduler runs another job while one waits
// for the disk. Job 2 runs until it ends, or until it must wait for the mount
// lock. The lock double below works like usched_mtx: a job that finds the lock
// taken by the other job waits, and gives the turn back. An unlock hands the
// lock straight to the waiting job, which runs when job 1 has finished its
// operation. With the lock, job 2 therefore runs after job 1. Without it, job
// 2 runs in the middle of job 1, and job 1 writes back what job 2 left in the
// transfer buffer.
//
// The test also counts lock and unlock calls around each Fs* function, on
// success and on each failure path, and checks FsDestroy.
#define MFS_TEST_OWN_DOUBLES
#define main __MfsCreateTestMain
#include "mfs_create_test.c"
#undef main

#include <pthread.h>

extern oserr_t FsOpen(void*, mstring_t*, void**);
extern oserr_t FsStat(void*, struct VFSStatFS*);
extern oserr_t FsUnlink(void*, mstring_t*);
extern oserr_t FsLink(void*, void*, mstring_t*, mstring_t*, int);
extern oserr_t FsReadLink(void*, mstring_t*, mstring_t**);
extern oserr_t FsMove(void*, mstring_t*, mstring_t*, int);
extern oserr_t FsRead(void*, void*, uuid_t, void*, size_t, size_t, size_t*);
extern oserr_t FsWrite(void*, void*, uuid_t, void*, size_t, size_t, size_t*);
extern oserr_t FsSeek(void*, void*, uint64_t, uint64_t*);
extern oserr_t FsTruncate(void*, void*, uint64_t);
extern oserr_t FsDestroy(void*, unsigned int);

#define TEST_BUCKET_BYTES (TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE)

void OSHandleDestroy(OSHandle_t* handle) { }

// The turn. g_runningJob is the job that may run (1 or 2). A thread waits on
// g_turnChanged until it is its own number.
static pthread_mutex_t g_turn = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_turnChanged = PTHREAD_COND_INITIALIZER;
static int             g_runningJob = 1;
static _Thread_local int t_job = 1;

// What job 2 is doing.
// SECOND_JOB_NONE:    not started.
// SECOND_JOB_RUNNING: started, and runs or may run when it gets the turn.
// SECOND_JOB_WAITING: waits for the mount lock, which job 1 holds.
// SECOND_JOB_DONE:    its operation has ended.
enum __SecondJobState {
    SECOND_JOB_NONE,
    SECOND_JOB_RUNNING,
    SECOND_JOB_WAITING,
    SECOND_JOB_DONE
};

/**
 * Job 2 and when it starts. It starts at the first transfer of job 1 that
 * matches Write and lies in [FirstSector, EndSector).
 */
struct __SecondJob {
    enum __SecondJobState State;
    void                  (*Run)(void);
    pthread_t             Thread;
    bool                  Write;
    uint64_t              FirstSector;
    uint64_t              EndSector;
};

static struct __SecondJob g_second;

// Lock statistics. The lock double keeps the number of the job that holds the
// lock in usched_mtx::references (0 when free). Only the mount lock uses it.
static int g_lockCalls;
static int g_unlockCalls;

// Set when job 1 has returned from its operation. Job 2 records what it saw.
static bool g_firstReturned;
static bool g_secondSawFirstReturned;

// Gives the turn to job and waits until this thread has it again.
static void
__SwitchTo(
    _In_ int job)
{
    pthread_mutex_lock(&g_turn);
    g_runningJob = job;
    pthread_cond_broadcast(&g_turnChanged);
    while (g_runningJob != t_job) {
        pthread_cond_wait(&g_turnChanged, &g_turn);
    }
    pthread_mutex_unlock(&g_turn);
}

void usched_mtx_init(struct usched_mtx* mutex, int type)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->type = type;
}

void usched_mtx_lock(struct usched_mtx* mutex)
{
    g_lockCalls++;
    if (mutex->references == 0) {
        mutex->references = t_job;
        return;
    }

    // MFS uses a plain lock. Taking it twice in one job waits forever.
    assert(mutex->references != t_job);

    // Only job 2 can find the lock held by the other job. It waits until job
    // 1 hands the lock over in usched_mtx_unlock.
    assert(t_job == 2);
    g_second.State = SECOND_JOB_WAITING;
    __SwitchTo(1);
    assert(mutex->references == 2 && g_second.State == SECOND_JOB_RUNNING);
}

int usched_mtx_trylock(struct usched_mtx* mutex)
{
    if (mutex->references != 0) {
        return -1;
    }
    mutex->references = t_job;
    return 0;
}

void usched_mtx_unlock(struct usched_mtx* mutex)
{
    assert(mutex->references == t_job);
    g_unlockCalls++;
    if (t_job == 1 && g_second.State == SECOND_JOB_WAITING) {
        // Hand the lock to job 2. It runs once job 1 gives up the turn.
        mutex->references = 2;
        g_second.State = SECOND_JOB_RUNNING;
        return;
    }
    mutex->references = 0;
}

static void*
__SecondJobMain(
    _In_ void* context)
{
    t_job = 2;
    pthread_mutex_lock(&g_turn);
    while (g_runningJob != 2) {
        pthread_cond_wait(&g_turnChanged, &g_turn);
    }
    pthread_mutex_unlock(&g_turn);

    g_second.Run();

    pthread_mutex_lock(&g_turn);
    g_second.State = SECOND_JOB_DONE;
    g_runningJob = 1;
    pthread_cond_broadcast(&g_turnChanged);
    pthread_mutex_unlock(&g_turn);
    return NULL;
}

// The storage double calls this before each transfer. The first matching
// transfer of job 1 starts job 2 and gives it the turn.
static void
__MaybeStartSecondJob(
    _In_ bool     write,
    _In_ uint64_t sector,
    _In_ size_t   count)
{
    if (t_job != 1 || g_second.State != SECOND_JOB_NONE || write != g_second.Write) {
        return;
    }
    if (sector + count <= g_second.FirstSector || sector >= g_second.EndSector) {
        return;
    }

    g_second.State = SECOND_JOB_RUNNING;
    assert(pthread_create(&g_second.Thread, NULL, __SecondJobMain, NULL) == 0);
    __SwitchTo(2);
}

// Job 2 runs Run at the first transfer of job 1 that matches write and the
// sectors of length buckets starting at bucket.
static void
__ArmSecondJob(
    _In_ void     (*run)(void),
    _In_ bool     write,
    _In_ uint32_t bucket,
    _In_ uint32_t length)
{
    memset(&g_second, 0, sizeof(g_second));
    g_second.State = SECOND_JOB_NONE;
    g_second.Run = run;
    g_second.Write = write;
    g_second.FirstSector = MFS_GETSECTOR(&g_mfs, bucket);
    g_second.EndSector = g_second.FirstSector + MFS_SECTORCOUNT(&g_mfs, length);
    g_firstReturned = false;
    g_secondSawFirstReturned = false;
    g_beforeTransfer = __MaybeStartSecondJob;
}

// Job 1 has returned from its operation. The lock must be free, or handed to
// job 2. Job 2 gets the turn until it has ended.
static void
__FinishSecondJob(void)
{
    g_firstReturned = true;
    g_beforeTransfer = NULL;
    assert(g_second.State != SECOND_JOB_NONE);
    assert(g_second.State != SECOND_JOB_WAITING);
    if (g_second.State == SECOND_JOB_RUNNING) {
        __SwitchTo(2);
    }
    assert(g_second.State == SECOND_JOB_DONE);
    assert(pthread_join(g_second.Thread, NULL) == 0);
    assert(g_mfs.Lock.references == 0);
}

static mstring_t*
__Path(
    _In_ const char* path)
{
    mstring_t* mpath = mstr_path_new_u8(path);

    assert(mpath != NULL);
    return mpath;
}

static void
__FreeEntry(
    _In_ MFSEntry_t* entry)
{
    if (entry != NULL) {
        mstr_delete(entry->Name);
        free(entry);
    }
}

// Looks a path up on the disk, without the lock. Returns NULL when missing.
static MFSEntry_t*
__Find(
    _In_ const char* path)
{
    enum MFSSearchResult result;
    MFSEntry_t*          entry = NULL;
    mstring_t*           mpath = __Path(path);

    assert(MfsFindRecord(&g_mfs, &g_mfs.RootEntry, mpath, &entry, &result) == OS_EOK);
    mstr_delete(mpath);
    return result == MFS_SEARCH_FOUND ? entry : NULL;
}

static bool
__Exists(
    _In_ const char* path)
{
    MFSEntry_t* entry = __Find(path);

    __FreeEntry(entry);
    return entry != NULL;
}

static oserr_t
__CreateIn(
    _In_  MFSEntry_t*  parent,
    _In_  const char*  name,
    _In_  uint32_t     flags,
    _Out_ MFSEntry_t** entryOut)
{
    mstring_t* mname = mstr_new_u8(name);
    void*      data = NULL;
    oserr_t    oserr;

    assert(mname != NULL);
    oserr = FsCreate(&g_mfs, parent, mname, 0, flags, FILE_PERMISSION_READ, &data);
    mstr_delete(mname);
    *entryOut = data;
    return oserr;
}

static MFSEntry_t*
__Open(
    _In_ const char* path)
{
    mstring_t* mpath = __Path(path);
    void*      data = NULL;

    assert(FsOpen(&g_mfs, mpath, &data) == OS_EOK && data != NULL);
    mstr_delete(mpath);
    return data;
}

// Creates and closes a record. Fails the test when that does not work.
static void
__MustCreate(
    _In_ MFSEntry_t* parent,
    _In_ const char* name,
    _In_ uint32_t    flags)
{
    MFSEntry_t* entry;

    assert(__CreateIn(parent, name, flags, &entry) == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
}

// The tree every case starts from:
//   /served/apps/  directory, like the paths served sets up
//   /d/            directory with buckets, as it holds /d/x
//   /d/x           empty file
//   /a             empty file
//   /f             file of one bucket, every byte 0x5A
static void
__Setup(void)
{
    MFSEntry_t* served;
    MFSEntry_t* d;
    MFSEntry_t* f;

    __Format();
    assert(__CreateIn(&g_mfs.RootEntry, "served", FILE_FLAG_DIRECTORY, &served) == OS_EOK);
    __MustCreate(served, "apps", FILE_FLAG_DIRECTORY);
    assert(FsClose(&g_mfs, served) == OS_EOK);
    assert(__CreateIn(&g_mfs.RootEntry, "d", FILE_FLAG_DIRECTORY, &d) == OS_EOK);
    __MustCreate(d, "x", 0);
    assert(FsClose(&g_mfs, d) == OS_EOK);
    __MustCreate(&g_mfs.RootEntry, "a", 0);

    assert(__CreateIn(&g_mfs.RootEntry, "f", 0, &f) == OS_EOK);
    assert(FsTruncate(&g_mfs, f, TEST_BUCKET_BYTES) == OS_EOK);
    memset(__Bucket(f->StartBucket), 0x5A, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, f) == OS_EOK);

    g_errors = 0;
    g_writes = 0;
    g_lockCalls = 0;
    g_unlockCalls = 0;
}

static void
__Teardown(void)
{
    assert(g_mfs.Lock.references == 0);
    assert(g_lockCalls == g_unlockCalls);
    assert(g_errors == 0);
    __Unmount();
}

// Job 2 bodies. Each records whether job 1 had returned when its first
// operation got the lock and finished.
static oserr_t g_secondStatus;

static void
__SecondOpenServedApps(void)
{
    MFSEntry_t* entry = NULL;
    mstring_t*  path = __Path("/served/apps");

    g_secondStatus = FsOpen(&g_mfs, path, (void**)&entry);
    g_secondSawFirstReturned = g_firstReturned;
    mstr_delete(path);
    if (g_secondStatus == OS_EOK) {
        assert(FsClose(&g_mfs, entry) == OS_EOK);
    }
}

static void
__SecondCreateTwo(void)
{
    MFSEntry_t* entry = NULL;

    g_secondStatus = __CreateIn(&g_mfs.RootEntry, "two", FILE_FLAG_DIRECTORY, &entry);
    g_secondSawFirstReturned = g_firstReturned;
    if (g_secondStatus == OS_EOK) {
        assert(FsClose(&g_mfs, entry) == OS_EOK);
    }
}

static void
__SecondCreateInD(void)
{
    MFSEntry_t* d = __Open("/d");
    MFSEntry_t* entry = NULL;

    g_secondSawFirstReturned = g_firstReturned;
    g_secondStatus = __CreateIn(d, "y", 0, &entry);
    if (g_secondStatus == OS_EOK) {
        assert(FsClose(&g_mfs, entry) == OS_EOK);
    }
    assert(FsClose(&g_mfs, d) == OS_EOK);
}

// The case seen on AMD64: FsCreate adds a directory to the root directory,
// and FsOpen of /served/apps reads the root directory while the create writes
// it. The new record must be on the disk afterwards.
static void
__TestCreateWithOpen(void)
{
    MFSEntry_t* entry;

    __Setup();
    __ArmSecondJob(__SecondOpenServedApps, true, g_mfs.RootEntry.StartBucket, g_mfs.RootEntry.StartLength);
    assert(__CreateIn(&g_mfs.RootEntry, "exitdemo-dir", FILE_FLAG_DIRECTORY, &entry) == OS_EOK);
    __FinishSecondJob();
    assert(g_secondStatus == OS_EOK);
    assert(__Exists("/exitdemo-dir"));
    assert(__Exists("/served/apps"));
    assert(g_secondSawFirstReturned);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    __Teardown();
}

// Two creates in the root directory. Both look for a free record, so without
// the lock both pick the same one.
static void
__TestTwoCreatesInRoot(void)
{
    MFSEntry_t* one;
    MFSEntry_t* two;
    MFSEntry_t* entry;

    __Setup();
    __ArmSecondJob(__SecondCreateTwo, true, g_mfs.RootEntry.StartBucket, g_mfs.RootEntry.StartLength);
    assert(__CreateIn(&g_mfs.RootEntry, "one", FILE_FLAG_DIRECTORY, &entry) == OS_EOK);
    __FinishSecondJob();
    assert(g_secondStatus == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    one = __Find("/one");
    two = __Find("/two");
    assert(one != NULL && two != NULL);
    assert(one->DirectoryBucket != two->DirectoryBucket || one->DirectoryIndex != two->DirectoryIndex);
    __FreeEntry(one);
    __FreeEntry(two);
    assert(__Exists("/served/apps") && __Exists("/d/x") && __Exists("/a") && __Exists("/f"));
    assert(g_secondSawFirstReturned);
    __Teardown();
}

// Two creates in /d, each through its own handle of /d, as filed opens one
// for each request.
static void
__TestTwoCreatesInDirectory(void)
{
    MFSEntry_t* d;
    MFSEntry_t* entry;

    __Setup();
    d = __Open("/d");
    __ArmSecondJob(__SecondCreateInD, true, d->StartBucket, d->StartLength);
    assert(__CreateIn(d, "w", 0, &entry) == OS_EOK);
    __FinishSecondJob();
    assert(g_secondStatus == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(FsClose(&g_mfs, d) == OS_EOK);

    assert(__Exists("/d/w") && __Exists("/d/y") && __Exists("/d/x"));
    assert(g_secondSawFirstReturned);
    __Teardown();
}

// A rename in place writes the directory bucket as well. A create in the same
// directory must not undo it, and the rename must not undo the create.
static void
__TestRenameWithCreate(void)
{
    mstring_t* from = __Path("/a");
    mstring_t* to = __Path("/b");

    __Setup();
    __ArmSecondJob(__SecondCreateTwo, true, g_mfs.RootEntry.StartBucket, g_mfs.RootEntry.StartLength);
    assert(FsMove(&g_mfs, from, to, 0) == OS_EOK);
    __FinishSecondJob();
    assert(g_secondStatus == OS_EOK);
    mstr_delete(from);
    mstr_delete(to);

    assert(!__Exists("/a") && __Exists("/b") && __Exists("/two"));
    assert(g_secondSawFirstReturned);
    __Teardown();
}

// A small write goes through the transfer buffer: read the sector, copy the
// new bytes in, write the sector. An open in between reads a directory into
// the same buffer, and the write would put directory records into the file.
static void
__TestWriteWithOpen(void)
{
    MFSEntry_t* f;
    uint8_t     data[100];
    uint8_t*    sector;
    size_t      written;
    uint64_t    position;

    __Setup();
    f = __Open("/f");
    assert(FsSeek(&g_mfs, f, 10, &position) == OS_EOK && position == 10);
    memset(data, 0xC3, sizeof(data));
    __ArmSecondJob(__SecondOpenServedApps, true, f->StartBucket, f->StartLength);
    assert(FsWrite(&g_mfs, f, TEST_BUFFER_ID, data, 0, sizeof(data), &written) == OS_EOK);
    __FinishSecondJob();
    assert(written == sizeof(data));
    assert(g_secondStatus == OS_EOK);

    sector = __Bucket(f->StartBucket);
    for (size_t i = 0; i < TEST_SECTOR_SIZE; i++) {
        assert(sector[i] == ((i >= 10 && i < 10 + sizeof(data)) ? 0xC3 : 0x5A));
    }
    assert(g_secondSawFirstReturned);
    assert(FsClose(&g_mfs, f) == OS_EOK);
    __Teardown();
}

// Each Fs* call below must take the lock once and release it once, also when
// it fails, and leave it free.
#define EXPECT_LOCKED_ONCE(call, check)                                     \
    do {                                                                    \
        int     __locks = g_lockCalls;                                      \
        int     __unlocks = g_unlockCalls;                                  \
        oserr_t status = (call);                                            \
        if (!(check)) {                                                     \
            printf("line %d: %s returned %u\n", __LINE__, #call, status);  \
            fflush(stdout);                                                 \
            abort();                                                        \
        }                                                                   \
        assert(g_lockCalls == __locks + 1);                                 \
        assert(g_unlockCalls == __unlocks + 1);                             \
        assert(g_mfs.Lock.references == 0);                                 \
    } while (0)

static void
__TestLockOnEveryPath(void)
{
    struct VFSStatFS stat;
    MFSEntry_t*      root;
    MFSEntry_t*      f;
    MFSEntry_t*      entry = NULL;
    mstring_t*       pathA = __Path("/a");
    mstring_t*       pathB = __Path("/b");
    mstring_t*       pathF = __Path("/f");
    mstring_t*       pathG = __Path("/g");
    mstring_t*       pathMissing = __Path("/missing");
    mstring_t*       pathRoot = __Path("/");
    mstring_t*       name = mstr_new_u8("n");
    mstring_t*       otherName = mstr_new_u8("c");
    mstring_t*       linkOut = NULL;
    uint8_t          data[64];
    size_t           count;
    uint64_t         position;

    assert(name != NULL && otherName != NULL);
    __Setup();

    // FsOpen: found, missing, and a failed disk read.
    EXPECT_LOCKED_ONCE(FsOpen(&g_mfs, pathF, (void**)&f), status == OS_EOK);
    EXPECT_LOCKED_ONCE(FsOpen(&g_mfs, pathMissing, (void**)&entry), status == OS_ENOENT);
    g_failReadAtSector = MFS_GETSECTOR(&g_mfs, g_mfs.RootEntry.StartBucket);
    EXPECT_LOCKED_ONCE(FsOpen(&g_mfs, pathMissing, (void**)&entry), status == OS_ENOENT);
    assert(g_failReadAtSector == UINT64_MAX);

    // FsStat, FsLink and FsReadLink have one path each.
    EXPECT_LOCKED_ONCE(FsStat(&g_mfs, &stat), status == OS_EOK);
    EXPECT_LOCKED_ONCE(FsLink(&g_mfs, &g_mfs.RootEntry, name, pathA, 1), status == OS_ENOTSUPPORTED);
    EXPECT_LOCKED_ONCE(FsReadLink(&g_mfs, pathA, &linkOut), status == OS_ENOTSUPPORTED);

    // FsRead: a file, a directory, and a failed disk read.
    EXPECT_LOCKED_ONCE(FsRead(&g_mfs, f, TEST_BUFFER_ID, data, 0, sizeof(data), &count),
                       status == OS_EOK && count == sizeof(data) && data[0] == 0x5A);
    root = __Open("/");
    EXPECT_LOCKED_ONCE(FsRead(&g_mfs, root, TEST_BUFFER_ID, data, 0, sizeof(data), &count), status == OS_EOK);
    g_failReadAtSector = MFS_GETSECTOR(&g_mfs, f->StartBucket);
    assert(FsSeek(&g_mfs, f, 0, &position) == OS_EOK);
    EXPECT_LOCKED_ONCE(FsRead(&g_mfs, f, TEST_BUFFER_ID, data, 0, sizeof(data), &count), status == OS_ENOENT);

    // FsSeek: a file, and a directory, which refuses a seek while its size is 0.
    EXPECT_LOCKED_ONCE(FsSeek(&g_mfs, f, 3, &position), status == OS_EOK && position == 3);
    EXPECT_LOCKED_ONCE(FsSeek(&g_mfs, root, 1, &position), status == OS_EINVALPARAMS);

    // FsWrite: a directory, a file, and a failed disk write.
    EXPECT_LOCKED_ONCE(FsWrite(&g_mfs, root, TEST_BUFFER_ID, data, 0, sizeof(data), &count),
                       status == OS_EINVALPARAMS);
    EXPECT_LOCKED_ONCE(FsWrite(&g_mfs, f, TEST_BUFFER_ID, data, 0, sizeof(data), &count), status == OS_EOK);
    g_failOnlyWrite = 1;
    EXPECT_LOCKED_ONCE(FsWrite(&g_mfs, f, TEST_BUFFER_ID, data, 0, sizeof(data), &count), status == OS_ENOENT);

    // FsTruncate: grow, to zero, and grow with the disk gone.
    EXPECT_LOCKED_ONCE(FsTruncate(&g_mfs, f, 3 * TEST_BUCKET_BYTES), status == OS_EOK);
    EXPECT_LOCKED_ONCE(FsTruncate(&g_mfs, f, 0), status == OS_EOK);
    g_writesUntilRemoval = 1;
    EXPECT_LOCKED_ONCE(FsTruncate(&g_mfs, f, TEST_BUCKET_BYTES), status != OS_EOK);
    g_removed = false;
    g_writesUntilRemoval = -1;

    // FsClose: with a record to write, with the write failing, and with
    // nothing to write.
    EXPECT_LOCKED_ONCE(FsClose(&g_mfs, f), status == OS_EOK);
    f = __Open("/f");
    f->ActionOnClose = MFS_ACTION_UPDATE;
    g_failOnlyWrite = 1;
    EXPECT_LOCKED_ONCE(FsClose(&g_mfs, f), status == OS_ENOENT);
    EXPECT_LOCKED_ONCE(FsClose(&g_mfs, root), status == OS_EOK);

    // FsCreate: new, existing, and with the disk gone.
    EXPECT_LOCKED_ONCE(FsCreate(&g_mfs, &g_mfs.RootEntry, name, 0, 0, FILE_PERMISSION_READ, (void**)&entry),
                       status == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    EXPECT_LOCKED_ONCE(FsCreate(&g_mfs, &g_mfs.RootEntry, name, 0, 0, FILE_PERMISSION_READ, (void**)&entry),
                       status == OS_EEXISTS);
    g_failOnlyWrite = 1;
    EXPECT_LOCKED_ONCE(FsCreate(&g_mfs, &g_mfs.RootEntry, otherName, 0, 0, FILE_PERMISSION_READ, (void**)&entry),
                       status == OS_ENOENT);

    // FsMove: a copy and the root are refused before any lookup, a missing
    // source, a failed write, and a rename.
    EXPECT_LOCKED_ONCE(FsMove(&g_mfs, pathA, pathB, 1), status == OS_ENOTSUPPORTED);
    EXPECT_LOCKED_ONCE(FsMove(&g_mfs, pathRoot, pathB, 0), status == OS_EPERMISSIONS);
    EXPECT_LOCKED_ONCE(FsMove(&g_mfs, pathMissing, pathB, 0), status == OS_ENOENT);
    g_failOnlyWrite = 1;
    EXPECT_LOCKED_ONCE(FsMove(&g_mfs, pathA, pathB, 0), status == OS_ENOENT);
    EXPECT_LOCKED_ONCE(FsMove(&g_mfs, pathA, pathB, 0), status == OS_EOK);

    // FsUnlink: missing, a failed write, and removed. The file has buckets,
    // so the unlink also writes the bucket map and the master record.
    // mfs_entries_test unlinks a file without buckets.
    assert(__CreateIn(&g_mfs.RootEntry, "g", 0, &entry) == OS_EOK);
    assert(FsTruncate(&g_mfs, entry, TEST_BUCKET_BYTES) == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    EXPECT_LOCKED_ONCE(FsUnlink(&g_mfs, pathMissing), status == OS_ENOENT);
    g_failOnlyWrite = 1;
    EXPECT_LOCKED_ONCE(FsUnlink(&g_mfs, pathG), status == OS_ENOENT);
    EXPECT_LOCKED_ONCE(FsUnlink(&g_mfs, pathG), status == OS_EOK);
    assert(!__Exists("/g"));

    mstr_delete(pathA);
    mstr_delete(pathB);
    mstr_delete(pathG);
    mstr_delete(pathF);
    mstr_delete(pathMissing);
    mstr_delete(pathRoot);
    mstr_delete(name);
    mstr_delete(otherName);

    // The failed disk transfers above logged errors, as they should.
    assert(g_errors > 0);
    g_errors = 0;
    __Teardown();
}

// FsDestroy frees a mount whose lock is free, and keeps one whose lock is
// held by a running operation.
static void
__TestDestroy(void)
{
    FileSystemMFS_t* mfs = calloc(1, sizeof(FileSystemMFS_t));

    assert(mfs != NULL);
    usched_mtx_init(&mfs->Lock, USCHED_MUTEX_PLAIN);
    mfs->BucketMap = malloc(16);
    assert(mfs->BucketMap != NULL);

    // Another job holds the lock: the mount must not be freed. ASan reports
    // a use after free on the next line if it was.
    mfs->Lock.references = 2;
    g_errors = 0;
    assert(FsDestroy(mfs, 0) == OS_EBUSY);
    assert(mfs->Lock.references == 2 && g_errors == 1);
    g_errors = 0;

    // Free lock: the mount is freed. ASan reports a leak at exit if not.
    mfs->Lock.references = 0;
    assert(FsDestroy(mfs, 0) == OS_EOK);
    assert(FsDestroy(NULL, 0) == OS_EINVALPARAMS);
}

int
main(void)
{
    __TestCreateWithOpen();
    __TestTwoCreatesInRoot();
    __TestTwoCreatesInDirectory();
    __TestRenameWithCreate();
    __TestWriteWithOpen();
    __TestLockOnEveryPath();
    __TestDestroy();
    puts("MFS mount lock: create with open, two creates, rename with create, write with open, "
         "lock on every path and destroy passed");
    return 0;
}
