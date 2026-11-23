// MFS creates a directory inside a directory that has no buckets yet, while
// the disk goes away at each possible write. FsCreate must return the error
// of the failed write, and the filesystem in memory must match what is on
// the disk afterwards.
//
// Production code compiled here: modules/filesystems/mfs/main.c (FsCreate,
// FsClose), records.c, bucket_map.c and utilities.c, and the libds mstring
// code. Doubles replace:
// - FSStorageRead and FSStorageWrite (modules/filesystems/common/storage.c).
//   They copy sectors between the transfer buffer (or the caller's buffer of
//   a file read or write) and an in-memory partition image. From a chosen
//   write on, every read and write fails with OS_ENOENT
//   and changes nothing, which is what the SD storage driver does once the
//   card is gone. A transfer of zero sectors succeeds without touching the
//   disk, as in the logs where FsCreate returned 0.
// - SHM buffers, which are heap memory.
// - The file and directory read, write, seek and size handlers, which FsCreate
//   does not use.
// - The usched mutex functions for the mount lock. They check that the lock is
//   free when an operation takes it, and held when it is released.
//
// The image is formatted by the test: a boot area, the master record and its
// mirror, the bucket map, a root directory of MFS_ROOTSIZE buckets, and one
// free run of buckets. Free buckets are filled with a pattern, so a directory
// that points at a bucket that was never zeroed shows up as garbage records.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddk/utils.h>
#include <fs/common.h>
#include <os/shm.h>
#include <os/types/file.h>
#include "../modules/filesystems/mfs/mfs.h"

#define TEST_SECTOR_SIZE        512
#define TEST_SECTORS_PER_BUCKET 8
#define TEST_RESERVED_SECTORS   16
#define TEST_MASTER_SECTOR      1
#define TEST_MIRROR_SECTOR      2
#define TEST_MAP_SECTOR         3
#define TEST_BUCKETS            256
#define TEST_FREE_PATTERN       0xA5
#define TEST_IMAGE_SECTORS      (TEST_RESERVED_SECTORS + (TEST_BUCKETS * TEST_SECTORS_PER_BUCKET))
#define TEST_BUFFER_ID          42
#define TEST_USER_BUFFER_ID     43

static int g_errors;

// Trace lines MFS logged. mfs_runs_test.c checks that a file read or write
// logs none, as each line costs a message to the system log.
static int g_traces;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (level == OSSYSLOGLEVEL_TRACE) {
        g_traces++;
    }
    if (getenv("MFS_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        va_list arguments;
        va_start(arguments, format);
        vprintf(format, arguments);
        va_end(arguments);
        printf("\n");
    }
}

void* dsalloc(size_t size) { return malloc(size); }
void  dsfree(void* memory) { free(memory); }

// The transfer buffer is the only SHM buffer. It holds a whole root
// directory, like the buffer __ResizeTransferBuffer creates.
static uint8_t g_transferBuffer[TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE * MFS_ROOTSIZE];

// The caller's buffer for file reads and writes, TEST_USER_BUFFER_ID. MFS
// moves whole sectors straight between it and the disk, and copies the rest
// through the transfer buffer, so a test passes this buffer both as the
// handle and as the pointer. mfs_data_test.c uses it.
static uint8_t g_userBuffer[TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE * 8];

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    assert(handle->ID == TEST_BUFFER_ID);
    return g_transferBuffer;
}

size_t
SHMBufferLength(
    _In_ OSHandle_t* handle)
{
    assert(handle->ID == TEST_BUFFER_ID);
    return sizeof(g_transferBuffer);
}

// mfs_entries_test.c defines MFS_TEST_OWN_MOUNT_DOUBLES. It mounts with the
// real FsInitialize, which needs SHMCreate and FSStorageStat.
#ifndef MFS_TEST_OWN_MOUNT_DOUBLES
oserr_t SHMCreate(SHM_t* shm, OSHandle_t* handleOut) { abort(); }
#endif //!MFS_TEST_OWN_MOUNT_DOUBLES
size_t  MemoryPageSize(void) { return 4096; }

