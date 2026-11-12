// Exercise production spawn/startup clients, failure cleanup and bounded parsing.
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
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
#ifndef STARTUP_SOURCE
#define STARTUP_SOURCE "../librt/libc/os/init.c"
#endif
#include STARTUP_SOURCE
#undef malloc
#undef calloc
#undef strndup
#undef free
#define main __TestProgramMain
#include "../librt/libcrt/crt/crtinit.c"
#undef main

static bool g_inBootstrapJob;
static usched_task_fn g_bootstrapEntry;
static void* g_bootstrapArgument;
static unsigned int g_bootstrapDispatches;
static unsigned int g_bootstrapWaits;
static int g_bootstrapPendingError;
static bool g_bootstrapQueueFailure;
static bool g_bootstrapAdopted;
static bool g_standardHandlesConfigured;
static bool g_constructorsRan;
static unsigned int g_environmentRefreshes;
static int g_environmentError;
static jmp_buf g_exitJump;
static bool g_expectExit;
static int g_exitCode;
static struct thread_storage* g_testTLS;
static bool g_exportFiles;
static unsigned int g_fileExports;
oserr_t CRTWriteInheritanceBlock(const struct InheritanceOptions* options, void* buffer,
    uint32_t capacity, struct inheritance_export* exported)
{
    memset(exported, 0, sizeof(*exported));
    if (options->Flags == PROCESS_INHERIT_NONE) return OS_EOK;
    if (!g_exportFiles) return OS_ENOENT;
    assert(capacity >= 8);
    exported->controls = __TestMalloc(sizeof(uuid_t));
    if (exported->controls == NULL) return OS_EOOM;
    exported->count = 1;
    exported->length = 8;
    memset(buffer, 'I', 8);
    g_fileExports++;
    return OS_EOK;
}
void CRTReleaseInheritanceBlock(struct inheritance_export* exported)
{
    if (exported->count != 0) {
        assert(g_fileExports == 1);
        g_fileExports--;
        __TestFree(exported->controls);
        memset(exported, 0, sizeof(*exported));
    }
}
oserr_t CRTReadInheritanceBlock(const void* buffer, uint32_t length)
{
    g_bootstrapAdopted = length == 0;
    return length == 0 ? OS_EOK : OS_EINVALPARAMS;
}

int OsErrToErrNo(oserr_t error) { errno = EINVAL; return -1; }
void StdioInitialize(void) {}
void StdSignalInitialize(void) {}
void StdMmapInitialize(void) {}
void OSHandlesInitialize(void) {}
void usched_xunit_init(void) {}
uuid_t usched_job_queue(usched_task_fn entry, void* argument)
{
    assert(g_bootstrapEntry == NULL);
    if (g_bootstrapQueueFailure) {
        return UUID_INVALID;
    }
    g_bootstrapEntry = entry;
    g_bootstrapArgument = argument;
    return 1;
}
int usched_yield(struct timespec* deadline)
{
    struct thread_storage* initial = g_testTLS;
    struct thread_storage job = { .async_context = (void*)2 };
    // Exercise the wait path before dispatch. Constructors must not run merely
    // because the bootstrap job has been queued or the scheduler has yielded.
    if (g_bootstrapDispatches++ == 0) {
        assert(!g_constructorsRan);
        if (g_bootstrapPendingError == 0) {
            return 0;
        }
        *deadline = (struct timespec) { .tv_sec = 123, .tv_nsec = 456 };
        errno = g_bootstrapPendingError;
        return -1;
    }
    assert(g_bootstrapEntry != NULL);
    g_inBootstrapJob = true;
    g_testTLS = &job;
    g_bootstrapEntry(g_bootstrapArgument, NULL);
    assert(job.async_context == (void*)2);
    g_testTLS = initial;
    g_inBootstrapJob = false;
    g_bootstrapEntry = NULL;
    errno = ENOENT;
    return -1;
}
void usched_timedwait(const struct timespec* deadline)
{
    assert(g_bootstrapPendingError == EWOULDBLOCK);
    assert(deadline->tv_sec == 123 && deadline->tv_nsec == 456);
    assert(g_bootstrapEntry != NULL && !g_constructorsRan);
    g_bootstrapWaits++;
}
void usched_wait(void)
{
    assert(g_bootstrapPendingError == ENOENT);
    assert(g_bootstrapEntry != NULL && !g_constructorsRan);
    g_bootstrapWaits++;
}

