// Run the real client, local domain and rings in one process. RPC and SHM
// shims model two client descriptors mapped onto the same service socket.
#ifndef SOCKET_CLIENT_SOURCE
#define SOCKET_CLIENT_SOURCE "../librt/libos/services/net.c"
#endif
#define Socket __ClientSocket
#include SOCKET_CLIENT_SOURCE
#undef Socket
#include "../services/netd/socket.c"
#ifndef SOCKET_LOCAL_SOURCE
#define SOCKET_LOCAL_SOURCE "../services/netd/domains/local.c"
#endif
#include SOCKET_LOCAL_SOURCE
#include <ds/ds.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("line %d: %s\n", __LINE__, #condition); \
        abort(); \
    } \
} while (0)

/**
 * Each fake SHM object tracks its backing allocation and attached clients.
 * Server and inherited client descriptors see the same bytes.
 */
struct __TestBuffer {
    void*        Data;
    size_t       Size;
    unsigned int References;
};

static struct __TestBuffer g_buffers[6];
static unsigned int       g_bufferCount;
static Socket_t           g_sockets[3];
static SocketDomain_t     g_domains[3];
static struct thread_storage g_tls;
static uuid_t             g_requestSocket;
static oserr_t            g_connectStatus;
static unsigned int       g_rpcCount;

void*
dsalloc(
    _In_    size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_    void* memory)
{
    free(memory);
}

void
spinlock_init(
    _In_    spinlock_t* lock)
{
}

void
spinlock_acquire(
    _In_    spinlock_t* lock)
{
}

void
spinlock_release(
    _In_    spinlock_t* lock)
{
}

void
dswait(
    _In_    OSFutexParameters_t* parameters,
    _In_    OSAsyncContext_t*    context)
{
    abort();
}

void
dswake(
    _In_    OSFutexParameters_t* parameters)
{
}

int*
__errno(void)
{
    static int error;
    return &error;
}

struct thread_storage*
__tls_current(void)
{
    return &g_tls;
}

uuid_t
usched_notification_handle(void)
{
    return UUID_INVALID;
}

uuid_t
GetNetService(void)
{
    return 9;
}

gracht_client_t*
GetGrachtClient(void)
{
    return (gracht_client_t*)1;
}

oserr_t
MutexInitialize(
    _In_    Mutex_t* mutex,
    _In_    int      flags)
{
    return OS_EOK;
}

oserr_t
MutexLock(
    _In_    Mutex_t* mutex)
{
    return OS_EOK;
}

oserr_t
MutexUnlock(
    _In_    Mutex_t* mutex)
{
    return OS_EOK;
}

void
usched_mtx_lock(
    _In_    struct usched_mtx* mutex)
{
}

void
usched_mtx_unlock(
    _In_    struct usched_mtx* mutex)
{
}

void
SystemDebug(
    _In_    enum OSSysLogLevel level,
    _In_    const char*        format,
    ...)
{
}

size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
SHMCreate(
    _In_    SHM_t*      configuration,
    _Out_   OSHandle_t* handle)
{
    struct __TestBuffer* buffer;

    CHECK(g_bufferCount < 6);
    buffer = &g_buffers[g_bufferCount++];
    buffer->Size = configuration->Size;
    buffer->Data = calloc(1, buffer->Size);
    CHECK(buffer->Data != NULL);
    buffer->References = 1;
    *handle = (OSHandle_t){ .ID = g_bufferCount, .Type = OSHANDLE_SHM, .Payload = buffer };
    return OS_EOK;
}

oserr_t
OSGrantTokenCreate(
    _In_    uuid_t                     id,
    _In_    const GrantTokenOptions_t* options,
    _Out_   GrantToken_t*              token)
{
    *token = (GrantToken_t){ .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = id, .ExportGlobalHandleID = id, .Options = *options };
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_    uuid_t control)
{
    return OS_EOK;
}

oserr_t
SHMAttachToken(
    _In_    const GrantToken_t* token,
    _Out_   OSHandle_t*         handle)
{
    uuid_t id = token->ExportGlobalHandleID;
    struct __TestBuffer* buffer;

    CHECK(id > 0 && id <= g_bufferCount);
    buffer = &g_buffers[id - 1];
    buffer->References++;
    *handle = (OSHandle_t){ .ID = id, .Type = OSHANDLE_SHM, .Payload = buffer };
    return OS_EOK;
}

size_t
SHMBufferCapacity(
    _In_    OSHandle_t* handle)
{
    return ((struct __TestBuffer*)handle->Payload)->Size;
}

