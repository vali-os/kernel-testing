// Production VFS tables and RPC handlers; kernel operations are deterministic
// doubles here. Kernel grant/refcount behavior has its own provider tests.
#define MB(value) ((value) * 1024 * 1024)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#ifndef FILED_HANDLES_SOURCE
#define FILED_HANDLES_SOURCE "../services/filed/vfs/handles.c"
#endif
#ifndef FILED_API_SOURCE
#define FILED_API_SOURCE "../services/filed/api.c"
#endif
#include FILED_HANDLES_SOURCE
#include "../services/filed/vfs/handlers/close.c"
#include "../services/filed/vfs/handlers/read.c"
#include "../services/filed/vfs/handlers/write.c"
#include "../services/filed/vfs/handlers/handle_stat.c"
#include FILED_API_SOURCE
#define __HandlesHash __NodeHandlesHash
#define __HandlesCmp __NodeHandlesCmp
#ifndef FILED_VFS_SOURCE
#define FILED_VFS_SOURCE "../services/filed/vfs/vfs.c"
#endif
#include FILED_VFS_SOURCE
#undef __HandlesHash
#undef __HandlesCmp
#ifndef FILED_FILESYSTEM_SOURCE
#define FILED_FILESYSTEM_SOURCE "../services/filed/filesystem.c"
#endif
#include FILED_FILESYSTEM_SOURCE

static struct gracht_security_context g_caller = {.source = GRACHT_SECURITY_KERNEL,
                                                  .principal = 77};
static struct VFS g_vfs;
static struct VFSNode g_node;
static struct VFSInterface g_interface;
static OSHandle_t g_initial;
static bool g_initialLive;
static bool g_tokenLive;
static oserr_t g_tokenError;
static oserr_t g_openError;
static oserr_t g_response;
static uint32_t g_access;
static uint32_t g_required;
static int g_backendCloses;
static int g_backendReads;
static int g_backendWrites;
static int g_revokes;
static int g_initialReleases;
static int g_sendError;
static uint64_t g_group;
static uuid_t g_reference;
static GrantToken_t g_token;
static struct sys_grant_token g_wire;
static unsigned int g_cleanupCount;
static jmp_buf g_jobExit;
static struct test_tls g_tls;
static struct VFSNode *g_closingNode;
static bool g_backendDestroyed;
void
mstr_delete(mstring_t *value)
{
    assert(value == NULL);
}
void *
dsalloc(size_t size)
{
    return malloc(size);
}
void
dsfree(void *memory)
{
    free(memory);
}
uuid_t
usched_notification_handle(void)
{
    return UUID_INVALID;
}
void
SystemDebug(enum OSSysLogLevel level, const char *format, ...)
{
}

