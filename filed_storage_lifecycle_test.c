// Register, unregister and register again device-backed storage through
// filed's real sys_storage handlers, including unregister requests that
// arrive while registration is still mounting or parsing the disk.
//
// Production code compiled here: storage/storage.c, filesystem.c,
// storage/utils.c (filesystem identifiers) and the libds list, guid and
// mstring code. Doubles replace:
// - VFSStorageCreateDeviceBacked and VFSStorageQueryDeviceStats
//   (storage/storage_device.c). Same storage setup, and fixed values in place
//   of the ctt_storage stat IPC. The query can fail like a failed stat.
// - VFSStorageParse (storage/layouts/detect.c). Sets up two partitions
//   through the real VFSStorageRegisterAndSetupPartition. It can fail after
//   the first partition, and it can run a hook between the partitions, which
//   is where the real parse waits for disk reads.
// - The VFS node layer (vfs/). A flat table of directories that applies the
//   same rules as the real code, see __TestNode.
// - Filesystem interfaces (interfaces/modules.c). Each load allocates a new
//   interface, as VFSInterfaceLoadDriver does, and every release is counted.
// - mstring (libds/mstring). The real mstr_fmt passes va_list pointers in a
//   way gcc rejects on x86-64, so mstrings here are plain heap strings. Every
//   mstring is counted, so a leaked path shows as a test failure.
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef FILED_STORAGE_SOURCE
#define FILED_STORAGE_SOURCE "../services/filed/storage/storage.c"
#endif
#ifndef FILED_FILESYSTEM_SOURCE
#define FILED_FILESYSTEM_SOURCE "../services/filed/filesystem.c"
#endif
#ifndef FILED_IDENTIFIER_SOURCE
#define FILED_IDENTIFIER_SOURCE "../services/filed/storage/utils.c"
#endif
#include FILED_STORAGE_SOURCE
#include FILED_FILESYSTEM_SOURCE
#include FILED_IDENTIFIER_SOURCE

#define TEST_PARTITIONS  2
#define TEST_MAX_NODES   32
#define TEST_MAX_HANDLES 256

// The root scope is only compared by address.
static char         g_rootScopeMemory;
#define TEST_ROOT_SCOPE ((struct VFS*)&g_rootScopeMemory)

// One directory in the root scope. The rules follow the real VFS code:
// - VFSNodeNewDirectory fails with OS_EEXISTS for an existing directory and
//   with OS_ENOENT when the parent is missing (vfs/utils.c).
// - VFSNodeMkdir only gives a handle when it created the directory
//   (vfs/handlers/mkdir.c).
// - VFSNodeUnlink refuses directories with children, open handles or a
//   mounted filesystem (vfs/handlers/unlink.c).
struct __TestNode {
    bool        Used;
    char        Path[96];
    int         Handles;
    struct VFS* MountedVFS;
};

static struct VFSStorageOperations g_operations = { 0 };
static struct __TestNode g_nodes[TEST_MAX_NODES];
static int               g_handleNodes[TEST_MAX_HANDLES];
static uuid_t            g_nextHandle = 1;
static int               g_spinlockDepth;
static int               g_errors;
static int               g_unknownDeviceWarnings;
static int               g_interfacesCreated;
static int               g_interfacesDeleted;
static int               g_instancesCreated;
static int               g_instancesDestroyed;
static int               g_vfsCreated;
static int               g_vfsDestroyed;
static bool              g_failStat;
static bool              g_failDeviceDirectory;
static bool              g_failParse;
static int               g_failMountPartition = -1;
static int               g_mountCount;

// A hook runs once, at the point the real code would wait, and then clears
// itself. g_hookDevice is the device the hook unregisters.
static void (*g_directoryHook)(void);
static void (*g_parseHook)(void);
static uuid_t g_hookDevice;
static bool   g_hookRan;

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
    if (getenv("FILED_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        printf("log(%i): %s\n", level, format);
    }
}

// An mstring here is a heap allocated struct __TestString cast to mstring_t.
struct __TestString {
    char* Text;
};
static int g_stringsLive;

static mstring_t*
__NewString(
    _In_ const char* text)
{
    struct __TestString* string = malloc(sizeof(struct __TestString));

    assert(string != NULL);
    string->Text = strdup(text);
    assert(string->Text != NULL);
    g_stringsLive++;
    return (mstring_t*)string;
}

