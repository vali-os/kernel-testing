// Checks the results filed sends for sys_storage register and unregister, and
// that disks are found by driver ID and device ID together. The handlers and
// the disk list are the production code in storage.c. Filesystem, VFS and
// driver calls are recording doubles.
//
// Every register and unregister request must get exactly one result. The
// register result must be sent before the stat query goes back to the
// driver, and the unregister result before the first filesystem is unmounted.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FILED_STORAGE_SOURCE
#define FILED_STORAGE_SOURCE "../services/filed/storage/storage.c"
#endif
#include FILED_STORAGE_SOURCE

// Driver IDs and device IDs used by the test must stay below these values.
#define TEST_MAX_DRIVERS 4
#define TEST_MAX_DEVICES 16

// Two drivers that both number their disks from 0, as AHCI does.
#define TEST_DRIVER_A 1
#define TEST_DRIVER_B 2

static struct VFSStorageOperations g_operations = { 0 };
static char g_path[64];
static int  g_errors;
static int  g_warnings;

// Per disk counters, indexed by driver ID and device ID.
static int g_statQueries[TEST_MAX_DRIVERS][TEST_MAX_DEVICES];
static int g_unmounts[TEST_MAX_DRIVERS][TEST_MAX_DEVICES];
static int g_destroys[TEST_MAX_DRIVERS][TEST_MAX_DEVICES];

// Results filed sent. g_awaitingRegister and g_awaitingUnregister are set
// while a request is waiting for its result. The stat and unmount doubles
// check that the result was sent before they run.
static int     g_registerResults;
static int     g_unregisterResults;
static oserr_t g_lastRegisterResult;
static oserr_t g_lastUnregisterResult;
static bool    g_awaitingRegister;
static bool    g_awaitingUnregister;
static int     g_failResponseSend;

// Failure switches for the doubles.
static bool    g_failCreate;
static oserr_t g_statResult = OS_EOK;

// Runs once in the stat query, which is where the real query waits for IPC.
static void (*g_statHook)(void);

void
SystemDebug(
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
        printf("log(%i): %s\n", level, format);
    }
}

int
sys_storage_register_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result)
{
    assert(g_awaitingRegister);
    g_awaitingRegister = false;
    g_lastRegisterResult = result;
    g_registerResults++;
    if (g_failResponseSend > 0) {
        g_failResponseSend--;
        return -1;
    }
    return 0;
}

int
sys_storage_unregister_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result)
{
    assert(g_awaitingUnregister);
    g_awaitingUnregister = false;
    g_lastUnregisterResult = result;
    g_unregisterResults++;
    if (g_failResponseSend > 0) {
        g_failResponseSend--;
        return -1;
    }
    return 0;
}

// The test is single threaded, so locks only need to exist.
void spinlock_init(spinlock_t* lock) { (void)lock; }
void spinlock_acquire(spinlock_t* lock) { (void)lock; }
void spinlock_release(spinlock_t* lock) { (void)lock; }
void usched_mtx_init(struct usched_mtx* mutex, int type) { memset(mutex, 0, sizeof(*mutex)); }
void usched_mtx_lock(struct usched_mtx* mutex) { assert(mutex->references++ == 0); }
void usched_mtx_unlock(struct usched_mtx* mutex) { assert(--mutex->references == 0); }

mstring_t* mstr_fmt(const char* fmt, ...) { return (mstring_t*)g_path; }
void mstr_delete(mstring_t* string) { assert(string == NULL || string == (mstring_t*)g_path); }
char* mstr_u8(mstring_t* string) { assert(string == (mstring_t*)g_path); return strdup(g_path); }
struct VFS* VFSScopeGet(uuid_t processId) { return NULL; }
oserr_t VFSNodeNewDirectory(struct VFS* vfs, mstring_t* path, uint32_t permissions, struct VFSNode** nodeOut)
{
    return OS_EOK;
}
oserr_t VFSNodeUnlink(struct VFS* vfs, const char* path) { return OS_EOK; }
uuid_t VFSIdentifierAllocate(struct VFSStorage* storage) { return 0; }
void VFSIdentifierFree(struct VFSStorage* storage, uuid_t id) { }

// VFSStorageRegisterAndSetupPartition is not used by this test, but it is in
// the same file and needs these symbols to link.
const char* FileSystemParseGuid(guid_t* guid) { abort(); }
oserr_t VFSInterfaceLoadInternal(const char* type, struct VFSInterface** interfaceOut) { abort(); }
oserr_t VFSInterfaceLoadDriver(uuid_t driverID, struct VFSInterface** interfaceOut) { abort(); }
oserr_t VFSFileSystemConnectInterface(FileSystem_t* fileSystem, struct VFSInterface* interface) { abort(); }
oserr_t VFSFileSystemMount(FileSystem_t* fileSystem, mstring_t* mountPoint) { abort(); }

