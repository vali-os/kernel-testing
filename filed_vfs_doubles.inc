// Doubles shared by the filed VFS suites (filed_vfs_teardown_test,
// filed_vfs_move_test and filed_driver_ops_test). They stand in for the parts
// of filed and the system that the real VFS code calls:
// - libds allocation and mstr_fmt.
// - usched mutexes, reader-writer locks and conditions. They block for real,
//   built on one pthread mutex and condition variable. Each request runs on
//   its own thread, the way a usched job would run on its own stack.
// - Kernel services: handles, SHM buffers, grant tokens and time.
// - The VFS handle table (vfs/handles.c). A flat table with the same Get, Put,
//   Add and Remove rules. The real one needs the kernel handle services.
// - Filesystem interfaces, as interfaces/modules.c makes them.
// - Storage ready events, which are only counted in g_events.
// - The root scope (scope.c). VFSScopeGet returns a memfs VFS created the way
//   scope.c creates it, with the same /storage directory.
// The including test defines OSSysLogWrite, and includes filesystem.c and the
// private VFS header first. Kernel handle IDs are never reused, so a test that
// makes many handles can raise TEST_MAX_OSHANDLES before the include.

#ifndef TEST_MAX_OSHANDLES
#define TEST_MAX_OSHANDLES 512
#endif
#ifndef TEST_MAX_HANDLES
#define TEST_MAX_HANDLES 256
#endif

void* dsalloc(size_t size) { return malloc(size); }
void  dsfree(void* memory) { free(memory); }

// Supports the conversions filed uses in paths and labels: %s, %i, %u and %ms.
// A NULL mstring prints as <null>, like in libds.
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
            // The real mstr_fmt prints a NULL string as <null>.
            mstring_t* string = va_arg(arguments, mstring_t*);
            char*      text = string != NULL ? mstr_u8(string) : strdup("<null>");
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

// usched locks. All lock state is changed under g_schedulerLock, and every
// change wakes all waiters through g_schedulerChanged. A mutex is held while
// references is 1. A reader-writer lock keeps its reader count in readers and
// marks a writer in sync_object.references. A condition counts its
// notifications in the queue pointer, so a waiter sees a new notification as
// a changed value.
static pthread_mutex_t g_schedulerLock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_schedulerChanged = PTHREAD_COND_INITIALIZER;

void spinlock_init(spinlock_t* lock) { (void)lock; }
void spinlock_acquire(spinlock_t* lock) { (void)lock; }
void spinlock_release(spinlock_t* lock) { (void)lock; }
void usched_job_yield(void) { sched_yield(); }

void usched_mtx_init(struct usched_mtx* mutex, int type) { memset(mutex, 0, sizeof(*mutex)); }

void
usched_mtx_lock(struct usched_mtx* mutex)
{
    pthread_mutex_lock(&g_schedulerLock);
    while (mutex->references != 0) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    mutex->references = 1;
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    pthread_mutex_lock(&g_schedulerLock);
    assert(mutex->references == 1);
    mutex->references = 0;
    pthread_cond_broadcast(&g_schedulerChanged);
    pthread_mutex_unlock(&g_schedulerLock);
}

void usched_rwlock_init(struct usched_rwlock* lock) { memset(lock, 0, sizeof(*lock)); }