static const char*
__StringText(
    _In_ mstring_t* string)
{
    assert(string != NULL);
    return ((struct __TestString*)string)->Text;
}

mstring_t* mstr_new_u8(const char* text) { return __NewString(text); }
mstring_t* mstr_clone(mstring_t* string) { return __NewString(__StringText(string)); }
int        mstr_cmp(mstring_t* lh, mstring_t* rh) { return strcmp(__StringText(lh), __StringText(rh)); }
char*      mstr_u8(mstring_t* string) { return strdup(__StringText(string)); }

void
mstr_delete(
    _In_ mstring_t* string)
{
    if (string == NULL) {
        return;
    }
    assert(g_stringsLive-- > 0);
    free(((struct __TestString*)string)->Text);
    free(string);
}

// Supports the conversions filed uses in paths and labels: %s, %i, %u and %ms.
mstring_t*
mstr_fmt(
    _In_ const char* format,
    ...)
{
    char    buffer[128];
    size_t  length = 0;
    va_list arguments;

    va_start(arguments, format);
    for (const char* i = format; *i != '\0'; i++) {
        assert(length < sizeof(buffer) - 32);
        if (*i != '%') {
            buffer[length++] = *i;
            continue;
        }
        i++;
        if (i[0] == 'm' && i[1] == 's') {
            length += snprintf(&buffer[length], sizeof(buffer) - length, "%s",
                               __StringText(va_arg(arguments, mstring_t*)));
            i++;
        } else if (*i == 's') {
            length += snprintf(&buffer[length], sizeof(buffer) - length, "%s", va_arg(arguments, char*));
        } else if (*i == 'i') {
            length += snprintf(&buffer[length], sizeof(buffer) - length, "%i", va_arg(arguments, int));
        } else if (*i == 'u') {
            length += snprintf(&buffer[length], sizeof(buffer) - length, "%u", va_arg(arguments, unsigned int));
        } else {
            abort();
        }
    }
    va_end(arguments);
    assert(length < sizeof(buffer));
    buffer[length] = '\0';
    return __NewString(buffer);
}

// The list spinlock must never be held across code that can wait. A usched
// mutex can wait, so taking one under a spinlock is a test failure.
void spinlock_init(spinlock_t* lock) { (void)lock; }
void spinlock_acquire(spinlock_t* lock) { (void)lock; g_spinlockDepth++; }
void spinlock_release(spinlock_t* lock) { (void)lock; assert(g_spinlockDepth-- > 0); }
void usched_mtx_init(struct usched_mtx* mutex, int type) { memset(mutex, 0, sizeof(*mutex)); }
void
usched_mtx_lock(struct usched_mtx* mutex)
{
    assert(g_spinlockDepth == 0);
    assert(mutex->references++ == 0);
}
void usched_mtx_unlock(struct usched_mtx* mutex) { assert(--mutex->references == 0); }

gracht_server_t* __crt_get_service_server(void) { return NULL; }
int
sys_file_event_storage_ready_all(gracht_server_t* server, const char* path)
{
    // Only sent for bind mounts and explicit mount points, which this test
    // does not use.
    abort();
}

struct VFS* VFSScopeGet(uuid_t processId) { return TEST_ROOT_SCOPE; }

static int
__FindNode(
    _In_ const char* path)
{
    for (int i = 0; i < TEST_MAX_NODES; i++) {
        if (g_nodes[i].Used && !strcmp(g_nodes[i].Path, path)) {
            return i;
        }
    }
    return -1;
}

static int
__AddNode(
    _In_ const char* path)
{
    for (int i = 0; i < TEST_MAX_NODES; i++) {
        if (!g_nodes[i].Used) {
            memset(&g_nodes[i], 0, sizeof(struct __TestNode));
            g_nodes[i].Used = true;
            assert(strlen(path) < sizeof(g_nodes[i].Path));
            strcpy(g_nodes[i].Path, path);
            return i;
        }
    }
    abort();
}

static bool
__HasChildren(
    _In_ const char* path)
{
    size_t length = strlen(path);
    for (int i = 0; i < TEST_MAX_NODES; i++) {
        if (g_nodes[i].Used && !strncmp(g_nodes[i].Path, path, length) && g_nodes[i].Path[length] == '/') {
            return true;
        }
    }
    return false;
}

