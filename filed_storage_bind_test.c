// Bind mounts made by filed for registered disks, and unregister requests that
// arrive while registration waits for the disk's stat reply. The VFS here is
// the real one, so the rules for bind, unbind and unlink are the production
// rules.
//
// Production code compiled here: storage/storage.c, filesystem.c,
// storage/utils.c, storage/storage_device.c, storage/storage_memory.c,
// scope.c (the root scope and /storage), vfs/vfs.c, vfs/utils.c, vfs/bind.c,
// vfs/mount.c, vfs/memfs.c (the root scope filesystem), the mkdir, open,
// close, unlink and handle_stat handlers, and the libds list, hashtable, guid
// and mstring code. Doubles replace:
// - The VFS handle table (vfs/handles.c). A flat table with the same Get, Put,
//   Add and Remove rules. The real one needs the kernel handle and grant
//   services.
// - Kernel services: handles, SHM buffers, grant tokens and time. An SHM buffer
//   is heap memory, and a grant token names the buffer it was made for.
// - The ctt_storage stat IPC below the real VFSStorageQueryDeviceStats. The
//   wait for the reply (gracht_client_await) can run a hook, which is where an
//   unregister for the disk can arrive.
// - VFSStorageParse (storage/layouts). Sets up two partitions through the real
//   VFSStorageRegisterAndSetupPartition and, like the GPT parser, goes on when
//   a partition fails.
// - Filesystem drivers (interfaces/modules.c). The partition driver reports
//   the label the test sets for each partition. VFSInterfaceNew and
//   VFSInterfaceDelete do what interfaces/modules.c does.
// - mstr_fmt (libds/mstring/core/mstr_fmt.c). The real one passes va_list
//   pointers in a way gcc rejects on x86-64. This one supports %s, %i, %u and
//   %ms, which is what filed uses.
// - usched mutexes and reader-writer locks. They check the locking rules for a
//   single thread instead of waiting.
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
#include "../services/filed/vfs/private.h"
#include <ctt_storage_service_client.h>

#define TEST_PARTITIONS    2
#define TEST_MAX_OSHANDLES 512
#define TEST_MAX_HANDLES   256
#define TEST_MAX_EVENTS    16

static int g_errors;
static int g_bindErrors;
static int g_unknownDeviceWarnings;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
    if (strstr(format, "__MountFileSystemAt failed to bind filesystem") == format) {
        g_bindErrors++;
    }
    if (!strcmp(format, "sys_storage_unregister_invocation no storage registered for driver %u device %u")) {
        g_unknownDeviceWarnings++;
    }
    if (getenv("FILED_TEST_VERBOSE") != NULL && level >= OSSYSLOGLEVEL_WARNING) {
        printf("log(%i): %s\n", level, format);
    }
}

// Results filed sent. Every register and unregister request must get exactly
// one, which __Register and __Unregister check.
static int g_registerResults;
static int g_unregisterResults;

int
sys_storage_register_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result)
{
    assert(result == OS_EOK);
    g_registerResults++;
    return 0;
}

int
sys_storage_unregister_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result)
{
    assert(result == OS_EOK);
    g_unregisterResults++;
    return 0;
}