void StdioConfigureStandardHandles(void)
{
    assert(g_bootstrapAdopted || g_isPhoenix);
    assert(g_inBootstrapJob && g_testTLS->async_context == NULL);
    assert(g_gclient != NULL);
    g_standardHandlesConfigured = true;
}
stdio_handle_t* stdio_handle_get(int fd) { return NULL; }
void stdio_handle_set_flags(stdio_handle_t* handle, unsigned int flags) {}
struct thread_storage* __tls_current(void) { return g_testTLS; }
int __tls_update_environment(void)
{
    assert(!g_inBootstrapJob && g_testTLS->async_context == (void*)1);
    assert(g_bootstrapAdopted && g_standardHandlesConfigured);
    g_environmentRefreshes++;
    return g_environmentError;
}
int __tls_initialize(struct thread_storage* tls, int use)
{
    memset(tls, 0, sizeof(*tls));
    tls->async_context = (void*)1;
    g_testTLS = tls;
    return 0;
}
void __cxa_module_global_init(void) {}
void __cxa_module_tls_global_init(void)
{
    assert(g_standardHandlesConfigured);
    assert(g_isPhoenix || (g_bootstrapAdopted && g_environmentRefreshes == 1));
}
void __cxa_module_global_finit(void) {}
void __cxa_module_tls_thread_init(void) {}
void __cxa_module_tls_thread_finit(void) {}
void __cxa_runinitializers(const uintptr_t* libraries, void (*init)(void),
    void (*cleanup)(void), void (*threadInit)(void), void (*threadCleanup)(void))
{
    assert(!g_inBootstrapJob && g_standardHandlesConfigured);
    if (g_isPhoenix) {
        assert(libraries == NULL && g_processId == UUID_INVALID);
    } else {
        assert(g_bootstrapAdopted && g_processId == 77);
        assert(libraries != NULL && libraries[0] == 0x1234);
        assert(g_environmentRefreshes == 1);
    }
    g_constructorsRan = true;
}
void ThreadsFastExit(int exitCode)
{
    assert(g_expectExit && !g_constructorsRan && !g_inBootstrapJob);
    g_exitCode = exitCode;
    longjmp(g_exitJump, 1);
}
int gracht_link_vali_create(struct gracht_link_vali** link)
{
    assert(g_inBootstrapJob && g_testTLS->async_context == NULL);
    *link = (void*)1;
    return 0;
}
void gracht_client_configuration_init(gracht_client_configuration_t* config) {}
void gracht_client_configuration_set_link(gracht_client_configuration_t* config, struct gracht_link* link) {}
int gracht_client_create(gracht_client_configuration_t* config, gracht_client_t** client)
{
    *client = (void*)1;
    return 0;
}
int gracht_client_connect(gracht_client_t* client) { return 0; }
gracht_conn_t gracht_client_iod(gracht_client_t* client) { return 3; }

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
    assert(transfer.RelativeOffset == 0 && __SHMTransferLength(&transfer) == g_capacity);
    assert(configuration->inherit_block_length == (g_exportFiles ? 8U : 0U));
    if (g_exportFiles) {
        assert(g_fileExports == 1);
        assert(memcmp(g_buffer, "IIIIIIII", 8) == 0);
        assert(strcmp((char*)g_buffer + 8, "LDPATH=/boot") == 0);
    }
    for (i = configuration->inherit_block_length + configuration->environ_block_length; i < g_capacity; i++) {
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
    assert(transfer.RelativeOffset == 0 && __SHMTransferLength(&transfer) == PROCESS_STARTUP_MAX_SIZE);
    memcpy(output, &header, sizeof(header));
    memcpy(output + sizeof(header), "abc", 3);
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
    assert(g_capacity == PROCESS_STARTUP_MAX_SIZE);
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
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_ENOENT);
    g_exportFiles = true;
    OSProcessOptionsSetInheritance(&options, PROCESS_INHERIT_STDOUT, 0, 7, 2);
    assert(options.InheritanceOptions.StdOutHandle == 7);
    assert(OSProcessSpawnOpts("/test", &options, &handle) == OS_EOK);
    assert(g_fileExports == 0);

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
        assert(g_fileExports == 0);
        __ResetFailure();
    }
    g_exportFiles = false;
}

static void
__TestStartup(void)
{
    int failure;

    assert(__get_startup_info() == OS_EOK);
    assert(g_processId == 77 && strcmp(g_rawCommandLine, "abc") == 0);
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
        assert(g_processId == (failure >= 6 ? 77 : UUID_INVALID));
        assert(g_rawCommandLine == NULL && g_baseLibraries == NULL);
        __ClearStartup();
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
            case 3: header.InheritationLength = 1; header.EnvironmentBlockLength = 0; break;
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
    g_failAfter = 2;
    assert(__build_environment("A=b\0C=d\0", 9, &pairs) == OS_EOOM);
    assert(pairs == NULL && g_allocations == 0);
    g_failAfter = -1;
    assert(__build_environment("A=b\0", 5, &pairs) == OS_EOK);
    assert(strcmp(pairs[0], "A=b") == 0 && pairs[1] == NULL);
    __free_environment(pairs);
    assert(g_allocations == 0);
}

