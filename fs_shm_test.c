/** Filesystem requests keep CPU addresses and storage offsets in the same buffer. */
#include "../modules/filesystems/common/requests.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint8_t g_buffer[4096];
static uint8_t g_written[128];
static SHMHandle_t g_mapping;
static size_t g_offset;
static size_t g_length;
static size_t g_completed;
static size_t g_responseBytes;
static unsigned int g_access;
static unsigned int g_attaches;
static unsigned int g_maps;
static unsigned int g_releases;
static unsigned int g_operations;
static unsigned int g_responses;
static bool g_failAttach;
static bool g_failMap;
static bool g_failOperation;
static oserr_t g_responseStatus;
static oserr_t g_operationStatus;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}

oserr_t
SHMAttachToken(
    _In_  const GrantToken_t* token,
    _Out_ OSHandle_t* handle)
{
    CHECK(token->ExportGlobalHandleID == 7);
    g_attaches++;
    if (g_failAttach) {
        return OS_ENOENT;
    }
    g_mapping.Capacity = sizeof(g_buffer);
    *handle = (OSHandle_t){ .ID = 42, .Type = OSHANDLE_SHM, .Payload = &g_mapping };
    return OS_EOK;
}

size_t
SHMBufferCapacity(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    return g_mapping.Capacity;
}

oserr_t
SHMMap(
    _In_ OSHandle_t* handle,
    _In_ size_t      offset,
    _In_ size_t      length,
    _In_ unsigned int access)
{
    CHECK(handle->ID == 42);
    CHECK(offset <= sizeof(g_buffer) && length <= sizeof(g_buffer) - offset);
    CHECK(offset == 0 && length == g_offset + g_length && access == g_access);
    g_maps++;
    if (g_failMap) {
        return OS_EOOM;
    }
    g_mapping.Buffer = g_buffer + offset;
    g_mapping.Offset = offset;
    g_mapping.Length = length;
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    return g_mapping.Buffer;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    g_releases++;
    memset(handle, 0, sizeof(*handle));
}

static void
__CheckOperation(
    _In_ void*  context,
    _In_ void*  file,
    _In_ uuid_t id,
    _In_ void*  buffer,
    _In_ size_t offset,
    _In_ size_t count)
{
    CHECK(context == (void*)0x17 && file == (void*)0x22 && id == 42);
    CHECK(buffer == g_buffer && offset == g_offset && count == g_length);
    g_operations++;
}

oserr_t
FsRead(
    _In_  void*   context,
    _In_  void*   file,
    _In_  uuid_t  id,
    _In_  void*   buffer,
    _In_  size_t  offset,
    _In_  size_t  count,
    _Out_ size_t* read)
{
    __CheckOperation(context, file, id, buffer, offset, count);
    if (g_failOperation) {
        return OS_EUNKNOWN;
    }
    memset((uint8_t*)buffer + offset, 0xab, g_completed);
    *read = g_completed;
    return g_operationStatus;
}

oserr_t
FsWrite(
    _In_  void*   context,
    _In_  void*   file,
    _In_  uuid_t  id,
    _In_  void*   buffer,
    _In_  size_t  offset,
    _In_  size_t  count,
    _Out_ size_t* written)
{
    __CheckOperation(context, file, id, buffer, offset, count);
    if (g_failOperation) {
        return OS_EUNKNOWN;
    }
    memcpy(g_written, (const uint8_t*)buffer + offset, g_completed);
    *written = g_completed;
    return g_operationStatus;
}

int
ctt_filesystem_read_response(
    _In_ struct gracht_message* message,
    _In_ const oserr_t          status,
    _In_ const uint64_t         count)
{
    (void)message;
    g_responses++;
    g_responseStatus = status;
    g_responseBytes = count;
    return 0;
}

int
ctt_filesystem_write_response(
    _In_ struct gracht_message* message,
    _In_ const oserr_t          status,
    _In_ const uint64_t         count)
{
    return ctt_filesystem_read_response(message, status, count);
}