// Supports the conversions filed uses in paths and labels: %s, %i, %u and %ms.
mstring_t*
mstr_fmt(
    _In_ const char* format,
    ...)
{
    char    buffer[256];
    size_t  length = 0;
    va_list arguments;

    va_start(arguments, format);
    for (const char* i = format; *i != '\0'; i++) {
        assert(length < sizeof(buffer) - 64);
        if (*i != '%') {
            buffer[length++] = *i;
            continue;
        }
        i++;
        if (i[0] == 'm' && i[1] == 's') {
            char* text = mstr_u8(va_arg(arguments, mstring_t*));
            assert(text != NULL);
            length += snprintf(&buffer[length], sizeof(buffer) - length, "%s", text);
            free(text);
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
    return mstr_new_u8(buffer);
}

// Locks. The list spinlock must never be held across code that can wait, and
// a usched mutex can wait. A reader-writer lock keeps its reader count in
// readers and marks a writer in sync_object.references. Taking a write lock
// while any reader is present would wait forever in a single thread.
static int g_spinlockDepth;

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
void usched_rwlock_init(struct usched_rwlock* lock) { memset(lock, 0, sizeof(*lock)); }
void
usched_rwlock_r_lock(struct usched_rwlock* lock)
{
    assert(g_spinlockDepth == 0 && lock->sync_object.references == 0);
    lock->readers++;
}
void usched_rwlock_r_unlock(struct usched_rwlock* lock) { assert(lock->readers-- > 0); }
void
usched_rwlock_w_lock(struct usched_rwlock* lock)
{
    assert(g_spinlockDepth == 0 && lock->readers == 0 && lock->sync_object.references == 0);
    lock->sync_object.references = 1;
}
void usched_rwlock_w_unlock(struct usched_rwlock* lock) { assert(lock->sync_object.references-- == 1); }
void
usched_rwlock_w_promote(struct usched_rwlock* lock)
{
    assert(lock->readers == 1 && lock->sync_object.references == 0);
    lock->readers = 0;
    lock->sync_object.references = 1;
}
void
usched_rwlock_w_demote(struct usched_rwlock* lock)
{
    assert(lock->readers == 0 && lock->sync_object.references == 1);
    lock->sync_object.references = 0;
    lock->readers = 1;
}
void usched_job_yield(void) { abort(); }

oserr_t
OSGetTime(
    _In_ enum OSTimeSource source,
    _In_ OSTimestamp_t*    time)
{
    memset(time, 0, sizeof(*time));
    return OS_EOK;
}

// Kernel handles. A handle is an index in g_osHandles. VFS resources and SHM
// buffers are both handles. An SHM attachment shares the buffer of the handle
// its token was made for and does not free it.
struct __TestOSHandle {
    bool   Used;
    bool   Resource;
    bool   Attached;
    void*  Buffer;
    size_t Size;
};
static struct __TestOSHandle g_osHandles[TEST_MAX_OSHANDLES];
static uuid_t                g_nextOSHandle = 1;

static uuid_t
__NewOSHandle(void)
{
    uuid_t id = g_nextOSHandle++;

    assert(id < TEST_MAX_OSHANDLES);
    memset(&g_osHandles[id], 0, sizeof(struct __TestOSHandle));
    g_osHandles[id].Used = true;
    return id;
}

static struct __TestOSHandle*
__OSHandle(
    _In_ uuid_t id)
{
    assert(id > 0 && id < TEST_MAX_OSHANDLES && g_osHandles[id].Used);
    return &g_osHandles[id];
}

static int
__ResourceHandles(void)
{
    int count = 0;
    for (int i = 0; i < TEST_MAX_OSHANDLES; i++) {
        count += g_osHandles[i].Used && g_osHandles[i].Resource ? 1 : 0;
    }
    return count;
}

oserr_t
SHMCreate(
    _In_  SHM_t*      shm,
    _Out_ OSHandle_t* handleOut)
{
    uuid_t id = __NewOSHandle();

    g_osHandles[id].Buffer = calloc(1, shm->Size);
    assert(g_osHandles[id].Buffer != NULL);
    g_osHandles[id].Size = shm->Size;
    memset(handleOut, 0, sizeof(*handleOut));
    handleOut->ID = id;
    return OS_EOK;
}

oserr_t
SHMAttachToken(
    _In_  const GrantToken_t* token,
    _Out_ OSHandle_t*         handleOut)
{
    struct __TestOSHandle* source = __OSHandle(token->ExportGlobalHandleID);
    uuid_t                 id = __NewOSHandle();

    g_osHandles[id].Attached = true;
    g_osHandles[id].Buffer = source->Buffer;
    g_osHandles[id].Size = source->Size;
    memset(handleOut, 0, sizeof(*handleOut));
    handleOut->ID = id;
    return OS_EOK;
}

oserr_t
SHMMap(
    _In_ OSHandle_t*  handle,
    _In_ size_t       offset,
    _In_ size_t       length,
    _In_ unsigned int flags)
{
    assert(offset + length <= __OSHandle(handle->ID)->Size);
    return OS_EOK;
}

void*  SHMBuffer(OSHandle_t* handle) { return __OSHandle(handle->ID)->Buffer; }
size_t SHMBufferLength(OSHandle_t* handle) { return __OSHandle(handle->ID)->Size; }
size_t SHMBufferCapacity(OSHandle_t* handle) { return __OSHandle(handle->ID)->Size; }

void
OSHandleDestroy(
    _In_ struct OSHandle* handle)
{
    struct __TestOSHandle* osHandle = __OSHandle(handle->ID);

    if (!osHandle->Attached) {
        free(osHandle->Buffer);
    }
    osHandle->Used = false;
}

oserr_t
OSHandlesFind(
    _In_ uuid_t           id,
    _In_ struct OSHandle* handle)
{
    if (id == 0 || id >= TEST_MAX_OSHANDLES || !g_osHandles[id].Used) {
        return OS_ENOENT;
    }
    memset(handle, 0, sizeof(*handle));
    handle->ID = id;
    return OS_EOK;
}

oserr_t
OSGrantTokenCreate(
    _In_  uuid_t                     handleID,
    _In_  const GrantTokenOptions_t* options,
    _Out_ GrantToken_t*              tokenOut)
{
    (void)__OSHandle(handleID);
    memset(tokenOut, 0, sizeof(*tokenOut));
    tokenOut->ExportGlobalHandleID = handleID;
    tokenOut->ControlHandleID = __NewOSHandle();
    memcpy(&tokenOut->Options, options, sizeof(GrantTokenOptions_t));
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_ uuid_t controlHandleID)
{
    __OSHandle(controlHandleID)->Used = false;
    return OS_EOK;
}

// The VFS handle table. VFSNodeHandleCreate makes the kernel handle for a
// resource, and the table maps its ID to the node.
static struct VFSNodeHandle g_handles[TEST_MAX_HANDLES];
static bool                 g_handleUsed[TEST_MAX_HANDLES];

oserr_t
VFSNodeHandleCreate(
    _In_  uint32_t    accessKind,
    _Out_ OSHandle_t* handleOut)
{
    uuid_t id = __NewOSHandle();

    g_osHandles[id].Resource = true;
    memset(handleOut, 0, sizeof(*handleOut));
    handleOut->ID = id;
    return OS_EOK;
}

oserr_t
VFSNodeHandleAdd(
    _In_ OSHandle_t*     handle,
    _In_ struct VFSNode* node,
    _In_ void*           data,
    _In_ uint32_t        accessKind)
{
    assert(handle->ID < TEST_MAX_HANDLES);
    if (g_handleUsed[handle->ID]) {
        return OS_EEXISTS;
    }
    memset(&g_handles[handle->ID], 0, sizeof(struct VFSNodeHandle));
    g_handles[handle->ID].Id = handle->ID;
    g_handles[handle->ID].Node = node;
    g_handles[handle->ID].Data = data;
    g_handles[handle->ID].AccessKind = accessKind;
    g_handles[handle->ID].Mode = MODE_NONE;
    g_handleUsed[handle->ID] = true;
    return OS_EOK;
}

oserr_t
VFSNodeHandleRemove(
    _In_ uuid_t handleId)
{
    if (handleId >= TEST_MAX_HANDLES || !g_handleUsed[handleId]) {
        return OS_ENOENT;
    }
    g_handleUsed[handleId] = false;
    return OS_EOK;
}

oserr_t
VFSNodeHandleGet(
    _In_  uuid_t                 handleId,
    _Out_ struct VFSNodeHandle** handleOut)
{
    if (handleId >= TEST_MAX_HANDLES || !g_handleUsed[handleId]) {
        return OS_ENOENT;
    }
    *handleOut = &g_handles[handleId];
    return OS_EOK;
}

void VFSNodeHandlePut(struct VFSNodeHandle* handle) { (void)handle; }

// Filesystem interfaces. memfs gets its interface from VFSInterfaceNew, the
// partitions get theirs from VFSInterfaceLoadInternal.
static int g_interfacesCreated;
static int g_interfacesDeleted;
static int g_instancesCreated;
static int g_instancesDestroyed;

// The label each partition driver reports. NULL means no label, and filed
// then uses p<index>.
static const char* g_labels[TEST_PARTITIONS];
static const char* g_mountPoints[TEST_PARTITIONS];

struct __TestInstance {
    mstring_t* Label;
};

struct VFSInterface*
VFSInterfaceNew(
    _In_ uuid_t                driverID,
    _In_ struct VFSOperations* operations)
{
    struct VFSInterface* interface = calloc(1, sizeof(struct VFSInterface));

    assert(interface != NULL);
    interface->DriverID = driverID;
    usched_mtx_init(&interface->Lock, USCHED_MUTEX_PLAIN);
    if (operations) {
        memcpy(&interface->Operations, operations, sizeof(struct VFSOperations));
    }
    g_interfacesCreated++;
    return interface;
}

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

static int
__PartitionOfSector(
    _In_ uint64_t sector)
{
    int index = (int)(sector / 2048) - 1;

    assert(index >= 0 && index < TEST_PARTITIONS);
    return index;
}

static oserr_t
__TestInitialize(
    _In_  struct VFSInterface*         interface,
    _In_  struct VFSStorageParameters* storageParameters,
    _Out_ void**                       instanceData)
{
    struct __TestInstance* instance = calloc(1, sizeof(struct __TestInstance));
    const char*            label;

    assert(instance != NULL);
    assert(storageParameters->StorageType == VFSSTORAGE_TYPE_DEVICE);
    label = g_labels[__PartitionOfSector(storageParameters->SectorStart.QuadPart)];
    if (label != NULL) {
        instance->Label = mstr_new_u8(label);
        assert(instance->Label != NULL);
    }
    *instanceData = instance;
    g_instancesCreated++;
    return OS_EOK;
}

static oserr_t
__TestDestroy(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ unsigned int         unmountFlags)
{
    struct __TestInstance* instance = instanceData;

    assert(instance != NULL);
    mstr_delete(instance->Label);
    free(instance);
    g_instancesDestroyed++;
    return OS_EOK;
}

static oserr_t
__TestStat(
    _In_ struct VFSInterface* interface,
    _In_ void*                instanceData,
    _In_ struct VFSStatFS*    stat)
{
    struct __TestInstance* instance = instanceData;

    stat->Label = instance->Label;
    return OS_EOK;
}

static struct VFSOperations g_partitionOperations = {
    .Initialize = __TestInitialize,
    .Destroy = __TestDestroy,
    .Stat = __TestStat,
};

oserr_t
VFSInterfaceLoadInternal(
    _In_  const char*           type,
    _Out_ struct VFSInterface** interfaceOut)
{
    assert(type != NULL && !strcmp(type, "mfs"));
    *interfaceOut = VFSInterfaceNew(UUID_INVALID, &g_partitionOperations);
    return OS_EOK;
}

oserr_t VFSInterfaceLoadDriver(uuid_t driverID, struct VFSInterface** interfaceOut) { abort(); }

// Two partitions per disk, set up the way the GPT parser does it. The result
// of each partition is not passed on, so one failed partition does not stop
// the next one.
static int g_parseCount;

oserr_t
VFSStorageParse(
    _In_ struct VFSStorage* storage)
{
    guid_t guid = { 0 };

    g_parseCount++;
    assert(storage->Stats.SectorSize == 512);
    for (int i = 0; i < TEST_PARTITIONS; i++) {
        UInteger64_t sector = { .QuadPart = 2048 * (uint64_t)(i + 1) };
        mstring_t*   mountPoint = NULL;

        if (g_mountPoints[i] != NULL) {
            mountPoint = mstr_new_u8(g_mountPoints[i]);
            assert(mountPoint != NULL);
        }
        (void)VFSStorageRegisterAndSetupPartition(
            storage, i, &sector, &guid, "mfs", &guid, UUID_INVALID, mountPoint);
        mstr_delete(mountPoint);
    }
    return OS_EOK;
}

oserr_t
__StorageTransfer(
    _In_  uuid_t              driverID,
    _In_  uuid_t              deviceID,
    _In_  uuid_t              bufferID,
    _In_  size_t              offset,
    _In_  const UInteger64_t* sector,
    _In_  size_t              sectorSize,
    _In_  size_t              count,
    _In_  bool                write,
    _Out_ size_t*             completed)
{
    abort();
}

// The ctt_storage stat IPC. g_statHook runs once while the reply is awaited.
// g_statSendFails makes the request fail to send, and g_statNoReply makes the
// wait end without a reply. g_statResult is the driver's
// answer and g_statSectorSize the sector size it reports.
static void   (*g_statHook)(void);
static bool   g_statSendFails;
static bool   g_statNoReply;
static oserr_t g_statResult = OS_EOK;
static size_t g_statSectorSize = 512;
static uuid_t g_statDevice;
static int    g_statQueries;
static char   g_statSerial[32];
static char   g_statModel[] = "test-disk";

gracht_client_t* GetGrachtClient(void) { return NULL; }

int
ctt_storage_stat(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t                   deviceId)
{
    g_statQueries++;
    g_statDevice = deviceId;
    return g_statSendFails ? -1 : 0;
}

int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                   flags)
{
    if (g_statHook != NULL) {
        void (*hook)(void) = g_statHook;
        g_statHook = NULL;
        hook();
    }
    return 0;
}

int
gracht_client_get_status(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    return g_statNoReply ? GRACHT_MESSAGE_ERROR : GRACHT_MESSAGE_COMPLETED;
}

int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* context) { return 0; }