// mfs_lock_test.c defines MFS_TEST_OWN_DOUBLES. It links the real file and
// directory handlers, and brings its own mount lock double, which can let a
// second operation run while the first one waits for the disk.
#ifndef MFS_TEST_OWN_DOUBLES
void OSHandleDestroy(OSHandle_t* handle) { abort(); }

oserr_t FsReadFromFile(FileSystemMFS_t* m, MFSEntry_t* e, uuid_t b, void* d, size_t o, size_t c, size_t* r) { abort(); }
oserr_t FsWriteToFile(FileSystemMFS_t* m, MFSEntry_t* e, uuid_t b, void* d, size_t o, size_t c, size_t* w) { abort(); }
oserr_t FsSeekInFile(FileSystemMFS_t* m, MFSEntry_t* e, uint64_t p) { abort(); }
oserr_t FsChangeFileSize(FileSystemMFS_t* m, MFSEntry_t* e, uint64_t s) { abort(); }
oserr_t FsReadFromDirectory(FileSystemMFS_t* m, MFSEntry_t* e, void* d, size_t o, size_t c, size_t* r) { abort(); }
oserr_t FsSeekInDirectory(FileSystemMFS_t* m, MFSEntry_t* e, uint64_t p) { abort(); }

// The mount lock (FileSystemMFS_t::Lock). These tests run one operation at a
// time, so the lock must always be free when it is taken. A second lock by
// the same operation would wait forever on the plain lock MFS uses, and an
// unlock of a free lock is an error as well.
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
#endif //!MFS_TEST_OWN_DOUBLES

#ifndef MFS_TEST_OWN_MOUNT_DOUBLES
oserr_t
FSStorageStat(
    _InOut_ struct VFSStorageParameters* storageParameters,
    _Out_   StorageDescriptor_t*         stat)
{
    abort();
}
#endif //!MFS_TEST_OWN_MOUNT_DOUBLES

// The partition image. g_writesUntilRemoval counts down with each write that
// transfers sectors. The write that brings it to zero, and every transfer
// after it, fails. A negative value never removes the disk.
static uint8_t g_image[TEST_IMAGE_SECTORS * TEST_SECTOR_SIZE];
static int     g_writesUntilRemoval = -1;
static bool    g_removed;
static int     g_writes;

// Single failures for the move test. g_failOnlyWrite makes that one write
// (counted like g_writesUntilRemoval) fail, and the writes after it work
// again. g_failReadAtSector makes the next read that starts at that sector
// fail once. Both fail with OS_ENOENT and change nothing.
static int      g_failOnlyWrite = -1;
static uint64_t g_failReadAtSector = UINT64_MAX;

// g_failWriteAtSector makes the next write that starts at that sector fail
// once with OS_ENOENT, and changes nothing. mfs_data_test.c uses it to fail
// the write of one record, or of the bucket map.
static uint64_t g_failWriteAtSector = UINT64_MAX;

// Called for each transfer of one or more sectors, before any data moves. The
// lock test uses it to let a second operation run while the first one waits
// for the disk. NULL calls nothing.
static void (*g_beforeTransfer)(bool write, uint64_t sector, size_t count);

