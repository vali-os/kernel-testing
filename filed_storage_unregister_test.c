// Register and unregister device-backed storage through filed's real
// sys_storage handlers. The disk list is the production one in storage.c.
// Filesystem, VFS and driver calls are recording doubles.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FILED_STORAGE_SOURCE
#define FILED_STORAGE_SOURCE "../services/filed/storage/storage.c"
#endif
#include FILED_STORAGE_SOURCE

// Device IDs used by the test must stay below this value.
#define TEST_MAX_DEVICES 16

static struct VFSStorageOperations g_operations = { 0 };
static int  g_unmounts[TEST_MAX_DEVICES];
static int  g_disconnects[TEST_MAX_DEVICES];
static int  g_destroys[TEST_MAX_DEVICES];
static int  g_unknownDeviceWarnings;
static int  g_directoryRemovals;
static int  g_errors;
static char g_path[64];

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (!strcmp(format, "sys_storage_unregister_invocation no storage registered for device %u")) {
        g_unknownDeviceWarnings++;
    }
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

// Unregister removes the device directory that registration created.
oserr_t
VFSNodeUnlink(
    _In_ struct VFS* vfs,
    _In_ const char* path)
{
    g_directoryRemovals++;
    return OS_EOK;
}
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
__DeviceOf(
    _In_ FileSystem_t* fileSystem)
{
    uuid_t deviceID = fileSystem->Storage->Protocol.Storage.Device.DeviceID;
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
    assert(fileSystem->State == FileSystemState_MOUNTED);
    fileSystem->State = FileSystemState_CONNECTED;
    g_unmounts[__DeviceOf(fileSystem)]++;
    return OS_EOK;
}

oserr_t
VFSFileSystemDisconnectInterface(
    _In_ FileSystem_t* fileSystem,
    _In_ unsigned int  flags)
{
    assert(fileSystem->State == FileSystemState_CONNECTED);
    fileSystem->State = FileSystemState_NO_INTERFACE;
    g_disconnects[__DeviceOf(fileSystem)]++;
    return OS_EOK;
}

// A filesystem must be unmounted and disconnected before it is destroyed.
void
FileSystemDestroy(
    _In_ FileSystem_t* fileSystem)
{
    assert(fileSystem->State == FileSystemState_NO_INTERFACE);
    g_destroys[__DeviceOf(fileSystem)]++;
    free(fileSystem);
}

// Same storage setup as the real VFSStorageCreateDeviceBacked, which does not
// query the driver.
struct VFSStorage*
VFSStorageCreateDeviceBacked(
    _In_ uuid_t       deviceID,
    _In_ uuid_t       driverID,
    _In_ unsigned int flags)
{
    struct VFSStorage* storage = VFSStorageNew(&g_operations, flags);
    assert(storage != NULL);
    storage->Protocol.StorageType = VFSSTORAGE_TYPE_DEVICE;
    storage->Protocol.Storage.Device.DriverID = driverID;
    storage->Protocol.Storage.Device.DeviceID = deviceID;
    return storage;
}

// Replaces the driver query of the real VFSStorageQueryDeviceStats with fixed
// values. The storage must already be in the disk list when this runs.
oserr_t
VFSStorageQueryDeviceStats(
    _In_ struct VFSStorage* storage)
{
    uuid_t deviceID = storage->Protocol.Storage.Device.DeviceID;

    assert(list_find(&g_disks, (void*)(uintptr_t)deviceID) == &storage->ListHeader);
    storage->Stats.DeviceID = deviceID;
    snprintf(&storage->Stats.Serial[0], sizeof(storage->Stats.Serial), "disk-%u", deviceID);
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

static void
__Register(
    _In_ uuid_t deviceID)
{
    sys_storage_register_invocation(NULL, 1, deviceID, 0);
}

static struct VFSStorage*
__FindDisk(
    _In_ uuid_t deviceID)
{
    element_t* header = list_find(&g_disks, (void*)(uintptr_t)deviceID);
    if (header == NULL) {
        return NULL;
    }
    return header->value;
}

// Unregisters deviceID and checks that only that disk was torn down.
static void
__UnregisterAndCheck(
    _In_ uuid_t deviceID)
{
    int destroysBefore[TEST_MAX_DEVICES];
    int disksBefore = list_count(&g_disks);
    int removalsBefore = g_directoryRemovals;

    memcpy(destroysBefore, g_destroys, sizeof(g_destroys));
    assert(__FindDisk(deviceID) != NULL);
    sys_storage_unregister_invocation(NULL, deviceID, 0);

    assert(list_count(&g_disks) == disksBefore - 1);
    assert(g_directoryRemovals == removalsBefore + 1);
    assert(__FindDisk(deviceID) == NULL);
    for (uuid_t i = 0; i < TEST_MAX_DEVICES; i++) {
        int expected = destroysBefore[i] + (i == deviceID ? 1 : 0);
        assert(g_destroys[i] == expected);
    }
    assert(g_unmounts[deviceID] == g_destroys[deviceID]);
    assert(g_disconnects[deviceID] == g_destroys[deviceID]);
}

int
main(void)
{
    struct VFSStorage* storage;

    VFSStorageStartup();

    // AHCI numbers its disks from 0. Device 0 must be found by its own key.
    __Register(0);
    storage = __FindDisk(0);
    assert(storage != NULL && storage->Protocol.Storage.Device.DeviceID == 0);
    assert(storage->State == VFSSTORAGE_STATE_CONNECTED);
    __UnregisterAndCheck(0);
    assert(list_count(&g_disks) == 0);

    // A nonzero device ID.
    __Register(7);
    __UnregisterAndCheck(7);
    assert(list_count(&g_disks) == 0);

    // Two disks: only the named one goes. Device 0 is placed second so a
    // lookup that matches key 0 on any disk would hit device 3 first.
    __Register(3);
    __Register(0);
    __UnregisterAndCheck(0);
    storage = __FindDisk(3);
    assert(storage != NULL && storage->Protocol.Storage.Device.DeviceID == 3);
    assert(g_unmounts[3] == 0 && g_destroys[3] == 0);
    __UnregisterAndCheck(3);

    __Register(5);
    __Register(9);
    __UnregisterAndCheck(9);
    storage = __FindDisk(5);
    assert(storage != NULL && storage->Protocol.Storage.Device.DeviceID == 5);
    __UnregisterAndCheck(5);
    assert(list_count(&g_disks) == 0);

    // Unknown device IDs leave registered disks alone and are logged.
    sys_storage_unregister_invocation(NULL, 4, 0);
    assert(g_unknownDeviceWarnings == 1);
    __Register(2);
    sys_storage_unregister_invocation(NULL, 4, 0);
    sys_storage_unregister_invocation(NULL, 0, 0);
    assert(g_unknownDeviceWarnings == 3);
    assert(list_count(&g_disks) == 1 && g_destroys[2] == 0 && g_unmounts[2] == 0);
    __UnregisterAndCheck(2);
    assert(list_count(&g_disks) == 0);

    assert(g_errors == 0);
    puts("filed storage unregister: device 0, nonzero device, two disks and unknown device passed");
    return 0;
}