int
ctt_storage_stat_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                       result_out,
    _Out_ struct sys_disk_descriptor*    descriptor_out)
{
    memset(descriptor_out, 0, sizeof(*descriptor_out));
    snprintf(g_statSerial, sizeof(g_statSerial), "disk-%u", g_statDevice);
    descriptor_out->device_id = g_statDevice;
    descriptor_out->serial = g_statSerial;
    descriptor_out->model = g_statModel;
    descriptor_out->geometry.sector_size = g_statSectorSize;
    descriptor_out->geometry.sectors_total = 1 << 20;
    *result_out = g_statResult;
    return 0;
}

// Storage ready events, sent for bind mounts and explicit mount points.
static char g_events[TEST_MAX_EVENTS][96];
static int  g_eventCount;

gracht_server_t* __crt_get_service_server(void) { return NULL; }

int
sys_file_event_storage_ready_all(
    _In_ gracht_server_t* server,
    _In_ const char*      path)
{
    assert(g_eventCount < TEST_MAX_EVENTS && strlen(path) < sizeof(g_events[0]));
    strcpy(g_events[g_eventCount++], path);
    return 0;
}

static bool
__EventSent(
    _In_ const char* path)
{
    for (int i = 0; i < g_eventCount; i++) {
        if (!strcmp(g_events[i], path)) {
            return true;
        }
    }
    return false;
}

