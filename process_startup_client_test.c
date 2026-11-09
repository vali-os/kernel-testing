// Exercise production spawn/startup clients, failure cleanup and bounded parsing.
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define _set_errno(value) (errno = (value))
static void* __TestMalloc(size_t);
static void* __TestCalloc(size_t, size_t);
static char* __TestStrndup(const char*, size_t);
static void __TestFree(void*);
#define malloc __TestMalloc
#define calloc __TestCalloc
#define strndup __TestStrndup
#define free __TestFree
#include "../librt/libos/services/process.c"
#include "../librt/libc/os/init.c"
#undef malloc
#undef calloc
#undef strndup
#undef free

static int g_allocations;
static int g_failAfter = -1;
static unsigned int g_buffers;
static unsigned int g_tokens;
static unsigned int g_sends;
static unsigned int g_abandons;
static void* g_buffer;
static size_t g_capacity;
static oserr_t g_createError;
static oserr_t g_tokenError;
static oserr_t g_remoteError;
static int g_sendError;
static int g_waitError;
static int g_resultError;
static int g_messageStatus = GRACHT_MESSAGE_COMPLETED;
static uint32_t g_replyBytes;
static uint32_t g_overrideBytes;

static void*
__TestMalloc(
    _In_ size_t length)
{
    void* memory;
    if (g_failAfter == 0) {
        return NULL;
    }
    if (g_failAfter > 0) {
        g_failAfter--;
    }
    memory = malloc(length);
    assert(memory != NULL);
    g_allocations++;
    return memory;
}

static void*
__TestCalloc(
    _In_ size_t count,
    _In_ size_t size)
{
    void* memory = __TestMalloc(count * size);
    if (memory != NULL) {
        memset(memory, 0, count * size);
    }
    return memory;
}

static char*
__TestStrndup(
    _In_ const char* source,
    _In_ size_t length)
{
    size_t actual = strnlen(source, length);
    char* result = __TestMalloc(actual + 1);
    if (result != NULL) {
        memcpy(result, source, actual);
        result[actual] = '\0';
    }
    return result;
}

static void
__TestFree(
    _In_ void* memory)
{
    if (memory != NULL) {
        assert(g_allocations > 0);
        g_allocations--;
        free(memory);
    }
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    _In_ ...)
{
    (void)level;
    (void)format;
}
uuid_t
GetProcessService(
    void)
{
    return 10;
}
size_t
MemoryPageSize(
    void)
{
    return 4096;
}
uuid_t
ThreadsCurrentId(
    void)
{
    return 88;
}

oserr_t
SHMCreate(
    _In_ SHM_t* parameters,
    _In_ OSHandle_t* handle)
{
    assert(g_buffers == 0);
    assert(parameters->Flags == (SHM_COMMIT | SHM_CLEAN));
    assert(parameters->Size % 4096 == 0 && parameters->Size <= PROCESS_STARTUP_MAX_SIZE);
    if (g_createError != OS_EOK) {
        return g_createError;
    }
    g_buffer = calloc(1, parameters->Size);
    assert(g_buffer != NULL);
    g_capacity = parameters->Size;
    *handle = (OSHandle_t) { .ID = 19, .Type = OSHANDLE_SHM, .Payload = g_buffer };
    g_buffers++;
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return handle->Payload;
}
size_t
SHMBufferCapacity(
    _In_ OSHandle_t* handle)
{
    return g_capacity;
}
void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    assert(g_buffers == 1 && g_tokens == 0);
    g_buffers--;
    free(g_buffer);
    g_buffer = NULL;
}

oserr_t
OSGrantTokenCreate(
    _In_ uuid_t handle,
    _In_ const GrantTokenOptions_t* options,
    _In_ GrantToken_t* token)
{
    assert(g_buffers == 1 && handle == 19 && g_tokens == 0);
    assert(options->Data.SHM.Offset == 0 && options->Data.SHM.Length == g_capacity);
    assert(options->TargetType == GrantTokenTargetTypeAny);
    assert(options->TargetGlobalHandleID == UUID_INVALID);
    if (g_tokenError != OS_EOK) {
        return g_tokenError;
    }
    *token = (GrantToken_t) {
        .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 5,
        .ExportGlobalHandleID = 5,
        .Options = *options
    };
    g_tokens++;
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_ uuid_t handle)
{
    assert(handle == 5 && g_tokens == 1 && g_buffers == 1);
    g_tokens--;
    return OS_EOK;
}