void
usched_rwlock_init(struct usched_rwlock *lock)
{
    memset(lock, 0, sizeof(*lock));
}
void
usched_rwlock_r_lock(struct usched_rwlock *lock)
{
    lock->readers++;
}
void
usched_rwlock_r_unlock(struct usched_rwlock *lock)
{
    assert(lock->readers > 0);
    lock->readers--;
}
void
usched_rwlock_w_lock(struct usched_rwlock *lock)
{
    assert(lock->readers == 0);
}
void
usched_rwlock_w_unlock(struct usched_rwlock *lock)
{
    assert(lock->readers == 0);
}
void
usched_rwlock_w_promote(struct usched_rwlock *lock)
{
    assert(lock->readers == 1);
    lock->readers--;
}
void
usched_rwlock_w_demote(struct usched_rwlock *lock)
{
    assert(lock->readers == 0);
    lock->readers++;
}
void
usched_mtx_lock(struct usched_mtx *lock)
{
    assert(!lock->references);
    lock->references = 1;
}
void
usched_mtx_unlock(struct usched_mtx *lock)
{
    assert(lock->references == 1);
    lock->references = 0;
}
struct test_tls *
__tls_current(void)
{
    return &g_tls;
}
void
usched_job_yield(void)
{
    // Resume a close that removed its global record before teardown found it.
    if (g_closingNode != NULL) {
        assert(!g_backendDestroyed);
        g_backendCloses++;
        hashtable_remove(&g_closingNode->Handles, &(struct __VFSHandle){.Id = 100});
        g_closingNode = NULL;
    }
}
oserr_t
OSNotificationQueueWait(OSHandle_t *q,
                        struct ioset_event *events,
                        int max,
                        int poll,
                        OSTimestamp_t *deadline,
                        int *count,
                        OSAsyncContext_t *async)
{
    if (!g_cleanupCount)
        longjmp(g_jobExit, 1);
    *count = 1;
    return OS_EOK;
}
oserr_t
OSHandleReadCleanup(OSHandle_t *registration, uuid_t *ids, uint32_t capacity, uint32_t *count)
{
    assert(capacity == 32);
    *count = g_cleanupCount;
    for (unsigned int i = 0; i < g_cleanupCount; i++)
        ids[i] = 100;
    g_cleanupCount = 0;
    return OS_EOK;
}
oserr_t
OSHandleCreate(OSHandle_t *registration, uint32_t access, OSHandle_t *result)
{
    assert(registration == &g_registration);
    g_access = access;
    g_initial = (OSHandle_t){.ID = 100};
    g_initialLive = true;
    *result = g_initial;
    return OS_EOK;
}
oserr_t
OSHandlesFind(uuid_t id, OSHandle_t *result)
{
    if (id != 100 || !g_initialLive)
        return OS_ENOENT;
    *result = g_initial;
    return OS_EOK;
}
oserr_t
OSHandleRelease(OSHandle_t *handle)
{
    assert(handle->ID == 100 && g_initialLive);
    g_initialLive = false;
    g_initialReleases++;
    return OS_EOK;
}
void
OSHandleDestroy(OSHandle_t *handle)
{
    if (handle->ID != UUID_INVALID)
        assert(OSHandleRelease(handle) == OS_EOK);
}
oserr_t
OSGrantTokenCreate(uuid_t id, const GrantTokenOptions_t *options, GrantToken_t *result)
{
    assert(id == 100 && g_initialLive);
    if (g_tokenError != OS_EOK)
        return g_tokenError;
    assert(options->Flags == GrantTokenFlagsSingleUse);
    assert(options->TargetType == GrantTokenTargetTypeSpecific);
    assert(options->TargetGlobalHandleID == 77);
    assert(options->Scope == g_access);
    assert(options->Data.Words[0] == 0 && options->Data.Words[1] == 0);
    g_token = (GrantToken_t){.Version = GRANT_TOKEN_VERSION,
                             .ExportGlobalHandleID = 200,
                             .ControlHandleID = 200,
                             .Options = *options};
    g_tokenLive = true;
    *result = g_token;
    return OS_EOK;
}
oserr_t
OSGrantTokenRevoke(uuid_t id)
{
    assert(id == 200);
    if (!g_tokenLive)
        return OS_ENOENT;
    g_tokenLive = false;
    g_revokes++;
    return OS_EOK;
}
oserr_t
OSHandleValidate(
    OSHandle_t *registration, uint64_t group, uuid_t ref, uint32_t required, uuid_t *resource)
{
    g_required = required;
    g_group = group;
    g_reference = ref;
    if (group != 77 || ref != 300 || (g_access & required) != required)
        return OS_EPERMISSIONS;
    *resource = 100;
    return OS_EOK;
}
const struct gracht_security_context *
gracht_server_get_security_context(const struct gracht_message *message)
{
    return &g_caller;
}
struct VFS *
VFSScopeGet(uuid_t id)
{
    return &g_vfs;
}
oserr_t
VFSNodeOpen(struct VFS *vfs, const char *path, uint32_t options, uint32_t permissions, uuid_t *id)
{
    if (g_openError != OS_EOK)
        return g_openError;
    assert(VFSNodeHandleCreate(options, &g_initial) == OS_EOK);
    assert(VFSNodeHandleAdd(&g_initial, &g_node, (void *)1234, options) == OS_EOK);
    hashtable_set(&g_node.Handles, &(struct __VFSHandle){.Id = 100, .AccessKind = options});
    *id = 100;
    return OS_EOK;
}
static oserr_t
__TestClose(struct VFSInterface *interface, void *instance, void *data)
{
    assert(data == (void *)1234 && !g_backendDestroyed);
    g_backendCloses++;
    return OS_EOK;
}
static oserr_t
__TestRead(struct VFSInterface *interface,
           void *instance,
           void *data,
           const SHMTransfer_t *transfer,
           size_t *bytes)
{
    assert(data == (void *)1234);
    g_backendReads++;
    *bytes = 8;
    return OS_EOK;
}
static oserr_t
__TestWrite(struct VFSInterface *interface,
            void *instance,
            void *data,
            const SHMTransfer_t *transfer,
            size_t *bytes)
{
    assert(data == (void *)1234);
    g_backendWrites++;
    *bytes = 8;
    return OS_EOK;
}
int
sys_file_open_response(struct gracht_message *message,
                       oserr_t result,
                       struct sys_grant_token *token)
{
    g_response = result;
    g_wire = *token;
    return g_sendError;
}
int
sys_file_transfer_response(struct gracht_message *message, oserr_t result, size_t count)
{
    g_response = result;
    return 0;
}
int
sys_file_get_position_response(struct gracht_message *message,
                               oserr_t result,
                               unsigned int low,
                               unsigned int high)
{
    g_response = result;
    assert(result != OS_EOK || low == 8);
    return 0;
}
int
sys_file_set_access_response(struct gracht_message *message, oserr_t result)
{
    g_response = result;
    return 0;
}
static uint64_t
__TestHash(const void *entry)
{
    return ((const struct __VFSHandle *)entry)->Id;
}
static int
__TestCompare(const void *a, const void *b)
{
    return ((const struct __VFSHandle *)a)->Id == ((const struct __VFSHandle *)b)->Id ? 0 : 1;
}

