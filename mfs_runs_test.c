// MFS bucket runs: the cost of a request on a long file, the joining of runs
// that lie next to each other on disk, and handles whose copy of a run length
// is shorter than the bucket map. It runs on the in-memory partition of
// mfs_create_test.c, with the helpers of mfs_entries_test.c and
// mfs_data_test.c.
//
// Production code compiled here: modules/filesystems/mfs/main.c, records.c,
// bucket_map.c, utilities.c, file_operations.c and directory_operations.c,
// and the libds mstring code. mfs_runs_test.py compiles every MFS source but
// bucket_map.c with MFSBucketMapGetLengthAndLink renamed to
// TestCountedMapLookup, which counts the call and passes it on. So the count
// holds every map lookup of the file code (seek, read, write, the search for
// the last run), and none of the allocator, whose lookups depend on the free
// list and not on the file.
//
// The image has 256 buckets of 4 KiB (8 sectors of 512 bytes). The map holds
// 64 entries per sector. Buckets 0 to 7 are the root directory.
//
// The cases:
// - walk: a file grows by one bucket per request, 100 requests. With the free
//   list in holes of one bucket each request adds a run, and without holes
//   each request makes the one run longer. Then the file is read in requests
//   of one bucket, and seeked forward one run at a time. The map lookups of
//   each request must stay the same as the file grows, and no request may log
//   a trace line. Seeks back into the first run and into a middle run must
//   land on the right bucket and byte.
// - join-file: MfsEnsureRecordSpace makes the last run longer when the new
//   buckets follow it on disk, for the first run and for a later one, and
//   links a run that does not. StartLength, the record written at close and
//   the data read back must match.
// - join-allocate: MFSBucketMapAllocate hands out free runs that lie next to
//   each other as one run, keeps a gap as two runs, splits the last free run,
//   writes no map entry that already holds its value, and refuses an
//   allocation larger than the free list without writing anything.
// - failures: each disk write of an allocation, and of a write that joins a
//   run, fails in turn. Memory must match the disk afterwards, and no bucket
//   may be in two places (a file, the root directory, the free list).
// - stale-length: a second handle of a file whose run another handle made
//   longer, and a handle whose record has a StartLength shorter than the map.
//   Reads, writes and seeks through it must see every byte of the grown run.
#define MFS_DATA_TEST_OWN_MAIN
#include "mfs_data_test.c"

#define TEST_REQUESTS 100

static int g_mapLookups;

// Every MFS source but bucket_map.c calls this instead of
// MFSBucketMapGetLengthAndLink (see mfs_runs_test.py).
oserr_t
TestCountedMapLookup(
    _In_  FileSystemMFS_t* mfs,
    _In_  bucket_t         bucket,
    _Out_ MapRecord_t*     link)
{
    g_mapLookups++;
    return MFSBucketMapGetLengthAndLink(mfs, bucket, link);
}

// Replaces the free list on disk and in memory. runs holds the first bucket
// and the length of each free run, in list order. The map entries of the
// buckets after the root are cleared first, so a bucket outside the list is
// in no file and not free.
static void
__SetFreeList(
    _In_ const uint32_t* runs,
    _In_ int             runCount)
{
    uint32_t* map = __MapOnDisk();
    uint32_t  head = runCount > 0 ? runs[0] : MFS_ENDOFCHAIN;

    memset(&map[MFS_ROOTSIZE * 2], 0, (TEST_BUCKETS - MFS_ROOTSIZE) * sizeof(MapRecord_t));
    for (int i = 0; i < runCount; i++) {
        uint32_t bucket = runs[i * 2];

        map[bucket * 2] = (i + 1 < runCount) ? runs[(i + 1) * 2] : MFS_ENDOFCHAIN;
        map[(bucket * 2) + 1] = runs[(i * 2) + 1];
    }
    __MasterOnDisk()->FreeBucket = head;
    ((MasterRecord_t*)&g_image[TEST_MIRROR_SECTOR * TEST_SECTOR_SIZE])->FreeBucket = head;
    g_mfs.MasterRecord.FreeBucket = head;
    memcpy(g_mfs.BucketMap, map, TEST_BUCKETS * sizeof(MapRecord_t));
}

