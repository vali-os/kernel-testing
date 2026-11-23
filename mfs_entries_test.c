// MFS directory expansion, unlink, seek in an empty file and the memory of a
// mount, on the in-memory partition of mfs_create_test.c.
//
// Production code compiled here: modules/filesystems/mfs/main.c, records.c,
// bucket_map.c, utilities.c, file_operations.c and directory_operations.c,
// and the libds mstring code. The storage double, the image and its format
// come from mfs_create_test.c. This file adds the doubles that the real
// FsInitialize needs: SHMCreate and FSStorageStat.
//
// The cases:
// - Expansion. A directory that runs out of free records gets
//   MFS_DIRECTORYEXPANSION (4) new buckets, linked from its last bucket. That
//   bucket must keep its own length in the bucket map. The root directory
//   has MFS_ROOTSIZE (8) buckets, and when the link also set its length to 4,
//   the search read only the first half of the root, so records 16 to 31 were
//   no longer found. The root and a subdirectory are filled past two
//   expansions. In a subdirectory every bucket has length 4, so there only
//   the names and the chain are checked.
// - Unlink. A file that was never written has no buckets. MfsFreeBuckets
//   refuses a length of 0, so the unlink failed and the record stayed. A file
//   with data must give its buckets back to the free list.
// - Seek. A seek in a file without buckets walked the bucket map from bucket
//   MFS_ENDOFCHAIN and failed with OS_EDEVFAULT. A seek to 0 must work, and so
//   must a write and a read after it. A seek past 0 is accepted as well. The
//   write after it must land at that position, also when the first run of
//   buckets it gets is shorter than the position.
// - Mount memory. FsDestroy and a failed FsInitialize must free the label and
//   the name of the root. LeakSanitizer reports a leak at exit otherwise.
#define MFS_TEST_OWN_DOUBLES
#define MFS_TEST_OWN_MOUNT_DOUBLES
#define main __MfsCreateTestMain
#include "mfs_create_test.c"
#undef main

extern oserr_t FsInitialize(struct VFSStorageParameters*, void**);
extern oserr_t FsDestroy(void*, unsigned int);
extern oserr_t FsOpen(void*, mstring_t*, void**);
extern oserr_t FsStat(void*, struct VFSStatFS*);
extern oserr_t FsUnlink(void*, mstring_t*);
extern oserr_t FsRead(void*, void*, uuid_t, void*, size_t, size_t, size_t*);
extern oserr_t FsWrite(void*, void*, uuid_t, void*, size_t, size_t, size_t*);
extern oserr_t FsSeek(void*, void*, uint64_t, uint64_t*);
extern oserr_t FsTruncate(void*, void*, uint64_t);

#define TEST_BUCKET_BYTES      (TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE)
#define TEST_RECORDS_PER_BUCKET ((TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE) / sizeof(FileRecord_t))

void OSHandleDestroy(OSHandle_t* handle) { }

// The mount lock. Only one operation runs at a time, so the lock must be free
// when it is taken, and held when it is released.
void usched_mtx_init(struct usched_mtx* mutex, int type)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->type = type;
}

void usched_mtx_lock(struct usched_mtx* mutex)
{
    assert(mutex->references == 0);
    mutex->references = 1;
}

int usched_mtx_trylock(struct usched_mtx* mutex)
{
    if (mutex->references != 0) {
        return -1;
    }
    mutex->references = 1;
    return 0;
}

void usched_mtx_unlock(struct usched_mtx* mutex)
{
    assert(mutex->references == 1);
    mutex->references = 0;
}

// FsInitialize creates the transfer buffer twice: one sector first, then a
// whole root directory. Both are the one test buffer. g_shmCreatesUntilFailure
// counts down with each call, and the call that brings it to zero fails. A
// negative value never fails.
static int g_shmCreatesUntilFailure = -1;

oserr_t
SHMCreate(
    _In_  SHM_t*      shm,
    _Out_ OSHandle_t* handleOut)
{
    if (g_shmCreatesUntilFailure > 0 && --g_shmCreatesUntilFailure == 0) {
        return OS_EOOM;
    }
    assert(shm->Size <= sizeof(g_transferBuffer));
    memset(handleOut, 0, sizeof(*handleOut));
    handleOut->ID = TEST_BUFFER_ID;
    return OS_EOK;
}

