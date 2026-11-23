// MFS moves files and directories with FsMove, on the in-memory partition of
// mfs_create_test.c, whose doubles and image are reused here.
//
// filed calls FsMove with two paths inside the filesystem, both starting at
// its root, for example "/d/x" and "/e/x" (services/filed/vfs/handlers/move.c,
// SourcePath and TargetPath). After a move the target name must lead to the
// data of the source and the source name must be gone. filed only sends
// files. A copy is refused with OS_ENOTSUPPORTED, because MFS cannot copy
// bucket data yet (MFSCloneBucketData).
//
// The old FsMove looked up the directory of the source and created the new
// record with the source's name, so every move failed with OS_EEXISTS or put
// the file in the wrong place. It also read OS_ENOENT from the target lookup
// as "target missing", although a storage driver that lost its disk fails
// reads with OS_ENOENT as well.
//
// The disk cases remove the disk at each write of a move, or fail one write
// only. Afterwards the file must be reachable under at least one of the two
// names with its data, and under exactly one, except when the disk is gone
// for both the removal of the source record and the undo of the new record.
#define main __MfsCreateTestMain
#include "mfs_create_test.c"
#undef main

extern oserr_t FsMove(void*, mstring_t*, mstring_t*, int);
extern oserr_t MfsEnsureRecordSpace(FileSystemMFS_t*, MFSEntry_t*, uint64_t);

#define TEST_FILE_BUCKETS 2
#define TEST_BUCKET_BYTES (TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE)

/**
 * A file the test created, with the data buckets it got and the byte its
 * data is filled with.
 */
struct __TestFile {
    uint32_t StartBucket;
    uint32_t StartLength;
    uint8_t  Pattern;
};

// The tree every case starts from:
//   /a        file, pattern 0x11
//   /d/x      file, pattern 0x22
//   /d/sub/   directory
//   /e/y      file, pattern 0x33
//   /empty/   directory without buckets, so a move into it expands it
static struct __TestFile g_fileA;
static struct __TestFile g_fileX;
static struct __TestFile g_fileY;

static MFSEntry_t*
__CreateEntry(
    _In_ MFSEntry_t* parent,
    _In_ const char* name,
    _In_ uint32_t    flags)
{
    mstring_t* mname = mstr_new_u8(name);
    void*      data = NULL;
    oserr_t    oserr;

    assert(mname != NULL);
    oserr = FsCreate(&g_mfs, parent, mname, 0, flags, FILE_PERMISSION_READ, &data);
    mstr_delete(mname);
    assert(oserr == OS_EOK && data != NULL);
    return data;
}

// Creates a file with TEST_FILE_BUCKETS buckets of data filled with pattern.
static struct __TestFile
__CreateFile(
    _In_ MFSEntry_t* parent,
    _In_ const char* name,
    _In_ uint8_t     pattern)
{
    MFSEntry_t*       entry = __CreateEntry(parent, name, 0);
    struct __TestFile file;

    assert(MfsEnsureRecordSpace(&g_mfs, entry, TEST_FILE_BUCKETS * TEST_BUCKET_BYTES) == OS_EOK);
    entry->ActualSize = TEST_FILE_BUCKETS * TEST_BUCKET_BYTES;
    file.StartBucket = entry->StartBucket;
    file.StartLength = entry->StartLength;
    file.Pattern = pattern;
    memset(__Bucket(file.StartBucket), pattern, (size_t)file.StartLength * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    return file;
}

static void
__Setup(void)
{
    MFSEntry_t* d;
    MFSEntry_t* e;

    __Format();
    g_fileA = __CreateFile(&g_mfs.RootEntry, "a", 0x11);
    d = __CreateEntry(&g_mfs.RootEntry, "d", FILE_FLAG_DIRECTORY);
    g_fileX = __CreateFile(d, "x", 0x22);
    assert(FsClose(&g_mfs, __CreateEntry(d, "sub", FILE_FLAG_DIRECTORY)) == OS_EOK);
    e = __CreateEntry(&g_mfs.RootEntry, "e", FILE_FLAG_DIRECTORY);
    g_fileY = __CreateFile(e, "y", 0x33);
    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "empty", FILE_FLAG_DIRECTORY)) == OS_EOK);
    assert(FsClose(&g_mfs, d) == OS_EOK);
    assert(FsClose(&g_mfs, e) == OS_EOK);
    g_errors = 0;
    g_writes = 0;
}