void*
SHMBuffer(
    _In_    OSHandle_t* handle)
{
    return handle->Payload != NULL ? ((struct __TestBuffer*)handle->Payload)->Data : NULL;
}

oserr_t
SHMMap(
    _In_    OSHandle_t*  handle,
    _In_    size_t       offset,
    _In_    size_t       length,
    _In_    unsigned int access)
{
    CHECK(offset == 0 && length == SHMBufferCapacity(handle));
    return OS_EOK;
}

void
OSHandleDestroy(
    _InOut_ OSHandle_t* handle)
{
    struct __TestBuffer* buffer = handle->Payload;

    CHECK(handle->Type == OSHANDLE_SHM && buffer->References != 0);
    if (--buffer->References == 0) {
        free(buffer->Data);
        buffer->Data = NULL;
    }
    memset(handle, 0, sizeof(*handle));
}

oserr_t
SHMRelease(
    _InOut_ OSHandle_t* handle)
{
    OSHandleDestroy(handle);
    return OS_EOK;
}

oserr_t
OSHandleWrap(
    _In_    uuid_t            id,
    _In_    enum OSHandleType type,
    _In_    void*             payload,
    _In_    bool              owned,
    _Out_   OSHandle_t*       handle)
{
    *handle = (OSHandle_t){ .ID = id, .Type = type, .Payload = payload };
    return OS_EOK;
}

SCTYPE
syscall1(
    _In_    SCTYPE function,
    _In_    SCTYPE id)
{
    CHECK(function == 64);
    return OS_EOK;
}

oserr_t
OSNotificationQueuePost(
    _In_    OSHandle_t*  handle,
    _In_    unsigned int flags)
{
    return OS_EOK;
}

int
gracht_client_await(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context,
    _In_    unsigned int                   flags)
{
    return 0;
}

int
gracht_client_get_status(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context)
{
    return GRACHT_MESSAGE_COMPLETED;
}

int
gracht_client_abandon(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context)
{
    abort();
}

Socket_t*
NetworkManagerSocketGetUnsafe(
    _In_    uuid_t id)
{
    return id >= 1 && id <= 3 ? &g_sockets[id - 1] : NULL;
}

int
sys_socket_acquire(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context,
    _In_    const uuid_t                   id)
{
    CHECK(NetworkManagerSocketGetUnsafe(id) != NULL);
    g_requestSocket = id;
    g_rpcCount++;
    return 0;
}

int
sys_socket_acquire_result(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context,
    _Out_   oserr_t*                       status,
    _Out_   struct sys_socket_info*        info)
{
    Socket_t* socket = NetworkManagerSocketGetUnsafe(g_requestSocket);

    *info = (struct sys_socket_info){ .domain = AF_LOCAL, .type = socket->Type };
    CHECK(__ToSysSHMTransfer(&socket->Receive.Transfer, &info->recv_buffer) == OS_EOK);
    CHECK(__ToSysSHMTransfer(&socket->Send.Transfer, &info->send_buffer) == OS_EOK);
    *status = OS_EOK;
    return 0;
}

int
sys_socket_connect(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context,
    _In_    const uuid_t                   id,
    _In_    const uint8_t*                 address,
    _In_    const uint32_t                 length)
{
    g_rpcCount++;
    g_connectStatus = DomainLocalConnect(NULL, NetworkManagerSocketGetUnsafe(id),
                                         (const struct sockaddr*)address);
    return 0;
}

int
sys_socket_connect_result(
    _In_    gracht_client_t*               client,
    _In_    struct gracht_message_context* context,
    _Out_   oserr_t*                       status)
{
    *status = g_connectStatus;
    return 0;
}

int
sys_socket_connect_response(
    _In_    struct gracht_message* message,
    _In_    oserr_t                status)
{
    g_connectStatus = status;
    return 0;
}

// Stream accept paths are linked through DomainLocalConnect but are not used
// by these datagram tests. Abort if a test unexpectedly enters one.
int
sys_socket_accept_response(
    _In_    struct gracht_message*  message,
    _In_    oserr_t                 status,
    _In_    uint8_t*                address,
    _In_    uint32_t                length,
    _In_    struct sys_grant_token* token,
    _In_    struct sys_socket_info* info)
{
    abort();
}

void
gracht_server_defer_message(
    _In_    struct gracht_message* message,
    _In_    struct gracht_message* deferred)
{
    abort();
}