oserr_t
FSStorageStat(
    _InOut_ struct VFSStorageParameters* storageParameters,
    _Out_   StorageDescriptor_t*         stat)
{
    memset(stat, 0, sizeof(*stat));
    stat->SectorSize = TEST_SECTOR_SIZE;
    stat->SectorCount = TEST_IMAGE_SECTORS;
    return OS_EOK;
}

static uint32_t*
__MapOnDisk(void)
{
    return (uint32_t*)&g_image[TEST_MAP_SECTOR * TEST_SECTOR_SIZE];
}

static MasterRecord_t*
__MasterOnDisk(void)
{
    return (MasterRecord_t*)&g_image[TEST_MASTER_SECTOR * TEST_SECTOR_SIZE];
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
__Free(
    _In_ MFSEntry_t* entry)
{
    if (entry != NULL) {
        mstr_delete(entry->Name);
        free(entry);
    }
}

// Looks a path up on the disk. Returns NULL when it is missing.
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

    __Free(entry);
    return entry != NULL;
}

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
    if (oserr != OS_EOK) {
        printf("FsCreate of %s returned %u\n", name, oserr);
        fflush(stdout);
        abort();
    }
    return data;
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

static oserr_t
__Unlink(
    _In_ const char* path)
{
    mstring_t* mpath = __Path(path);
    oserr_t    oserr;

    oserr = FsUnlink(&g_mfs, mpath);
    mstr_delete(mpath);
    return oserr;
}

// True when bucket lies in a run of the free list that the disk describes.
static bool
__BucketIsFree(
    _In_ uint32_t bucket)
{
    uint32_t* map = __MapOnDisk();
    uint32_t  current;
    int       steps = 0;

    for (current = __MasterOnDisk()->FreeBucket; current != MFS_ENDOFCHAIN; current = map[current * 2]) {
        assert(steps++ < TEST_BUCKETS && current < TEST_BUCKETS);
        if (bucket >= current && bucket < current + map[(current * 2) + 1]) {
            return true;
        }
    }
    return false;
}

// The bucket map and the free list head in memory match the disk.
static void
__ExpectMapOnDisk(void)
{
    assert(!memcmp(__MapOnDisk(), g_mfs.BucketMap, TEST_BUCKETS * sizeof(MapRecord_t)));
    assert(__MasterOnDisk()->FreeBucket == g_mfs.MasterRecord.FreeBucket);
}

// Creates count files named <prefix>NN in parent, and checks that every name
// created so far is still found, after each create.
static void
__FillDirectory(
    _In_ MFSEntry_t* parent,
    _In_ const char* directoryPath,
    _In_ int         count)
{
    char name[32];
    char path[64];

    for (int i = 0; i < count; i++) {
        snprintf(name, sizeof(name), "f%02d", i);
        assert(FsClose(&g_mfs, __CreateEntry(parent, name, 0)) == OS_EOK);
        for (int j = 0; j <= i; j++) {
            snprintf(path, sizeof(path), "%s/f%02d", directoryPath, j);
            if (!__Exists(path)) {
                printf("%s is missing after creating f%02d\n", path, i);
                fflush(stdout);
                abort();
            }
        }
    }
}

// Checks the bucket chain of a directory on disk: the first bucket keeps
// firstLength, and it is followed by expansions buckets of length
// MFS_DIRECTORYEXPANSION each.
static void
__ExpectDirectoryChain(
    _In_ uint32_t firstBucket,
    _In_ uint32_t firstLength,
    _In_ int      expansions)
{
    uint32_t* map = __MapOnDisk();
    uint32_t  bucket = firstBucket;

    if (map[(bucket * 2) + 1] != firstLength) {
        printf("bucket %u of the directory has length %u on disk, expected %u\n",
               bucket, map[(bucket * 2) + 1], firstLength);
        fflush(stdout);
        abort();
    }
    for (int i = 0; i < expansions; i++) {
        bucket = map[bucket * 2];
        assert(bucket != MFS_ENDOFCHAIN && bucket < TEST_BUCKETS);
        assert(map[(bucket * 2) + 1] == MFS_DIRECTORYEXPANSION);
        assert(!__BucketIsFree(bucket));
    }
    assert(map[bucket * 2] == MFS_ENDOFCHAIN);
}

