// Exercise private copies, shared direct transfers and file-view publication.
#ifndef FILE_VIEW_SOURCE
#define FILE_VIEW_SOURCE "../librt/libos/services/fileview.c"
#endif
#include FILE_VIEW_SOURCE
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static char g_backing[3 * 65536];
static char g_shared[65536];
static SHMHandle_t g_local;
static SHMHandle_t g_temporary;
static size_t g_expectedOffset;
static size_t g_limit = SIZE_MAX;
static size_t g_capacity;
static unsigned int g_attaches;
static unsigned int g_maps;
static unsigned int g_creates;
static unsigned int g_releases;
static unsigned int g_tokens;
static unsigned int g_revokes;
static unsigned int g_commits;
static unsigned int g_sends;
static unsigned int g_direct;
static unsigned int g_access;
static oserr_t g_attachError;
static oserr_t g_mapError;
static oserr_t g_createError;
static oserr_t g_tokenError;
static oserr_t g_rpcError;
static bool g_writing;

size_t MemoryPageSize(void) { return 4096; }

oserr_t
SHMAttach(uuid_t id, OSHandle_t* out)
{
    CHECK(id == 10);
    g_attaches++;
    if (g_attachError != OS_EOK) {
        return g_attachError;
    }
    *out = (OSHandle_t){ .ID = 11, .Payload = &g_local };
    return OS_EOK;
}

oserr_t
SHMMap(OSHandle_t* handle, size_t offset, size_t length, unsigned int access)
{
    CHECK(handle->ID == 11);
    CHECK(offset == g_expectedOffset && length <= sizeof(g_backing) - offset);
    g_maps++;
    g_access = access;
    if (g_mapError != OS_EOK) {
        return g_mapError;
    }
    g_local.Buffer = g_backing + offset;
    g_local.Length = length;
    return OS_EOK;
}

oserr_t
SHMCreate(SHM_t* options, OSHandle_t* out)
{
    g_creates++;
    CHECK(options->Flags == (SHM_COMMIT | SHM_CLEAN));
    CHECK(options->Size != 0 && options->Size <= sizeof(g_shared) && options->Size % 4096 == 0);
    if (g_createError != OS_EOK) {
        return g_createError;
    }
    g_capacity = options->Size;
    g_temporary.Buffer = g_shared;
    *out = (OSHandle_t){ .ID = 12, .Payload = &g_temporary };
    return OS_EOK;
}

void* SHMBuffer(OSHandle_t* handle)
{
    if (handle->ID == 10) {
        return g_backing;
    }
    CHECK(handle->ID == 11 || handle->ID == 12);
    return ((SHMHandle_t*)handle->Payload)->Buffer;
}

void OSHandleDestroy(OSHandle_t* handle)
{
    CHECK(handle->ID == 11 || handle->ID == 12);
    g_releases++;
    handle->ID = UUID_INVALID;
}

oserr_t
OSGrantTokenCreate(uuid_t id, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    CHECK(id == 12);
    CHECK(options->Data.SHM.Offset == 0 && options->Data.SHM.Length == g_capacity);
    CHECK(options->Scope == (g_writing ? GrantTokenScopeRead : GrantTokenScopeRead | GrantTokenScopeWrite));
    if (g_tokenError != OS_EOK) {
        return g_tokenError;
    }
    *token = (GrantToken_t){ .ControlHandleID = 20, .Options = *options };
    g_tokens++;
    return OS_EOK;
}

oserr_t OSGrantTokenRevoke(uuid_t id)
{
    CHECK(id == 20 && g_tokens == g_revokes + 1);
    g_revokes++;
    return OS_EOK;
}

oserr_t
OSTransferFileBufferAt(uuid_t file, const UInteger64_t* position, const SHMTransfer_t* transfer,
                       bool write, size_t* completed)
{
    size_t i;
    size_t length = transfer->RequestedLength;

    CHECK(file == 4 && write == g_writing);
    CHECK(position->QuadPart == 8192 + g_expectedOffset);
    CHECK(transfer->RelativeOffset == 0 && transfer->RangeMode == SHMTransferRangeSubrange);
    g_sends++;
    for (i = write ? length : 0; i < g_capacity; i++) {
        CHECK(g_shared[i] == 0);
    }
    *completed = MIN(length, g_limit);
    if (write) {
        CHECK(memcmp(g_shared, g_backing + g_expectedOffset, length) == 0);
    } else {
        memset(g_shared, 0x3c, *completed);
    }
    g_expectedOffset += length;
    return g_rpcError;
}