oserr_t
NetworkManagerSocketCreate(
    _In_    int                      domain,
    _In_    int                      type,
    _In_    int                      protocol,
    _In_    uuid_t*                  id,
    _In_    struct sys_shm_transfer* receive,
    _In_    struct sys_shm_transfer* send)
{
    abort();
}

oserr_t
NetworkManagerSocketExport(
    _In_    const struct gracht_message* message,
    _In_    uuid_t                       id,
    _In_    struct sys_grant_token*      reference,
    _In_    struct sys_socket_info*      info)
{
    abort();
}

oserr_t
NetworkManagerSocketShutdown(
    _In_    uuid_t id,
    _In_    int    options)
{
    abort();
}

oserr_t
NetworkManagerSocketPair(
    _In_    uuid_t first,
    _In_    uuid_t second)
{
    abort();
}

void
NetworkManagerSocketWaitForCredit(
    _In_    Socket_t* sender,
    _In_    Socket_t* receiver)
{
    // The queued packet stays in netd until the test supplies receive credit.
}

static void
__CreateSockets(void)
{
    Socket_t* socket;
    int       i;

    for (i = 0; i < 3; i++) {
        socket = &g_sockets[i];
        socket->Domain = &g_domains[i];
        socket->DomainType = AF_LOCAL;
        socket->Type = SOCK_DGRAM;
        socket->Handle.ID = i + 1;
        RB_LEAF_INIT(&socket->Header, socket->Handle.ID, socket);
        CHECK(DomainLocalAllocateAddress(socket) == OS_EOK);
        CHECK(CreateSocketPipe(&socket->Receive, false) == OS_EOK);
        CHECK(CreateSocketPipe(&socket->Send, true) == OS_EOK);
    }
}

static struct sockaddr_lc
__Address(
    _In_    uuid_t id)
{
    struct sockaddr_lc address = { 0 };

    CHECK(DomainLocalGetAddress(NetworkManagerSocketGetUnsafe(id), SYS_ADDRESS_TYPE_THIS,
                               (struct sockaddr*)&address) == OS_EOK);
    return address;
}

static void
__Connect(
    _In_    OSHandle_t* handle,
    _In_    uuid_t      peer)
{
    struct sockaddr_lc address = __Address(peer);

    CHECK(OSSocketConnect(handle, (struct sockaddr*)&address, sizeof(address)) == OS_EOK);
}

static void
__Send(
    _In_    OSHandle_t*         handle,
    _In_    struct sockaddr_lc* address,
    _In_    const char*         text)
{
    struct iovec iov = { .iov_base = (void*)text, .iov_len = strlen(text) + 1 };
    const struct msghdr message = { .msg_name = address,
        .msg_namelen = address != NULL ? sizeof(*address) : 0, .msg_iov = &iov, .msg_iovlen = 1 };
    unsigned int rpcCount = g_rpcCount;
    size_t       count = 0;

    CHECK(OSSocketSend(handle, &message, MSG_DONTWAIT, &count) == OS_EOK);
    CHECK(count == iov.iov_len && g_rpcCount == rpcCount);
    CHECK(message.msg_name == address);
    CHECK(message.msg_namelen == (address != NULL ? sizeof(*address) : 0));
}

static void
__Flush(
    _In_    uuid_t id)
{
    CHECK(DomainLocalSend(NetworkManagerSocketGetUnsafe(id)) == OS_EOK);
}

static void
__Receive(
    _In_    OSHandle_t* handle,
    _In_    uuid_t      source,
    _In_    const char* text,
    _In_    socklen_t   capacity)
{
    unsigned char* address = malloc(capacity + 1);
    struct sockaddr_lc expected = __Address(source);
    char          payload[64] = { 0 };
    struct iovec  iov = { .iov_base = payload, .iov_len = sizeof(payload) };
    struct msghdr message = { .msg_name = address, .msg_namelen = capacity,
        .msg_iov = &iov, .msg_iovlen = 1 };
    unsigned int  rpcCount = g_rpcCount;
    size_t        count = 0;

    CHECK(address != NULL);
    memset(address, 0xa5, capacity + 1);
    CHECK(OSSocketRecv(handle, &message, MSG_DONTWAIT, &count) == OS_EOK);
    CHECK(g_rpcCount == rpcCount && count == strlen(text) + 1);
    CHECK(strcmp(payload, text) == 0);
    CHECK(message.msg_namelen == sizeof(expected));
    CHECK(memcmp(address, &expected, MIN(capacity, sizeof(expected))) == 0);
    CHECK(address[capacity] == 0xa5);
    free(address);
}