// Looks a path up on the disk. Returns NULL when it is missing.
static MFSEntry_t*
__Find(
    _In_ const char* path)
{
    enum MFSSearchResult result;
    MFSEntry_t*          entry = NULL;
    mstring_t*           mpath = mstr_new_u8(path);

    assert(mpath != NULL);
    assert(MfsFindRecord(&g_mfs, &g_mfs.RootEntry, mpath, &entry, &result) == OS_EOK);
    mstr_delete(mpath);
    return result == MFS_SEARCH_FOUND ? entry : NULL;
}

static void
__Free(
    _In_ MFSEntry_t* entry)
{
    if (entry != NULL) {
        mstr_delete(entry->Name);
        free(entry);
    }
}

static bool
__Exists(
    _In_ const char* path)
{
    MFSEntry_t* entry = __Find(path);

    __Free(entry);
    return entry != NULL;
}

// No bucket of the file is on the free list that the disk describes.
static bool
__BucketsInUse(
    _In_ const struct __TestFile* file)
{
    MasterRecord_t* master = (MasterRecord_t*)&g_image[TEST_MASTER_SECTOR * TEST_SECTOR_SIZE];
    uint32_t*       map = (uint32_t*)&g_image[TEST_MAP_SECTOR * TEST_SECTOR_SIZE];
    uint32_t        bucket;
    int             steps = 0;

    for (bucket = master->FreeBucket; bucket != MFS_ENDOFCHAIN; bucket = map[bucket * 2]) {
        uint32_t length = map[(bucket * 2) + 1];

        assert(steps++ < TEST_BUCKETS);
        if (bucket < file->StartBucket + file->StartLength && bucket + length > file->StartBucket) {
            return false;
        }
    }
    return true;
}

// path is a record of the file: same buckets, same size, data unchanged,
// and the buckets are not free.
static void
__ExpectFile(
    _In_ const char*              path,
    _In_ const struct __TestFile* file)
{
    MFSEntry_t* entry = __Find(path);
    uint8_t*    data;

    assert(entry != NULL);
    assert(!(entry->NativeFlags & MFS_FILERECORD_DIRECTORY));
    assert(entry->StartBucket == file->StartBucket && entry->StartLength == file->StartLength);
    assert(entry->ActualSize == TEST_FILE_BUCKETS * TEST_BUCKET_BYTES);
    data = __Bucket(file->StartBucket);
    for (size_t i = 0; i < (size_t)file->StartLength * TEST_BUCKET_BYTES; i++) {
        assert(data[i] == file->Pattern);
    }
    assert(__BucketsInUse(file));
    __Free(entry);
}

static oserr_t
__Move(
    _In_ const char* from,
    _In_ const char* to,
    _In_ int         copy)
{
    mstring_t* mfrom = mstr_path_new_u8(from);
    mstring_t* mto = mstr_path_new_u8(to);
    oserr_t    oserr;

    assert(mfrom != NULL && mto != NULL);
    oserr = FsMove(&g_mfs, mfrom, mto, copy);
    mstr_delete(mfrom);
    mstr_delete(mto);
    return oserr;
}

// A move that is refused changes nothing on the disk.
static void
__ExpectRefused(
    _In_ const char* from,
    _In_ const char* to,
    _In_ int         copy,
    _In_ oserr_t     expected)
{
    g_writes = 0;
    assert(__Move(from, to, copy) == expected);
    assert(g_writes == 0);
}

