// Exercise libc file copies with managed storage and bounded RPC results.
#define open __TestOpen
#ifndef FILE_IO_SOURCE
#define FILE_IO_SOURCE "../librt/libc/io/open.c"
#endif
#include FILE_IO_SOURCE
#undef open
#include <stdlib.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)

static unsigned char g_shared[65536];
static unsigned char g_data[2 * sizeof(g_shared) + 137];
static size_t g_capacity;
static size_t g_position;
static size_t g_limit = SIZE_MAX;
static unsigned int g_calls;
static unsigned int g_creates;
static unsigned int g_releases;
static bool g_failCreate;
static bool g_overreport;
static oserr_t g_result;
static unsigned int g_tokens;
static unsigned int g_revokes;
static bool g_failToken;
static GrantTokenScope_t g_scope;

size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
SHMCreate(
    _In_ SHM_t* options,
    _Out_ OSHandle_t* handle)
{
    CHECK(options->Size != 0 && options->Size <= sizeof(g_shared));
    CHECK(options->Size % 4096 == 0);
    CHECK(options->Flags == (SHM_COMMIT | SHM_CLEAN));
    CHECK(options->Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    g_creates++;
    if (g_failCreate) {
        return OS_EOOM;
    }
    g_capacity = options->Size;
    memset(g_shared, 0, sizeof(g_shared));
    *handle = (OSHandle_t){ .ID = 42 };
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    return g_shared;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    g_releases++;
    handle->ID = UUID_INVALID;
}

oserr_t
OSTransferFileBuffer(
    _In_ uuid_t file,
    _In_ const SHMTransfer_t* transfer,
    _In_ bool write,
    _Out_ size_t* transferred)
{
    size_t i;
    size_t length = transfer->RequestedLength;
    size_t count = MIN(length, g_limit);

    CHECK(file == 7 && transfer->Token.ExportGlobalHandleID == 77);
    CHECK(transfer->RelativeOffset == 0 && transfer->RangeMode == SHMTransferRangeSubrange);
    CHECK(g_scope == (write ? GrantTokenScopeRead : GrantTokenScopeRead | GrantTokenScopeWrite));
    CHECK(length != 0 && length <= g_capacity);
    for (i = write ? length : 0; i < g_capacity; i++) {
        CHECK(g_shared[i] == 0);
    }
    g_calls++;
    if (write) {
        CHECK(memcmp(g_shared, g_data + g_position, length) == 0);
    } else {
        memcpy(g_shared, g_data + g_position, count);
    }
    *transferred = g_overreport ? length + 1 : count;
    g_position += count;
    return g_result;
}

int
main(void)
{
    stdio_handle_t handle = { .OSHandle.ID = 7 };
    unsigned char output[sizeof(g_data) + 2];
    size_t count;
    size_t i;
    unsigned int creates;
    unsigned int calls;

    for (i = 0; i < sizeof(g_data); i++) {
        g_data[i] = (unsigned char)(i * 37 + i / 256);
    }
    CHECK(__file_read(&handle, output, 0, &count) == OS_EOK && count == 0);
    CHECK(g_creates == 0 && g_calls == 0);
    g_failCreate = true;
    CHECK(__file_read(&handle, output, 1, &count) == OS_EOOM && count == 0);
    CHECK(g_releases == 0 && g_calls == 0);
    g_failCreate = false;

    memset(output, 0xa5, sizeof(output));
    CHECK(__file_read(&handle, output + 1, sizeof(g_data), &count) == OS_EOK);
    CHECK(count == sizeof(g_data) && g_calls == 3 && g_releases == 1);
    CHECK(memcmp(output + 1, g_data, count) == 0);
    CHECK(output[0] == 0xa5 && output[sizeof(output) - 1] == 0xa5);
    g_position = 0;
    CHECK(__file_write(&handle, g_data, sizeof(g_data), &count) == OS_EOK);
    CHECK(count == sizeof(g_data) && g_calls == 6 && g_releases == 2);

    // A short result ends the operation without requesting the rest again.
    calls = g_calls;
    g_position = 0;
    g_limit = 13;
    memset(output, 0xa5, sizeof(output));
    CHECK(__file_read(&handle, output, sizeof(g_data), &count) == OS_EOK);
    CHECK(count == 13 && g_calls == calls + 1 && g_releases == 3);
    CHECK(memcmp(output, g_data, count) == 0 && output[count] == 0xa5);
    g_position = 0;
    CHECK(__file_write(&handle, g_data, sizeof(g_data), &count) == OS_EOK);
    CHECK(count == 13 && g_calls == calls + 2);

    g_limit = 0;
    CHECK(__file_read(&handle, output, 1, &count) == OS_EOK && count == 0);
    CHECK(g_calls == calls + 3);
    g_position = 0;
    g_limit = 7;
    g_result = OS_EDEVFAULT;
    CHECK(__file_read(&handle, output, 32, &count) == OS_EDEVFAULT && count == 7);
    CHECK(memcmp(output, g_data, count) == 0);

    g_position = 0;
    g_result = OS_EOK;
    g_overreport = true;
    memset(output, 0xa5, sizeof(output));
    CHECK(__file_read(&handle, output, 32, &count) == OS_EPROTOCOL && count == 0);
    CHECK(output[0] == 0xa5);
    g_overreport = false;
    g_limit = SIZE_MAX;
    g_position = 0;
    creates = g_creates;
    CHECK(__file_read(&handle, output, 1, &count) == OS_EOK && count == 1);
    CHECK(g_capacity == 4096 && g_creates == creates + 1);
    CHECK(g_creates == g_releases + 1);
    CHECK(g_tokens == g_revokes && g_tokens == g_releases);
    puts("libc file transfers: managed pages, chunks, padding, short/error results and cleanup passed");
    return 0;
}

int*
__errno(void)
{
    static int error;
    return &error;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    CHECK(false);
}

oserr_t
OSGetFilePosition(
    _In_ uuid_t handle,
    _Out_ UInteger64_t* position)
{
    CHECK(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
OSGetFileSize(
    _In_ uuid_t handle,
    _Out_ UInteger64_t* size)
{
    CHECK(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
OSSetFileSize(
    _In_ uuid_t handle,
    _In_ UInteger64_t* size)
{
    CHECK(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
OSSeekFile(
    _In_ uuid_t handle,
    _In_ UInteger64_t* position)
{
    CHECK(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
OSGrantTokenCreate(
    _In_ uuid_t id,
    _In_ const GrantTokenOptions_t* options,
    _Out_ GrantToken_t* token)
{
    CHECK(id == 42 && options->Data.SHM.Offset == 0 && options->Data.SHM.Length == g_capacity);
    CHECK(options->ResourceType == GrantTokenResourceTypeSHM);
    CHECK(options->TargetType == GrantTokenTargetTypeAny && options->TargetGlobalHandleID == UUID_INVALID);
    if (g_failToken) {
        return OS_EOOM;
    }
    g_tokens++;
    g_scope = options->Scope;
    *token = (GrantToken_t){ .ControlHandleID = 77, .ExportGlobalHandleID = 77, .Options = *options };
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_ uuid_t id)
{
    CHECK(id == 77 && g_tokens == g_revokes + 1);
    g_revokes++;
    return OS_EOK;
}