static uuid_t
__DriverOf(
    _In_ struct VFSStorage* storage)
{
    uuid_t driverID = storage->Protocol.Storage.Device.DriverID;
    assert(driverID < TEST_MAX_DRIVERS);
    return driverID;
}

static uuid_t
__DeviceOf(
    _In_ struct VFSStorage* storage)
{
    uuid_t deviceID = storage->Protocol.Storage.Device.DeviceID;
    assert(deviceID < TEST_MAX_DEVICES);
    return deviceID;
}

// Same element setup as the real FileSystemNew. The filesystem starts out
// mounted so unregister has to walk the full unmount path.
FileSystem_t*
FileSystemNew(
    _In_ struct VFSStorage* storage,
    _In_ int                partitionIndex,
    _In_ UInteger64_t*      sector,
    _In_ uuid_t             id,
    _In_ guid_t*            guid)
{
    FileSystem_t* fileSystem = calloc(1, sizeof(FileSystem_t));
    assert(fileSystem != NULL);
    ELEMENT_INIT(&fileSystem->Header, (uintptr_t)storage->ID, fileSystem);
    fileSystem->ID = id;
    fileSystem->Storage = storage;
    fileSystem->PartitionIndex = partitionIndex;
    fileSystem->State = FileSystemState_MOUNTED;
    return fileSystem;
}

oserr_t
VFSFileSystemUnmount(
    _In_ FileSystem_t* fileSystem,
    _In_ unsigned int  flags)
{
    assert(!g_awaitingUnregister);
    assert(fileSystem->State == FileSystemState_MOUNTED);
    fileSystem->State = FileSystemState_CONNECTED;
    g_unmounts[__DriverOf(fileSystem->Storage)][__DeviceOf(fileSystem->Storage)]++;
    return OS_EOK;
}

oserr_t
VFSFileSystemDisconnectInterface(
    _In_ FileSystem_t* fileSystem,
    _In_ unsigned int  flags)
{
    assert(fileSystem->State == FileSystemState_CONNECTED);
    fileSystem->State = FileSystemState_NO_INTERFACE;
    return OS_EOK;
}

void
FileSystemDestroy(
    _In_ FileSystem_t* fileSystem)
{
    assert(fileSystem->State == FileSystemState_NO_INTERFACE);
    g_destroys[__DriverOf(fileSystem->Storage)][__DeviceOf(fileSystem->Storage)]++;
    free(fileSystem);
}

// Same storage setup as the real VFSStorageCreateDeviceBacked. g_failCreate
// makes it fail like an allocation failure.
struct VFSStorage*
VFSStorageCreateDeviceBacked(
    _In_ uuid_t       deviceID,
    _In_ uuid_t       driverID,
    _In_ unsigned int flags)
{
    struct VFSStorage* storage;

    if (g_failCreate) {
        return NULL;
    }
    storage = VFSStorageNew(&g_operations, flags);
    assert(storage != NULL);
    storage->Protocol.StorageType = VFSSTORAGE_TYPE_DEVICE;
    storage->Protocol.Storage.Device.DriverID = driverID;
    storage->Protocol.Storage.Device.DeviceID = deviceID;
    return storage;
}

// Replaces the stat IPC to the driver. The register result must already be
// sent, and the storage must be the listed disk for its pair.
oserr_t
VFSStorageQueryDeviceStats(
    _In_ struct VFSStorage* storage)
{
    uuid_t driverID = __DriverOf(storage);
    uuid_t deviceID = __DeviceOf(storage);

    assert(!g_awaitingRegister);
    assert(g_lastRegisterResult == OS_EOK);
    assert(__FindRegisteredDisk(driverID, deviceID) == storage);
    g_statQueries[driverID][deviceID]++;
    if (g_statHook != NULL) {
        void (*hook)(void) = g_statHook;
        g_statHook = NULL;
        hook();
    }
    if (g_statResult != OS_EOK) {
        return g_statResult;
    }
    storage->Stats.DeviceID = deviceID;
    snprintf(&storage->Stats.Serial[0], sizeof(storage->Stats.Serial), "disk-%u-%u", driverID, deviceID);
    return OS_EOK;
}

// Every disk gets one partition, added through the real
// VFSStorageRegisterPartition.
oserr_t
VFSStorageParse(
    _In_ struct VFSStorage* storage)
{
    UInteger64_t  sector = { .QuadPart = 0 };
    guid_t        guid = { 0 };
    FileSystem_t* fileSystem;

    assert(VFSStorageRegisterPartition(storage, 0, &sector, &guid, &fileSystem) == OS_EOK);
    return OS_EOK;
}