// Marks the buckets of the chain that starts at bucket, following the map on
// disk. A bucket that is already marked is in two places.
static void
__MarkChain(
    _In_ uint8_t*    marks,
    _In_ uint32_t    bucket,
    _In_ const char* what)
{
    uint32_t* map = __MapOnDisk();
    int       steps = 0;

    for (uint32_t run = bucket; run != MFS_ENDOFCHAIN; run = map[run * 2]) {
        if (steps++ >= TEST_BUCKETS || run >= TEST_BUCKETS) {
            __Fail("the chain of %s does not end (run %u)", what, run);
        }
        for (uint32_t i = run; i < run + map[(run * 2) + 1]; i++) {
            if (i >= TEST_BUCKETS) {
                __Fail("run %u of %s goes past the disk", run, what);
            }
            if (marks[i]) {
                __Fail("bucket %u of %s is also in another chain or on the free list", i, what);
            }
            marks[i] = 1;
        }
    }
}

// Memory matches the disk, and no bucket is in two of: the root directory,
// the chains of the given handles, the free list on disk. With wasFree given,
// the free list holds only buckets that were free before.
static void
__ExpectNoBucketTwice(
    _In_ MFSEntry_t**   files,
    _In_ int            fileCount,
    _In_ const uint8_t* wasFree)
{
    uint8_t freeMarks[TEST_BUCKETS] = { 0 };
    uint8_t marks[TEST_BUCKETS] = { 0 };

    __ExpectMapOnDisk();
    __MarkChain(marks, 0, "the root directory");
    for (int i = 0; i < fileCount; i++) {
        if (files[i]->StartBucket != MFS_ENDOFCHAIN) {
            __MarkChain(marks, files[i]->StartBucket, "a file");
        }
    }
    __MarkChain(freeMarks, __MasterOnDisk()->FreeBucket, "the free list");
    for (uint32_t i = 0; i < TEST_BUCKETS; i++) {
        if (freeMarks[i] && marks[i]) {
            __Fail("bucket %u is on the free list and in use", i);
        }
        if (freeMarks[i] && wasFree != NULL && !wasFree[i]) {
            __Fail("bucket %u is on the free list, and was not free before", i);
        }
    }
}

// The buckets of the free list on disk, as marks.
static void
__FreeMarks(
    _Out_ uint8_t* marks)
{
    memset(marks, 0, TEST_BUCKETS);
    __MarkChain(marks, __MasterOnDisk()->FreeBucket, "the free list");
}

// The number of runs of the chain that starts at bucket, on disk.
static int
__RunCount(
    _In_ uint32_t bucket)
{
    int runs = 0;

    for (uint32_t run = bucket; run != MFS_ENDOFCHAIN; run = __MapOnDisk()[run * 2]) {
        assert(runs < TEST_BUCKETS);
        runs++;
    }
    return runs;
}

static void
__ExpectMapEntry(
    _In_ uint32_t bucket,
    _In_ uint32_t link,
    _In_ uint32_t length)
{
    uint32_t* map = __MapOnDisk();

    if (map[bucket * 2] != link || map[(bucket * 2) + 1] != length) {
        __Fail("map entry of bucket %u is link %u, length %u on disk, expected link %u, length %u",
               bucket, map[bucket * 2], map[(bucket * 2) + 1], link, length);
    }
}

// Every request of the file of the walk case is one bucket written with the
// request number as seed. Checks length bytes at position, read with a seek.
static void
__ExpectRequestBytes(
    _In_ MFSEntry_t* entry,
    _In_ uint64_t    position,
    _In_ size_t      length)
{
    __ReadAt(entry, position, length);
    for (size_t i = 0; i < length; i++) {
        uint64_t offset = position + i;
        uint8_t  expected = __PatternByte((uint8_t)(offset / TEST_BUCKET_BYTES), offset % TEST_BUCKET_BYTES);

        if (g_userBuffer[i] != expected) {
            __Fail("byte %llu is 0x%02x, expected 0x%02x", (unsigned long long)offset, g_userBuffer[i], expected);
        }
    }
}

// All counts from first to last must be the same, and at most limit.
static void
__ExpectSameCount(
    _In_ const char* what,
    _In_ const int*  counts,
    _In_ int         first,
    _In_ int         last,
    _In_ int         limit)
{
    for (int i = first; i <= last; i++) {
        if (counts[i] != counts[first] || counts[i] > limit) {
            __Fail("%s %d made %d map lookups, %s %d made %d (at most %d expected)",
                   what, i, counts[i], what, first, counts[first], limit);
        }
    }
}

