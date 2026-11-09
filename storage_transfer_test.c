// Validate exact storage ranges, cached geometry and temporary token lifetime.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../librt/libddk/storage.c"
#include "../modules/filesystems/common/storage.c"

static unsigned int g_tokens;
static unsigned int g_revokes;
static unsigned int g_sends;
static unsigned int g_stats;
static unsigned int g_attaches;
static int g_sendError;
static int g_waitError;
static int g_decodeError;
static oserr_t g_remoteError;
static size_t g_completed = 1;
static size_t g_sectorSize = 512;
static SHMTransfer_t g_received;
static GrantTokenOptions_t g_options;
static uint64_t g_sector;
static bool g_write;

size_t MemoryPageSize(void) { return 4096; }
gracht_client_t* GetGrachtClient(void) { return (void*)1; }
uuid_t GetFileService(void) { return 3; }
uuid_t __crt_process_id(void) { return 2; }
uuid_t OSProcessCurrentID(void) { return 2; }
oserr_t OSGrantTokenCreate(uuid_t id, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    assert(id == 42 && g_tokens == g_revokes);
    g_tokens++;
    g_options = *options;
    *token = (GrantToken_t) { .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 7, .ExportGlobalHandleID = 7, .Options = *options };
    return OS_EOK;
}
oserr_t OSGrantTokenRevoke(uuid_t id)
{
    assert(id == 7 && g_tokens == g_revokes + 1);
    g_revokes++;
    return OS_EOK;
}
oserr_t SHMAttachToken(const GrantToken_t* token, OSHandle_t* handle)
{
    g_attaches++;
    handle->ID = 91;
    return OS_EOK;
}
int gracht_client_await(gracht_client_t* client, struct gracht_message_context* msg, unsigned int flags)
{ return g_waitError; }
int gracht_client_get_status(gracht_client_t* client, struct gracht_message_context* msg)
{ return GRACHT_MESSAGE_COMPLETED; }
int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* msg) { return 0; }
int ctt_storage_transfer(gracht_client_t* client, struct gracht_message_context* msg,
                         uuid_t device, enum sys_transfer_direction direction,
                         unsigned int low, unsigned int high,
                         const struct sys_shm_transfer* buffer, size_t count)
{
    assert(device == 4 && __FromSysSHMTransfer(buffer, &g_received) == OS_EOK);
    g_sector = ((uint64_t)high << 32) | low;
    g_write = direction == SYS_TRANSFER_DIRECTION_WRITE;
    g_sends++;
    return g_sendError;
}
int ctt_storage_transfer_result(gracht_client_t* client, struct gracht_message_context* msg,
                                oserr_t* status, size_t* count)
{ *status = g_remoteError; *count = g_completed; return g_decodeError; }
int ctt_storage_stat(gracht_client_t* client, struct gracht_message_context* msg, uuid_t device)
{ g_stats++; return g_sendError; }
int ctt_storage_stat_result(gracht_client_t* client, struct gracht_message_context* msg,
                            oserr_t* status, struct sys_disk_descriptor* descriptor)
{
    sys_disk_descriptor_init(descriptor);
    descriptor->geometry.sector_size = g_sectorSize;
    descriptor->model = "test";
    descriptor->serial = "test";
    *status = g_remoteError;
    return g_decodeError;
}
int sys_file_fstat(gracht_client_t* client, struct gracht_message_context* msg, uuid_t process, uuid_t file)
{ return -1; }
int sys_file_fstat_result(gracht_client_t* client, struct gracht_message_context* msg,
                         oserr_t* status, struct sys_file_descriptor* descriptor)
{ return -1; }
oserr_t OSTransferFileAt(uuid_t file, const UInteger64_t* position, uuid_t buffer,
                         size_t offset, bool write, size_t length, size_t* completed)
{ assert(0); return OS_EUNKNOWN; }