// Sends one register request and checks that it got exactly one result.
// A hook that registers another disk sends a second result, so tests with
// such a hook call the handler directly.
static oserr_t
__Register(
    _In_ uuid_t driverID,
    _In_ uuid_t deviceID)
{
    int resultsBefore = g_registerResults;

    g_awaitingRegister = true;
    sys_storage_register_invocation(NULL, driverID, deviceID, 0);
    assert(g_registerResults == resultsBefore + 1);
    assert(!g_awaitingRegister);
    return g_lastRegisterResult;
}

// Sends one unregister request and checks that it got exactly one result.
static oserr_t
__Unregister(
    _In_ uuid_t driverID,
    _In_ uuid_t deviceID)
{
    int resultsBefore = g_unregisterResults;

    g_awaitingUnregister = true;
    sys_storage_unregister_invocation(NULL, driverID, deviceID, 0);
    assert(g_unregisterResults == resultsBefore + 1);
    assert(!g_awaitingUnregister);
    return g_lastUnregisterResult;
}

static struct VFSStorage*
__FindDisk(
    _In_ uuid_t driverID,
    _In_ uuid_t deviceID)
{
    return __FindRegisteredDisk(driverID, deviceID);
}

static void
__CheckNothingLeft(void)
{
    assert(list_count(&g_disks) == 0);
    for (int i = 0; i < TEST_MAX_DRIVERS; i++) {
        for (int j = 0; j < TEST_MAX_DEVICES; j++) {
            assert(g_unmounts[i][j] == g_destroys[i][j]);
        }
    }
}

static void
__TestSameDeviceFromTwoDrivers(void)
{
    struct VFSStorage* storageA;
    struct VFSStorage* storageB;

    // Both drivers use device ID 0. Each disk is listed and queried through
    // its own driver.
    assert(__Register(TEST_DRIVER_A, 0) == OS_EOK);
    assert(__Register(TEST_DRIVER_B, 0) == OS_EOK);
    assert(list_count(&g_disks) == 2);
    assert(g_statQueries[TEST_DRIVER_A][0] == 1 && g_statQueries[TEST_DRIVER_B][0] == 1);
    storageA = __FindDisk(TEST_DRIVER_A, 0);
    storageB = __FindDisk(TEST_DRIVER_B, 0);
    assert(storageA != NULL && storageB != NULL && storageA != storageB);
    assert(storageA->State == VFSSTORAGE_STATE_CONNECTED);
    assert(storageB->State == VFSSTORAGE_STATE_CONNECTED);

    // Removing driver A's disk leaves driver B's disk alone.
    assert(__Unregister(TEST_DRIVER_A, 0) == OS_EOK);
    assert(g_destroys[TEST_DRIVER_A][0] == 1 && g_destroys[TEST_DRIVER_B][0] == 0);
    assert(__FindDisk(TEST_DRIVER_A, 0) == NULL && __FindDisk(TEST_DRIVER_B, 0) == storageB);

    // Driver A's disk is gone, so a second unregister finds nothing.
    assert(__Unregister(TEST_DRIVER_A, 0) == OS_ENOENT);
    assert(g_destroys[TEST_DRIVER_B][0] == 0);

    // The other order, with a nonzero device ID.
    assert(__Register(TEST_DRIVER_A, 5) == OS_EOK);
    assert(__Register(TEST_DRIVER_B, 5) == OS_EOK);
    assert(__Unregister(TEST_DRIVER_B, 5) == OS_EOK);
    assert(g_destroys[TEST_DRIVER_B][5] == 1 && g_destroys[TEST_DRIVER_A][5] == 0);
    assert(__Unregister(TEST_DRIVER_A, 5) == OS_EOK);
    assert(__Unregister(TEST_DRIVER_B, 0) == OS_EOK);
    __CheckNothingLeft();
    assert(g_errors == 0 && g_warnings == 1);
    g_warnings = 0;
    puts("filed storage results: same device ID from two drivers passed");
}

static void
__TestDuplicateAndUnknown(void)
{
    struct VFSStorage* storage;

    // The same pair twice. The second request is refused, is not queried and
    // leaves the first disk untouched.
    assert(__Register(TEST_DRIVER_A, 3) == OS_EOK);
    storage = __FindDisk(TEST_DRIVER_A, 3);
    assert(__Register(TEST_DRIVER_A, 3) == OS_EEXISTS);
    assert(list_count(&g_disks) == 1 && __FindDisk(TEST_DRIVER_A, 3) == storage);
    assert(g_statQueries[TEST_DRIVER_A][3] == 1);
    assert(storage->State == VFSSTORAGE_STATE_CONNECTED);

    // Unknown pairs: an unused device ID, and a used device ID under another
    // driver. Neither touches the registered disk.
    assert(__Unregister(TEST_DRIVER_A, 4) == OS_ENOENT);
    assert(__Unregister(TEST_DRIVER_B, 3) == OS_ENOENT);
    assert(list_count(&g_disks) == 1 && g_destroys[TEST_DRIVER_A][3] == 0);

    assert(__Unregister(TEST_DRIVER_A, 3) == OS_EOK);
    __CheckNothingLeft();
    assert(g_errors == 0 && g_warnings == 3);
    g_warnings = 0;
    puts("filed storage results: duplicate and unknown disks passed");
}

