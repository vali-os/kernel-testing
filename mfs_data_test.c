// MFS file data: where a write lands, which bytes a read can return, and the
// order in which records and buckets are removed. It runs on the in-memory
// partition of mfs_create_test.c, with the helpers of mfs_entries_test.c.
//
// Production code compiled here: modules/filesystems/mfs/main.c, records.c,
// bucket_map.c, utilities.c, file_operations.c and directory_operations.c,
// and the libds mstring code. File data goes through g_userBuffer
// (TEST_USER_BUFFER_ID), so MFS can move whole sectors straight between the
// caller's buffer and the disk, as it does in the system.
//
// The cases:
// - Create and write. filed writes on the handle FsCreate returns without a
//   seek first (the copy in services/filed/vfs/handlers/move.c). The handle
//   must start with no data bucket (MFS_ENDOFCHAIN). With a data bucket of 0
//   the write stepped on from bucket 0, the root directory.
// - Directory buckets. A directory gets MFS_DIRECTORYEXPANSION (4) buckets at
//   a time. When the free list is in pieces, they come as several runs, and
//   every run must be zeroed. Old records left in a run show up in a listing.
// - Truncate on an open handle. After a truncate to 0 the handle must not
//   point at the freed buckets. A truncate to a smaller size keeps every
//   bucket, so the handle stays valid.
// - A write past the allocated end. A seek past the last bucket stops at that
//   bucket. The write after it must find the bucket of the position once the
//   file has grown, and must not write outside the file.
// - Old data. Bytes of a file that were never written must read as zero: the
//   gap before a write that starts past the end, the bytes a truncate adds,
//   and the rest of a sector that was written in part.
// - Remove order. An unlink, and a truncate to 0, write the record first and
//   free the buckets after it. A failed record write must leave the file as
//   it was. A failed free loses the buckets but must never put them on the
//   free list while a record still uses them.
#define MFS_ENTRIES_TEST_OWN_MAIN
#include "mfs_entries_test.c"

#define TEST_OLD_SEED     0xEE
#define TEST_MAX_NEIGHBOURS 8

static uint8_t  g_snapshot[sizeof(g_image)];
static uint32_t g_neighbourBuckets[TEST_MAX_NEIGHBOURS];
static uint8_t  g_neighbourSeeds[TEST_MAX_NEIGHBOURS];
static int      g_neighbourCount;

static void
__Fail(
    _In_ const char* format,
    ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    printf("\n");
    fflush(stdout);
    abort();
}

// The byte at index of data written with seed. It changes with each byte and
// each sector, so data in the wrong place does not match by chance.
static uint8_t
__PatternByte(
    _In_ uint8_t  seed,
    _In_ uint64_t index)
{
    return (uint8_t)(seed + (index * 31) + (index / TEST_SECTOR_SIZE));
}

// Seeks to position and writes length bytes of the pattern of seed.
static void
__WriteAt(
    _In_ MFSEntry_t* entry,
    _In_ uint64_t    position,
    _In_ uint8_t     seed,
    _In_ size_t      length)
{
    uint64_t positionOut;
    size_t   count;
    oserr_t  oserr;

    assert(length <= sizeof(g_userBuffer));
    assert(FsSeek(&g_mfs, entry, position, &positionOut) == OS_EOK && positionOut == position);
    for (size_t i = 0; i < length; i++) {
        g_userBuffer[i] = __PatternByte(seed, i);
    }
    oserr = FsWrite(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, length, &count);
    if (oserr != OS_EOK || count != length) {
        __Fail("FsWrite of %zu bytes at %llu returned %u with %zu written",
               length, (unsigned long long)position, oserr, count);
    }
}

// Writes length bytes of the pattern of seed at the position of the handle,
// without a seek first.
static void
__WriteHere(
    _In_ MFSEntry_t* entry,
    _In_ uint8_t     seed,
    _In_ size_t      length)
{
    size_t  count;
    oserr_t oserr;

    assert(length <= sizeof(g_userBuffer));
    for (size_t i = 0; i < length; i++) {
        g_userBuffer[i] = __PatternByte(seed, i);
    }
    oserr = FsWrite(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, length, &count);
    if (oserr != OS_EOK || count != length) {
        __Fail("FsWrite of %zu bytes without a seek returned %u with %zu written", length, oserr, count);
    }
}