// Looks up the node at path itself. The last path element is not resolved
// through a bind or mount, which is what VFSNodeOpen and VFSNodeUnlink do.
static struct VFSNode*
__FindNode(
    _In_ const char* cpath)
{
    struct VFSNode* parent;
    struct VFSNode* node = NULL;
    mstring_t*      path = mstr_path_new_u8(cpath);
    mstring_t*      parentPath;
    mstring_t*      name;
    oserr_t         oserr;

    assert(path != NULL);
    parentPath = mstr_path_dirname(path);
    name = mstr_path_basename(path);
    mstr_delete(path);
    assert(parentPath != NULL && name != NULL);

    oserr = VFSNodeGet(VFSScopeGet(UUID_INVALID), parentPath, 1, &parent);
    if (oserr == OS_EOK) {
        if (VFSNodeFind(parent, name, &node) != OS_EOK) {
            node = NULL;
        }
        VFSNodePut(parent);
    }
    mstr_delete(parentPath);
    mstr_delete(name);
    return node;
}

// Resolves path the way a client open does, through binds and mounts.
static struct VFSNode*
__ResolvePath(
    _In_ const char* cpath)
{
    struct VFSNode* node;
    mstring_t*      path = mstr_path_new_u8(cpath);
    oserr_t         oserr;

    assert(path != NULL);
    oserr = VFSNodeGet(VFSScopeGet(UUID_INVALID), path, 1, &node);
    mstr_delete(path);
    if (oserr != OS_EOK) {
        return NULL;
    }
    VFSNodePut(node);
    return node;
}