static int
__NodeCount(void)
{
    int count = 0;
    for (int i = 0; i < TEST_MAX_NODES; i++) {
        count += g_nodes[i].Used ? 1 : 0;
    }
    return count;
}

// /storage/<Serial> is a device directory, deeper paths are filesystem
// mount directories.
static bool
__IsDeviceDirectory(
    _In_ const char* path)
{
    return !strncmp(path, "/storage/", 9) && strchr(path + 9, '/') == NULL;
}

static int
__NodeOfHandle(
    _In_ uuid_t handle)
{
    assert(handle < TEST_MAX_HANDLES);
    return g_handleNodes[handle];
}

oserr_t
VFSNodeNewDirectory(
    _In_  struct VFS*      vfs,
    _In_  mstring_t*       path,
    _In_  uint32_t         permissions,
    _Out_ struct VFSNode** nodeOut)
{
    char*   cpath = mstr_u8(path);
    char    parent[96];
    char*   separator;
    int     index;
    oserr_t oserr = OS_EOK;

    assert(vfs == TEST_ROOT_SCOPE && cpath != NULL);
    if (__IsDeviceDirectory(cpath) && g_directoryHook != NULL) {
        void (*hook)(void) = g_directoryHook;
        g_directoryHook = NULL;
        hook();
    }
    if (__IsDeviceDirectory(cpath) && g_failDeviceDirectory) {
        free(cpath);
        return OS_EOOM;
    }

    strcpy(parent, cpath);
    separator = strrchr(parent, '/');
    assert(separator != NULL && separator != parent);
    *separator = '\0';
    index = __FindNode(cpath);
    if (__FindNode(parent) < 0) {
        oserr = OS_ENOENT;
    } else if (index >= 0) {
        *nodeOut = (struct VFSNode*)&g_nodes[index];
        oserr = OS_EEXISTS;
    } else {
        *nodeOut = (struct VFSNode*)&g_nodes[__AddNode(cpath)];
    }
    free(cpath);
    return oserr;
}

oserr_t
VFSNodeMkdir(
    _In_  struct VFS* vfs,
    _In_  mstring_t*  path,
    _In_  uint32_t    access,
    _Out_ uuid_t*     handleOut)
{
    struct VFSNode* node;
    oserr_t         oserr;

    oserr = VFSNodeNewDirectory(vfs, path, access, &node);
    if (oserr != OS_EOK) {
        return oserr;
    }
    assert(g_nextHandle < TEST_MAX_HANDLES);
    g_handleNodes[g_nextHandle] = (int)((struct __TestNode*)node - &g_nodes[0]);
    g_nodes[g_handleNodes[g_nextHandle]].Handles++;
    *handleOut = g_nextHandle++;
    return OS_EOK;
}

oserr_t
VFSNodeMount(
    _In_ struct VFS* vfs,
    _In_ uuid_t      atID,
    _In_ struct VFS* what)
{
    int index = __NodeOfHandle(atID);

    assert(vfs == TEST_ROOT_SCOPE && index >= 0 && what != NULL);
    assert(g_nodes[index].MountedVFS == NULL);
    if (g_mountCount++ == g_failMountPartition) {
        return OS_EINVALPARAMS;
    }
    g_nodes[index].MountedVFS = what;
    return OS_EOK;
}

// The real VFSNodeUnmount and VFSNodeClose ignore the scope, so this is the
// only place a wrong scope shows.
oserr_t
VFSNodeUnmount(
    _In_ struct VFS* vfs,
    _In_ uuid_t      directoryHandleID)
{
    int index = __NodeOfHandle(directoryHandleID);

    assert(vfs == TEST_ROOT_SCOPE);
    if (index < 0 || g_nodes[index].MountedVFS == NULL) {
        return OS_EINVALPARAMS;
    }
    g_nodes[index].MountedVFS = NULL;
    return OS_EOK;
}

oserr_t
VFSNodeClose(
    _In_ struct VFS* vfs,
    _In_ uuid_t      handleID)
{
    int index = __NodeOfHandle(handleID);

    assert(vfs == TEST_ROOT_SCOPE);
    if (index < 0) {
        return OS_ENOENT;
    }
    g_nodes[index].Handles--;
    g_handleNodes[handleID] = -1;
    return OS_EOK;
}