oserr_t
OSTransferFileAt(uuid_t file, const UInteger64_t* position, uuid_t buffer,
                 size_t offset, bool write, size_t length, size_t* completed)
{
    CHECK(file == 4 && buffer == 10 && position->QuadPart == 8192 + offset);
    g_direct++;
    *completed = MIN(length, g_limit);
    return g_rpcError;
}

oserr_t
SHMCommit(OSHandle_t* handle, vaddr_t address, size_t length)
{
    CHECK(handle->ID == 10 && address >= (vaddr_t)g_backing);
    CHECK(length <= sizeof(g_backing) - (address - (vaddr_t)g_backing));
    g_commits++;
    return OS_EOK;
}

int
main(void)
{
    struct __FileView view = {
        .SHM.ID = 10, .FileHandle.ID = 4, .Offset = 8192,
        .Length = sizeof(g_backing), .Flags = FILEVIEW_READ
    };
    size_t i;
    unsigned int releases;
    unsigned int commits;
    unsigned int sends;

    memset(g_backing, 0xa5, sizeof(g_backing));
    g_expectedOffset = 4096;
    CHECK(__FillData(&view, 4096, 2 * 65536) == OS_EOK);
    CHECK(g_tokens == 1 && g_revokes == 1 && g_sends == 2 && g_releases == 2 && g_commits == 1);
    CHECK(g_access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE | SHM_ACCESS_COMMIT));
    for (i = 0; i < sizeof(g_backing); i++) {
        CHECK((unsigned char)g_backing[i] == (i >= 4096 && i < 4096 + 2 * 65536 ? 0x3c : 0xa5));
    }

    g_expectedOffset = 0;
    g_limit = 13;
    CHECK(__FillData(&view, 0, 2 * 65536) == OS_EOK);
    for (i = 0; i < 2 * 65536; i++) {
        CHECK(g_backing[i] == (i < 13 ? 0x3c : 0));
    }
    g_writing = true;
    g_expectedOffset = 4096;
    CHECK(__TransferView(&view, 4096, 4096, true) == OS_EINCOMPLETE);
    CHECK(g_access == SHM_ACCESS_READ);
    g_limit = SIZE_MAX;
    g_expectedOffset = 4096;
    CHECK(__TransferView(&view, 4096, 4096, true) == OS_EOK);
    g_writing = false;

    releases = g_releases;
    commits = g_commits;
    g_expectedOffset = 0;
    g_attachError = OS_ENOENT;
    CHECK(__FillData(&view, 0, 4096) == OS_ENOENT && g_releases == releases);
    g_attachError = OS_EOK;
    g_mapError = OS_EOOM;
    CHECK(__FillData(&view, 0, 4096) == OS_EOOM && g_releases == ++releases);
    g_mapError = OS_EOK;
    g_createError = OS_EOOM;
    CHECK(__FillData(&view, 0, 4096) == OS_EOOM && g_releases == ++releases);
    g_createError = OS_EOK;
    g_tokenError = OS_EOOM;
    releases += 2;
    CHECK(__FillData(&view, 0, 4096) == OS_EOOM && g_releases == releases);
    g_tokenError = OS_EOK;
    g_rpcError = OS_EDEVFAULT;
    releases += 2;
    CHECK(__FillData(&view, 0, 4096) == OS_EDEVFAULT && g_releases == releases);
    CHECK(g_commits == commits && g_tokens == g_revokes);
    g_rpcError = OS_EOK;

    view.Flags |= FILEVIEW_SHARED;
    sends = g_sends;
    CHECK(__FillData(&view, 4096, 4096) == OS_EOK && g_direct == 1);
    CHECK(g_sends == sends && g_commits == commits + 1);
    CHECK(__TransferView(&view, sizeof(g_backing), 1, false) == OS_EINVALPARAMS);
    CHECK(__TransferView(&view, 1, SIZE_MAX, false) == OS_EINVALPARAMS);
    CHECK(g_direct == 1);
    puts("File views: private staging, read-only fill alias, EOF padding, direct sharing and failure cleanup passed");
    return 0;
}