static void
__MakeDirectory(
    _In_ const char* cpath)
{
    struct VFSNode* node;
    mstring_t*      path = mstr_path_new_u8(cpath);

    assert(path != NULL);
    assert(VFSNodeNewDirectory(VFSScopeGet(UUID_INVALID), path, FILE_PERMISSION_READ, &node) == OS_EOK);
    mstr_delete(path);
}

static void
__RemoveDirectory(
    _In_ const char* cpath)
{
    assert(VFSNodeUnlink(VFSScopeGet(UUID_INVALID), cpath) == OS_EOK);
}

// A directory with nothing on it: no bind or mount, not bound anywhere, and no
// open handles.
static bool
__IsPlainDirectory(
    _In_ struct VFSNode* node)
{
    return node != NULL && __NodeIsRegular(node) && node->Mounts.element_count == 0 &&
           node->Handles.element_count == 0;
}

// The filesystem of partition index on the disk.
static FileSystem_t*
__FileSystemOf(
    _In_ uuid_t deviceID,
    _In_ int    index)
{
    struct VFSStorage* storage = __FindRegisteredDisk(1, deviceID);
    element_t*         i;

    if (storage == NULL) {
        return NULL;
    }
    _foreach(i, &storage->Filesystems) {
        FileSystem_t* fileSystem = i->value;
        if (fileSystem->PartitionIndex == index) {
            return fileSystem;
        }
    }
    return NULL;
}