static void
__TestExpansion(void)
{
    // Records that fit in the root, and in one expansion.
    int         rootRecords = (int)(MFS_ROOTSIZE * TEST_RECORDS_PER_BUCKET);
    int         expansionRecords = (int)(MFS_DIRECTORYEXPANSION * TEST_RECORDS_PER_BUCKET);
    MFSEntry_t* sub;
    MFSEntry_t* found;

    // The root: full, then two expansions and a few records in the second.
    __Format();
    g_errors = 0;
    __FillDirectory(&g_mfs.RootEntry, "", rootRecords + (2 * expansionRecords) - 3);
    __ExpectDirectoryChain(g_mfs.RootEntry.StartBucket, MFS_ROOTSIZE, 2);
    __ExpectMapOnDisk();
    assert(g_errors == 0);
    __Unmount();

    // A subdirectory: its first buckets, then two expansions.
    __Format();
    g_errors = 0;
    sub = __CreateEntry(&g_mfs.RootEntry, "sub", FILE_FLAG_DIRECTORY);
    __FillDirectory(sub, "/sub", (3 * expansionRecords) - 2);
    assert(FsClose(&g_mfs, sub) == OS_EOK);
    found = __Find("/sub");
    assert(found != NULL && found->StartLength == MFS_DIRECTORYEXPANSION);
    __ExpectDirectoryChain(found->StartBucket, MFS_DIRECTORYEXPANSION, 2);
    __Free(found);
    __ExpectMapOnDisk();
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestUnlink(void)
{
    MFSEntry_t* entry;
    uint32_t    startBucket;
    uint32_t    startLength;
    uint32_t    directoryBucket;
    size_t      directoryIndex;
    uint32_t    freeBefore;

    // A file that was never written: the record goes, and nothing is freed.
    __Format();
    g_errors = 0;
    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "empty", 0)) == OS_EOK);
    entry = __Find("/empty");
    assert(entry != NULL && entry->StartBucket == MFS_ENDOFCHAIN);
    directoryBucket = entry->DirectoryBucket;
    directoryIndex = entry->DirectoryIndex;
    __Free(entry);
    freeBefore = g_mfs.MasterRecord.FreeBucket;

    assert(__Unlink("/empty") == OS_EOK);
    assert(!__Exists("/empty"));
    assert(!(((FileRecord_t*)(__Bucket(directoryBucket)) + directoryIndex)->Flags & MFS_FILERECORD_INUSE));
    assert(g_mfs.MasterRecord.FreeBucket == freeBefore);
    __ExpectMapOnDisk();

    // The name can be used again, and gets the same record.
    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "empty", 0)) == OS_EOK);
    entry = __Find("/empty");
    assert(entry != NULL && entry->DirectoryBucket == directoryBucket && entry->DirectoryIndex == directoryIndex);
    __Free(entry);
    assert(__Unlink("/empty") == OS_EOK);
    assert(!__Exists("/empty"));

    // A file with three buckets of data: they go back on the free list.
    entry = __CreateEntry(&g_mfs.RootEntry, "data", 0);
    assert(FsTruncate(&g_mfs, entry, 3 * TEST_BUCKET_BYTES) == OS_EOK);
    startBucket = entry->StartBucket;
    startLength = entry->StartLength;
    assert(startBucket != MFS_ENDOFCHAIN && startLength == 3);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    for (uint32_t i = 0; i < startLength; i++) {
        assert(!__BucketIsFree(startBucket + i));
    }

    assert(__Unlink("/data") == OS_EOK);
    assert(!__Exists("/data"));
    for (uint32_t i = 0; i < startLength; i++) {
        assert(__BucketIsFree(startBucket + i));
    }
    assert(__MasterOnDisk()->FreeBucket == startBucket);
    __ExpectMapOnDisk();
    assert(__Unlink("/data") == OS_ENOENT);
    assert(g_errors == 0);
    __Unmount();
}

static void
__ExpectRead(
    _In_ MFSEntry_t*    entry,
    _In_ uint64_t       position,
    _In_ const uint8_t* expected,
    _In_ size_t         length)
{
    uint8_t  data[TEST_SECTOR_SIZE];
    uint64_t positionOut;
    size_t   count;

    assert(length <= sizeof(data));
    assert(FsSeek(&g_mfs, entry, position, &positionOut) == OS_EOK && positionOut == position);
    memset(data, 0, sizeof(data));
    assert(FsRead(&g_mfs, entry, TEST_BUFFER_ID, data, 0, length, &count) == OS_EOK);
    assert(count == length);
    assert(!memcmp(data, expected, length));
}

