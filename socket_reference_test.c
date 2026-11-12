// Exercise netd's actual acquire/export boundary with deterministic IPC peers.
#ifndef SOCKET_MANAGER_SOURCE
#define SOCKET_MANAGER_SOURCE "../services/netd/manager.c"
#endif
#include SOCKET_MANAGER_SOURCE
#ifndef SOCKET_IMPLEMENTATION_SOURCE
#define SOCKET_IMPLEMENTATION_SOURCE "../services/netd/socket.c"
#endif
#include SOCKET_IMPLEMENTATION_SOURCE
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)

static int g_lockDepth;
static int g_exports;
static int g_releases;
static oserr_t g_responseStatus;
static struct sys_socket_info g_responseInfo;
static GrantTokenOptions_t g_exportOptions;
static uuid_t g_cleanupResources[2];
static uint32_t g_cleanupCount;
static oserr_t g_cleanupStatus;
static unsigned int g_ringCloses;
static unsigned int g_revokes;
static unsigned int g_disconnects;
static unsigned int g_domainDeletes;
static unsigned int g_queueRemovals;

void* dsalloc(size_t size) { return malloc(size); }
void dsfree(void* memory) { free(memory); }
int* __errno(void) { static int error; return &error; }
void spinlock_init(spinlock_t* lock) { }
void spinlock_acquire(spinlock_t* lock) { }
void spinlock_release(spinlock_t* lock) { }
void usched_mtx_lock(struct usched_mtx* lock) { g_lockDepth++; }
void usched_mtx_unlock(struct usched_mtx* lock) { CHECK(g_lockDepth > 0); g_lockDepth--; }

const struct gracht_security_context*
gracht_server_get_security_context(const struct gracht_message* message)
{
    return &message->security;
}

oserr_t
OSHandleValidate(OSHandle_t* registration, uint64_t group, uuid_t reference,
                 uint32_t access, uuid_t* resource)
{
    CHECK(g_lockDepth > 0 && access == OS_SOCKET_ACCESS);
    if (group != 41 || reference != 23) {
        return OS_EPERMISSIONS;
    }
    *resource = 100;
    return OS_EOK;
}

int
sys_socket_acquire_response(struct gracht_message* message, oserr_t status,
                            struct sys_socket_info* info)
{
    CHECK(g_lockDepth == 0);
    g_responseStatus = status;
    g_responseInfo = *info;
    return 0;
}

oserr_t
OSGrantTokenCreate(uuid_t handle, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    CHECK(g_lockDepth > 0 && handle == 100);
    g_exports++;
    g_exportOptions = *options;
    *token = (GrantToken_t){ .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 50, .ExportGlobalHandleID = 50, .Options = *options };
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_ uuid_t control)
{
    CHECK(g_lockDepth > 0);
    CHECK(control == 7 || control == 8 || control == 50);
    g_revokes++;
    return OS_EOK;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    // The initial socket reference must already be gone. Final service cleanup
    // releases only the two ring attachments and their export controls.
    if (handle->ID == UUID_INVALID) {
        return;
    }
    CHECK(handle->Type == OSHANDLE_SHM && (handle->ID == 20 || handle->ID == 21));
    g_ringCloses++;
    memset(handle, 0, sizeof(*handle));
}

oserr_t
OSHandleReadCleanup(
    _In_  OSHandle_t* registration,
    _Out_ uuid_t*     resources,
    _In_  uint32_t    capacity,
    _Out_ uint32_t*   count)
{
    CHECK(g_lockDepth > 0 && registration == &g_socketRegistration);
    if (g_cleanupStatus != OS_EOK) {
        return g_cleanupStatus;
    }
    CHECK(capacity >= g_cleanupCount);
    memcpy(resources, g_cleanupResources, g_cleanupCount * sizeof(uuid_t));
    *count = g_cleanupCount;
    g_cleanupCount = 0;
    return OS_EOK;
}

oserr_t
OSNotificationQueueCtrl(
    _In_ OSHandle_t*         queue,
    _In_ int                 operation,
    _In_ OSHandle_t*         handle,
    _In_ struct ioset_event* event)
{
    CHECK(g_lockDepth > 0 && queue == &g_socketSet);
    CHECK(operation == IOSET_DEL && handle->ID == 100);
    g_queueRemovals++;
    return OS_EOK;
}

oserr_t OSNotificationQueuePost(OSHandle_t* handle, unsigned int flags) { abort(); }
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { }

oserr_t
DomainDisconnect(
    _In_ Socket_t* socket)
{
    CHECK(g_lockDepth > 0);
    CHECK(NetworkManagerSocketGetUnsafe(100) == NULL);
    g_disconnects++;
    return OS_EOK;
}

void
DomainDestroy(
    _In_ SocketDomain_t* domain)
{
    CHECK(g_lockDepth > 0 && domain == (SocketDomain_t*)1);
    g_domainDeletes++;
}

int
sys_socket_close_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                status)
{
    CHECK(g_lockDepth == 0);
    g_responseStatus = status;
    return 0;
}
oserr_t
OSHandleRelease(OSHandle_t* handle)
{
    CHECK(g_lockDepth > 0 && handle->ID == 100);
    g_releases++;
    memset(handle, 0, sizeof(*handle));
    return OS_EOK;
}