// Reads length bytes at position into g_userBuffer.
static void
__ReadAt(
    _In_ MFSEntry_t* entry,
    _In_ uint64_t    position,
    _In_ size_t      length)
{
    uint64_t positionOut;
    size_t   count;
    oserr_t  oserr;

    assert(length <= sizeof(g_userBuffer));
    assert(FsSeek(&g_mfs, entry, position, &positionOut) == OS_EOK && positionOut == position);
    memset(g_userBuffer, 0x5A, length);
    oserr = FsRead(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, length, &count);
    if (oserr != OS_EOK || count != length) {
        __Fail("FsRead of %zu bytes at %llu returned %u with %zu read",
               length, (unsigned long long)position, oserr, count);
    }
}

// The file holds the pattern of seed at position.
static void
__ExpectPattern(
    _In_ MFSEntry_t* entry,
    _In_ uint64_t    position,
    _In_ uint8_t     seed,
    _In_ size_t      length)
{
    __ReadAt(entry, position, length);
    for (size_t i = 0; i < length; i++) {
        if (g_userBuffer[i] != __PatternByte(seed, i)) {
            __Fail("byte %llu is 0x%02x, expected 0x%02x of the data written there",
                   (unsigned long long)(position + i), g_userBuffer[i], __PatternByte(seed, i));
        }
    }
}

// The file holds zeros from position to end. These bytes were never written.
static void
__ExpectZeros(
    _In_ MFSEntry_t* entry,
    _In_ uint64_t    position,
    _In_ uint64_t    end)
{
    while (position < end) {
        size_t length = sizeof(g_userBuffer);

        if (end - position < length) {
            length = (size_t)(end - position);
        }

        __ReadAt(entry, position, length);
        for (size_t i = 0; i < length; i++) {
            if (g_userBuffer[i] != 0) {
                __Fail("byte %llu was never written but reads as 0x%02x",
                       (unsigned long long)(position + i), g_userBuffer[i]);
            }
        }
        position += length;
    }
}

// Reads length bytes at the position of the handle, without a seek first,
// and checks that they are zero.
static void
__ExpectZerosHere(
    _In_ MFSEntry_t* entry,
    _In_ size_t      length)
{
    size_t  count;
    oserr_t oserr;

    assert(length <= sizeof(g_userBuffer));
    memset(g_userBuffer, 0x5A, length);
    oserr = FsRead(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, length, &count);
    if (oserr != OS_EOK || count != length) {
        __Fail("FsRead of %zu bytes without a seek returned %u with %zu read", length, oserr, count);
    }
    for (size_t i = 0; i < length; i++) {
        if (g_userBuffer[i] != 0) {
            __Fail("byte %zu was never written but reads as 0x%02x", i, g_userBuffer[i]);
        }
    }
}

static void
__Snapshot(void)
{
    memcpy(g_snapshot, g_image, sizeof(g_image));
}

static bool
__BucketChanged(
    _In_ uint32_t bucket)
{
    size_t offset = (size_t)(__Bucket(bucket) - g_image);

    return memcmp(&g_image[offset], &g_snapshot[offset], TEST_BUCKET_BYTES) != 0;
}

// No bucket on the free list was written since the snapshot.
static void
__ExpectFreeBucketsUnchanged(void)
{
    for (uint32_t bucket = MFS_ROOTSIZE; bucket < TEST_BUCKETS; bucket++) {
        if (__BucketIsFree(bucket) && __BucketChanged(bucket)) {
            __Fail("bucket %u is on the free list, and was written", bucket);
        }
    }
}

// The buckets from bucket to bucket + length - 1 were not written since the
// snapshot.
static void
__ExpectBucketsUnchanged(
    _In_ uint32_t    bucket,
    _In_ uint32_t    length,
    _In_ const char* what)
{
    for (uint32_t i = 0; i < length; i++) {
        if (__BucketChanged(bucket + i)) {
            __Fail("bucket %u of %s was written", bucket + i, what);
        }
    }
}