static void
__TestSeek(void)
{
    // Past the first bucket and not on a sector boundary. Reads and writes
    // stay below one sector, so they go through the transfer buffer.
    const uint64_t farPosition = TEST_BUCKET_BYTES + (TEST_BUCKET_BYTES / 2) + 10;
    MFSEntry_t*    entry;
    MFSEntry_t*    other;
    uint8_t        data[100];
    uint8_t        otherData[TEST_SECTOR_SIZE];
    uint8_t        readBack[sizeof(data)];
    uint64_t       position;
    size_t         count;
    uint32_t       otherBucket;

    // Seek to 0 in an empty file, read nothing, write, read it back.
    __Format();
    g_errors = 0;
    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "s", 0)) == OS_EOK);
    entry = __Open("/s");
    assert(entry->StartBucket == MFS_ENDOFCHAIN);
    assert(FsSeek(&g_mfs, entry, 0, &position) == OS_EOK && position == 0);
    assert(FsRead(&g_mfs, entry, TEST_BUFFER_ID, readBack, 0, sizeof(readBack), &count) == OS_EOK);
    assert(count == 0);
    memset(data, 0x3C, sizeof(data));
    assert(FsWrite(&g_mfs, entry, TEST_BUFFER_ID, data, 0, sizeof(data), &count) == OS_EOK);
    assert(count == sizeof(data) && entry->StartBucket != MFS_ENDOFCHAIN);
    __ExpectRead(entry, 0, data, sizeof(data));
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/s");
    assert(entry->ActualSize == sizeof(data));
    __ExpectRead(entry, 0, data, sizeof(data));
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // The handle FsCreate returns, seeked to 0 before its first write.
    entry = __CreateEntry(&g_mfs.RootEntry, "c", 0);
    assert(FsSeek(&g_mfs, entry, 0, &position) == OS_EOK && position == 0);
    memset(data, 0x4D, sizeof(data));
    assert(FsWrite(&g_mfs, entry, TEST_BUFFER_ID, data, 0, sizeof(data), &count) == OS_EOK);
    assert(count == sizeof(data));
    __ExpectRead(entry, 0, data, sizeof(data));
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();

    // Seek past 0 in an empty file. The free list is first split, so that the
    // file gets a first run of one bucket and the position lies in its second
    // run: "hole" takes one bucket, "other" the next, and the unlink of
    // "hole" puts its bucket first on the free list.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "hole", 0);
    assert(FsTruncate(&g_mfs, entry, TEST_BUCKET_BYTES) == OS_EOK);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    other = __CreateEntry(&g_mfs.RootEntry, "other", 0);
    assert(FsTruncate(&g_mfs, other, TEST_SECTOR_SIZE) == OS_EOK);
    otherBucket = other->StartBucket;
    memset(otherData, 0x77, sizeof(otherData));
    assert(FsSeek(&g_mfs, other, 0, &position) == OS_EOK);
    assert(FsWrite(&g_mfs, other, TEST_BUFFER_ID, otherData, 0, sizeof(otherData) - 1, &count) == OS_EOK);
    assert(FsClose(&g_mfs, other) == OS_EOK);
    assert(__Unlink("/hole") == OS_EOK);
    assert(g_mfs.BucketMap[(g_mfs.MasterRecord.FreeBucket * 2) + 1] == 1);

    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "far", 0)) == OS_EOK);
    entry = __Open("/far");
    assert(FsSeek(&g_mfs, entry, farPosition, &position) == OS_EOK && position == farPosition);
    assert(FsRead(&g_mfs, entry, TEST_BUFFER_ID, readBack, 0, sizeof(readBack), &count) == OS_EOK);
    assert(count == 0);
    memset(data, 0xE1, sizeof(data));
    assert(FsWrite(&g_mfs, entry, TEST_BUFFER_ID, data, 0, sizeof(data), &count) == OS_EOK);
    assert(count == sizeof(data));
    assert(entry->StartLength == 1 && entry->ActualSize == farPosition + sizeof(data));
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // The data is at the position, and the bucket of the other file was not
    // written to.
    for (size_t i = 0; i < TEST_BUCKET_BYTES; i++) {
        assert(__Bucket(otherBucket)[i] != 0xE1);
    }
    entry = __Open("/far");
    assert(entry->ActualSize == farPosition + sizeof(data));
    __ExpectRead(entry, farPosition, data, sizeof(data));
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    other = __Open("/other");
    assert(other->StartBucket == otherBucket);
    __ExpectRead(other, 0, otherData, sizeof(otherData) - 1);
    assert(FsClose(&g_mfs, other) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();
}