static void
__TestRename(void)
{
    MFSEntry_t* before;
    MFSEntry_t* after;

    // Within one directory the record is renamed in place, with one write.
    __Setup();
    before = __Find("/a");
    assert(__Move("/a", "/b", 0) == OS_EOK && g_writes == 1);
    assert(!__Exists("/a"));
    __ExpectFile("/b", &g_fileA);
    after = __Find("/b");
    assert(after->DirectoryBucket == before->DirectoryBucket && after->DirectoryIndex == before->DirectoryIndex);
    __Free(before);
    __Free(after);

    g_writes = 0;
    assert(__Move("/d/x", "/d/z", 0) == OS_EOK && g_writes == 1);
    assert(!__Exists("/d/x"));
    __ExpectFile("/d/z", &g_fileX);

    // A shorter name leaves nothing of the longer one behind.
    assert(__Move("/e/y", "/e/longer-name", 0) == OS_EOK);
    assert(__Move("/e/longer-name", "/e/s", 0) == OS_EOK);
    assert(!__Exists("/e/longer-name"));
    __ExpectFile("/e/s", &g_fileY);
    assert(g_errors == 0);
    __Unmount();

    // The disk is gone at the only write: the file keeps its old name.
    __Setup();
    g_writesUntilRemoval = 1;
    assert(__Move("/a", "/b", 0) == OS_ENOENT);
    g_removed = false;
    __ExpectFile("/a", &g_fileA);
    assert(!__Exists("/b"));
    __Unmount();
}