static void
__TestInheritedReconnect(
    _In_    OSHandle_t* parent,
    _In_    OSHandle_t* child,
    _In_    OSHandle_t* a,
    _In_    OSHandle_t* b)
{
    OSHandle_t earlyChild;
    struct sockaddr_lc explicitAddress = __Address(2);
    struct sockaddr_lc missing = { .slc_len = sizeof(missing), .slc_family = AF_LOCAL,
        .slc_addr = "missing" };

    CHECK(OSSocketAcquire(parent->ID, &earlyChild) == OS_EOK);
    __Connect(parent, 2);
    __Send(&earlyChild, NULL, "connected after inheritance");
    __Flush(1);
    __Receive(a, 1, "connected after inheritance", sizeof(struct sockaddr_lc));
    CHECK(__SocketRelease(&earlyChild) == OS_EOK);
    __SocketDestroy(&earlyChild);
    CHECK(OSSocketAcquire(parent->ID, child) == OS_EOK);
    __Send(child, NULL, "inherited");
    __Flush(1);
    __Receive(a, 1, "inherited", sizeof(struct sockaddr_lc));

    // The packet stays in the ring until after the shared peer changes.
    __Send(parent, NULL, "before reconnect");
    __Connect(child, 3);
    __Send(parent, NULL, "after reconnect");
    __Flush(1);
    __Receive(a, 1, "before reconnect", sizeof(struct sockaddr_lc));
    __Receive(b, 1, "after reconnect", sizeof(struct sockaddr_lc));

    __Send(parent, &explicitAddress, "explicit");
    __Send(parent, NULL, "still connected");
    __Flush(1);
    __Receive(a, 1, "explicit", sizeof(struct sockaddr_lc));
    __Receive(b, 1, "still connected", sizeof(struct sockaddr_lc));

    CHECK(OSSocketConnect(child, (struct sockaddr*)&missing, sizeof(missing)) == OS_EHOSTUNREACHABLE);
    __Send(parent, NULL, "failed reconnect");
    __Flush(1);
    __Receive(b, 1, "failed reconnect", sizeof(struct sockaddr_lc));
}

static void
__TestEmptyDatagram(
    _In_    OSHandle_t* parent,
    _In_    OSHandle_t* a)
{
    struct sockaddr_lc destination = __Address(1);
    struct sockaddr_lc source = { 0 };
    struct sockaddr_lc expected = __Address(2);
    char payload[32] = { 0 };
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) };
    const struct msghdr outgoing = { .msg_name = &destination, .msg_namelen = sizeof(destination) };
    struct msghdr incoming = { .msg_name = &source, .msg_namelen = sizeof(source) };
    size_t count = 1;
    unsigned int rpcCount = g_rpcCount;

    CHECK(OSSocketSend(a, &outgoing, MSG_DONTWAIT, &count) == OS_EOK && count == 0);
    __Flush(2);
    CHECK(OSSocketRecv(parent, &incoming, MSG_DONTWAIT, &count) == OS_EOK && count == 0);
    CHECK(incoming.msg_namelen == sizeof(source));
    CHECK(memcmp(&source, &expected, sizeof(source)) == 0);

    // A NULL address discards the packet's address without shifting payload.
    __Send(a, &destination, "discard address");
    __Flush(2);
    incoming.msg_name = NULL;
    incoming.msg_iov = &iov;
    incoming.msg_iovlen = 1;
    CHECK(OSSocketRecv(parent, &incoming, MSG_DONTWAIT, &count) == OS_EOK);
    CHECK(incoming.msg_namelen == 0);
    CHECK(strcmp(payload, "discard address") == 0 && count == sizeof("discard address"));
    CHECK(g_rpcCount == rpcCount);
}

