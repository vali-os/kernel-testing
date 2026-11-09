// Exercise file transfer RPCs and filed forwarding with production conversions.
#include <assert.h>
#include <string.h>
#include <stdio.h>
#ifndef FILE_TRANSFER_SOURCE
#define FILE_TRANSFER_SOURCE "../librt/libos/services/file_transfer.c"
#endif
#include FILE_TRANSFER_SOURCE
#include "../services/filed/interfaces/driver_ops.c"

static unsigned int g_tokens;
static unsigned int g_revokes;
static unsigned int g_sends;
static unsigned int g_abandons;
static int g_sendError;
static int g_waitError;
static int g_decodeError;
static int g_messageStatus = GRACHT_MESSAGE_COMPLETED;
static oserr_t g_tokenError;
static oserr_t g_remoteError;
static size_t g_completed = 13;
static GrantTokenOptions_t g_options;
static SHMTransfer_t g_received;
static UInteger64_t g_position;
static bool g_absolute;
static bool g_write;

size_t MemoryPageSize(void) { return 4096; }
uuid_t GetFileService(void) { return 3; }
gracht_client_t* GetGrachtClient(void) { return (void*)1; }
uuid_t __crt_process_id(void) { return 2; }

oserr_t
OSGrantTokenCreate(uuid_t id, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    assert(id == 42);
    if (g_tokenError != OS_EOK) {
        return g_tokenError;
    }
    assert(g_tokens == g_revokes);
    g_tokens++;
    g_options = *options;
    *token = (GrantToken_t) {
        .Version = GRANT_TOKEN_VERSION, .ControlHandleID = 7,
        .ExportGlobalHandleID = 7, .Options = *options
    };
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(uuid_t id)
{
    assert(id == 7 && g_tokens == g_revokes + 1);
    g_revokes++;
    return OS_EOK;
}

int gracht_client_await(gracht_client_t* client, struct gracht_message_context* message, unsigned int flags)
{
    return g_waitError;
}
int gracht_client_get_status(gracht_client_t* client, struct gracht_message_context* message)
{
    return g_messageStatus;
}
int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* message)
{
    g_abandons++;
    return 0;
}

int
sys_file_transfer(gracht_client_t* client, struct gracht_message_context* message,
                  uuid_t process, uuid_t file, enum sys_transfer_direction direction,
                  const struct sys_shm_transfer* buffer)
{
    assert(client == (void*)1 && process == 2 && file == 4);
    assert(__FromSysSHMTransfer(buffer, &g_received) == OS_EOK);
    g_write = direction == SYS_TRANSFER_DIRECTION_WRITE;
    g_sends++;
    return g_sendError;
}

int
sys_file_transfer_absolute(gracht_client_t* client, struct gracht_message_context* message,
                           uuid_t process, uuid_t file, enum sys_transfer_direction direction,
                           unsigned int low, unsigned int high, const struct sys_shm_transfer* buffer)
{
    g_absolute = true;
    g_position.u.LowPart = low;
    g_position.u.HighPart = high;
    return sys_file_transfer(client, message, process, file, direction, buffer);
}

int
sys_file_transfer_result(gracht_client_t* client, struct gracht_message_context* message,
                         oserr_t* status, size_t* completed)
{
    *status = g_remoteError;
    *completed = g_completed;
    return g_decodeError;
}

int
sys_file_transfer_absolute_result(gracht_client_t* client, struct gracht_message_context* message,
                                  oserr_t* status, size_t* completed)
{
    return sys_file_transfer_result(client, message, status, completed);
}

int
ctt_filesystem_read(gracht_client_t* client, struct gracht_message_context* message,
                    uintptr_t fs, uintptr_t file, const struct ctt_fs_transfer_params* params)
{
    assert(fs == 17 && file == 18);
    assert(__FromSysSHMTransfer(&params->buffer, &g_received) == OS_EOK);
    g_sends++;
    return g_sendError;
}

int
ctt_filesystem_write(gracht_client_t* client, struct gracht_message_context* message,
                     uintptr_t fs, uintptr_t file, const struct ctt_fs_transfer_params* params)
{
    return ctt_filesystem_read(client, message, fs, file, params);
}

int
ctt_filesystem_read_result(gracht_client_t* client, struct gracht_message_context* message,
                           oserr_t* status, uint64_t* completed)
{
    *status = g_remoteError;
    *completed = g_completed;
    return g_decodeError;
}