void
usched_rwlock_r_lock(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    while (lock->sync_object.references != 0) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    lock->readers++;
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_rwlock_r_unlock(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    assert(lock->readers > 0);
    lock->readers--;
    pthread_cond_broadcast(&g_schedulerChanged);
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_rwlock_w_lock(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    while (lock->sync_object.references != 0 || lock->readers != 0) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    lock->sync_object.references = 1;
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_rwlock_w_unlock(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    assert(lock->sync_object.references == 1);
    lock->sync_object.references = 0;
    pthread_cond_broadcast(&g_schedulerChanged);
    pthread_mutex_unlock(&g_schedulerLock);
}

// The caller holds one reader lock, which becomes the write lock once no
// other reader or writer is left.
void
usched_rwlock_w_promote(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    assert(lock->readers > 0);
    lock->readers--;
    while (lock->sync_object.references != 0 || lock->readers != 0) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    lock->sync_object.references = 1;
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_rwlock_w_demote(struct usched_rwlock* lock)
{
    pthread_mutex_lock(&g_schedulerLock);
    assert(lock->sync_object.references == 1);
    lock->sync_object.references = 0;
    lock->readers++;
    pthread_cond_broadcast(&g_schedulerChanged);
    pthread_mutex_unlock(&g_schedulerLock);
}

void usched_cnd_init(struct usched_cnd* condition) { memset(condition, 0, sizeof(*condition)); }

void
usched_cnd_wait(
    struct usched_cnd* condition,
    struct usched_mtx* mutex)
{
    uintptr_t notifications;

    pthread_mutex_lock(&g_schedulerLock);
    assert(mutex->references == 1);
    notifications = (uintptr_t)condition->queue;
    mutex->references = 0;
    pthread_cond_broadcast(&g_schedulerChanged);
    while ((uintptr_t)condition->queue == notifications) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    while (mutex->references != 0) {
        pthread_cond_wait(&g_schedulerChanged, &g_schedulerLock);
    }
    mutex->references = 1;
    pthread_mutex_unlock(&g_schedulerLock);
}

void
usched_cnd_notify_all(struct usched_cnd* condition)
{
    pthread_mutex_lock(&g_schedulerLock);
    condition->queue = (struct usched_wait*)((uintptr_t)condition->queue + 1);
    pthread_cond_broadcast(&g_schedulerChanged);
    pthread_mutex_unlock(&g_schedulerLock);
}

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
// its token was made for and does not free it. Handles are made from several
// threads, so they are taken under g_osHandlesLock.
struct __TestOSHandle {
    bool   Used;
    bool   Resource;
    bool   Attached;
    void*  Buffer;
    size_t Size;
};
static struct __TestOSHandle g_osHandles[TEST_MAX_OSHANDLES];
static uuid_t                g_nextOSHandle = 1;
static pthread_mutex_t       g_osHandlesLock = PTHREAD_MUTEX_INITIALIZER;

static uuid_t
__NewOSHandle(void)
{
    uuid_t id;

    pthread_mutex_lock(&g_osHandlesLock);
    id = g_nextOSHandle++;
    assert(id < TEST_MAX_OSHANDLES);
    memset(&g_osHandles[id], 0, sizeof(struct __TestOSHandle));
    g_osHandles[id].Used = true;
    pthread_mutex_unlock(&g_osHandlesLock);
    return id;
}

static struct __TestOSHandle*
__OSHandle(
    _In_ uuid_t id)
{
    assert(id > 0 && id < TEST_MAX_OSHANDLES && g_osHandles[id].Used);
    return &g_osHandles[id];
}

// The number of SHMCreate calls that fail with OS_EOOM, before the next
// ones succeed again.
static int g_shmCreateFailures;

oserr_t
SHMCreate(
    _In_  SHM_t*      shm,
    _Out_ OSHandle_t* handleOut)
{
    uuid_t id;

    if (g_shmCreateFailures > 0) {
        g_shmCreateFailures--;
        return OS_EOOM;
    }

    id = __NewOSHandle();

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

// A handle that was never made (UUID_INVALID) is ignored. The real one only
// logs a warning for it, as VFSDestroy of a VFS that VFSNew could not finish
// destroys its transfer buffer without checking.
void
OSHandleDestroy(
    _In_ struct OSHandle* handle)
{
    struct __TestOSHandle* osHandle;

    if (handle->ID == UUID_INVALID) {
        return;
    }

    osHandle = __OSHandle(handle->ID);

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
    struct __TestOSHandle* source = __OSHandle(handleID);
    uuid_t                 id = __NewOSHandle();

    // The export is its own handle, which shares the buffer of the handle it
    // was made for, and is also the handle that revokes it. This is what
    // __GrantTokenFieldsValid checks before a token is sent to a driver.
    g_osHandles[id].Attached = true;
    g_osHandles[id].Buffer = source->Buffer;
    g_osHandles[id].Size = source->Size;
    memset(tokenOut, 0, sizeof(*tokenOut));
    tokenOut->Version = GRANT_TOKEN_VERSION;
    tokenOut->ExportGlobalHandleID = id;
    tokenOut->ControlHandleID = id;
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
// resource, and the table maps its ID to the node. The table is changed under
// g_handlesLock, like the reader-writer lock of the real table.
static struct VFSNodeHandle g_handles[TEST_MAX_HANDLES];
static bool                 g_handleUsed[TEST_MAX_HANDLES];
static pthread_mutex_t      g_handlesLock = PTHREAD_MUTEX_INITIALIZER;

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
    oserr_t oserr = OS_EOK;

    assert(handle->ID < TEST_MAX_HANDLES);
    pthread_mutex_lock(&g_handlesLock);
    if (g_handleUsed[handle->ID]) {
        oserr = OS_EEXISTS;
    } else {
        memset(&g_handles[handle->ID], 0, sizeof(struct VFSNodeHandle));
        g_handles[handle->ID].Id = handle->ID;
        g_handles[handle->ID].Node = node;
        g_handles[handle->ID].Data = data;
        g_handles[handle->ID].AccessKind = accessKind;
        g_handles[handle->ID].Mode = MODE_NONE;
        g_handleUsed[handle->ID] = true;
    }
    pthread_mutex_unlock(&g_handlesLock);
    return oserr;
}

oserr_t
VFSNodeHandleRemove(
    _In_ uuid_t handleId)
{
    oserr_t oserr = OS_EOK;

    pthread_mutex_lock(&g_handlesLock);
    if (handleId >= TEST_MAX_HANDLES || !g_handleUsed[handleId]) {
        oserr = OS_ENOENT;
    } else {
        g_handleUsed[handleId] = false;
    }
    pthread_mutex_unlock(&g_handlesLock);
    return oserr;
}

oserr_t
VFSNodeHandleGet(
    _In_  uuid_t                 handleId,
    _Out_ struct VFSNodeHandle** handleOut)
{
    oserr_t oserr = OS_EOK;

    pthread_mutex_lock(&g_handlesLock);
    if (handleId >= TEST_MAX_HANDLES || !g_handleUsed[handleId]) {
        oserr = OS_ENOENT;
    } else {
        *handleOut = &g_handles[handleId];
    }
    pthread_mutex_unlock(&g_handlesLock);
    return oserr;
}

void VFSNodeHandlePut(struct VFSNodeHandle* handle) { (void)handle; }

static int
__OpenHandles(void)
{
    int count = 0;

    pthread_mutex_lock(&g_handlesLock);
    for (int i = 0; i < TEST_MAX_HANDLES; i++) {
        count += g_handleUsed[i] ? 1 : 0;
    }
    pthread_mutex_unlock(&g_handlesLock);
    return count;
}

// Filesystem interfaces, as interfaces/modules.c makes them.
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
    return interface;
}

void
VFSInterfaceDelete(
    _In_ struct VFSInterface* interface)
{
    free(interface);
}

// Storage ready events. Only counted.
static int g_events;

gracht_server_t* __crt_get_service_server(void) { return NULL; }

int
sys_file_event_storage_ready_all(
    _In_ gracht_server_t* server,
    _In_ const char*      path)
{
    g_events++;
    return 0;
}

// The root scope, set up like scope.c does it: a memfs VFS on a memory
// storage, with the /storage directory.
static struct VFS*       g_rootScope;
static struct VFSStorage g_rootStorage;

struct VFS*
VFSScopeGet(
    _In_ uuid_t processId)
{
    return g_rootScope;
}

static void
__MakeDirectory(
    _In_ const char* cpath)
{
    struct VFSNode* node;
    mstring_t*      path = mstr_path_new_u8(cpath);

    assert(path != NULL);
    assert(VFSNodeNewDirectory(g_rootScope, path, FILE_PERMISSION_READ, &node) == OS_EOK);
    mstr_delete(path);
}

static void
__CreateRootScope(void)
{
    struct VFSStorageParameters parameters = { 0 };
    struct VFSInterface*        interface = MemFSNewInterface();
    guid_t                      guid = GUID_EMPTY;
    void*                       data = NULL;

    assert(interface != NULL);
    assert(interface->Operations.Initialize(interface, &parameters, &data) == OS_EOK);
    assert(VFSNew(UUID_INVALID, &guid, &g_rootStorage, interface, data, &g_rootScope) == OS_EOK);
    __MakeDirectory("/storage");
}