int
main(void)
{
    Socket_t* socket;
    struct gracht_message request = { 0 };
    struct sys_grant_token reference;
    struct sys_socket_info info;
    struct sys_socket_info first;
    SHMTransfer_t transfer = { .Token = { .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 7, .ExportGlobalHandleID = 7,
        .Options = { .ResourceType = GrantTokenResourceTypeSHM,
            .TargetType = GrantTokenTargetTypeAny,
            .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
            .Data.SHM.Length = 4096 } }, .RangeMode = SHMTransferRangeFull };

    socket = calloc(1, sizeof(*socket));
    CHECK(socket != NULL);
    socket->DomainType = AF_LOCAL;
    socket->Type = SOCK_STREAM;
    socket->Domain = (SocketDomain_t*)1;
    socket->Handle.ID = 100;
    socket->Receive.SHM = (OSHandle_t){ .ID = 20, .Type = OSHANDLE_SHM };
    socket->Send.SHM = (OSHandle_t){ .ID = 21, .Type = OSHANDLE_SHM };
    rb_tree_construct(&g_sockets);
    RB_LEAF_INIT(&socket->Header, 100, socket);
    CHECK(rb_tree_append(&g_sockets, &socket->Header) == 0);
    socket->References.Initial.ID = 100;
    socket->Receive.Transfer = transfer;
    transfer.Token.ControlHandleID = transfer.Token.ExportGlobalHandleID = 8;
    socket->Send.Transfer = transfer;

    // A guessed reference and untrusted credentials must not disclose ring tokens.
    request.security.principal = 41;
    sys_socket_acquire_invocation(&request, 23);
    CHECK(g_responseStatus == OS_EPERMISSIONS && g_responseInfo.recv_buffer.token.export_id == 0);
    request.security.source = GRACHT_SECURITY_KERNEL;
    request.security.principal = 42;
    sys_socket_acquire_invocation(&request, 23);
    CHECK(g_responseStatus == OS_EPERMISSIONS);
    request.security.principal = 41;
    sys_socket_acquire_invocation(&request, 24);
    CHECK(g_responseStatus == OS_EPERMISSIONS);

    // Acquire repeatedly describes existing rings, without minting tokens or
    // changing the socket reference count. Both replies carry identical tokens.
    sys_socket_acquire_invocation(&request, 23);
    CHECK(g_responseStatus == OS_EOK);
    first = g_responseInfo;
    sys_socket_acquire_invocation(&request, 23);
    CHECK(g_responseStatus == OS_EOK && memcmp(&first, &g_responseInfo, sizeof(first)) == 0);
    CHECK(first.recv_buffer.token.export_id == 7 && first.send_buffer.token.export_id == 8);
    CHECK(g_exports == 0 && g_releases == 0);

    CHECK(NetworkManagerSocketExport(&request, 100, &reference, &info) == OS_EOK);
    CHECK(g_exports == 1 && g_releases == 1 && socket->References.Initial.ID == UUID_INVALID);
    CHECK(g_exportOptions.TargetType == GrantTokenTargetTypeSpecific);
    CHECK(g_exportOptions.TargetGlobalHandleID == 41 && g_exportOptions.Scope == OS_SOCKET_ACCESS);
    CHECK(g_exportOptions.Flags == GrantTokenFlagsSingleUse && socket->References.ExportControl == 50);
    CHECK(memcmp(&first, &info, sizeof(info)) == 0 && g_lockDepth == 0);

    // A consumer cannot destroy the shared socket through the shutdown RPC.
    sys_socket_close_invocation(&request, 23, SYS_CLOSE_OPTIONS_DESTROY);
    CHECK(g_responseStatus == OS_EINVALPARAMS);
    CHECK(NetworkManagerSocketGetUnsafe(100) == socket && g_domainDeletes == 0);

    // No cleanup record means that references still exist. A failed cleanup
    // read must also leave the socket table and ring attachments intact.
    usched_mtx_lock(&g_socketExecution);
    __DrainSocketCleanup();
    g_cleanupStatus = OS_EUNKNOWN;
    __DrainSocketCleanup();
    CHECK(NetworkManagerSocketGetUnsafe(100) == socket && g_ringCloses == 0);

    // Feed the existing final-reference record into the real netd destructor.
    // A repeated ID is tolerated and cannot destroy the socket a second time.
    g_cleanupStatus = OS_EOK;
    g_cleanupResources[0] = g_cleanupResources[1] = 100;
    g_cleanupCount = 2;
    __DrainSocketCleanup();
    CHECK(NetworkManagerSocketGetUnsafe(100) == NULL);
    CHECK(g_ringCloses == 2 && g_revokes == 3 && g_releases == 1);
    CHECK(g_disconnects == 1 && g_domainDeletes == 1 && g_queueRemovals == 1);
    __DrainSocketCleanup();
    CHECK(g_ringCloses == 2 && g_revokes == 3);
    usched_mtx_unlock(&g_socketExecution);
    puts("Socket references: authenticated acquire, initial export and final-reference cleanup passed");
    return 0;
}