static void
__TestStartupBounds(
    void)
{
    unsigned char               storage[128] = { 0 };
    OSHandle_t                  handle = { .Payload = storage + 1 };
    ProcessStartupInformation_t header = { 3, 0, sizeof(uintptr_t), 0 };
    uint32_t                    length = sizeof(header) + 3 + sizeof(uintptr_t);
    uintptr_t                   library = 0x1234;
    uintptr_t                   modules[PROCESS_MAXMODULES] = { 0x1234 };
    uintptr_t*                  parsed = NULL;

    memcpy(storage + 1, &header, sizeof(header));
    memcpy(storage + 1 + sizeof(header), "abc", 3);
    memcpy(storage + 1 + sizeof(header) + 3, &library, sizeof(library));
    g_capacity = length - 1;
    assert(__parse_startup_info(&handle, length) == OS_EBUFFER);
    assert(g_allocations == 0);

    g_capacity = length;
    assert(__parse_startup_info(&handle, length) == OS_EOK);
    assert(strcmp(g_rawCommandLine, "abc") == 0);
    assert(g_baseLibraries[0] == library && g_baseLibraries[1] == 0);
    assert(g_environment == NULL && __crt_environment()[0] == NULL);
    __ClearStartup();

    assert(__parse_library_handles((const char*)modules, 0, &parsed) == OS_ENOENT);
    assert(__parse_library_handles((const char*)modules, sizeof(uintptr_t) - 1,
        &parsed) == OS_EINVALPARAMS);
    assert(__parse_library_handles((const char*)modules, sizeof(modules),
        &parsed) == OS_EINVALPARAMS);
    assert(parsed == NULL);
    assert(__parse_library_handles((const char*)modules, sizeof(modules) - sizeof(uintptr_t),
        &parsed) == OS_EOK);
    assert(parsed[0] == library && parsed[PROCESS_MAXMODULES - 1] == 0);
    __TestFree(parsed);
    __ResetFailure();
}

static void
__ResetBootstrap(
    _In_ int pendingError)
{
    g_bootstrapEntry = NULL;
    g_bootstrapArgument = NULL;
    g_bootstrapDispatches = 0;
    g_bootstrapWaits = 0;
    g_bootstrapPendingError = pendingError;
    g_bootstrapQueueFailure = false;
    g_bootstrapAdopted = false;
    g_standardHandlesConfigured = false;
    g_constructorsRan = false;
    g_environmentRefreshes = 0;
    g_environmentError = 0;
    g_expectExit = false;
    g_gclient = NULL;
    g_gclientLink = NULL;
}

static void
__TestBootstrap(
    _In_ int  pendingError,
    _In_ bool isPhoenix)
{
    struct thread_storage tls;
    unsigned int          sends = g_sends;

    __ResetBootstrap(pendingError);
    __crt_initialize(&tls, isPhoenix);
    assert(g_constructorsRan && tls.async_context == (void*)1);
    assert(g_bootstrapEntry == NULL && g_bootstrapDispatches == 2);
    assert(g_bootstrapWaits == (pendingError == 0 ? 0U : 1U));
    assert(g_sends == sends + (isPhoenix ? 0U : 1U));
    assert(g_environmentRefreshes == (isPhoenix ? 0U : 1U));
    __ClearStartup();
    __ResetFailure();
}

static void
__TestBootstrapFailures(
    void)
{
    struct thread_storage tls;
    int                   failure;

    for (failure = 0; failure < 3; failure++) {
        __ResetBootstrap(0);
        if (failure == 0) {
            g_bootstrapQueueFailure = true;
        } else if (failure == 1) {
            g_bootstrapPendingError = ESHUTDOWN;
        } else {
            g_environmentError = -1;
        }
        g_expectExit = true;
        if (setjmp(g_exitJump) == 0) {
            __crt_initialize(&tls, false);
            assert(0 && "Bootstrap failure must prevent constructors");
        }
        assert(g_exitCode == (failure == 1 ? -ESHUTDOWN : -ENOMEM));
        assert(g_bootstrapWaits == 0 && !g_constructorsRan);
        __ClearStartup();
        __ResetFailure();
    }
}

int
main(void)
{
    __TestSpawn();
    __TestStartup();
    __TestMalformedPayloads();
    __TestStartupBounds();
    assert(g_abandons == 3);
    __TestBootstrap(EWOULDBLOCK, false);
    __TestBootstrap(ENOENT, false);
    __TestBootstrap(0, false);
    __TestBootstrap(EWOULDBLOCK, true);
    __TestBootstrapFailures();
    puts("Startup clients: token scopes, padding, transport failures, parsing and allocation cleanup passed");
    return 0;
}