oserr_t
VFSNodeUnlink(
    _In_ struct VFS* vfs,
    _In_ const char* cpath)
{
    int index = __FindNode(cpath);

    assert(vfs == TEST_ROOT_SCOPE);
    if (index < 0) {
        return OS_ENOENT;
    }
    if (__HasChildren(cpath)) {
        return OS_EDIRNOTEMPTY;
    }
    if (g_nodes[index].Handles != 0 || g_nodes[index].MountedVFS != NULL) {
        return OS_EBUSY;
    }
    g_nodes[index].Used = false;
    return OS_EOK;
}

// Bind mounts and mount point paths are only used for labels in the default
// mount list and explicit mount points. The labels here are p0 and p1.
// filed_storage_bind_test covers bind mounts with the real VFS code.
oserr_t VFSNodeBind(struct VFS* vfs, uuid_t fromID, uuid_t toID) { abort(); }
oserr_t VFSNodeUnbind(struct VFS* vfs, uuid_t directoryHandleID) { abort(); }
oserr_t VFSNodeOpen(struct VFS* vfs, const char* cpath, uint32_t options, uint32_t permissions, uuid_t* handleOut) { abort(); }
oserr_t VFSNodeGetPathHandle(uuid_t handleID, mstring_t** pathOut) { abort(); }

oserr_t
VFSNew(
    _In_  uuid_t               id,
    _In_  guid_t*              guid,
    _In_  struct VFSStorage*   storage,
    _In_  struct VFSInterface* interface,
    _In_  void*                data,
    _Out_ struct VFS**         vfsOut)
{
    *vfsOut = malloc(1);
    assert(*vfsOut != NULL);
    g_vfsCreated++;
    return OS_EOK;
}

// The real VFSDestroy closes open handles and can yield, so it must never
// run under a spinlock. This is checked even for a NULL VFS.
void
VFSDestroy(
    _In_ struct VFS* vfs)
{
    assert(g_spinlockDepth == 0);
    if (vfs == NULL) {
        return;
    }
    g_vfsDestroyed++;
    free(vfs);
}

static oserr_t
__TestInitialize(
    _In_  struct VFSInterface*         interface,
    _In_  struct VFSStorageParameters* storageParameters,
    _Out_ void**                       instanceData)
{
    assert(storageParameters->StorageType == VFSSTORAGE_TYPE_DEVICE);
    *instanceData = malloc(1);
    assert(*instanceData != NULL);
    g_instancesCreated++;
    return OS_EOK;
}

static oserr_t
__TestDestroy(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ unsigned int         unmountFlags)
{
    assert(instanceData != NULL);
    free(instanceData);
    g_instancesDestroyed++;
    return OS_EOK;
}

oserr_t
VFSInterfaceLoadInternal(
    _In_  const char*           type,
    _Out_ struct VFSInterface** interfaceOut)
{
    struct VFSInterface* interface;

    assert(type != NULL && !strcmp(type, "mfs"));
    interface = calloc(1, sizeof(struct VFSInterface));
    assert(interface != NULL);
    interface->Operations.Initialize = __TestInitialize;
    interface->Operations.Destroy = __TestDestroy;
    g_interfacesCreated++;
    *interfaceOut = interface;
    return OS_EOK;
}

oserr_t VFSInterfaceLoadDriver(uuid_t driverID, struct VFSInterface** interfaceOut) { abort(); }

void
VFSInterfaceDelete(
    _In_ struct VFSInterface* interface)
{
    if (interface == NULL) {
        return;
    }
    g_interfacesDeleted++;
    free(interface);
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
    storage->Stats.Flags = flags;
    return storage;
}

// The stat IPC of the real VFSStorageQueryDeviceStats is replaced by fixed
// values. g_failStat makes it fail like a failed stat.
oserr_t
VFSStorageQueryDeviceStats(
    _In_ struct VFSStorage* storage)
{
    uuid_t deviceID = storage->Protocol.Storage.Device.DeviceID;

    if (g_failStat) {
        return OS_EUNKNOWN;
    }
    storage->Stats.DeviceID = deviceID;
    storage->Stats.SectorSize = 512;
    snprintf(&storage->Stats.Serial[0], sizeof(storage->Stats.Serial), "disk-%u", deviceID);
    return OS_EOK;
}