// Splits the free list into holes of one bucket. Files n00 to n<2 * holes>
// get one bucket each. The odd ones are filled with the pattern of
// TEST_OLD_SEED and unlinked, so their buckets go back on the free list with
// that old data in them. The even ones stay, each filled with its own
// pattern, and are the neighbours __ExpectNeighbours checks. The free list
// then starts with the holes, the last one first, followed by the rest of the
// disk.
static void
__SplitFreeList(
    _In_ int holes)
{
    char        name[16];
    char        path[16];
    MFSEntry_t* entry;

    assert(holes + 1 <= TEST_MAX_NEIGHBOURS);
    g_neighbourCount = 0;
    for (int i = 0; i <= 2 * holes; i++) {
        uint8_t seed = (i % 2) ? TEST_OLD_SEED : (uint8_t)(0x60 + i);

        snprintf(name, sizeof(name), "n%02d", i);
        entry = __CreateEntry(&g_mfs.RootEntry, name, 0);
        __WriteAt(entry, 0, seed, TEST_BUCKET_BYTES);
        assert(entry->StartLength == 1);
        if (!(i % 2)) {
            g_neighbourBuckets[g_neighbourCount] = entry->StartBucket;
            g_neighbourSeeds[g_neighbourCount] = seed;
            g_neighbourCount++;
        }
        assert(FsClose(&g_mfs, entry) == OS_EOK);
    }
    for (int i = 1; i < 2 * holes; i += 2) {
        snprintf(path, sizeof(path), "/n%02d", i);
        assert(__Unlink(path) == OS_EOK);
        assert(g_mfs.BucketMap[(g_mfs.MasterRecord.FreeBucket * 2) + 1] == 1);
    }
}

// The bucket of each neighbour file still holds its own pattern.
static void
__ExpectNeighbours(void)
{
    for (int i = 0; i < g_neighbourCount; i++) {
        uint8_t* data = __Bucket(g_neighbourBuckets[i]);

        for (size_t j = 0; j < TEST_BUCKET_BYTES; j++) {
            if (data[j] != __PatternByte(g_neighbourSeeds[i], j)) {
                __Fail("bucket %u of a neighbour file was written at byte %zu", g_neighbourBuckets[i], j);
            }
        }
    }
}

