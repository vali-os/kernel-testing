// Use the real local handle registry, descriptor table and open/import client.
#include <internal/_syscalls.h>
#include <os/handle.h>
#include <stdlib.h>
#include <stdio.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("line %d: %s\n", __LINE__, #x);                                                 \
            abort();                                                                               \
        }                                                                                          \
    } while (0)
static oserr_t __TestDestroy(uuid_t id);
static oserr_t __TestImport(const GrantToken_t *token, uuid_t memorySpace, uuid_t *id);
#undef Syscall_HandleDestroy
#undef Syscall_HandleImport
#define Syscall_HandleDestroy(id) __TestDestroy(id)
#define Syscall_HandleImport(token, memorySpace, id) __TestImport(token, memorySpace, id)
#define g_ipcOps g_handleIpcOps
#include "../librt/libos/handles.c"
#undef g_ipcOps
#include "../librt/libos/handle_provider.c"
#ifndef FILE_CLIENT_SOURCE
#define FILE_CLIENT_SOURCE "../librt/libos/services/file.c"
#endif
#ifndef DESCRIPTOR_SOURCE
#define DESCRIPTOR_SOURCE "../librt/libc/stdio/core/core.c"
#endif
#include FILE_CLIENT_SOURCE
#include DESCRIPTOR_SOURCE
#include "../librt/libc/stdio/core/iod_bitmap.c"
#include "test-close.c"
#define dup test_dup
#include "../librt/libc/io/dup.c"
#undef dup
struct DIR;
int test_seekdir(struct DIR *dir, long index);
#define mkdir test_mkdir
#define readdir test_readdir
#define rewinddir test_rewinddir
#define seekdir test_seekdir
#define telldir test_telldir
#define opendir test_opendir
#define closedir test_closedir
#include "../librt/libc/io/dirent.c"
#undef opendir
#undef closedir
#include "../librt/libds/hashtable.c"

const OSHandleOps_t g_handleIpcOps = {0};
const OSHandleOps_t g_eventOps = {0};
const OSHandleOps_t g_hqueueOps = {0};
const OSHandleOps_t g_shmOps = {0};
const OSHandleOps_t g_socketOps = {0};
stdio_ops_t g_fmemOps, g_memstreamOps, g_evtOps, g_iosetOps;
stdio_ops_t g_fileOps, g_pipeOps, g_ipcOps;
stdio_ops_t g_netOps;
static int g_drops;
static int g_imports;
static int g_abandons;
static int g_sendError;
static int g_waitError;
static int g_decodeError;
static int g_messageStatus = GRACHT_MESSAGE_COMPLETED;
static oserr_t g_openError;
static oserr_t g_importError;
static unsigned int g_openFlags;
static GrantToken_t g_token;
static bool g_badToken;
static int g_errno;