static oserr_t
__TestDestroyBackend(struct VFSInterface *interface, void *data, unsigned int flags)
{
    assert(g_closingNode == NULL && g_backendCloses == 5);
    assert(data == (void *)5678);
    g_backendDestroyed = true;
    return OS_EOK;
}

int
main(void)
{
    FileSystem_t filesystem = {0};
    struct VFS *teardown;
    GrantToken_t token;
    struct VFSNodeHandle *record;
    struct sys_shm_transfer wire;
    SHMTransfer_t transfer = {
        .Token = {.Version = GRANT_TOKEN_VERSION,
                  .ControlHandleID = 10,
                  .ExportGlobalHandleID = 10,
                  .Options = {.ResourceType = GrantTokenResourceTypeSHM,
                              .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
                              .TargetType = GrantTokenTargetTypeAny,
                              .Data.SHM = {.Length = 4096}}},
        .RangeMode = SHMTransferRangeFull};
    g_vfs.Interface = &g_interface;
    g_node.FileSystem = &g_vfs;
    g_node.Stats.Flags = FILE_FLAG_FILE;
    g_interface.Operations.Close = __TestClose;
    g_interface.Operations.Read = __TestRead;
    g_interface.Operations.Write = __TestWrite;
    assert(hashtable_construct(
               &g_handles, 0, sizeof(struct VFSNodeHandle), __HandlesHash, __HandlesCmp) == 0);
    assert(hashtable_construct(
               &g_node.Handles, 0, sizeof(struct __VFSHandle), __TestHash, __TestCompare) == 0);
    sys_file_open_invocation(NULL, 999, "/file", __FILE_READ_ACCESS | __FILE_READ_SHARE, 0);
    assert(g_response == OS_EOK && !g_initialLive && g_initialReleases == 1);
    assert(__FromSysGrantToken(&g_wire, &token) == OS_EOK);
    assert(token.Options.Scope == __FILE_READ_ACCESS);
    assert(VFSNodeHandleGet(100, &record) == OS_EOK);
    assert(record->TokenControlHandleID == 200);
    VFSNodeHandlePut(record);

    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    sys_file_transfer_invocation(NULL, 999, 300, SYS_TRANSFER_DIRECTION_READ, &wire);
    assert(g_response == OS_EOK && g_backendReads == 1 && g_group == 77 && g_reference == 300);
    sys_file_get_position_invocation(NULL, 999, 300);
    assert(g_response == OS_EOK && g_required == 0);
    sys_file_transfer_invocation(NULL, 77, 300, SYS_TRANSFER_DIRECTION_WRITE, &wire);
    assert(g_response == OS_EPERMISSIONS && g_backendWrites == 0 &&
           g_required == __FILE_WRITE_ACCESS);
    sys_file_transfer_invocation(NULL, 77, 100, SYS_TRANSFER_DIRECTION_READ, &wire);
    assert(g_response == OS_EPERMISSIONS && g_backendReads == 1);
    g_caller.principal = 88;
    sys_file_transfer_invocation(NULL, 77, 300, SYS_TRANSFER_DIRECTION_READ, &wire);
    assert(g_response == OS_EPERMISSIONS && g_backendReads == 1);
    g_caller.principal = 77;
    sys_file_set_access_invocation(NULL, 77, 300, __FILE_WRITE_ACCESS);
    assert(g_response == OS_ENOTSUPPORTED && g_access == __FILE_READ_ACCESS);

    // A consumed token's control record is still closed by final cleanup. A
    // duplicate/stale cleanup ID must not close the filesystem state twice.
    g_cleanupCount = 2;
    if (setjmp(g_jobExit) == 0)
        __CleanupJob(NULL, NULL);
    assert(g_backendCloses == 1 && g_revokes == 1 && g_node.Handles.element_count == 0);
    assert(VFSNodeHandleGet(100, &record) == OS_ENOENT);
    assert(g_handlesLock.readers == 0);

    // Export failure rolls back both backend state and the initial reference.
    g_tokenError = OS_EOOM;
    sys_file_open_invocation(NULL, 77, "/file", __FILE_READ_ACCESS, 0);
    assert(g_response == OS_EOOM && !g_initialLive && g_backendCloses == 2);
    g_tokenError = OS_EOK;
    g_sendError = -1;
    sys_file_open_invocation(NULL, 77, "/file", __FILE_READ_ACCESS, 0);
    assert(g_response == OS_EOK && !g_tokenLive && g_revokes == 2);
    assert(VFSNodeClose(NULL, 100) == OS_EOK && g_backendCloses == 3);

    // Internal opens keep their first reference until synchronous internal close.
    uuid_t internal;
    assert(VFSNodeOpen(&g_vfs, "/internal", __FILE_READ_ACCESS, 0, &internal) == OS_EOK);
    assert(g_initialLive);
    assert(VFSNodeClose(NULL, internal) == OS_EOK && !g_initialLive);
    assert(g_backendCloses == 4 && g_handlesLock.readers == 0);
    // Simulate an asynchronous close already past global-record removal. VFS
    // teardown must wait for its per-node entry before destroying the backend.
    teardown = calloc(1, sizeof(*teardown));
    teardown->Root = calloc(1, sizeof(*teardown->Root));
    teardown->Interface = &g_interface;
    teardown->Root->FileSystem = teardown;
    g_closingNode = teardown->Root;
    assert(hashtable_construct(
               &g_closingNode->Handles, 0, sizeof(struct __VFSHandle), __TestHash, __TestCompare) ==
           0);
    hashtable_set(&g_closingNode->Handles, &(struct __VFSHandle){.Id = 100});
    filesystem.VFS = teardown;
    filesystem.Interface = &g_interface;
    filesystem.Data = (void *)5678;
    filesystem.State = FileSystemState_CONNECTED;
    g_interface.Operations.Destroy = __TestDestroyBackend;
    assert(VFSFileSystemDisconnectInterface(&filesystem, 0) == OS_EOK);
    assert(g_backendDestroyed && filesystem.VFS == NULL);
    assert(filesystem.State == FileSystemState_NO_INTERFACE);

    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_node.Handles);
    puts("filed handoff, authenticated access and cleanup passed");
    return 0;
}