static void
__TestBetweenDirectories(void)
{
    __Setup();
    assert(__Move("/d/x", "/e/x", 0) == OS_EOK);
    assert(!__Exists("/d/x"));
    __ExpectFile("/e/x", &g_fileX);

    // To and from the root.
    assert(__Move("/e/y", "/y", 0) == OS_EOK);
    assert(!__Exists("/e/y"));
    __ExpectFile("/y", &g_fileY);
    assert(__Move("/a", "/d/sub/a", 0) == OS_EOK);
    assert(!__Exists("/a"));
    __ExpectFile("/d/sub/a", &g_fileA);

    // Into a directory without buckets, which gets its first buckets.
    assert(__Move("/y", "/empty/y", 0) == OS_EOK);
    assert(!__Exists("/y"));
    __ExpectFile("/empty/y", &g_fileY);
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestRefusals(void)
{
    __Setup();
    // The target exists, also when it differs only in case: names compare
    // without case, so a case-only rename is refused as well.
    __ExpectRefused("/a", "/d", 0, OS_EEXISTS);
    __ExpectRefused("/d/x", "/e/y", 0, OS_EEXISTS);
    __ExpectRefused("/a", "/A", 0, OS_EEXISTS);

    // The source or the target directory is missing, or the target directory
    // is a file.
    __ExpectRefused("/missing", "/q", 0, OS_ENOENT);
    __ExpectRefused("/d/missing", "/e/q", 0, OS_ENOENT);
    __ExpectRefused("/a", "/nodir/a", 0, OS_ENOENT);
    __ExpectRefused("/a", "/empty/sub/a", 0, OS_ENOENT);
    __ExpectRefused("/d/x", "/a/x", 0, OS_ENOTDIR);

    // The root is no record, and nothing replaces it.
    __ExpectRefused("/", "/r", 0, OS_EPERMISSIONS);
    __ExpectRefused("/a", "/", 0, OS_EEXISTS);

    // A copy is not supported, and nothing is created for it.
    __ExpectRefused("/a", "/c", 1, OS_ENOTSUPPORTED);
    __ExpectRefused("/missing", "/c", 1, OS_ENOTSUPPORTED);
    assert(!__Exists("/c"));

    __ExpectFile("/a", &g_fileA);
    __ExpectFile("/d/x", &g_fileX);
    __ExpectFile("/e/y", &g_fileY);
    __Unmount();
}

static void
__TestDirectory(void)
{
    MFSEntry_t* moved;

    // A directory moves with everything in it, as its record points at the
    // same buckets.
    __Setup();
    assert(__Move("/d", "/e/d", 0) == OS_EOK);
    assert(!__Exists("/d") && !__Exists("/d/x"));
    __ExpectFile("/e/d/x", &g_fileX);
    moved = __Find("/e/d/sub");
    assert(moved != NULL && (moved->NativeFlags & MFS_FILERECORD_DIRECTORY));
    __Free(moved);

    // Not into itself, and not onto itself.
    __ExpectRefused("/e", "/e/d/e", 0, OS_EINVALPARAMS);
    __ExpectRefused("/e", "/e/d/sub/e", 0, OS_EINVALPARAMS);
    __ExpectRefused("/e", "/e", 0, OS_EEXISTS);
    __ExpectFile("/e/d/x", &g_fileX);
    __Unmount();
}

// A read of the target directory fails once. That is a disk error, not a
// missing target, so the move fails with it and nothing is written.
static void
__TestLookupError(void)
{
    MFSEntry_t* e;

    __Setup();
    e = __Find("/e");
    g_failReadAtSector = MFS_GETSECTOR(&g_mfs, e->StartBucket);
    __Free(e);
    __ExpectRefused("/d/x", "/e/w", 0, OS_ENOENT);
    assert(g_failReadAtSector == UINT64_MAX && g_errors > 0);
    __ExpectFile("/d/x", &g_fileX);
    assert(!__Exists("/e/w"));
    __Unmount();
}

// Moves /a into /empty with the disk removed at write failAt, or with only
// that write failing (removeDisk false). A negative failAt fails nothing.
// Returns the number of writes of the move.
static int
__MoveWithFailure(
    _In_ int  failAt,
    _In_ bool removeDisk)
{
    MFSEntry_t* directory;
    oserr_t     oserr;
    int         names;
    int         writes;

    __Setup();
    if (removeDisk) {
        g_writesUntilRemoval = failAt;
    } else {
        g_failOnlyWrite = failAt;
    }
    oserr = __Move("/a", "/empty/a", 0);
    writes = g_writes;
    g_removed = false;
    g_writesUntilRemoval = -1;

    names = (int)__Exists("/a") + (int)__Exists("/empty/a");
    assert(names >= 1);
    if (__Exists("/a")) {
        __ExpectFile("/a", &g_fileA);
    }
    if (__Exists("/empty/a")) {
        __ExpectFile("/empty/a", &g_fileA);
    }

    if (failAt < 0) {
        assert(oserr == OS_EOK && names == 1 && __Exists("/empty/a"));
    } else {
        assert(oserr == OS_ENOENT && g_errors > 0);
    }

    // With only one failed write the undo works, so exactly one name is
    // left. A removed disk also fails the undo when it is gone at the removal
    // of the source record, the last write, and then both names remain.
    if (failAt > 0 && !removeDisk) {
        assert(names == 1 && __Exists("/a"));
    }

    directory = __Find("/empty");
    assert(directory != NULL);
    __CheckConsistency(directory);
    __Free(directory);
    __Unmount();
    return writes;
}

static void
__TestDiskFailures(void)
{
    int writes;
    int i;

    writes = __MoveWithFailure(-1, false);
    assert(writes > 2);
    for (i = 1; i <= writes; i++) {
        (void)__MoveWithFailure(i, true);
        (void)__MoveWithFailure(i, false);
    }
    printf("MFS move into a directory without buckets: %i writes, disk removed or one write failed at each\n",
           writes);
}

// The last removal case again, to show its outcome: both names share the data.
static void
__TestBothNames(void)
{
    int writes = __MoveWithFailure(-1, false);

    __Setup();
    g_writesUntilRemoval = writes;
    assert(__Move("/a", "/empty/a", 0) == OS_ENOENT);
    g_removed = false;
    __ExpectFile("/a", &g_fileA);
    __ExpectFile("/empty/a", &g_fileA);
    __Unmount();

    // Removed one write earlier, at the creation of the new record: only the
    // old name is left.
    __Setup();
    g_writesUntilRemoval = writes - 1;
    assert(__Move("/a", "/empty/a", 0) == OS_ENOENT);
    g_removed = false;
    __ExpectFile("/a", &g_fileA);
    assert(!__Exists("/empty/a"));
    __Unmount();
}

int
main(void)
{
    __TestRename();
    __TestBetweenDirectories();
    __TestRefusals();
    __TestDirectory();
    __TestLookupError();
    __TestDiskFailures();
    __TestBothNames();
    puts("MFS move: rename, between directories, root, refusals, directories, lookup errors and disk failures passed");
    return 0;
}