// Create and write: the handle FsCreate returns is written without a seek.
// The data must be in the buckets of the file, the root directory must not
// change, and a reopen must read the data back. Once with the root as
// formatted, once after the root has expanded.
static void
__ExpectCreateAndWrite(
    _In_ const char* name)
{
    char        path[16];
    MFSEntry_t* entry;
    uint32_t    rootBucket;
    int         steps = 0;

    snprintf(path, sizeof(path), "/%s", name);
    // The new handle has no buckets and no data bucket (see
    // MFSEntryResetPosition in mfs.h).
    entry = __CreateEntry(&g_mfs.RootEntry, name, 0);
    assert(entry->StartBucket == MFS_ENDOFCHAIN && entry->Position == 0);
    assert(entry->DataBucketPosition == MFS_ENDOFCHAIN && entry->DataBucketLength == 0);
    __Snapshot();
    __WriteHere(entry, 0x11, 100);
    assert(entry->StartBucket != MFS_ENDOFCHAIN && entry->ActualSize == 100);

    // The data is in the first bucket of the file.
    for (size_t i = 0; i < 100; i++) {
        assert(__Bucket(entry->StartBucket)[i] == __PatternByte(0x11, i));
    }

    // No bucket of the root directory was written. Its records change only
    // when the handle is closed.
    for (rootBucket = g_mfs.RootEntry.StartBucket; rootBucket != MFS_ENDOFCHAIN;
         rootBucket = __MapOnDisk()[rootBucket * 2]) {
        assert(steps++ < TEST_BUCKETS);
        __ExpectBucketsUnchanged(rootBucket, __MapOnDisk()[(rootBucket * 2) + 1], "the root directory");
    }
    __ExpectFreeBucketsUnchanged();
    __ExpectPattern(entry, 0, 0x11, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // An opened handle starts in the first bucket of the file.
    entry = __Open(path);
    assert(entry->ActualSize == 100 && entry->Position == 0 && entry->BucketByteBoundary == 0);
    assert(entry->DataBucketPosition == entry->StartBucket && entry->DataBucketLength == entry->StartLength);
    __ExpectPattern(entry, 0, 0x11, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
}

static void
__TestCreateAndWrite(void)
{
    int rootRecords = (int)(MFS_ROOTSIZE * TEST_RECORDS_PER_BUCKET);

    __Format();
    g_errors = 0;
    __ExpectCreateAndWrite("new");
    assert(g_errors == 0);
    __Unmount();

    // A full root, so the record of the new file is in the expansion, and
    // bucket 0 links to it.
    __Format();
    g_errors = 0;
    __FillDirectory(&g_mfs.RootEntry, "", rootRecords);
    __ExpectCreateAndWrite("new2");
    assert(__MapOnDisk()[0] != MFS_ENDOFCHAIN);
    assert(g_errors == 0);
    __Unmount();
}

// Old records in a free bucket, as a deleted directory leaves them.
static void
__PlantOldRecords(
    _In_ uint32_t bucket)
{
    FileRecord_t* record = (FileRecord_t*)__Bucket(bucket);

    for (size_t i = 0; i < TEST_RECORDS_PER_BUCKET; i++, record++) {
        memset(record, 0, sizeof(*record));
        record->Flags = MFS_FILERECORD_INUSE;
        record->StartBucket = MFS_ENDOFCHAIN;
        snprintf((char*)&record->Name[0], sizeof(record->Name), "ghost%u_%zu", bucket, i);
    }
}

// Puts old records in the free buckets near the start of the disk, which
// includes every bucket a directory can get from __SplitFreeList.
static void
__PlantOldRecordsInFreeBuckets(void)
{
    for (uint32_t bucket = MFS_ROOTSIZE; bucket < MFS_ROOTSIZE + 32; bucket++) {
        if (__BucketIsFree(bucket)) {
            __PlantOldRecords(bucket);
        }
    }
}

// Every bucket of the directory chain from firstBucket on is zero, apart from
// the records in use named in names. Returns the number of buckets checked.
static uint32_t
__ExpectCleanDirectoryBuckets(
    _In_ uint32_t     firstBucket,
    _In_ const char** names,
    _In_ int          nameCount)
{
    uint32_t* map = __MapOnDisk();
    uint32_t  buckets = 0;
    int       steps = 0;

    for (uint32_t run = firstBucket; run != MFS_ENDOFCHAIN; run = map[run * 2]) {
        assert(steps++ < TEST_BUCKETS && run < TEST_BUCKETS);
        for (uint32_t bucket = run; bucket < run + map[(run * 2) + 1]; bucket++, buckets++) {
            FileRecord_t* record = (FileRecord_t*)__Bucket(bucket);

            for (size_t i = 0; i < TEST_RECORDS_PER_BUCKET; i++, record++) {
                bool named = false;

                for (int j = 0; j < nameCount; j++) {
                    named |= (record->Flags & MFS_FILERECORD_INUSE) && !strcmp((char*)&record->Name[0], names[j]);
                }
                if (named) {
                    continue;
                }
                for (size_t j = 0; j < sizeof(*record); j++) {
                    if (((uint8_t*)record)[j] != 0) {
                        __Fail("record %zu of directory bucket %u (run %u) is not zero at byte %zu: 0x%02x",
                               i, bucket, run, j, ((uint8_t*)record)[j]);
                    }
                }
            }
        }
    }
    return buckets;
}

// Lists the directory at path with FsRead and checks that it has count
// entries, none of them an old record.
static void
__ExpectListing(
    _In_ const char* path,
    _In_ int         count)
{
    static uint8_t listing[64 * 1024];
    MFSEntry_t*    directory = __Open(path);
    size_t         length;
    size_t         offset = 0;
    int            entries = 0;

    assert(FsRead(&g_mfs, directory, TEST_BUFFER_ID, listing, 0, sizeof(listing), &length) == OS_EOK);
    while (offset < length) {
        struct VFSDirectoryEntry entry;
        const char*              name = (const char*)&listing[offset + sizeof(entry)];

        memcpy(&entry, &listing[offset], sizeof(entry));
        if (!strncmp(name, "ghost", 5)) {
            __Fail("the listing of %s shows the old record %s", path, name);
        }
        entries++;
        offset += sizeof(entry) + entry.NameLength + entry.LinkLength;
    }
    if (entries != count) {
        __Fail("the listing of %s has %i entries, expected %i", path, entries, count);
    }
    assert(FsClose(&g_mfs, directory) == OS_EOK);
}

static void
__TestDirectoryBuckets(void)
{
    int         rootRecords = (int)(MFS_ROOTSIZE * TEST_RECORDS_PER_BUCKET);
    const char* expansionNames[] = { "x" };
    const char* subNames[] = { "y" };
    MFSEntry_t* sub;
    MFSEntry_t* found;
    char        name[16];
    uint32_t    firstRun;

    // The root expands while the free list is in holes of one bucket. The
    // holes hold old records.
    __Format();
    g_errors = 0;
    __SplitFreeList(3);
    __PlantOldRecordsInFreeBuckets();

    // The neighbours n00, n02, n04 and n06 keep their records. Fill the root.
    for (int i = 0; i < rootRecords - g_neighbourCount; i++) {
        snprintf(name, sizeof(name), "r%02d", i);
        assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, name, 0)) == OS_EOK);
    }
    firstRun = g_mfs.MasterRecord.FreeBucket;
    assert(FsClose(&g_mfs, __CreateEntry(&g_mfs.RootEntry, "x", 0)) == OS_EOK);
    assert(__MapOnDisk()[0] == firstRun);
    assert(__MapOnDisk()[(firstRun * 2)] != MFS_ENDOFCHAIN && __MapOnDisk()[(firstRun * 2) + 1] == 1);
    assert(__ExpectCleanDirectoryBuckets(firstRun, expansionNames, 1) == MFS_DIRECTORYEXPANSION);
    __ExpectListing("/", rootRecords + 1);
    __ExpectMapOnDisk();
    assert(g_errors == 0);
    __Unmount();

    // A subdirectory gets its first buckets the same way.
    __Format();
    g_errors = 0;
    __SplitFreeList(3);
    __PlantOldRecordsInFreeBuckets();
    sub = __CreateEntry(&g_mfs.RootEntry, "sub", FILE_FLAG_DIRECTORY);
    assert(FsClose(&g_mfs, __CreateEntry(sub, "y", 0)) == OS_EOK);
    assert(FsClose(&g_mfs, sub) == OS_EOK);
    found = __Find("/sub");
    assert(found != NULL && found->StartBucket != MFS_ENDOFCHAIN);
    assert(__MapOnDisk()[(found->StartBucket * 2)] != MFS_ENDOFCHAIN);
    assert(__ExpectCleanDirectoryBuckets(found->StartBucket, subNames, 1) == MFS_DIRECTORYEXPANSION);
    __Free(found);
    __ExpectListing("/sub", 1);
    __ExpectMapOnDisk();
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestTruncate(void)
{
    MFSEntry_t* entry;
    MFSEntry_t* other;
    uint32_t    freedBucket;
    uint32_t    startBucket;
    uint64_t    allocatedSize;
    uint64_t    position;

    // Truncate to 0, then write on the same handle without a seek. The
    // position stays at 300. Another file takes the freed bucket first.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "t", 0);
    __WriteAt(entry, 0, 0x31, 300);
    freedBucket = entry->StartBucket;
    assert(FsTruncate(&g_mfs, entry, 0) == OS_EOK);
    assert(entry->StartBucket == MFS_ENDOFCHAIN && entry->ActualSize == 0 && entry->Position == 300);
    assert(__BucketIsFree(freedBucket));

    other = __CreateEntry(&g_mfs.RootEntry, "b", 0);
    __WriteAt(other, 0, 0xB0, TEST_BUCKET_BYTES);
    assert(other->StartBucket == freedBucket);
    assert(FsClose(&g_mfs, other) == OS_EOK);

    __Snapshot();
    __WriteHere(entry, 0x32, 50);
    assert(entry->StartBucket != MFS_ENDOFCHAIN && entry->StartBucket != freedBucket);
    assert(entry->ActualSize == 350);
    __ExpectBucketsUnchanged(freedBucket, 1, "the file that took the freed bucket");
    __ExpectFreeBucketsUnchanged();
    __ExpectZeros(entry, 0, 300);
    __ExpectPattern(entry, 300, 0x32, 50);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    entry = __Open("/t");
    assert(entry->ActualSize == 350);
    __ExpectZeros(entry, 0, 300);
    __ExpectPattern(entry, 300, 0x32, 50);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    other = __Open("/b");
    __ExpectPattern(other, 0, 0xB0, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, other) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();

    // Truncate to a smaller size that is not 0. The buckets stay with the
    // file, and the handle still points into them. The write at the old
    // position must not bring the cut off data back.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "s", 0);
    __WriteAt(entry, 0, 0x41, 3 * TEST_BUCKET_BYTES);
    startBucket = entry->StartBucket;
    allocatedSize = entry->AllocatedSize;
    assert(FsTruncate(&g_mfs, entry, TEST_BUCKET_BYTES + 100) == OS_EOK);
    assert(entry->StartBucket == startBucket && entry->AllocatedSize == allocatedSize);
    for (uint32_t i = 0; i < 3; i++) {
        assert(!__BucketIsFree(startBucket + i));
    }
    __ExpectPattern(entry, 0, 0x41, TEST_BUCKET_BYTES + 100);

    // FsSeek in the check above moved the handle. Back to the old end.
    assert(FsSeek(&g_mfs, entry, 3 * TEST_BUCKET_BYTES, &position) == OS_EOK);
    __Snapshot();
    __WriteHere(entry, 0x42, 10);
    assert(entry->ActualSize == (3 * TEST_BUCKET_BYTES) + 10);
    __ExpectFreeBucketsUnchanged();
    __ExpectPattern(entry, 0, 0x41, TEST_BUCKET_BYTES + 100);
    __ExpectZeros(entry, TEST_BUCKET_BYTES + 100, 3 * TEST_BUCKET_BYTES);
    __ExpectPattern(entry, 3 * TEST_BUCKET_BYTES, 0x42, 10);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();

    // A truncate that gives a file without buckets its first ones, then a
    // read on the same handle. The data bucket of the handle is still
    // MFS_ENDOFCHAIN.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "g", 0);
    assert(FsTruncate(&g_mfs, entry, 1000) == OS_EOK);
    __ExpectZerosHere(entry, 1000);
    __ExpectZeros(entry, 0, 1000);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestWritePastEnd(void)
{
    const uint64_t nextRun = (2 * TEST_BUCKET_BYTES) + 200;
    const uint64_t farRun = (6 * TEST_BUCKET_BYTES) + 300;
    const size_t   longWrite = (3 * TEST_BUCKET_BYTES) + 1000;
    MFSEntry_t*    entry;
    int            steps = 0;
    int            runs = 0;

    // The free list is in holes of one bucket, so each new bucket of the file
    // is its own run, next to a neighbour file.
    __Format();
    g_errors = 0;
    __SplitFreeList(5);
    entry = __CreateEntry(&g_mfs.RootEntry, "w", 0);
    __WriteAt(entry, 0, 0x70, 100);
    assert(entry->StartLength == 1);

    // Past the allocated end, into the run after the next one.
    __Snapshot();
    __WriteAt(entry, nextRun, 0x71, 100);
    __ExpectFreeBucketsUnchanged();
    __ExpectNeighbours();

    // Several runs further.
    __Snapshot();
    __WriteAt(entry, farRun, 0x72, 100);
    __ExpectFreeBucketsUnchanged();
    __ExpectNeighbours();
    assert(entry->ActualSize == farRun + 100);
    __ExpectPattern(entry, 0, 0x70, 100);
    __ExpectZeros(entry, 100, nextRun);
    __ExpectPattern(entry, nextRun, 0x71, 100);
    __ExpectZeros(entry, nextRun + 100, farRun);
    __ExpectPattern(entry, farRun, 0x72, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    entry = __Open("/w");
    __ExpectPattern(entry, nextRun, 0x71, 100);
    __ExpectPattern(entry, farRun, 0x72, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();

    // One write that crosses four runs of one bucket. It starts inside a
    // sector, so it mixes copies through the transfer buffer with transfers
    // straight from the caller's buffer.
    __Format();
    g_errors = 0;
    __SplitFreeList(5);
    entry = __CreateEntry(&g_mfs.RootEntry, "x", 0);
    __Snapshot();
    __WriteAt(entry, 100, 0x73, longWrite);
    for (uint32_t run = entry->StartBucket; run != MFS_ENDOFCHAIN; run = __MapOnDisk()[run * 2]) {
        assert(steps++ < TEST_BUCKETS);
        runs++;
    }
    assert(runs == 4);
    __ExpectFreeBucketsUnchanged();
    __ExpectNeighbours();
    __ExpectZeros(entry, 0, 100);
    __ExpectPattern(entry, 100, 0x73, longWrite);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/x");
    __ExpectPattern(entry, 100, 0x73, longWrite);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestOldData(void)
{
    MFSEntry_t* entry;

    // Six buckets of old data, given back to the free list.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "old", 0);
    __WriteAt(entry, 0, TEST_OLD_SEED, 6 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(__Unlink("/old") == OS_EOK);

    // A sector written in part. The write at 600 leaves the rest of its
    // sector as it was, and the truncate then grows the file over it.
    entry = __CreateEntry(&g_mfs.RootEntry, "part", 0);
    __WriteAt(entry, 0, 0x53, 10);
    __WriteAt(entry, 600, 0x54, 10);
    __ExpectPattern(entry, 0, 0x53, 10);
    __ExpectZeros(entry, 10, 600);
    __ExpectPattern(entry, 600, 0x54, 10);
    assert(FsTruncate(&g_mfs, entry, 1500) == OS_EOK);
    __ExpectZeros(entry, 610, 1500);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/part");
    __ExpectZeros(entry, 10, 600);
    __ExpectZeros(entry, 610, 1500);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // A write that starts past the end of a new file.
    entry = __CreateEntry(&g_mfs.RootEntry, "gap", 0);
    __WriteAt(entry, 5000, 0x51, 100);
    __ExpectZeros(entry, 0, 5000);
    __ExpectPattern(entry, 5000, 0x51, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // A truncate that makes a file larger.
    entry = __CreateEntry(&g_mfs.RootEntry, "grow", 0);
    __WriteAt(entry, 0, 0x52, 100);
    assert(FsTruncate(&g_mfs, entry, (3 * TEST_BUCKET_BYTES) + 50) == OS_EOK);
    __ExpectPattern(entry, 0, 0x52, 100);
    __ExpectZeros(entry, 100, (3 * TEST_BUCKET_BYTES) + 50);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/grow");
    assert(entry->ActualSize == (3 * TEST_BUCKET_BYTES) + 50);
    __ExpectZeros(entry, 100, (3 * TEST_BUCKET_BYTES) + 50);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();
}

// Collects the buckets of the chain that starts at bucket into buckets, and
// returns how many there are. A chain whose free failed may link into the
// free list afterwards, so the buckets of a file are collected before it is
// removed.
static int
__ChainBuckets(
    _In_  uint32_t  bucket,
    _Out_ uint32_t* buckets,
    _In_  int       maxBuckets)
{
    uint32_t* map = __MapOnDisk();
    int       count = 0;

    for (uint32_t run = bucket; run != MFS_ENDOFCHAIN; run = map[run * 2]) {
        for (uint32_t i = run; i < run + map[(run * 2) + 1]; i++) {
            assert(count < maxBuckets);
            buckets[count++] = i;
        }
    }
    return count;
}

static void
__ExpectBucketsNotFree(
    _In_ const uint32_t* buckets,
    _In_ int             count)
{
    for (int i = 0; i < count; i++) {
        if (__BucketIsFree(buckets[i])) {
            __Fail("bucket %u is on the free list", buckets[i]);
        }
    }
}

// Unlinks path with the write of the given sector failing. The unlink must
// return OS_EOK, the record must be gone and the buckets must not be on the
// free list. They are lost.
static void
__ExpectUnlinkWithFailedFree(
    _In_ const char* path,
    _In_ uint64_t    failingSector)
{
    MFSEntry_t* found = __Find(path);
    uint32_t    buckets[TEST_BUCKETS];
    int         count;

    assert(found != NULL);
    count = __ChainBuckets(found->StartBucket, buckets, TEST_BUCKETS);
    assert(count > 0);
    __Free(found);
    g_errors = 0;
    g_failWriteAtSector = failingSector;
    assert(__Unlink(path) == OS_EOK);
    assert(g_failWriteAtSector == UINT64_MAX && g_errors > 0);
    assert(!__Exists(path));
    __ExpectBucketsNotFree(buckets, count);
    __ExpectMapOnDisk();
}

static void
__TestRemoveOrder(void)
{
    MFSEntry_t*   entry;
    MFSEntry_t*   found;
    FileRecord_t* record;
    uint32_t      startBucket;
    uint64_t      recordSector;
    uint32_t      buckets[TEST_BUCKETS];
    int           count;

    __Format();
    entry = __CreateEntry(&g_mfs.RootEntry, "u", 0);
    __WriteAt(entry, 0, 0x61, 2 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    found = __Find("/u");
    startBucket = found->StartBucket;
    count = __ChainBuckets(startBucket, buckets, TEST_BUCKETS);
    recordSector = MFS_GETSECTOR(&g_mfs, found->DirectoryBucket);
    __Free(found);

    // Unlink with the record write failing. Nothing may change.
    g_errors = 0;
    g_failWriteAtSector = recordSector;
    assert(__Unlink("/u") == OS_ENOENT);
    assert(g_failWriteAtSector == UINT64_MAX && g_errors > 0);
    __ExpectBucketsNotFree(buckets, count);
    __ExpectMapOnDisk();
    entry = __Open("/u");
    assert(entry->StartBucket == startBucket && entry->ActualSize == 2 * TEST_BUCKET_BYTES);
    __ExpectPattern(entry, 0, 0x61, 2 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    // Unlink with the bucket map write, and then the master record write, of
    // the free failing.
    __ExpectUnlinkWithFailedFree("/u", TEST_MAP_SECTOR);
    entry = __CreateEntry(&g_mfs.RootEntry, "u2", 0);
    __WriteAt(entry, 0, 0x63, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    __ExpectUnlinkWithFailedFree("/u2", TEST_MIRROR_SECTOR);

    // Truncate to 0 with the record write failing. The handle and the disk
    // keep the file as it was.
    entry = __CreateEntry(&g_mfs.RootEntry, "v", 0);
    __WriteAt(entry, 0, 0x62, 2 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/v");
    startBucket = entry->StartBucket;
    count = __ChainBuckets(startBucket, buckets, TEST_BUCKETS);
    g_errors = 0;
    g_failWriteAtSector = MFS_GETSECTOR(&g_mfs, entry->DirectoryBucket);
    assert(FsTruncate(&g_mfs, entry, 0) == OS_ENOENT);
    assert(g_failWriteAtSector == UINT64_MAX && g_errors > 0);
    assert(entry->StartBucket == startBucket && entry->ActualSize == 2 * TEST_BUCKET_BYTES);
    __ExpectBucketsNotFree(buckets, count);
    __ExpectMapOnDisk();
    __ExpectPattern(entry, 0, 0x62, 2 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/v");
    assert(entry->StartBucket == startBucket && entry->ActualSize == 2 * TEST_BUCKET_BYTES);

    // Truncate to 0 with the free failing. The record on disk is empty, the
    // buckets are lost, and the file can be written again.
    g_errors = 0;
    g_failWriteAtSector = TEST_MAP_SECTOR;
    assert(FsTruncate(&g_mfs, entry, 0) == OS_EOK);
    assert(g_failWriteAtSector == UINT64_MAX && g_errors > 0);
    record = __RecordOnDisk(entry);
    assert(record->StartBucket == MFS_ENDOFCHAIN && record->Size == 0 && record->AllocatedSize == 0);
    __ExpectBucketsNotFree(buckets, count);
    __ExpectMapOnDisk();
    __WriteAt(entry, 0, 0x64, 100);
    assert(entry->StartBucket != startBucket);
    __ExpectPattern(entry, 0, 0x64, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    entry = __Open("/v");
    __ExpectPattern(entry, 0, 0x64, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    g_errors = 0;
    __Unmount();
}

// mfs_runs_test.c includes this file for its helpers, and brings its own
// cases and main.
#ifndef MFS_DATA_TEST_OWN_MAIN
static const struct {
    const char* Name;
    void        (*Run)(void);
} g_cases[] = {
    { "create", __TestCreateAndWrite },
    { "directory", __TestDirectoryBuckets },
    { "truncate", __TestTruncate },
    { "past-end", __TestWritePastEnd },
    { "old-data", __TestOldData },
    { "remove-order", __TestRemoveOrder },
};

// Runs every case, or only the one named on the command line.
int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], g_cases[i].Name)) {
            continue;
        }
        g_cases[i].Run();
        printf("MFS data: %s passed\n", g_cases[i].Name);
    }
    puts("MFS data: create and write, directory buckets, truncate, write past the end, old data and remove order passed");
    return 0;
}
#endif //!MFS_DATA_TEST_OWN_MAIN