static oserr_t
__Transfer(
    _In_  uuid_t        buffer,
    _In_  size_t        offset,
    _In_  UInteger64_t* sector,
    _In_  size_t        count,
    _In_  bool          write,
    _Out_ size_t*       transferred)
{
    size_t   bytes = count * TEST_SECTOR_SIZE;
    uint8_t* memory = g_transferBuffer;
    size_t   memorySize = sizeof(g_transferBuffer);

    assert(buffer == TEST_BUFFER_ID || buffer == TEST_USER_BUFFER_ID);
    if (buffer == TEST_USER_BUFFER_ID) {
        memory = g_userBuffer;
        memorySize = sizeof(g_userBuffer);
    }
    *transferred = 0;
    if (count == 0) {
        return OS_EOK;
    }
    if (g_beforeTransfer != NULL) {
        g_beforeTransfer(write, sector->QuadPart, count);
    }

    if (write && !g_removed && g_writesUntilRemoval > 0 && --g_writesUntilRemoval == 0) {
        g_removed = true;
    }
    if (g_removed) {
        return OS_ENOENT;
    }
    if (write && g_failOnlyWrite > 0 && --g_failOnlyWrite == 0) {
        return OS_ENOENT;
    }
    if (!write && sector->QuadPart == g_failReadAtSector) {
        g_failReadAtSector = UINT64_MAX;
        return OS_ENOENT;
    }
    if (write && sector->QuadPart == g_failWriteAtSector) {
        g_failWriteAtSector = UINT64_MAX;
        return OS_ENOENT;
    }

    assert(sector->QuadPart + count <= TEST_IMAGE_SECTORS);
    assert(offset + bytes <= memorySize);
    if (write) {
        memcpy(&g_image[sector->QuadPart * TEST_SECTOR_SIZE], &memory[offset], bytes);
        g_writes++;
    } else {
        memcpy(&memory[offset], &g_image[sector->QuadPart * TEST_SECTOR_SIZE], bytes);
    }
    *transferred = count;
    return OS_EOK;
}

oserr_t
FSStorageRead(
    _In_  struct VFSStorageParameters* storageParameters,
    _In_  uuid_t                       buffer,
    _In_  size_t                       offset,
    _In_  UInteger64_t*                sector,
    _In_  size_t                       count,
    _Out_ size_t*                      read)
{
    return __Transfer(buffer, offset, sector, count, false, read);
}

oserr_t
FSStorageWrite(
    _In_  struct VFSStorageParameters* storageParameters,
    _In_  uuid_t                       buffer,
    _In_  size_t                       offset,
    _In_  UInteger64_t*                sector,
    _In_  size_t                       count,
    _Out_ size_t*                      written)
{
    return __Transfer(buffer, offset, sector, count, true, written);
}

extern oserr_t FsCreate(void*, void*, mstring_t*, uint32_t, uint32_t, uint32_t, void**);
extern oserr_t FsClose(void*, void*);

static FileSystemMFS_t g_mfs;
static uint32_t        g_bucketMap[(TEST_BUCKETS * 2) + (TEST_SECTOR_SIZE / sizeof(uint32_t))];

static uint8_t*
__Bucket(
    _In_ uint32_t bucket)
{
    return &g_image[MFS_GETSECTOR(&g_mfs, bucket) * TEST_SECTOR_SIZE];
}