int *
__errno(void)
{
    return &g_errno;
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
void
spinlock_init(spinlock_t *lock)
{
    memset(lock, 0, sizeof(*lock));
}
void
spinlock_acquire(spinlock_t *lock)
{
}
void
spinlock_release(spinlock_t *lock)
{
}
int
OsErrToErrNo(oserr_t error)
{
    return error == OS_EOK ? 0 : -1;
}
SCTYPE
syscall2(SCTYPE number, SCTYPE a, SCTYPE b)
{
    return OS_EOK;
}
uuid_t
GetFileService(void)
{
    return 9;
}
uuid_t
__crt_process_id(void)
{
    return 77;
}
gracht_client_t *
GetGrachtClient(void)
{
    return (void *)1;
}
int
gracht_client_await(gracht_client_t *client,
                    struct gracht_message_context *message,
                    unsigned int flags)
{
    return g_waitError;
}
int
gracht_client_get_status(gracht_client_t *client, struct gracht_message_context *message)
{
    return g_messageStatus;
}
int
gracht_client_abandon(gracht_client_t *client, struct gracht_message_context *message)
{
    g_abandons++;
    return 0;
}
int
sys_file_open(gracht_client_t *client,
              struct gracht_message_context *message,
              uuid_t process,
              const char *path,
              unsigned int flags,
              unsigned int permissions)
{
    g_openFlags = flags;
    return g_sendError;
}
int
sys_file_open_result(gracht_client_t *client,
                     struct gracht_message_context *message,
                     oserr_t *error,
                     struct sys_grant_token *token)
{
    if (g_decodeError)
        return g_decodeError;
    *error = g_openError;
    if (*error != OS_EOK)
        return 0;
    CHECK(__ToSysGrantToken(&g_token, token) == OS_EOK);
    if (g_badToken)
        token->version = 0;
    return 0;
}
static oserr_t
__TestImport(const GrantToken_t *token, uuid_t memorySpace, uuid_t *id)
{
    CHECK(memorySpace == UUID_INVALID);
    CHECK(token->Options.Flags == GrantTokenFlagsSingleUse);
    g_imports++;
    if (g_importError != OS_EOK)
        return g_importError;
    *id = 300;
    return OS_EOK;
}
static oserr_t
__TestDestroy(uuid_t id)
{
    CHECK(id == 300);
    g_drops++;
    return OS_EOK;
}

int
main(void)
{
    OSHandle_t file;
    OSHandle_t found;
    stdio_handle_t *descriptor;
    int duplicate;
    int second;
    struct DIR *directory;

    OSHandlesInitialize();
    CHECK(hashtable_construct(
              &g_stdioObjects, 0, sizeof(struct stdio_object_entry), stdio_hash, stdio_cmp) == 0);
    CHECK(stdio_bitmap_initialize() == 0);
    g_token = (GrantToken_t){.Version = GRANT_TOKEN_VERSION,
                             .ControlHandleID = 200,
                             .ExportGlobalHandleID = 200,
                             .Options = {.ResourceType = GrantTokenResourceTypeProvider,
                                         .Scope = __FILE_READ_ACCESS,
                                         .TargetType = GrantTokenTargetTypeSpecific,
                                         .TargetGlobalHandleID = 77,
                                         .Flags = GrantTokenFlagsSingleUse}};

    g_sendError = -1;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EPROTOCOL && g_imports == 0);
    g_sendError = 0;
    g_waitError = -1;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EUNKNOWN && g_abandons == 1);
    g_waitError = 0;
    g_messageStatus = GRACHT_MESSAGE_ERROR;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EUNKNOWN && g_abandons == 2);
    g_messageStatus = GRACHT_MESSAGE_COMPLETED;
    g_decodeError = -1;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EPROTOCOL && g_imports == 0);
    g_decodeError = 0;
    g_openError = OS_ENOENT;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_ENOENT && g_imports == 0);
    g_openError = OS_EOK;
    g_badToken = true;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EINVALPARAMS && g_imports == 0);
    g_badToken = false;
    g_importError = OS_EPERMISSIONS;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EPERMISSIONS);
    g_importError = OS_EOK;
    CHECK(OSOpenPath("/file", __FILE_READ_ACCESS, 0, &file) == OS_EOK);
    CHECK(file.Type == OSHANDLE_NULL && file.ID == 300);
    CHECK(stdio_handle_create(-1, 0, 0, FILE_SIGNATURE, NULL, &descriptor) == 0);
    descriptor->Shared = true;
    CHECK(stdio_handle_set_handle(descriptor, &file) == 0);
    duplicate = test_dup(descriptor->IOD);
    CHECK(duplicate >= 0 && stdio_handle_get(duplicate) != NULL);
    CHECK(stdio_handle_get(duplicate)->OSHandle.ID == 300);
    CHECK(!(stdio_handle_get(duplicate)->XTFlags & __IO_PERSISTANT));
    second = test_dup(duplicate);
    CHECK(second >= 0 && stdio_handle_get(second) != NULL);
    CHECK(test_close(descriptor->IOD) == 0 && g_drops == 0);
    CHECK(test_close(duplicate) == 0 && g_drops == 0);
    CHECK(OSHandlesFind(300, &found) == OS_EOK);
    CHECK(test_close(second) == 0 && g_drops == 1);
    CHECK(OSHandlesFind(300, &found) == OS_ENOENT);

    // A failed local reference must roll back the duplicate's IOD.
    CHECK(stdio_handle_create(-1, 0, 0, FILE_SIGNATURE, NULL, &descriptor) == 0);
    descriptor->Shared = true;
    descriptor->OSHandle.ID = 301;
    CHECK(test_dup(descriptor->IOD) == -1);
    CHECK(g_stdioObjects.element_count == 1 && g_drops == 1);
    stdio_handle_delete(descriptor);

    directory = test_opendir("/directory");
    CHECK(directory != NULL && (g_openFlags & __FILE_READ_ACCESS));
    CHECK(g_openFlags & __FILE_READ_SHARE);
    CHECK(test_closedir(directory) == 0 && g_drops == 2);
    hashtable_destroy(&g_stdioObjects);
    hashtable_destroy(&g_osHandles);
    free(g_stdioHandles);
    puts("file open/import errors, local dup references and directory access passed");
    return 0;
}