int gracht_client_await(gracht_client_t* client, struct gracht_message_context* context, unsigned int flags)
{
    assert(g_buffers == 1 && g_tokens == 1);
    return g_waitError;
}
int gracht_client_get_status(gracht_client_t* client, struct gracht_message_context* context)
{
    return g_messageStatus;
}
int gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* context)
{
    g_abandons++;
    return 0;
}

int
sys_process_spawn(gracht_client_t* client, struct gracht_message_context* message,
                  const char* path, const char* arguments, const struct sys_process_configuration* configuration)
{
    SHMTransfer_t transfer;
    size_t i;

    g_sends++;
    assert(__FromSysSHMTransfer(&configuration->data_buffer, &transfer) == OS_EOK);
    assert(transfer.Token.Options.Scope == GrantTokenScopeRead);
    assert(transfer.RangeMode == SHMTransferRangeFull);
    assert(configuration->inherit_block_length == 0);
    for (i = configuration->environ_block_length; i < g_capacity; i++) {
        assert(((char*)g_buffer)[i] == 0);
    }
    return g_sendError;
}

int
sys_process_spawn_result(gracht_client_t* client, struct gracht_message_context* message,
                         oserr_t* status, uuid_t* handle)
{
    *status = g_remoteError;
    *handle = 77;
    return g_resultError;
}

int
sys_process_get_startup_information(gracht_client_t* client, struct gracht_message_context* message,
                                   uuid_t thread, const struct sys_shm_transfer* destination)
{
    SHMTransfer_t transfer;
    ProcessStartupInformation_t header = { 3, 0, sizeof(uintptr_t), 1 };
    uintptr_t library = 0x1234;
    char* output = g_buffer;

    g_sends++;
    assert(thread == 88);
    assert(__FromSysSHMTransfer(destination, &transfer) == OS_EOK);
    assert(transfer.Token.Options.Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    assert(transfer.RangeMode == SHMTransferRangeFull);
    memcpy(output, &header, sizeof(header));
    memcpy(output + sizeof(header), "ab", 3);
    memcpy(output + sizeof(header) + 3, &library, sizeof(library));
    g_replyBytes = sizeof(header) + 3 + sizeof(library) + 1;
    return g_sendError;
}

int
sys_process_get_startup_information_result(gracht_client_t* client, struct gracht_message_context* message,
                                          oserr_t* status, uuid_t* handle, uint32_t* bytes)
{
    *status = g_remoteError;
    *handle = 77;
    *bytes = g_overrideBytes != 0 ? g_overrideBytes : g_replyBytes;
    return g_resultError;
}

static void
__ResetFailure(void)
{
    assert(g_allocations == 0 && g_buffers == 0 && g_tokens == 0);
    g_createError = g_tokenError = g_remoteError = OS_EOK;
    g_sendError = g_waitError = g_resultError = 0;
    g_messageStatus = GRACHT_MESSAGE_COMPLETED;
    g_overrideBytes = 0;
    g_failAfter = -1;
}

static void
__ClearStartup(void)
{
    __TestFree(g_rawCommandLine);
    __TestFree(g_baseLibraries);
    __free_environment(g_environment);
    g_rawCommandLine = NULL;
    g_baseLibraries = NULL;
    g_environment = NULL;
    g_processId = UUID_INVALID;
}

static void
__TestSpawn(void)
{
    OSProcessOptions_t options;
    const char* environment[] = { "LDPATH=/boot", NULL };
    char* oversized = malloc(PROCESS_STARTUP_MAX_SIZE + 1);
    const char* largeEnvironment[] = { oversized, NULL };
    uuid_t handle = 444;
    unsigned int sends;
    int failure;

    __OSProcessOptionsConstruct(&options);
    options.Environment = environment;
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_EOK && handle == 77);
    __ResetFailure();
    options.Environment = NULL;
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_EOK);
    assert(g_capacity == 4096);
    __ResetFailure();
    memset(oversized, 'x', PROCESS_STARTUP_MAX_SIZE);
    oversized[PROCESS_STARTUP_MAX_SIZE] = '\0';
    options.Environment = largeEnvironment;
    sends = g_sends;
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_EBUFFER);
    assert(g_sends == sends);
    __ResetFailure();
    free(oversized);
    options.Environment = environment;
    options.InheritanceOptions.Flags = PROCESS_INHERIT_STDOUT;
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_ENOTSUPPORTED);
    options.InheritanceOptions.Flags = PROCESS_INHERIT_NONE;

    for (failure = 0; failure < 8; failure++) {
        handle = 444;
        sends = g_sends;
        switch (failure) {
            case 0: g_failAfter = 0; break;
            case 1: g_createError = OS_EOOM; break;
            case 2: g_tokenError = OS_EPERMISSIONS; break;
            case 3: g_sendError = -1; break;
            case 4: g_waitError = -1; break;
            case 5: g_messageStatus = GRACHT_MESSAGE_INPROGRESS; break;
            case 6: g_resultError = -1; break;
            case 7: g_remoteError = OS_EBUFFER; break;
        }
        assert(OSProcessSpawnOpts("/test", &options, &handle) != OS_EOK);
        assert(handle == 444);
        assert(g_sends <= sends + 1);
        __ResetFailure();
    }
}

