// Verify socket rings import typed grants and roll back partial setup.
#include "../librt/libos/services/net.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static unsigned int g_attaches;
static unsigned int g_live;
static unsigned int g_failAttach;
static unsigned int g_failMap;

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
    puts("Socket buffers: typed import, full-range RW mapping and partial failure cleanup passed");
    return 0;
}
