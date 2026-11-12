// Verify socket rings import typed grants and roll back partial setup.
#include "../librt/libos/services/net.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static unsigned int g_attaches;
static unsigned int g_live;
static unsigned int g_failAttach;
static unsigned int g_failMap;
static unsigned int g_imports;
static unsigned int g_drops;
static unsigned int g_failRelease;
static bool g_failWrap;
static oserr_t g_acquireStatus;
static struct sys_socket_info g_info;

uuid_t GetNetService(void) { return 9; }
gracht_client_t* GetGrachtClient(void) { return (gracht_client_t*)1; }
oserr_t MutexInitialize(Mutex_t* mutex, int flags) { return OS_EOK; }
oserr_t MutexLock(Mutex_t* mutex) { return OS_EOK; }
oserr_t MutexUnlock(Mutex_t* mutex) { return OS_EOK; }
int gracht_client_await(gracht_client_t* client, struct gracht_message_context* context, unsigned int flags)
{ return 0; }
int gracht_client_get_status(gracht_client_t* client, struct gracht_message_context* context)
{ return GRACHT_MESSAGE_COMPLETED; }
int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* context)
{ return 0; }
int sys_socket_acquire(gracht_client_t* client, struct gracht_message_context* context, const uuid_t handle)
{ CHECK(handle == 42); return 0; }
int sys_socket_acquire_result(gracht_client_t* client, struct gracht_message_context* context,
                             oserr_t* result, struct sys_socket_info* info)
{ *result = g_acquireStatus; *info = g_info; return 0; }
oserr_t OSHandleWrap(uuid_t id, enum OSHandleType type, void* payload, bool owned, OSHandle_t* out)
{
    CHECK(id == 42 && type == OSHANDLE_SOCKET && owned);
    if (g_failWrap) return OS_EOOM;
    *out = (OSHandle_t){ .ID = id, .Type = type, .Payload = payload };
    return OS_EOK;
}
SCTYPE syscall3(SCTYPE function, SCTYPE token, SCTYPE destination, SCTYPE out)
{
    CHECK(function == 63 && destination == UUID_INVALID);
    g_imports++;
    *(uuid_t*)(uintptr_t)out = 42;
    return OS_EOK;
}
SCTYPE syscall1(SCTYPE function, SCTYPE id)
{
    CHECK(function == 64 && id == 42);
    g_drops++;
    return OS_EINCOMPLETE;
}