static void
__TestInvalidRequests(void)
{
    // An invalid driver ID is refused before anything is created.
    assert(__Register(UUID_INVALID, 1) == OS_EINVALPARAMS);
    assert(list_count(&g_disks) == 0 && g_statQueries[UUID_INVALID][1] == 0);
    assert(g_errors == 1);

    // The storage object cannot be created.
    g_failCreate = true;
    assert(__Register(TEST_DRIVER_A, 1) == OS_EOOM);
    g_failCreate = false;
    assert(list_count(&g_disks) == 0 && g_statQueries[TEST_DRIVER_A][1] == 0);
    assert(g_errors == 2);

    // A stat failure comes after the result. The driver got OS_EOK, but the
    // disk is not kept, so its unregister finds nothing.
    g_statResult = OS_EUNKNOWN;
    assert(__Register(TEST_DRIVER_A, 1) == OS_EOK);
    g_statResult = OS_EOK;
    assert(list_count(&g_disks) == 0);
    assert(__Unregister(TEST_DRIVER_A, 1) == OS_ENOENT);
    assert(g_errors == 3);

    // A failed send is logged, and the request still gets only one result.
    g_failResponseSend = 1;
    assert(__Register(TEST_DRIVER_A, 1) == OS_EOK);
    assert(__FindDisk(TEST_DRIVER_A, 1) != NULL);
    g_failResponseSend = 1;
    assert(__Unregister(TEST_DRIVER_A, 1) == OS_EOK);
    assert(g_errors == 5);
    __CheckNothingLeft();
    g_errors = 0;
    g_warnings = 0;
    puts("filed storage results: invalid requests and failed sends passed");
}

// Unregisters the disk whose stat query is running.
static void
__UnregisterDuringStat(void)
{
    assert(__Unregister(TEST_DRIVER_A, 6) == OS_EOK);
}

// Unregisters the disk whose stat query is running, and then registers the
// same pair again at once, as a driver that resets its device would.
static void
__UnregisterAndRegisterDuringStat(void)
{
    assert(__Unregister(TEST_DRIVER_A, 6) == OS_EOK);
    assert(__Register(TEST_DRIVER_A, 6) == OS_EOK);
}

static void
__TestUnregisterDuringRegistration(void)
{
    struct VFSStorage* storage;

    // The unregister arrives during the stat query. It gets OS_EOK at once,
    // and registration tears the disk down when it finishes.
    g_statHook = __UnregisterDuringStat;
    assert(__Register(TEST_DRIVER_A, 6) == OS_EOK);
    assert(g_statHook == NULL);
    assert(__FindDisk(TEST_DRIVER_A, 6) == NULL && list_count(&g_disks) == 0);
    assert(g_statQueries[TEST_DRIVER_A][6] == 1);
    __CheckNothingLeft();

    // The deferred disk is no longer found, so the same pair can be
    // registered again while the first registration is still running. When
    // the first registration finishes, it only tears down its own storage.
    g_statHook = __UnregisterAndRegisterDuringStat;
    g_awaitingRegister = true;
    sys_storage_register_invocation(NULL, TEST_DRIVER_A, 6, 0);
    assert(g_statHook == NULL && !g_awaitingRegister);
    storage = __FindDisk(TEST_DRIVER_A, 6);
    assert(storage != NULL && storage->State == VFSSTORAGE_STATE_CONNECTED);
    assert(list_count(&g_disks) == 1 && g_statQueries[TEST_DRIVER_A][6] == 3);
    assert(g_destroys[TEST_DRIVER_A][6] == 0);

    assert(__Unregister(TEST_DRIVER_A, 6) == OS_EOK);
    assert(g_destroys[TEST_DRIVER_A][6] == 1);
    __CheckNothingLeft();
    assert(g_errors == 0 && g_warnings == 0);
    puts("filed storage results: unregister during registration passed");
}

int
main(void)
{
    VFSStorageStartup();
    __TestSameDeviceFromTwoDrivers();
    __TestDuplicateAndUnknown();
    __TestInvalidRequests();
    __TestUnregisterDuringRegistration();
    return 0;
}