oserr_t
VFSStorageParse(
    _In_ struct VFSStorage* storage)
{
    guid_t  guid = { 0 };
    oserr_t oserr;

    for (int i = 0; i < TEST_PARTITIONS; i++) {
        UInteger64_t sector = { .QuadPart = 2048 * (uint64_t)(i + 1) };

        if (i == 1 && g_parseHook != NULL) {
            void (*hook)(void) = g_parseHook;
            g_parseHook = NULL;
            hook();
        }
        if (i == 1 && g_failParse) {
            return OS_EUNKNOWN;
        }
        oserr = VFSStorageRegisterAndSetupPartition(
            storage, i, &sector, &guid, "mfs", &guid, UUID_INVALID, NULL);
        if (oserr != OS_EOK) {
            return oserr;
        }
    }
    return OS_EOK;
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

static void
__Register(
    _In_ uuid_t deviceID)
{
    sys_storage_register_invocation(NULL, 1, deviceID, SYS_STORAGE_FLAGS_REMOVABLE);
}

static void
__Unregister(
    _In_ uuid_t deviceID)
{
    sys_storage_unregister_invocation(NULL, deviceID, 0);
}

// Runs in place of the unregister job while registration waits.
static void
__UnregisterHook(void)
{
    g_hookRan = true;
    __Unregister(g_hookDevice);
}

// Checks that nothing from any registration is left: no disks, only the
// /storage directory, no open handles, and every interface, driver instance,
// VFS and filesystem identifier released.
static void
__CheckNothingLeft(void)
{
    int storageIndex = __FindNode("/storage");

    assert(list_count(&g_disks) == 0);
    assert(storageIndex >= 0 && __NodeCount() == 1);
    assert(g_nodes[storageIndex].Handles == 0);
    assert(g_interfacesCreated == g_interfacesDeleted);
    assert(g_instancesCreated == g_instancesDestroyed);
    assert(g_vfsCreated == g_vfsDestroyed);
    assert(g_stringsLive == 0);
    for (int i = 0; i < __FILEMANAGER_MAXDISKS; i++) {
        assert(g_diskTable[i] == 0);
    }
}

// Checks a fully registered disk: connected, its directory present, and every
// partition mounted at /storage/<Serial>/p<index>.
static void
__CheckRegistered(
    _In_ uuid_t deviceID)
{
    struct VFSStorage* storage = __FindDisk(deviceID);
    char               path[96];
    int                index;

    assert(storage != NULL && storage->State == VFSSTORAGE_STATE_CONNECTED);
    assert(list_count(&storage->Filesystems) == TEST_PARTITIONS);
    snprintf(path, sizeof(path), "/storage/disk-%u", deviceID);
    assert(__FindNode(path) >= 0);
    for (int i = 0; i < TEST_PARTITIONS; i++) {
        snprintf(path, sizeof(path), "/storage/disk-%u/p%i", deviceID, i);
        index = __FindNode(path);
        assert(index >= 0 && g_nodes[index].MountedVFS != NULL && g_nodes[index].Handles == 1);
    }
}

static void
__TestRegisterAgain(void)
{
    uuid_t     firstIDs[TEST_PARTITIONS];
    int        i;
    element_t* header;

    // A card that is taken out and put back in gets the same serial. Each
    // round must mount it again and reuse the same filesystem identifiers.
    for (int round = 0; round < 3; round++) {
        __Register(4);
        __CheckRegistered(4);
        i = 0;
        _foreach(header, &__FindDisk(4)->Filesystems) {
            FileSystem_t* fileSystem = header->value;
            if (round == 0) {
                firstIDs[i] = fileSystem->ID;
            }
            assert(fileSystem->ID == firstIDs[i]);
            i++;
        }
        __Unregister(4);
        __CheckNothingLeft();
    }
    assert(g_interfacesCreated == 3 * TEST_PARTITIONS);
    assert(g_vfsCreated == 3 * TEST_PARTITIONS);
    assert(g_errors == 0);
    puts("filed storage lifecycle: register, unregister and register again passed");
}

static void
__TestUnregisterDuringMount(void)
{
    // The unregister arrives while __StorageMount waits. Registration still
    // uses the storage, so it must not be freed, and the disk must end up
    // torn down once registration finishes.
    g_hookDevice = 5;
    g_hookRan = false;
    g_directoryHook = __UnregisterHook;
    __Register(5);
    assert(g_hookRan);
    assert(__FindDisk(5) == NULL);
    __CheckNothingLeft();

    // And the disk can be registered again afterwards.
    __Register(5);
    __CheckRegistered(5);
    __Unregister(5);
    __CheckNothingLeft();
    assert(g_errors == 0 && g_unknownDeviceWarnings == 0);
    puts("filed storage lifecycle: unregister while mounting passed");
}

static void
__TestUnregisterDuringParse(void)
{
    // The unregister arrives after the first partition is mounted, while the
    // parse waits for the next disk read.
    g_hookDevice = 6;
    g_hookRan = false;
    g_parseHook = __UnregisterHook;
    __Register(6);
    assert(g_hookRan);
    assert(__FindDisk(6) == NULL);
    __CheckNothingLeft();

    // Same while a second disk stays registered: only the unregistered one goes.
    __Register(7);
    g_hookDevice = 6;
    g_hookRan = false;
    g_parseHook = __UnregisterHook;
    __Register(6);
    assert(g_hookRan && __FindDisk(6) == NULL);
    __CheckRegistered(7);
    __Unregister(7);
    __CheckNothingLeft();

    // A parse that fails after the unregister arrived is torn down too.
    g_hookDevice = 6;
    g_parseHook = __UnregisterHook;
    g_failParse = true;
    __Register(6);
    g_failParse = false;
    assert(__FindDisk(6) == NULL);
    __CheckNothingLeft();
    g_errors = 0;

    __Register(6);
    __CheckRegistered(6);
    __Unregister(6);
    __CheckNothingLeft();
    assert(g_errors == 0 && g_unknownDeviceWarnings == 0);
    puts("filed storage lifecycle: unregister while parsing passed");
}

static void
__TestRegistrationFailures(void)
{
    struct VFSStorage* storage;

    // Stat failure: the storage is never listed and nothing is created.
    g_failStat = true;
    __Register(8);
    g_failStat = false;
    assert(__FindDisk(8) == NULL);
    __CheckNothingLeft();
    assert(g_errors == 1);
    g_errors = 0;

    // Mount failure: the device directory is not created. The disk stays
    // listed as disconnected until its unregister.
    g_failDeviceDirectory = true;
    __Register(8);
    g_failDeviceDirectory = false;
    storage = __FindDisk(8);
    assert(storage != NULL && storage->State == VFSSTORAGE_STATE_DISCONNECTED);
    assert(list_count(&storage->Filesystems) == 0 && __NodeCount() == 1);
    __Unregister(8);
    __CheckNothingLeft();
    g_errors = 0;
    __Register(8);
    __CheckRegistered(8);
    __Unregister(8);
    __CheckNothingLeft();

    // Parse failure after the first partition was mounted.
    g_failParse = true;
    __Register(8);
    g_failParse = false;
    storage = __FindDisk(8);
    assert(storage != NULL && storage->State == VFSSTORAGE_STATE_DISCONNECTED);
    assert(list_count(&storage->Filesystems) == 1);
    __Unregister(8);
    __CheckNothingLeft();
    g_errors = 0;
    __Register(8);
    __CheckRegistered(8);
    __Unregister(8);
    __CheckNothingLeft();

    // Mounting the second partition fails. Its directory must be removed at
    // once, as nothing else refers to it.
    g_mountCount = 0;
    g_failMountPartition = 1;
    __Register(8);
    g_failMountPartition = -1;
    storage = __FindDisk(8);
    assert(storage != NULL && storage->State == VFSSTORAGE_STATE_DISCONNECTED);
    assert(__FindNode("/storage/disk-8/p0") >= 0 && __FindNode("/storage/disk-8/p1") < 0);
    __Unregister(8);
    __CheckNothingLeft();
    g_errors = 0;
    __Register(8);
    __CheckRegistered(8);
    __Unregister(8);
    __CheckNothingLeft();
    assert(g_errors == 0);
    puts("filed storage lifecycle: stat, mount and parse failures passed");
}

int
main(void)
{
    memset(g_handleNodes, 0xff, sizeof(g_handleNodes));
    __AddNode("/storage");
    VFSStorageStartup();
    VFSFileSystemStartup();

    __TestRegisterAgain();
    __TestUnregisterDuringMount();
    __TestUnregisterDuringParse();
    __TestRegistrationFailures();
    return 0;
}