static void
__TestStreamPair(void)
{
    OSHandle_t sender;
    OSHandle_t receiver;
    unsigned char address[2] = { 0xa5, 0xa5 };
    char payload[16] = { 0 };
    struct iovec iov = { .iov_base = payload, .iov_len = sizeof(payload) };
    struct msghdr message = { .msg_name = address, .msg_namelen = sizeof(address),
        .msg_iov = &iov, .msg_iovlen = 1 };
    size_t count = 0;
    unsigned int rpcCount;

    g_sockets[0].Type = SOCK_STREAM;
    g_sockets[1].Type = SOCK_STREAM;
    CHECK(OSSocketAcquire(1, &sender) == OS_EOK);
    CHECK(OSSocketAcquire(2, &receiver) == OS_EOK);
    CHECK(DomainLocalPair(&g_sockets[0], &g_sockets[1]) == OS_EOK);
    __Send(&sender, NULL, "stream");
    __Flush(1);
    rpcCount = g_rpcCount;
    CHECK(OSSocketRecv(&receiver, &message, MSG_DONTWAIT, &count) == OS_EOK);
    CHECK(count == 7 && strcmp(payload, "stream") == 0);
    CHECK(message.msg_namelen == 0 && address[0] == 0xa5 && address[1] == 0xa5);
    CHECK(g_rpcCount == rpcCount);
    CHECK(__SocketRelease(&sender) == OS_EOK);
    __SocketDestroy(&sender);
    CHECK(__SocketRelease(&receiver) == OS_EOK);
    __SocketDestroy(&receiver);
}

static void
__TestReceiveAddresses(
    _In_    OSHandle_t* parent,
    _In_    OSHandle_t* child,
    _In_    OSHandle_t* a)
{
    struct sockaddr_lc destination = __Address(1);
    const socklen_t capacities[] = { 0, 1, 2, sizeof(struct sockaddr_lc) - 1,
        sizeof(struct sockaddr_lc), sizeof(struct sockaddr_storage) };
    size_t i;

    // Queue a packet from A, then reconnect to B before receiving it.
    __Connect(parent, 2);
    __Send(a, &destination, "old peer");
    __Flush(2);
    __Connect(child, 3);
    __Receive(parent, 2, "old peer", sizeof(struct sockaddr_lc));

    for (i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
        __Send(a, &destination, "short address");
        __Flush(2);
        __Receive(child, 2, "short address", capacities[i]);
    }
}

static void
__TestPairAndDisconnect(
    _In_    OSHandle_t* parent,
    _In_    OSHandle_t* child,
    _In_    OSHandle_t* a)
{
    struct iovec iov = { .iov_base = "x", .iov_len = 1 };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1 };
    unsigned int rpcCount;
    size_t       count = 0;

    CHECK(DomainLocalPair(&g_sockets[0], &g_sockets[1]) == OS_EOK);
    __Send(child, NULL, "paired");
    __Flush(1);
    __Receive(a, 1, "paired", 2);
    CHECK(DomainLocalDisconnect(&g_sockets[1]) == OS_EOK);
    rpcCount = g_rpcCount;
    CHECK(OSSocketSend(parent, &message, MSG_DONTWAIT, &count) == OS_ENOTCONNECTED);
    CHECK(OSSocketSend(child, &message, MSG_DONTWAIT, &count) == OS_ENOTCONNECTED);
    CHECK(g_rpcCount == rpcCount);
}

int
main(void)
{
    OSHandle_t parent;
    OSHandle_t child;
    OSHandle_t a;
    OSHandle_t b;
    struct msghdr message = { 0 };
    size_t count = 0;
    int i;

    __CreateSockets();
    CHECK(OSSocketAcquire(1, &parent) == OS_EOK);
    CHECK(OSSocketAcquire(2, &a) == OS_EOK);
    CHECK(OSSocketAcquire(3, &b) == OS_EOK);
    CHECK(OSSocketSend(&parent, &message, MSG_DONTWAIT, &count) == OS_ENOTCONNECTED);
    __TestInheritedReconnect(&parent, &child, &a, &b);
    __TestReceiveAddresses(&parent, &child, &a);
    __TestEmptyDatagram(&parent, &a);
    __TestPairAndDisconnect(&parent, &child, &a);
    CHECK(__SocketRelease(&parent) == OS_EOK);
    __SocketDestroy(&parent);
    CHECK(__SocketRelease(&child) == OS_EOK);
    __SocketDestroy(&child);
    CHECK(__SocketRelease(&a) == OS_EOK);
    __SocketDestroy(&a);
    CHECK(__SocketRelease(&b) == OS_EOK);
    __SocketDestroy(&b);
    __TestStreamPair();
    for (i = 0; i < 3; i++) {
        DomainLocalFreeAddress(&g_sockets[i]);
        __SocketPipeDestroy(&g_sockets[i].Send);
        __SocketPipeDestroy(&g_sockets[i].Receive);
    }
    for (i = 0; i < 6; i++) {
        CHECK(g_buffers[i].References == 0);
    }
    puts("Socket I/O: inherited peers, reconnects, packet destinations and bounded addresses passed");
    return 0;
}