// Grows "big" by one bucket per request. Returns with the handle closed.
static void
__AppendRequests(
    _In_ int limit)
{
    MFSEntry_t* entry = __CreateEntry(&g_mfs.RootEntry, "big", 0);
    int         lookups[TEST_REQUESTS];

    g_traces = 0;
    for (int i = 0; i < TEST_REQUESTS; i++) {
        g_mapLookups = 0;
        __WriteHere(entry, (uint8_t)i, TEST_BUCKET_BYTES);
        lookups[i] = g_mapLookups;
    }
    if (g_traces != 0) {
        __Fail("the writes logged %d trace lines", g_traces);
    }

    // The first request gives the file its first run, without a walk.
    __ExpectSameCount("write request", lookups, 1, TEST_REQUESTS - 1, limit);
    assert(entry->ActualSize == (uint64_t)TEST_REQUESTS * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
}

static void
__TestWalk(void)
{
    uint32_t    holes[TEST_REQUESTS * 2];
    int         lookups[TEST_REQUESTS];
    MFSEntry_t* entry;
    uint64_t    position;
    size_t      count;

    // A free list of holes of one bucket: 8, 10, 12 and on. Each request adds
    // a run to the file.
    __Format();
    g_errors = 0;
    for (int i = 0; i < TEST_REQUESTS; i++) {
        holes[i * 2] = MFS_ROOTSIZE + (2 * i);
        holes[(i * 2) + 1] = 1;
    }
    __SetFreeList(holes, TEST_REQUESTS);
    __AppendRequests(4);

    entry = __Open("/big");
    assert(__RunCount(entry->StartBucket) == TEST_REQUESTS);

    // Sequential reads of one bucket each, without a seek. Each read steps to
    // the next run at its end, but the last one, which has no next run.
    g_traces = 0;
    for (int i = 0; i < TEST_REQUESTS; i++) {
        g_mapLookups = 0;
        memset(g_userBuffer, 0x5A, TEST_BUCKET_BYTES);
        assert(FsRead(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, TEST_BUCKET_BYTES, &count) == OS_EOK);
        assert(count == TEST_BUCKET_BYTES);
        lookups[i] = g_mapLookups;
        for (size_t j = 0; j < TEST_BUCKET_BYTES; j++) {
            if (g_userBuffer[j] != __PatternByte((uint8_t)i, j)) {
                __Fail("read request %d has 0x%02x at byte %zu", i, g_userBuffer[j], j);
            }
        }
    }
    __ExpectSameCount("read request", lookups, 0, TEST_REQUESTS - 2, 2);

    // Forward seeks, one run at a time.
    assert(FsSeek(&g_mfs, entry, 0, &position) == OS_EOK);
    for (int i = 1; i < TEST_REQUESTS; i++) {
        g_mapLookups = 0;
        assert(FsSeek(&g_mfs, entry, ((uint64_t)i * TEST_BUCKET_BYTES) + 100, &position) == OS_EOK);
        lookups[i] = g_mapLookups;
        assert(entry->DataBucketPosition == MFS_ROOTSIZE + (2 * (uint32_t)i));
        assert(entry->BucketByteBoundary == (uint64_t)i * TEST_BUCKET_BYTES);
    }
    __ExpectSameCount("forward seek", lookups, 1, TEST_REQUESTS - 1, 2);
    if (g_traces != 0) {
        __Fail("the reads and seeks logged %d trace lines", g_traces);
    }

    // Back into the first run, and into a run in the middle.
    assert(FsSeek(&g_mfs, entry, 50, &position) == OS_EOK && position == 50);
    assert(entry->DataBucketPosition == MFS_ROOTSIZE && entry->BucketByteBoundary == 0);
    __ExpectRequestBytes(entry, 50, 10);
    assert(FsSeek(&g_mfs, entry, (TEST_REQUESTS - 1) * TEST_BUCKET_BYTES, &position) == OS_EOK);
    assert(FsSeek(&g_mfs, entry, (37 * TEST_BUCKET_BYTES) + 7, &position) == OS_EOK);
    assert(entry->DataBucketPosition == MFS_ROOTSIZE + 74 && entry->BucketByteBoundary == 37 * TEST_BUCKET_BYTES);
    __ExpectRequestBytes(entry, (37 * TEST_BUCKET_BYTES) + 7, 20);
    __ExpectRequestBytes(entry, (36 * TEST_BUCKET_BYTES) + 4000, 200);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    __ExpectNoBucketTwice(NULL, 0, NULL);
    assert(g_errors == 0);
    __Unmount();

    // One free run. Each request makes the run of the file longer.
    __Format();
    g_errors = 0;
    __AppendRequests(2);
    entry = __Open("/big");
    assert(__RunCount(entry->StartBucket) == 1 && entry->StartLength == TEST_REQUESTS);
    __ExpectRequestBytes(entry, 0, 3 * TEST_BUCKET_BYTES);
    __ExpectRequestBytes(entry, (TEST_REQUESTS - 3) * TEST_BUCKET_BYTES, 3 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    __ExpectNoBucketTwice(NULL, 0, NULL);
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestJoinFile(void)
{
    MFSEntry_t*   entry;
    MFSEntry_t*   neighbour;
    FileRecord_t* record;
    uint32_t      first;

    // The first run grows: 1 bucket, then 2, then 5.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "a", 0);
    __WriteAt(entry, 0, 0x11, 100);
    first = entry->StartBucket;
    assert(entry->StartLength == 1);
    __WriteAt(entry, TEST_BUCKET_BYTES, 0x12, 100);
    if (entry->StartLength != 2 || entry->DataBucketLength != 2) {
        __Fail("after a neighbouring extension StartLength is %u and DataBucketLength %u, expected 2",
               entry->StartLength, entry->DataBucketLength);
    }
    __ExpectMapEntry(first, MFS_ENDOFCHAIN, 2);
    __WriteAt(entry, 2 * TEST_BUCKET_BYTES, 0x13, 3 * TEST_BUCKET_BYTES);
    assert(entry->StartLength == 5 && entry->DataBucketLength == 5);
    assert(entry->AllocatedSize == 5 * TEST_BUCKET_BYTES);
    __ExpectMapEntry(first, MFS_ENDOFCHAIN, 5);
    __ExpectNoBucketTwice(&entry, 1, NULL);
    assert(FsClose(&g_mfs, entry) == OS_EOK);

    entry = __Open("/a");
    record = __RecordOnDisk(entry);
    if (record->StartBucket != first || record->StartLength != 5) {
        __Fail("the record has start %u, length %u, expected %u, 5", record->StartBucket, record->StartLength, first);
    }
    assert(record->Size == 5 * TEST_BUCKET_BYTES && record->AllocatedSize == 5 * TEST_BUCKET_BYTES);
    __ExpectPattern(entry, 0, 0x11, 100);
    __ExpectZeros(entry, 100, TEST_BUCKET_BYTES);
    __ExpectPattern(entry, TEST_BUCKET_BYTES, 0x12, 100);
    __ExpectZeros(entry, TEST_BUCKET_BYTES + 100, 2 * TEST_BUCKET_BYTES);
    __ExpectPattern(entry, 2 * TEST_BUCKET_BYTES, 0x13, 3 * TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();

    // A later run grows. "n" takes the bucket after the first run of "b", so
    // the second bucket of "b" is linked as a run of its own.
    __Format();
    g_errors = 0;
    entry = __CreateEntry(&g_mfs.RootEntry, "b", 0);
    __WriteAt(entry, 0, 0x21, TEST_BUCKET_BYTES);
    first = entry->StartBucket;
    neighbour = __CreateEntry(&g_mfs.RootEntry, "n", 0);
    __WriteAt(neighbour, 0, 0x2F, TEST_BUCKET_BYTES);
    assert(neighbour->StartBucket == first + 1);
    __WriteAt(entry, TEST_BUCKET_BYTES, 0x22, TEST_BUCKET_BYTES);
    __ExpectMapEntry(first, first + 2, 1);
    __ExpectMapEntry(first + 2, MFS_ENDOFCHAIN, 1);
    __WriteAt(entry, 2 * TEST_BUCKET_BYTES, 0x23, 100);
    __ExpectMapEntry(first, first + 2, 1);
    __ExpectMapEntry(first + 2, MFS_ENDOFCHAIN, 2);
    assert(entry->StartLength == 1);
    assert(entry->DataBucketPosition == first + 2 && entry->DataBucketLength == 2);
    assert(entry->BucketByteBoundary == TEST_BUCKET_BYTES);
    {
        MFSEntry_t* files[] = { entry, neighbour };
        __ExpectNoBucketTwice(files, 2, NULL);
    }
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);

    entry = __Open("/b");
    assert(__RecordOnDisk(entry)->StartLength == 1);
    __ExpectPattern(entry, 0, 0x21, TEST_BUCKET_BYTES);
    __ExpectPattern(entry, TEST_BUCKET_BYTES, 0x22, TEST_BUCKET_BYTES);
    __ExpectPattern(entry, 2 * TEST_BUCKET_BYTES, 0x23, 100);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    neighbour = __Open("/n");
    __ExpectPattern(neighbour, 0, 0x2F, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);
    assert(g_errors == 0);
    __Unmount();
}

// Allocates count buckets from the free list runs, and checks the result and
// the number of disk writes. expectedChain holds the first bucket and the
// length of each run of the chain, expectedFree the free list head after it.
static void
__ExpectAllocation(
    _In_ const uint32_t* runs,
    _In_ int             runCount,
    _In_ size_t          count,
    _In_ const uint32_t* expectedChain,
    _In_ int             chainRuns,
    _In_ uint32_t        expectedFree,
    _In_ int             expectedWrites)
{
    MapRecord_t record;
    uint8_t     wasFree[TEST_BUCKETS];
    uint8_t     chain[TEST_BUCKETS] = { 0 };
    oserr_t     oserr;

    __Format();
    g_errors = 0;
    __SetFreeList(runs, runCount);
    __FreeMarks(wasFree);
    g_writes = 0;
    oserr = MFSBucketMapAllocate(&g_mfs, count, &record);
    if (oserr != OS_EOK) {
        __Fail("MFSBucketMapAllocate of %zu buckets returned %u", count, oserr);
    }
    if (record.Link != expectedChain[0] || record.Length != expectedChain[1]) {
        __Fail("MFSBucketMapAllocate returned run %u of length %u, expected %u of length %u",
               record.Link, record.Length, expectedChain[0], expectedChain[1]);
    }
    for (int i = 0; i < chainRuns; i++) {
        __ExpectMapEntry(expectedChain[i * 2],
                         (i + 1 < chainRuns) ? expectedChain[(i + 1) * 2] : MFS_ENDOFCHAIN,
                         expectedChain[(i * 2) + 1]);
    }
    if (__MasterOnDisk()->FreeBucket != expectedFree) {
        __Fail("the free list head is %u, expected %u", __MasterOnDisk()->FreeBucket, expectedFree);
    }
    if (g_writes != expectedWrites) {
        __Fail("the allocation made %d disk writes, expected %d", g_writes, expectedWrites);
    }

    // The chain and the free list share no bucket, and together they hold
    // the buckets that were free.
    __ExpectNoBucketTwice(NULL, 0, wasFree);
    __MarkChain(chain, record.Link, "the allocation");
    for (uint32_t i = 0; i < TEST_BUCKETS; i++) {
        if (chain[i] && !wasFree[i]) {
            __Fail("bucket %u was handed out but was not free", i);
        }
    }
    assert(g_errors == 0);
    __Unmount();
}

static void
__TestJoinAllocate(void)
{
    // Four neighbouring runs of one bucket and the rest of the disk after
    // them. One run of 4, written once, and the master record twice.
    const uint32_t neighbours[] = { 8, 1, 9, 1, 10, 1, 11, 1, 12, 244 };
    const uint32_t neighboursChain[] = { 8, 4 };
    // A gap after 9.
    const uint32_t gap[] = { 8, 1, 9, 1, 11, 1, 12, 1, 20, 236 };
    const uint32_t gapChain[] = { 8, 2, 11, 2 };
    // The last free run is split, and its first bucket joins the run before.
    const uint32_t split[] = { 8, 1, 9, 3, 20, 236 };
    const uint32_t splitChain[] = { 8, 2 };
    // Free runs that are not neighbours are handed out as they are. Only the
    // split last run and its rest are written, both in map sector 0.
    const uint32_t apart[] = { 8, 1, 11, 1, 20, 236 };
    const uint32_t apartChain[] = { 8, 1, 11, 1, 20, 1 };
    const uint32_t tooShort[] = { 8, 1, 10, 1 };
    MapRecord_t    record;
    uint8_t        mapBefore[TEST_BUCKETS * sizeof(MapRecord_t)];
    MasterRecord_t masterBefore;

    __ExpectAllocation(neighbours, 5, 4, neighboursChain, 1, 12, 3);
    __ExpectAllocation(gap, 5, 4, gapChain, 2, 20, 4);
    __ExpectAllocation(split, 3, 2, splitChain, 1, 10, 4);
    __ExpectAllocation(apart, 3, 3, apartChain, 3, 21, 4);

    // More buckets than the free list holds: nothing changes.
    __Format();
    g_errors = 0;
    __SetFreeList(tooShort, 2);
    memcpy(mapBefore, __MapOnDisk(), sizeof(mapBefore));
    memcpy(&masterBefore, __MasterOnDisk(), sizeof(masterBefore));
    g_writes = 0;
    if (MFSBucketMapAllocate(&g_mfs, 3, &record) != OS_EINCOMPLETE) {
        __Fail("an allocation larger than the free list did not return OS_EINCOMPLETE");
    }
    assert(g_writes == 0);
    assert(!memcmp(mapBefore, __MapOnDisk(), sizeof(mapBefore)));
    assert(!memcmp(&masterBefore, __MasterOnDisk(), sizeof(masterBefore)));
    __ExpectMapOnDisk();
    __Unmount();
}

// Fails each disk write of an allocation from the free list runs in turn.
static void
__ExpectAllocationFailures(
    _In_ const uint32_t* runs,
    _In_ int             runCount,
    _In_ size_t          count)
{
    MapRecord_t record;
    uint8_t     wasFree[TEST_BUCKETS];
    int         writes;

    __Format();
    __SetFreeList(runs, runCount);
    g_writes = 0;
    assert(MFSBucketMapAllocate(&g_mfs, count, &record) == OS_EOK);
    writes = g_writes;
    __Unmount();

    for (int i = 1; i <= writes; i++) {
        __Format();
        g_errors = 0;
        __SetFreeList(runs, runCount);
        __FreeMarks(wasFree);
        g_failOnlyWrite = i;
        if (MFSBucketMapAllocate(&g_mfs, count, &record) == OS_EOK) {
            __Fail("the allocation did not fail with its write %d failing", i);
        }
        assert(g_failOnlyWrite == 0 && g_errors > 0);
        __ExpectNoBucketTwice(NULL, 0, wasFree);
        __Unmount();
    }
}

static void
__TestFailures(void)
{
    const uint32_t neighbours[] = { 8, 1, 9, 1, 10, 1, 11, 1, 12, 244 };
    const uint32_t gap[] = { 8, 1, 9, 1, 11, 1, 12, 1, 20, 236 };
    const uint32_t split[] = { 8, 1, 9, 3, 20, 236 };
    const uint32_t apart[] = { 8, 1, 11, 1, 20, 236 };
    const uint32_t farSplit[] = { 8, 1, 9, 1, 100, 100 };
    MFSEntry_t*    entry;
    size_t         count;
    uint32_t       first;
    int            writes;

    __ExpectAllocationFailures(neighbours, 5, 4);
    __ExpectAllocationFailures(gap, 5, 4);
    __ExpectAllocationFailures(split, 3, 2);
    __ExpectAllocationFailures(apart, 3, 3);
    // The chain and the split rest in different map sectors.
    __ExpectAllocationFailures(farSplit, 3, 4);

    // A write that makes the run of a file longer: the allocation, the map
    // write that joins the run, and the data.
    __Format();
    entry = __CreateEntry(&g_mfs.RootEntry, "j", 0);
    __WriteAt(entry, 0, 0x31, 100);
    g_writes = 0;
    __WriteAt(entry, TEST_BUCKET_BYTES, 0x32, 100);
    writes = g_writes;
    assert(entry->StartLength == 2);
    assert(FsClose(&g_mfs, entry) == OS_EOK);
    __Unmount();

    for (int i = 1; i <= writes; i++) {
        uint64_t position;

        __Format();
        entry = __CreateEntry(&g_mfs.RootEntry, "j", 0);
        __WriteAt(entry, 0, 0x31, 100);
        first = entry->StartBucket;
        assert(FsSeek(&g_mfs, entry, TEST_BUCKET_BYTES, &position) == OS_EOK);
        memset(g_userBuffer, 0x32, 100);
        g_errors = 0;
        g_failOnlyWrite = i;
        if (FsWrite(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, 100, &count) == OS_EOK) {
            __Fail("the write did not fail with its write %d failing", i);
        }
        assert(g_failOnlyWrite == 0 && g_errors > 0);
        __ExpectNoBucketTwice(&entry, 1, NULL);

        // Before the join is on disk the file keeps its one bucket. After it
        // the handle has the longer run.
        if (__MapOnDisk()[(first * 2) + 1] == 1) {
            assert(entry->StartLength == 1 && entry->AllocatedSize == TEST_BUCKET_BYTES);
        } else {
            assert(__MapOnDisk()[(first * 2) + 1] == 2 && entry->StartLength == 2);
        }
        __ExpectPattern(entry, 0, 0x31, 100);
        assert(FsClose(&g_mfs, entry) == OS_EOK);
        __ExpectNoBucketTwice(NULL, 0, NULL);
        __Unmount();
    }
}

// Reads count buckets from position 0 in one request, without a seek. Bucket
// k of the file must hold the pattern of seeds[k].
static void
__ExpectBucketSeedsHere(
    _In_ MFSEntry_t*    entry,
    _In_ const uint8_t* seeds,
    _In_ int            buckets)
{
    size_t length = (size_t)buckets * TEST_BUCKET_BYTES;
    size_t count;

    memset(g_userBuffer, 0x5A, length);
    assert(entry->Position == 0);
    assert(FsRead(&g_mfs, entry, TEST_USER_BUFFER_ID, g_userBuffer, 0, length, &count) == OS_EOK);
    if (count != length) {
        __Fail("a read of %zu bytes over the grown run returned %zu bytes", length, count);
    }
    for (size_t i = 0; i < length; i++) {
        uint8_t expected = __PatternByte(seeds[i / TEST_BUCKET_BYTES], i % TEST_BUCKET_BYTES);

        if (g_userBuffer[i] != expected) {
            __Fail("byte %zu is 0x%02x, expected 0x%02x", i, g_userBuffer[i], expected);
        }
    }
}

// Opens path, and sets the StartLength of its record on disk to 1, as a crash
// leaves it when the run grew but the record was not written.
static MFSEntry_t*
__OpenWithShortRecord(
    _In_ const char* path)
{
    MFSEntry_t* found = __Find(path);
    MFSEntry_t* entry;

    assert(found != NULL);
    __RecordOnDisk(found)->StartLength = 1;
    __Free(found);
    entry = __Open(path);
    assert(entry->StartLength == 1 && entry->DataBucketLength == 1);
    return entry;
}

static void
__TestStaleLength(void)
{
    const uint8_t seeds[] = { 0x41, 0x42, 0x43 };
    const uint8_t newSeeds[] = { 0x51, 0x52, 0x53 };
    MFSEntry_t*   writer;
    MFSEntry_t*   reader;
    MFSEntry_t*   neighbour;
    uint64_t      position;
    uint32_t      first;

    // A second handle. "f" has bucket 8, "h" 9 and "g" 10. "h" is removed,
    // so the free list is 9, then 11 on. The handle "reader" is opened while
    // "f" has one bucket. "writer" grows the run to 8 and 9. Then "reader"
    // writes two buckets and 100 bytes from 0. The run has grown under it,
    // and the bucket after it (11) is no neighbour, so the write starts with
    // the old length and must take the new one at the old end.
    __Format();
    g_errors = 0;
    writer = __CreateEntry(&g_mfs.RootEntry, "f", 0);
    __WriteAt(writer, 0, 0x61, TEST_BUCKET_BYTES);
    first = writer->StartBucket;
    assert(FsClose(&g_mfs, writer) == OS_EOK);
    writer = __CreateEntry(&g_mfs.RootEntry, "h", 0);
    __WriteAt(writer, 0, 0x62, TEST_BUCKET_BYTES);
    assert(writer->StartBucket == first + 1);
    assert(FsClose(&g_mfs, writer) == OS_EOK);
    neighbour = __CreateEntry(&g_mfs.RootEntry, "g", 0);
    __WriteAt(neighbour, 0, 0x63, TEST_BUCKET_BYTES);
    assert(neighbour->StartBucket == first + 2);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);
    assert(__Unlink("/h") == OS_EOK);
    assert(g_mfs.MasterRecord.FreeBucket == first + 1);

    reader = __Open("/f");
    writer = __Open("/f");
    __WriteAt(writer, TEST_BUCKET_BYTES, 0x64, 100);
    __ExpectMapEntry(first, MFS_ENDOFCHAIN, 2);
    assert(FsClose(&g_mfs, writer) == OS_EOK);
    assert(reader->DataBucketLength == 1 && reader->Position == 0);

    __WriteHere(reader, 0x65, (2 * TEST_BUCKET_BYTES) + 100);
    assert(reader->StartLength == 2);
    __ExpectMapEntry(first, first + 3, 2);
    for (size_t i = 0; i < TEST_BUCKET_BYTES; i++) {
        if (__Bucket(first + 1)[i] != __PatternByte(0x65, TEST_BUCKET_BYTES + i)) {
            __Fail("the second bucket of the grown run has 0x%02x at byte %zu", __Bucket(first + 1)[i], i);
        }
    }
    __ExpectPattern(reader, 0, 0x65, (2 * TEST_BUCKET_BYTES) + 100);
    assert(FsClose(&g_mfs, reader) == OS_EOK);
    neighbour = __Open("/g");
    __ExpectPattern(neighbour, 0, 0x63, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);
    __ExpectNoBucketTwice(NULL, 0, NULL);
    assert(g_errors == 0);
    __Unmount();

    // A record with a StartLength shorter than the map. "s" has a first run
    // of two buckets (joined), and a third bucket in a run of its own, as
    // "n" took the bucket in between.
    __Format();
    g_errors = 0;
    writer = __CreateEntry(&g_mfs.RootEntry, "s", 0);
    __WriteAt(writer, 0, seeds[0], TEST_BUCKET_BYTES);
    __WriteAt(writer, TEST_BUCKET_BYTES, seeds[1], TEST_BUCKET_BYTES);
    first = writer->StartBucket;
    neighbour = __CreateEntry(&g_mfs.RootEntry, "n", 0);
    __WriteAt(neighbour, 0, 0x6F, TEST_BUCKET_BYTES);
    assert(neighbour->StartBucket == first + 2);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);
    __WriteAt(writer, 2 * TEST_BUCKET_BYTES, seeds[2], TEST_BUCKET_BYTES);
    __ExpectMapEntry(first, first + 3, 2);
    assert(FsClose(&g_mfs, writer) == OS_EOK);

    // A read over the whole file. The handle takes the length of the map.
    reader = __OpenWithShortRecord("/s");
    __ExpectBucketSeedsHere(reader, seeds, 3);
    assert(reader->StartLength == 2);
    assert(FsClose(&g_mfs, reader) == OS_EOK);

    // A seek into the second bucket of the first run.
    reader = __OpenWithShortRecord("/s");
    assert(FsSeek(&g_mfs, reader, TEST_BUCKET_BYTES + 10, &position) == OS_EOK);
    if (reader->DataBucketPosition != first || reader->BucketByteBoundary != 0) {
        __Fail("a seek to %u landed in bucket %u at %llu, expected bucket %u at 0",
               TEST_BUCKET_BYTES + 10, reader->DataBucketPosition,
               (unsigned long long)reader->BucketByteBoundary, first);
    }
    assert(reader->DataBucketLength == 2 && reader->StartLength == 2);
    __ExpectPattern(reader, TEST_BUCKET_BYTES, seeds[1], 100);
    assert(FsClose(&g_mfs, reader) == OS_EOK);

    // A write over the whole file, within its allocated size.
    reader = __OpenWithShortRecord("/s");
    __WriteHere(reader, newSeeds[0], TEST_BUCKET_BYTES);
    __WriteHere(reader, newSeeds[1], TEST_BUCKET_BYTES);
    __WriteHere(reader, newSeeds[2], TEST_BUCKET_BYTES);
    assert(reader->AllocatedSize == 3 * TEST_BUCKET_BYTES);
    assert(FsSeek(&g_mfs, reader, 0, &position) == OS_EOK);
    __ExpectBucketSeedsHere(reader, newSeeds, 3);
    assert(FsClose(&g_mfs, reader) == OS_EOK);
    neighbour = __Open("/n");
    __ExpectPattern(neighbour, 0, 0x6F, TEST_BUCKET_BYTES);
    assert(FsClose(&g_mfs, neighbour) == OS_EOK);
    __ExpectNoBucketTwice(NULL, 0, NULL);
    assert(g_errors == 0);
    __Unmount();
}

static const struct {
    const char* Name;
    void        (*Run)(void);
} g_runCases[] = {
    { "walk", __TestWalk },
    { "join-file", __TestJoinFile },
    { "join-allocate", __TestJoinAllocate },
    { "failures", __TestFailures },
    { "stale-length", __TestStaleLength },
};

// Runs every case, or only the one named on the command line.
int
main(
    _In_ int    argc,
    _In_ char** argv)
{
    for (size_t i = 0; i < sizeof(g_runCases) / sizeof(g_runCases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], g_runCases[i].Name)) {
            continue;
        }
        g_runCases[i].Run();
        printf("MFS runs: %s passed\n", g_runCases[i].Name);
    }
    puts("MFS runs: walk cost, joined runs, allocation, failed writes and stale run lengths passed");
    return 0;
}