static void
__Reset(void)
{
    memset(g_buffer, 0x31, sizeof(g_buffer));
    memset(g_written, 0x55, sizeof(g_written));
    memset(&g_mapping, 0, sizeof(g_mapping));
    g_attaches = g_maps = g_releases = g_operations = g_responses = 0;
    g_failAttach = g_failMap = g_failOperation = false;
    g_operationStatus = OS_EOK;
    g_responseStatus = OS_EUNKNOWN;
    g_completed = 7;
    g_offset = 17;
    g_length = 128;
}

static void
__Invoke(
    _In_ bool     write,
    _In_ uint64_t count)
{
    struct ctt_fs_transfer_params params = {
        .buffer = {
            .token = {
                .version = GRANT_TOKEN_VERSION,
                .export_id = 7,
                .resource_type = GrantTokenResourceTypeSHM,
                .data1 = sizeof(g_buffer),
                .scope = GrantTokenScopeRead | GrantTokenScopeWrite,
                .target_type = GrantTokenTargetTypeAny
            },
            .range_mode = SHMTransferRangeSubrange,
            .relative_offset = (uint32_t)g_offset,
            .requested_length = (uint32_t)count
        }
    };

    g_access = write ? SHM_ACCESS_READ : SHM_ACCESS_READ | SHM_ACCESS_WRITE;
    if (write) {
        ctt_filesystem_write_invocation(NULL, 0x17, 0x22, &params);
    } else {
        ctt_filesystem_read_invocation(NULL, 0x17, 0x22, &params);
    }
    CHECK(g_responses == 1);
}

int
main(void)
{
    size_t i;
    int write;
    int partialError;

    for (write = 0; write < 2; write++) {
        for (partialError = 0; partialError < 2; partialError++) {
            __Reset();
            g_operationStatus = partialError ? OS_EUNKNOWN : OS_EOK;
            __Invoke(write, g_length);
            CHECK(g_responseStatus == g_operationStatus && g_responseBytes == 7);
            CHECK(g_attaches == 1 && g_maps == 1 && g_operations == 1 && g_releases == 1);
            // Short completion must leave bytes before and after the payload
            // unchanged. The buffer ID and offset passed to storage stay original.
            for (i = 0; i < sizeof(g_buffer); i++) {
                CHECK(g_buffer[i] == (!write && i >= 17 && i < 24 ? 0xab : 0x31));
            }
            if (write) {
                CHECK(g_written[0] == 0x31 && g_written[6] == 0x31 && g_written[7] == 0x55);
            }
        }

        __Reset();
        __Invoke(write, 0);
        CHECK(g_responseStatus == OS_EINVALPARAMS && g_responseBytes == 0);
        CHECK(g_attaches == 0 && g_maps == 0 && g_operations == 0 && g_releases == 0);

        __Reset();
        g_failAttach = true;
        __Invoke(write, g_length);
        CHECK(g_responseStatus == OS_ENOENT && g_releases == 0 && g_maps == 0);

        __Reset();
        g_failMap = true;
        __Invoke(write, g_length);
        CHECK(g_responseStatus == OS_EOOM && g_releases == 1 && g_operations == 0);

        __Reset();
        g_failOperation = true;
        __Invoke(write, g_length);
        CHECK(g_responseStatus == OS_EUNKNOWN && g_responseBytes == 0 && g_releases == 1);

        __Reset();
        __Invoke(write, UINT64_MAX);
        CHECK(g_responseStatus == OS_EINVALPARAMS && g_releases == 0 && g_attaches == 0);

        __Reset();
        g_offset = sizeof(g_buffer) + 1;
        __Invoke(write, 1);
        CHECK(g_responseStatus == OS_EINVALPARAMS && g_releases == 0 && g_attaches == 0);

        __Reset();
        g_offset = sizeof(g_buffer) - 1;
        __Invoke(write, 2);
        CHECK(g_responseStatus == OS_EINVALPARAMS && g_releases == 0 && g_attaches == 0);
    }
    puts("Filesystem SHM: original offsets, short transfers, empty requests, bounds and failure cleanup passed");
    return 0;
}