static void
__TestStartup(void)
{
    int failure;

    assert(__get_startup_info() == OS_EOK);
    assert(g_processId == 77 && strcmp(g_rawCommandLine, "ab") == 0);
    assert(g_baseLibraries[0] == 0x1234 && g_baseLibraries[1] == 0);
    __ClearStartup();
    __ResetFailure();
    for (failure = 0; failure < 11; failure++) {
        switch (failure) {
            case 0: g_createError = OS_EOOM; break;
            case 1: g_tokenError = OS_EPERMISSIONS; break;
            case 2: g_sendError = -1; break;
            case 3: g_waitError = -1; break;
            case 4: g_resultError = -1; break;
            case 5: g_remoteError = OS_EPERMISSIONS; break;
            case 6: g_overrideBytes = sizeof(ProcessStartupInformation_t) - 1; break;
            case 7: g_overrideBytes = PROCESS_STARTUP_MAX_SIZE + 1; break;
            case 8: g_failAfter = 0; break;
            case 9: g_failAfter = 1; break;
            case 10: g_failAfter = 2; break;
        }
        assert(__get_startup_info() != OS_EOK);
        assert(g_processId == UUID_INVALID && g_rawCommandLine == NULL && g_baseLibraries == NULL);
        __ResetFailure();
    }
}

static void
__TestMalformedPayloads(void)
{
    unsigned char storage[128] = { 0 };
    OSHandle_t handle = { .Payload = storage + 1 };
    ProcessStartupInformation_t header = { 3, 0, sizeof(uintptr_t), 1 };
    uint32_t length = sizeof(header) + 3 + sizeof(uintptr_t) + 1;
    uintptr_t library = 0x1234;
    char** pairs = NULL;
    int test;

    g_capacity = sizeof(storage) - 1;
    for (test = 0; test < 7; test++) {
        memset(storage, 0, sizeof(storage));
        header = (ProcessStartupInformation_t) { 3, 0, sizeof(uintptr_t), 1 };
        switch (test) {
            case 0: header.ArgumentsLength = UINT32_MAX; break;
            case 1: header.LibraryEntriesLength = UINT32_MAX; break;
            case 2: header.EnvironmentBlockLength = UINT32_MAX; break;
            case 3: header.InheritationLength = 1; break;
            case 4: header.LibraryEntriesLength--; header.EnvironmentBlockLength++; break;
            case 5: storage[length] = 'x'; break;
            case 6: header.EnvironmentBlockLength = 0; break;
        }
        memcpy(storage + 1, &header, sizeof(header));
        memcpy(storage + 1 + sizeof(header), "ab", 3);
        memcpy(storage + 1 + sizeof(header) + 3, &library, sizeof(library));
        assert(__parse_startup_info(&handle, length) != OS_EOK);
        assert(g_rawCommandLine == NULL && g_baseLibraries == NULL && g_environment == NULL);
        assert(g_allocations == 0);
    }
    assert(__build_environment("A=b\0", 4, &pairs) == OS_EINVALPARAMS);
    assert(__build_environment("\0tail", 6, &pairs) == OS_EINVALPARAMS);
    g_failAfter = 1;
    assert(__build_environment("A=b\0", 5, &pairs) == OS_EOOM);
    assert(pairs == NULL && g_allocations == 0);
    g_failAfter = -1;
    assert(__build_environment("A=b\0", 5, &pairs) == OS_EOK);
    assert(strcmp(pairs[0], "A=b") == 0 && pairs[1] == NULL);
    __free_environment(pairs);
    assert(g_allocations == 0);
}

int
main(void)
{
    __TestSpawn();
    __TestStartup();
    __TestMalformedPayloads();
    assert(g_abandons == 3);
    puts("Startup clients: token scopes, padding, transport failures, parsing and allocation cleanup passed");
    return 0;
}