oserr_t SHMAttachToken(const GrantToken_t* token, OSHandle_t* handle)
{
    g_attaches++;
    if (g_failAttach == g_attaches) return OS_EOOM;
    CHECK(token->Options.Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    *handle = (OSHandle_t){ .ID = token->ExportGlobalHandleID, .Type = OSHANDLE_SHM };
    g_live++;
    return OS_EOK;
}
size_t SHMBufferCapacity(OSHandle_t* handle) { return 4096; }
void* SHMBuffer(OSHandle_t* handle) { return handle->ID != UUID_INVALID ? (void*)0x1000 : NULL; }
oserr_t SHMMap(OSHandle_t* handle, size_t offset, size_t length, unsigned int access)
{
    CHECK(offset == 0 && length == 4096 && access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    return g_failMap == g_attaches ? OS_EOOM : OS_EOK;
}
void OSHandleDestroy(OSHandle_t* handle)
{
    CHECK(handle->ID != UUID_INVALID && g_live != 0);
    g_live--;
    memset(handle, 0, sizeof(*handle));
}
oserr_t SHMRelease(OSHandle_t* handle)
{
    if (handle->ID == UUID_INVALID) return OS_EOK;
    if (g_failRelease) {
        g_failRelease--;
        return OS_EBUSY;
    }
    OSHandleDestroy(handle);
    return OS_EOK;
}
int main(void)
{
    struct Socket socket = { 0 };
    struct sys_shm_transfer wire;
    SHMTransfer_t transfer = { .Token = { .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 7, .ExportGlobalHandleID = 7,
        .Options = { .ResourceType = GrantTokenResourceTypeSHM,
            .TargetType = GrantTokenTargetTypeAny, .TargetGlobalHandleID = UUID_INVALID,
            .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
            .Data.SHM.Length = 4096 } }, .RangeMode = SHMTransferRangeFull };
    int failure;
    OSHandle_t handle = { 0 };
    struct sys_grant_token reference;
    streambuffer_t* stream;
    GrantToken_t token = { .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 8, .ExportGlobalHandleID = 8,
        .Options = { .ResourceType = GrantTokenResourceTypeProvider,
            .TargetType = GrantTokenTargetTypeSpecific, .TargetGlobalHandleID = 3,
            .Scope = OS_SOCKET_ACCESS, .Flags = GrantTokenFlagsSingleUse } };

    CHECK(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    CHECK(__SocketAttachPipes(&socket, &wire, &wire) == OS_EOK);
    CHECK(g_live == 2 && socket.SendMapped && socket.RecvMapped);
    OSHandleDestroy(&socket.Send);
    OSHandleDestroy(&socket.Recv);
    for (failure = 1; failure <= 4; failure++) {
        memset(&socket, 0, sizeof(socket));
        g_attaches = 0;
        g_failAttach = failure <= 2 ? failure : 0;
        g_failMap = failure > 2 ? failure - 2 : 0;
        CHECK(__SocketAttachPipes(&socket, &wire, &wire) == OS_EOOM);
        CHECK(g_live == 0 && !socket.SendMapped && !socket.RecvMapped);
    }
    wire.token.scope = GrantTokenScopeRead;
    CHECK(__SocketAttachPipes(&socket, &wire, &wire) == OS_EPERMISSIONS && g_live == 0);
    wire.token.scope |= GrantTokenScopeWrite;
    wire.range_mode = SHMTransferRangeSubrange;
    wire.relative_offset = 1;
    wire.requested_length = 4095;
    CHECK(__SocketAttachPipes(&socket, &wire, &wire) == OS_EINVALPARAMS && g_live == 0);

    CHECK(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    CHECK(__ToSysGrantToken(&token, &reference) == OS_EOK);
    g_info = (struct sys_socket_info){ .domain = AF_LOCAL, .type = SOCK_STREAM,
        .recv_buffer = wire, .send_buffer = wire };
    g_failAttach = g_failMap = g_attaches = 0;

    // Startup already has the socket reference. Acquire imports only the two
    // ring views, and a failed mapping leaves that reference for startup cleanup.
    g_acquireStatus = OS_EPERMISSIONS;
    CHECK(OSSocketAcquire(42, &handle) == OS_EPERMISSIONS && g_attaches == 0);
    g_acquireStatus = OS_EOK;
    g_failMap = 2;
    CHECK(OSSocketAcquire(42, &handle) == OS_EOOM);
    CHECK(g_live == 0 && g_imports == 0 && g_drops == 0);
    g_failMap = 0;
    g_failWrap = true;
    CHECK(OSSocketAcquire(42, &handle) == OS_EOOM && g_live == 0);
    g_failWrap = false;
    CHECK(OSSocketAcquire(42, &handle) == OS_EOK && g_live == 2);
    CHECK(g_imports == 0 && g_drops == 0);

    // Failed final unmap keeps the socket reference until release can finish.
    g_failRelease = 1;
    CHECK(__SocketRelease(&handle) == OS_EBUSY && g_drops == 0 && g_live == 2);
    CHECK(SHMRelease(&((struct Socket*)handle.Payload)->Send) == OS_EOK);
    CHECK(OSSocketSendPipe(&handle, &stream) == OS_ECANCELLED && stream == NULL);
    CHECK(__SocketRelease(&handle) == OS_EOK && g_drops == 1 && g_live == 0);
    __SocketDestroy(&handle);

    // Create/accept import their initial reference. Failed local setup must
    // drop exactly that imported reference after rolling back any mapped ring.
    g_failWrap = true;
    CHECK(__SocketImport(&reference, &g_info, &handle) == OS_EOOM);
    CHECK(g_imports == 1 && g_drops == 2 && g_live == 0);
    g_failWrap = false;
    CHECK(__SocketImport(&reference, &g_info, &handle) == OS_EOK);
    CHECK(g_imports == 2 && g_live == 2);
    CHECK(__SocketRelease(&handle) == OS_EOK && g_drops == 3 && g_live == 0);
    __SocketDestroy(&handle);
    puts("Socket buffers: typed mapping, acquire, import rollback and retryable release passed");
    return 0;
}