// Formats the image and adds what FsInitialize reads on top of what
// __Format writes: the boot record and a partition name.
static void
__FormatForMount(void)
{
    BootRecord_t* boot = (BootRecord_t*)&g_image[0];

    __Format();
    __Unmount();
    boot->Magic = MFS_BOOTRECORD_MAGIC;
    boot->Version = 1;
    boot->SectorSize = TEST_SECTOR_SIZE;
    boot->SectorCount = TEST_IMAGE_SECTORS;
    boot->ReservedSectors = TEST_RESERVED_SECTORS;
    boot->SectorsPerBucket = TEST_SECTORS_PER_BUCKET;
    boot->MasterRecordSector = TEST_MASTER_SECTOR;
    boot->MasterRecordMirror = TEST_MIRROR_SECTOR;
    strcpy((char*)&__MasterOnDisk()->PartitionName[0], "vali-data");
    memcpy(&g_image[TEST_MIRROR_SECTOR * TEST_SECTOR_SIZE], __MasterOnDisk(), sizeof(MasterRecord_t));
    g_shmCreatesUntilFailure = -1;
}

// FsInitialize with the given failure armed. It must fail, log it and leave
// instanceData as it was. Whatever it had allocated must be freed again.
static void
__ExpectFailedMount(
    _In_ const char* step)
{
    struct VFSStorageParameters parameters = { 0 };
    void*                       instance = (void*)&parameters;
    oserr_t                     oserr;

    g_errors = 0;
    oserr = FsInitialize(&parameters, &instance);
    if (oserr == OS_EOK) {
        printf("FsInitialize did not fail at %s\n", step);
        fflush(stdout);
        abort();
    }
    assert(instance == (void*)&parameters);
    assert(g_errors > 0);
}

static void
__TestMountMemory(void)
{
    struct VFSStorageParameters parameters = { 0 };
    FileSystemMFS_t*            mfs = NULL;
    struct VFSStatFS            stat;
    mstring_t*                  path;
    void*                       data = NULL;

    // A mount that is set up, used and destroyed.
    __FormatForMount();
    g_errors = 0;
    assert(FsInitialize(&parameters, (void**)&mfs) == OS_EOK && mfs != NULL);
    assert(mfs->Label != NULL && mfs->RootEntry.Name != NULL);
    assert(FsStat(mfs, &stat) == OS_EOK && mstr_len(stat.Label) == strlen("vali-data"));
    path = __Path("/");
    assert(FsOpen(mfs, path, &data) == OS_EOK);
    assert(FsClose(mfs, data) == OS_EOK);
    mstr_delete(path);
    assert(FsDestroy(mfs, 0) == OS_EOK);
    assert(g_errors == 0);

    // Failures at each step of FsInitialize. The partition label is set from
    // the read of the bucket map on, and so is the bucket map.
    __FormatForMount();
    g_failReadAtSector = 0;
    __ExpectFailedMount("the read of the boot record");
    __FormatForMount();
    g_failReadAtSector = TEST_MASTER_SECTOR;
    __ExpectFailedMount("the read of the master record");
    __FormatForMount();
    g_shmCreatesUntilFailure = 2;
    __ExpectFailedMount("the larger transfer buffer");
    __FormatForMount();
    g_failReadAtSector = TEST_MAP_SECTOR;
    __ExpectFailedMount("the read of the bucket map");
    __FormatForMount();
    ((MasterRecord_t*)&g_image[TEST_MASTER_SECTOR * TEST_SECTOR_SIZE])->Magic = 0;
    __ExpectFailedMount("the check of the master record");
    g_errors = 0;
}

// mfs_data_test.c includes this file for its doubles and helpers, and brings
// its own main.
#ifndef MFS_ENTRIES_TEST_OWN_MAIN
int
main(void)
{
    __TestExpansion();
    __TestUnlink();
    __TestSeek();
    __TestMountMemory();
    puts("MFS entries: directory expansion, unlink, seek in an empty file and mount memory passed");
    return 0;
}
#endif //!MFS_ENTRIES_TEST_OWN_MAIN