int main(void)
{
    struct VFSStorageParameters storage = { .StorageType = VFSSTORAGE_TYPE_DEVICE,
        .Storage.Device = { .DriverID = 3, .DeviceID = 4 }, .SectorStart.QuadPart = 10 };
    StorageDescriptor_t stats;
    struct sys_shm_transfer wire;
    OSHandle_t handle = { 0 };
    SHMTransfer_t transfer;
    UInteger64_t sector = { .QuadPart = 2 };
    size_t completed;
    size_t offset;
    unsigned int sends;
    unsigned int attaches;
    int failure;

    assert(FSStorageRead(&storage, 42, 4113, &sector, 2, &completed) == OS_EINVALPARAMS);
    assert(g_sends == 0 && g_tokens == 0);
    assert(FSStorageStat(&storage, &stats) == OS_EOK && storage.SectorSize == 512);
    assert(FSStorageRead(&storage, 42, 4113, &sector, 2, &completed) == OS_EOK);
    assert(completed == 1 && g_stats == 1 && g_sector == 12 && !g_write);
    assert(g_received.RequestedLength == 1024 && g_received.RelativeOffset == 17);
    assert(g_options.Data.SHM.Offset == 4096 && g_options.Data.SHM.Length == 4096);
    assert(g_options.Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    assert(g_tokens == g_revokes);
    assert(FSStorageWrite(&storage, 42, 0, &sector, 2, &completed) == OS_EOK);
    assert(g_write && g_options.Scope == GrantTokenScopeRead && g_stats == 1);
    transfer = g_received;
    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    assert(__StorageAttachTransfer(&wire, 512, 2, true, &handle, &offset) == OS_EOK);
    assert(handle.ID == 91 && offset == 0);
    attaches = g_attaches;
    assert(__StorageAttachTransfer(&wire, 4096, 2, true, &handle, &offset) == OS_EINVALPARAMS);
    assert(__StorageAttachTransfer(&wire, 512, 0, true, &handle, &offset) == OS_EINVALPARAMS);
    assert(__StorageAttachTransfer(&wire, 512, SIZE_MAX, true, &handle, &offset) == OS_EINVALPARAMS);
    assert(__StorageAttachTransfer(&wire, 512, 2, false, &handle, &offset) == OS_EPERMISSIONS);
    assert(g_attaches == attaches);
    transfer.Token.Options.Scope = GrantTokenScopeWrite;
    transfer.RangeMode = SHMTransferRangeFull;
    transfer.RelativeOffset = transfer.RequestedLength = 0;
    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    assert(__StorageAttachTransfer(&wire, 512, 8, false, &handle, &offset) == OS_EOK);
    sends = g_sends;
    assert(FSStorageRead(&storage, 42, 0, &sector, SIZE_MAX, &completed) == OS_EINVALPARAMS);
    assert(FSStorageRead(&storage, 42, 0, &sector, 0, &completed) == OS_EOK && completed == 0);
    sector.QuadPart = UINT64_MAX;
    assert(FSStorageRead(&storage, 42, 0, &sector, 1, &completed) == OS_EINVALPARAMS);
    assert(g_sends == sends);
    sector.QuadPart = 2;
    for (failure = 0; failure < 5; failure++) {
        g_sendError = failure == 0 ? -1 : 0;
        g_waitError = failure == 1 ? -1 : 0;
        g_decodeError = failure == 2 ? -1 : 0;
        g_completed = failure == 3 ? 3 : 1;
        g_remoteError = failure == 4 ? OS_EDEVFAULT : OS_EOK;
        assert(FSStorageRead(&storage, 42, 0, &sector, 2, &completed) != OS_EOK);
        assert(completed == (failure == 4 ? 1 : 0) && g_tokens == g_revokes);
    }
    assert(FSStorageStat(&storage, &stats) == OS_EDEVFAULT && storage.SectorSize == 0);
    g_remoteError = OS_EOK;
    g_sectorSize = 0;
    assert(FSStorageStat(&storage, &stats) == OS_EINVALPARAMS && storage.SectorSize == 0);
    g_sectorSize = 4096;
    assert(FSStorageStat(&storage, &stats) == OS_EOK && storage.SectorSize == 4096);
    g_completed = 1;
    assert(FSStorageRead(&storage, 42, 0, &sector, 1, &completed) == OS_EOK);
    assert(g_received.RequestedLength == 4096);
    puts("Storage transfers: cached geometry, exact byte ranges, scope, overflow and RPC cleanup passed");
    return 0;
}