static struct VFSStorage*
__FindDisk(
    _In_ uuid_t deviceID)
{
    return __FindRegisteredDisk(1, deviceID);
}

static void
__Register(
    _In_ uuid_t deviceID)
{
    int resultsBefore = g_registerResults;

    sys_storage_register_invocation(NULL, 1, deviceID, SYS_STORAGE_FLAGS_REMOVABLE);
    assert(g_registerResults == resultsBefore + 1);
}

static void
__Unregister(
    _In_ uuid_t deviceID)
{
    int resultsBefore = g_unregisterResults;

    sys_storage_unregister_invocation(NULL, 1, deviceID, 0);
    assert(g_unregisterResults == resultsBefore + 1);
}

static uuid_t g_hookDevice;
static bool   g_hookRan;

static void
__UnregisterHook(void)
{
    g_hookRan = true;
    __Unregister(g_hookDevice);
}

// Number of entries directly in the directory at path.
static int
__ChildCount(
    _In_ const char* cpath)
{
    struct VFSNode* node = __ResolvePath(cpath);

    assert(node != NULL);
    return (int)node->Children.element_count;
}

// Checks that nothing from any registration is left: no disks, /storage is
// empty, the root holds only the entries the test expects, no VFS handle is
// open, and every interface, driver instance and identifier is released.
static void
__CheckNothingLeft(
    _In_ int rootEntries)
{
    struct VFSNode* storage = __FindNode("/storage");

    assert(list_count(&g_disks) == 0);
    assert(__IsPlainDirectory(storage) && storage->Children.element_count == 0);
    assert(__ChildCount("/") == rootEntries);
    assert(__ResourceHandles() == 0);
    assert(g_interfacesCreated == g_interfacesDeleted + 1);
    assert(g_instancesCreated == g_instancesDestroyed);
    for (int i = 0; i < __FILEMANAGER_MAXDISKS; i++) {
        assert(g_diskTable[i] == 0);
    }
}

// Checks that path is bound to the default mount of the partition, so a
// lookup of path ends in the root of that partition's filesystem.
static void
__CheckBound(
    _In_ const char* path,
    _In_ uuid_t      deviceID,
    _In_ int         index)
{
    FileSystem_t*   fileSystem = __FileSystemOf(deviceID, index);
    struct VFSNode* target = __FindNode(path);

    assert(fileSystem != NULL && fileSystem->State == FileSystemState_MOUNTED);
    assert(target != NULL && __NodeIsBindMount(target));
    assert(__ResolvePath(path) == fileSystem->VFS->Root);
}