// Formats the image and sets up the mounted filesystem the way FsInitialize
// leaves it.
static void
__Format(void)
{
    MasterRecord_t master = { 0 };
    uint32_t*      map = (uint32_t*)&g_image[TEST_MAP_SECTOR * TEST_SECTOR_SIZE];

    memset(g_image, 0, sizeof(g_image));
    memset(&g_mfs, 0, sizeof(g_mfs));
    g_mfs.SectorSize = TEST_SECTOR_SIZE;
    g_mfs.SectorsPerBucket = TEST_SECTORS_PER_BUCKET;
    g_mfs.ReservedSectorCount = TEST_RESERVED_SECTORS;
    g_mfs.MasterRecordSector = TEST_MASTER_SECTOR;
    g_mfs.MasterRecordMirrorSector = TEST_MIRROR_SECTOR;
    g_mfs.BucketsPerSectorInMap = TEST_SECTOR_SIZE / sizeof(MapRecord_t);
    g_mfs.BucketsInMap = TEST_BUCKETS;
    g_mfs.TransferBuffer.ID = TEST_BUFFER_ID;
    g_mfs.Label = mstr_new_u8("vali-data");

    // The root directory is buckets 0 to MFS_ROOTSIZE - 1, and the rest is one
    // free run.
    master.Magic = MFS_BOOTRECORD_MAGIC;
    master.FreeBucket = MFS_ROOTSIZE;
    master.RootIndex = 0;
    master.MapSector = TEST_MAP_SECTOR;
    master.MapSize = TEST_BUCKETS * sizeof(MapRecord_t);
    memcpy(&g_image[TEST_MASTER_SECTOR * TEST_SECTOR_SIZE], &master, sizeof(master));
    memcpy(&g_image[TEST_MIRROR_SECTOR * TEST_SECTOR_SIZE], &master, sizeof(master));
    memcpy(&g_mfs.MasterRecord, &master, sizeof(master));

    map[0] = MFS_ENDOFCHAIN;
    map[1] = MFS_ROOTSIZE;
    map[MFS_ROOTSIZE * 2] = MFS_ENDOFCHAIN;
    map[(MFS_ROOTSIZE * 2) + 1] = TEST_BUCKETS - MFS_ROOTSIZE;
    memset(g_bucketMap, 0, sizeof(g_bucketMap));
    memcpy(g_bucketMap, map, TEST_BUCKETS * sizeof(MapRecord_t));
    g_mfs.BucketMap = g_bucketMap;
    memset(__Bucket(MFS_ROOTSIZE), TEST_FREE_PATTERN,
           (TEST_BUCKETS - MFS_ROOTSIZE) * TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE);

    g_mfs.RootEntry.Name = mstr_new_u8("<root>");
    g_mfs.RootEntry.Flags = FILE_FLAG_DIRECTORY;
    g_mfs.RootEntry.NativeFlags = MFS_FILERECORD_DIRECTORY | MFS_FILERECORD_INUSE;
    g_mfs.RootEntry.StartBucket = 0;
    g_mfs.RootEntry.StartLength = MFS_ROOTSIZE;
    g_mfs.RootEntry.DirectoryBucket = MFS_ENDOFCHAIN;
    g_mfs.RootEntry.DataBucketPosition = 0;
    g_mfs.RootEntry.DataBucketLength = MFS_ROOTSIZE;

    g_writesUntilRemoval = -1;
    g_removed = false;
    g_writes = 0;
    g_failOnlyWrite = -1;
    g_failReadAtSector = UINT64_MAX;
    g_failWriteAtSector = UINT64_MAX;
    g_beforeTransfer = NULL;
}

static void
__Unmount(void)
{
    mstr_delete(g_mfs.Label);
    mstr_delete(g_mfs.RootEntry.Name);
}

// The record of an entry, as it is on the disk.
static FileRecord_t*
__RecordOnDisk(
    _In_ MFSEntry_t* entry)
{
    return (FileRecord_t*)(__Bucket(entry->DirectoryBucket) + (sizeof(FileRecord_t) * entry->DirectoryIndex));
}

// True when no byte of the buckets still has the free pattern. Zeroing
// removes it, and the records MFS writes afterwards never contain it.
static bool
__BucketsCleared(
    _In_ uint32_t bucket,
    _In_ uint32_t length)
{
    uint8_t* data = __Bucket(bucket);

    for (size_t i = 0; i < (size_t)length * TEST_SECTORS_PER_BUCKET * TEST_SECTOR_SIZE; i++) {
        if (data[i] == TEST_FREE_PATTERN) {
            return false;
        }
    }
    return true;
}