int
ctt_filesystem_write_result(gracht_client_t* client, struct gracht_message_context* message,
                            oserr_t* status, uint64_t* completed)
{
    return ctt_filesystem_read_result(client, message, status, completed);
}

int
main(void)
{
    SHMTransfer_t transfer;
    struct VFSInterface interface = { .DriverID = 9 };
    UInteger64_t position = { .QuadPart = UINT64_C(0x123456789abcdef0) };
    size_t count;
    unsigned int sends;
    unsigned int tokens;
    unsigned int abandons;
    int failure;
    int forwarding;

    assert(OSTransferFile(4, 42, 4113, false, 500, &count) == OS_EOK && count == 13);
    assert(g_options.Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    assert(g_options.Data.SHM.Offset == 4096 && g_options.Data.SHM.Length == 4096);
    assert(g_received.RelativeOffset == 17 && g_received.RequestedLength == 500);
    assert(!g_absolute && !g_write && g_tokens == g_revokes);
    assert(OSTransferFileAt(4, &position, 42, 3, true, 8192, &count) == OS_EOK);
    assert(g_write && g_absolute && g_position.QuadPart == position.QuadPart);
    assert(g_options.Scope == GrantTokenScopeRead && g_options.Data.SHM.Length == 12288);

    transfer = g_received;
    sends = g_sends;
    tokens = g_tokens;
    assert(OSTransferFileBuffer(4, &transfer, false, &count) == OS_EPERMISSIONS && count == 0);
    assert(g_sends == sends && g_tokens == tokens);
    transfer.Token.Options.Scope |= GrantTokenScopeWrite;
    assert(OSTransferFileBuffer(4, &transfer, false, &count) == OS_EOK);
    assert(g_tokens == tokens && g_revokes == tokens);
    assert(memcmp(&g_received, &transfer, sizeof(transfer)) == 0);
    assert(__DriverRead(&interface, (void*)17, (void*)18, &transfer, &count) == OS_EOK);
    assert(memcmp(&g_received, &transfer, sizeof(transfer)) == 0 && g_tokens == tokens);

    sends = g_sends;
    assert(OSTransferFile(4, UUID_INVALID, SIZE_MAX, true, 0, &count) == OS_EOK && count == 0);
    assert(OSTransferFile(4, 42, SIZE_MAX, true, 1, &count) == OS_EINVALPARAMS);
    assert(OSTransferFile(4, 42, 0, true, UINT32_MAX, &count) == OS_EINVALPARAMS);
    assert(g_sends == sends && g_tokens == tokens);
    transfer.RelativeOffset = UINT32_MAX;
    assert(OSTransferFileBuffer(4, &transfer, true, &count) == OS_EINVALPARAMS);
    assert(g_sends == sends);
    transfer = g_received;
    transfer.RangeMode = SHMTransferRangeFull;
    transfer.RelativeOffset = transfer.RequestedLength = 0;
    assert(OSTransferFileBuffer(4, &transfer, false, &count) == OS_EOK);

    for (forwarding = 0; forwarding < 2; forwarding++) {
        for (failure = 0; failure < 6; failure++) {
            g_sendError = failure == 0 ? -1 : 0;
            g_waitError = failure == 1 ? -1 : 0;
            g_messageStatus = failure == 2 ? GRACHT_MESSAGE_INPROGRESS : GRACHT_MESSAGE_COMPLETED;
            g_decodeError = failure == 3 ? -1 : 0;
            g_completed = failure == 4 ? 16384 : 13;
            g_remoteError = failure == 5 ? OS_EDEVFAULT : OS_EOK;
            sends = g_sends;
            abandons = g_abandons;
            count = SIZE_MAX;
            if (forwarding) {
                assert(__DriverWrite(&interface, (void*)17, (void*)18, &transfer, &count) != OS_EOK);
            } else {
                assert(OSTransferFile(4, 42, 3, true, 8192, &count) != OS_EOK);
            }
            assert(count == (failure == 5 ? 13 : 0));
            assert(g_sends == sends + 1 && g_tokens == g_revokes);
            assert(g_abandons == abandons + (failure == 1 || failure == 2));
        }
    }
    g_tokenError = OS_EOOM;
    sends = g_sends;
    assert(OSTransferFile(4, 42, 0, true, 1, &count) == OS_EOOM && count == 0);
    assert(g_sends == sends && g_tokens == g_revokes);
    puts("File transfers: scopes, page ranges, forwarding, token lifetime, bounded results and RPC failures passed");
    return 0;
}