static void
__ResetLabels(void)
{
    for (int i = 0; i < TEST_PARTITIONS; i++) {
        g_labels[i] = NULL;
        g_mountPoints[i] = NULL;
    }
}

static void
__TestDefaultBind(void)
{
    // In a normal boot the root scope only holds /storage (scope.c), so the
    // first disk with a vali-boot partition creates /boot for its bind.
    assert(__FindNode("/boot") == NULL);
    assert(__ChildCount("/") == 1);

    // Unregister must undo the bind and remove /boot, or the default mount
    // directory stays bound and the disk cannot be registered again.
    __ResetLabels();
    g_labels[0] = "vali-boot";
    for (int round = 0; round < 3; round++) {
        g_eventCount = 0;
        __Register(4);
        assert(__FindDisk(4)->State == VFSSTORAGE_STATE_CONNECTED);
        __CheckBound("/boot", 4, 0);
        assert(__EventSent("/boot"));
        assert(__FindNode("/storage/disk-4/p1") != NULL);
        assert(__ChildCount("/") == 2);

        __Unregister(4);
        assert(__FindNode("/boot") == NULL);
        __CheckNothingLeft(1);
    }
    assert(g_errors == 0);
    puts("filed storage bind: default bind, unregister and register again passed");
}

static void
__TestExistingTarget(void)
{
    struct VFSNode* data;

    // /data exists before the disk arrives, for example made by a user.
    // The bind uses it, and unregister leaves it in place, unbound and with
    // no handle open on it.
    __MakeDirectory("/data");
    __ResetLabels();
    g_labels[1] = "vali-data";
    for (int round = 0; round < 2; round++) {
        __Register(5);
        __CheckBound("/data", 5, 1);
        __Unregister(5);
        data = __FindNode("/data");
        assert(__IsPlainDirectory(data));
        __CheckNothingLeft(2);
    }
    __RemoveDirectory("/data");
    __CheckNothingLeft(1);
    assert(g_errors == 0);
    puts("filed storage bind: bind target that already exists passed");
}

static void
__TestSameLabels(void)
{
    FileSystem_t*   second;
    struct VFSNode* boot;

    // Two disks carry the same labels. The first one keeps /boot and /data.
    // The binds of the second one fail with a logged error, and its default
    // mounts still work.
    __ResetLabels();
    g_labels[0] = "vali-boot";
    g_labels[1] = "vali-data";
    __Register(10);
    __CheckBound("/boot", 10, 0);
    __CheckBound("/data", 10, 1);
    assert(g_errors == 0);

    __Register(11);
    assert(g_bindErrors == 2);
    __CheckBound("/boot", 10, 0);
    __CheckBound("/data", 10, 1);
    for (int i = 0; i < TEST_PARTITIONS; i++) {
        second = __FileSystemOf(11, i);
        assert(second != NULL && second->State == FileSystemState_MOUNTED);
        assert(second->BindPath == NULL);
        assert(__ResolvePath(i == 0 ? "/storage/disk-11/vali-boot" : "/storage/disk-11/vali-data")
               == second->VFS->Root);
    }

    // The first disk goes. Its binds and /boot and /data go with it, and the
    // second disk keeps its default mounts. It does not take over the paths.
    __Unregister(10);
    assert(__FindNode("/boot") == NULL && __FindNode("/data") == NULL);
    assert(__ResolvePath("/storage/disk-11/vali-boot") == __FileSystemOf(11, 0)->VFS->Root);
    __Unregister(11);
    __CheckNothingLeft(1);

    // The first disk can come back with its binds.
    g_errors = 0;
    g_bindErrors = 0;
    __Register(10);
    __CheckBound("/boot", 10, 0);
    boot = __FindNode("/boot");
    assert(boot != NULL);
    __Unregister(10);
    __CheckNothingLeft(1);
    assert(g_errors == 0);
    puts("filed storage bind: second disk with the same labels passed");
}