// Checks that the filesystem in memory matches the disk:
// - The primary master record on disk has the free list head memory has.
// - The bucket map on disk is the bucket map in memory.
// - The record of the directory on disk has the buckets memory has, and a
//   directory with buckets only points at buckets that were zeroed.
// - No bucket of the root directory or of the directory is on the free list
//   that the disk describes.
static void
__CheckConsistency(
    _In_ MFSEntry_t* directory)
{
    MasterRecord_t* master = (MasterRecord_t*)&g_image[TEST_MASTER_SECTOR * TEST_SECTOR_SIZE];
    uint32_t*       map = (uint32_t*)&g_image[TEST_MAP_SECTOR * TEST_SECTOR_SIZE];
    FileRecord_t*   record = __RecordOnDisk(directory);
    uint32_t        bucket;
    int             steps = 0;

    assert(master->FreeBucket == g_mfs.MasterRecord.FreeBucket);
    assert(!memcmp(map, g_mfs.BucketMap, TEST_BUCKETS * sizeof(MapRecord_t)));
    assert(record->StartBucket == directory->StartBucket);
    assert(record->StartLength == directory->StartLength);
    if (directory->StartBucket != MFS_ENDOFCHAIN) {
        assert(__BucketsCleared(directory->StartBucket, directory->StartLength));
    }

    for (bucket = master->FreeBucket; bucket != MFS_ENDOFCHAIN; bucket = map[bucket * 2]) {
        uint32_t length = map[(bucket * 2) + 1];

        assert(steps++ < TEST_BUCKETS && bucket < TEST_BUCKETS);
        assert(bucket >= MFS_ROOTSIZE);
        if (directory->StartBucket != MFS_ENDOFCHAIN) {
            assert(bucket + length <= directory->StartBucket ||
                   bucket >= directory->StartBucket + directory->StartLength);
        }
    }
}

static MFSEntry_t*
__Create(
    _In_ MFSEntry_t* parent,
    _In_ const char* name,
    _Out_ oserr_t*   resultOut)
{
    mstring_t* mname = mstr_new_u8(name);
    void*      data = NULL;

    assert(mname != NULL);
    *resultOut = FsCreate(&g_mfs, parent, mname, 0, FILE_FLAG_DIRECTORY, FILE_PERMISSION_READ, &data);
    mstr_delete(mname);
    return data;
}

// Creates /served, then /served/apps with the disk removed at the given write
// of the second create. Returns the number of writes the second create made.
static int
__CreateWithRemoval(
    _In_ int removeAtWrite)
{
    MFSEntry_t* served;
    MFSEntry_t* apps;
    oserr_t     oserr;
    int         writes;

    __Format();
    served = __Create(&g_mfs.RootEntry, "served", &oserr);
    assert(oserr == OS_EOK && served != NULL && served->StartBucket == MFS_ENDOFCHAIN);

    g_errors = 0;
    g_writes = 0;
    g_writesUntilRemoval = removeAtWrite;
    apps = __Create(served, "apps", &oserr);
    writes = g_writes;
    if (g_removed) {
        // The create failed with the storage driver's error, and logged it.
        assert(oserr == OS_ENOENT && apps == NULL);
        assert(g_errors > 0);
    } else {
        assert(oserr == OS_EOK && apps != NULL && g_errors == 0);
        assert(served->StartBucket != MFS_ENDOFCHAIN);
        assert(!strcmp((const char*)__RecordOnDisk(apps)->Name, "apps"));
        assert(FsClose(&g_mfs, apps) == OS_EOK);
    }
    __CheckConsistency(served);

    // The parent handle is closed without writing, as the VFS does it.
    assert(served->ActionOnClose == MFS_ACTION_NONE);
    g_removed = false;
    g_writesUntilRemoval = -1;
    assert(FsClose(&g_mfs, served) == OS_EOK);
    __Unmount();
    return writes;
}

int
main(void)
{
    int writes;

    // Without a removal, count the writes of the second create.
    writes = __CreateWithRemoval(-1);
    assert(writes > 0);

    // Remove the disk at every one of them.
    for (int i = 1; i <= writes; i++) {
        (void)__CreateWithRemoval(i);
    }
    printf("MFS create with the disk removed at each of its %i writes passed\n", writes);
    return 0;
}