static void
__TestExplicitMountPoint(void)
{
    // An explicit mount point is bound like a default path. Its parent
    // directory /mnt is not filed's and stays.
    __MakeDirectory("/mnt");
    __ResetLabels();
    g_mountPoints[0] = "/mnt/usb";
    for (int round = 0; round < 2; round++) {
        g_eventCount = 0;
        __Register(6);
        __CheckBound("/mnt/usb", 6, 0);
        assert(__EventSent("/storage/disk-6/p0"));
        __Unregister(6);
        assert(__FindNode("/mnt/usb") == NULL);
        assert(__IsPlainDirectory(__FindNode("/mnt")));
        __CheckNothingLeft(2);
    }
    __RemoveDirectory("/mnt");
    __CheckNothingLeft(1);
    assert(g_errors == 0);
    puts("filed storage bind: explicit mount point passed");
}

static void
__TestUnregisterDuringStat(void)
{
    int parses = g_parseCount;

    // The unregister arrives while registration waits for the stat reply.
    // It must find the storage, and registration must then tear it down
    // without mounting or parsing the disk.
    __ResetLabels();
    g_labels[0] = "vali-boot";
    g_hookDevice = 7;
    g_hookRan = false;
    g_statHook = __UnregisterHook;
    __Register(7);
    assert(g_hookRan);
    assert(g_unknownDeviceWarnings == 0);
    assert(g_parseCount == parses);
    assert(__FindDisk(7) == NULL && __FindNode("/boot") == NULL);
    __CheckNothingLeft(1);

    // The disk can be registered again afterwards.
    __Register(7);
    __CheckBound("/boot", 7, 0);
    __Unregister(7);
    __CheckNothingLeft(1);
    assert(g_errors == 0 && g_unknownDeviceWarnings == 0);
    puts("filed storage bind: unregister during the stat query passed");
}

static void
__TestStatFailure(void)
{
    // The driver answers the stat with an error, the request cannot be sent,
    // no reply arrives, or the disk reports a sector size of zero. Each time
    // nothing is left behind, and only the failed query is logged as an error.
    __ResetLabels();
    g_labels[0] = "vali-boot";
    for (int kind = 0; kind < 4; kind++) {
        g_statResult = kind == 0 ? OS_EUNKNOWN : OS_EOK;
        g_statSendFails = kind == 1;
        g_statNoReply = kind == 2;
        g_statSectorSize = kind == 3 ? 0 : 512;
        g_errors = 0;
        __Register(8);
        assert(g_errors == 1);
        assert(__FindDisk(8) == NULL);
        __CheckNothingLeft(1);
    }
    g_statResult = OS_EOK;
    g_statSendFails = false;
    g_statNoReply = false;
    g_statSectorSize = 512;

    // A failed stat with an unregister that arrived while it waited.
    g_errors = 0;
    g_statResult = OS_EUNKNOWN;
    g_hookDevice = 8;
    g_hookRan = false;
    g_statHook = __UnregisterHook;
    __Register(8);
    g_statResult = OS_EOK;
    assert(g_hookRan && g_unknownDeviceWarnings == 0 && g_errors == 1);
    assert(__FindDisk(8) == NULL);
    __CheckNothingLeft(1);

    // A later registration of the disk works.
    g_errors = 0;
    __Register(8);
    __CheckBound("/boot", 8, 0);
    __Unregister(8);
    __CheckNothingLeft(1);
    assert(g_errors == 0);
    puts("filed storage bind: stat failures passed");
}

int
main(void)
{
    // A failed check aborts, so keep the output of the passed parts.
    setvbuf(stdout, NULL, _IONBF, 0);
    VFSStorageStartup();
    VFSFileSystemStartup();
    VFSScopeStartup();
    assert(VFSScopeGet(UUID_INVALID) != NULL);
    assert(g_errors == 0);

    __TestDefaultBind();
    __TestExistingTarget();
    __TestSameLabels();
    __TestExplicitMountPoint();
    __TestUnregisterDuringStat();
    __TestStatFailure();
    return 0;
}
