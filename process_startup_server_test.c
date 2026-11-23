// Exercise real request handlers, startup authorization and bounded output.
#ifndef PROCESSD_SOURCE
#define PROCESSD_SOURCE "../services/processd/process.c"
#endif
#include PROCESSD_SOURCE
static oserr_t
__TestCreateProcess(
    _In_  const struct gracht_security_context* caller,
    _In_  const char*                          path,
    _In_  const char*                          arguments,
    _In_  struct ProcessOptions*               options,
    _Out_ OSProcessIdentity_t*                 identityOut);
#define PmCreateProcess __TestCreateProcess
#include "../services/processd/requests.c"
#undef PmCreateProcess

oserr_t Syscall_HandleImport(const GrantToken_t* token, uuid_t destination, uuid_t* reference)
{
    assert(0 && "Empty inheritance must not import");
    return OS_ENOTSUPPORTED;
}

static struct gracht_security_context g_caller = { .source = GRACHT_SECURITY_KERNEL, .principal = 42 };
static unsigned char g_storage[PROCESS_STARTUP_MAX_SIZE + 4096];
static const char g_environment[] = "LDPATH=/boot\0";
static SHMTransfer_t g_transfer;
static size_t g_mapOffset;
static size_t g_mapLength;
static unsigned int g_attaches;
static unsigned int g_active;
static unsigned int g_creates;
static unsigned int g_deleted;
static oserr_t g_attachError;
static oserr_t g_mapError;
static oserr_t g_moduleError;
static oserr_t g_response;
static uint32_t g_written;
static OSProcessIdentity_t g_spawnIdentity;
static int g_modules = 2;
static bool g_mutateInput;
static bool g_terminateDuringMap;
static bool g_terminateDuringWait;
static bool g_spuriousWake;
static void (*g_processUnlockHook)(void);

// processd's exit watcher. Syscall_GetMemorySpaceForThread answers
// g_threadLookupStatus, so OS_EOK means the primary thread still exists.
// usched_job_queue records the watcher job instead of running it, and fails
// while g_watcherQueueFails is set.
static oserr_t        g_threadLookupStatus = OS_EOK;
static unsigned int   g_threadLookups;
static unsigned int   g_watcherJobs;
static bool           g_watcherQueueFails;
static usched_task_fn g_watcherEntry;
static unsigned int   g_conditionNotifies;

// Tests that check the log set this. It gets every formatted log line.
static void (*g_logHook)(enum OSSysLogLevel level, const char* line);

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    _In_ ...)
{
    char    hostFormat[256];
    char    line[512];
    char*   mstring;
    va_list arguments;

    if (g_logHook == NULL) {
        return;
    }

    // processd prints mstring_t with %ms. The test's mstrings are plain C
    // strings, so the host printf gets %s instead.
    snprintf(hostFormat, sizeof(hostFormat), "%s", format);
    while ((mstring = strstr(hostFormat, "%ms")) != NULL) {
        memmove(mstring + 1, mstring + 2, strlen(mstring + 2) + 1);
    }
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), hostFormat, arguments);
    va_end(arguments);
    g_logHook(level, line);
}

oserr_t
Syscall_GetMemorySpaceForThread(
    _In_  uuid_t  thread,
    _Out_ uuid_t* memorySpaceOut)
{
    g_threadLookups++;
    if (g_threadLookupStatus == OS_EOK) {
        *memorySpaceOut = 555;
    }
    return g_threadLookupStatus;
}

uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void*          argument)
{
    if (g_watcherQueueFails) {
        return UUID_INVALID;
    }
    g_watcherJobs++;
    g_watcherEntry = entry;
    return 1;
}

int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    assert(until != NULL);
    return 0;
}
void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}
void
dsfree(
    _In_ void* memory)
{
    free(memory);
}
void
mstr_delete(
    _In_ mstring_t* string)
{
    free(string);
}
void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int type)
{
    memset(mutex, 0, sizeof(*mutex));
}
void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    assert(mutex->references == 0);
    mutex->references = 1;
}
void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    assert(mutex->references == 1);
    mutex->references = 0;
    if (mutex == &g_processesLock && g_processUnlockHook != NULL) {
        g_processUnlockHook();
    }
}
void
usched_cnd_notify_all(
    _In_ struct usched_cnd* condition)
{
    (void)condition;
    g_conditionNotifies++;
}
int
usched_cnd_timedwait(
    struct usched_cnd* condition,
    struct usched_mtx* mutex,
    const struct timespec* deadline)
{
    assert(mutex->references == 1);
    if (g_spuriousWake) {
        g_spuriousWake = false;
        return 0;
    }
    if (g_terminateDuringWait) {
        g_terminateDuringWait = false;
        usched_mtx_unlock(mutex);
        assert(PmTerminateProcess(77, 123) == OS_EOK);
        usched_mtx_lock(mutex);
    }
    return -1;
}
void
PEImageLoadContextDelete(
    _In_ struct PEImageLoadContext* context)
{
    free(context);
    g_deleted++;
}

oserr_t
PEModuleEntryPoints(
    _In_ struct PEImageLoadContext* context,
    _In_ uintptr_t* entries,
    _In_ int* count)
{
    int i;
    assert(*count == PROCESS_MAXMODULES);
    if (g_moduleError != OS_EOK) {
        return g_moduleError;
    }
    for (i = 0; i < g_modules && i < PROCESS_MAXMODULES; i++) {
        entries[i] = 0x1000 + i;
    }
    *count = g_modules;
    return OS_EOK;
}

const struct gracht_security_context*
gracht_server_get_security_context(
    _In_ const struct gracht_message* message)
{
    return &g_caller;
}

oserr_t
SHMAttachToken(
    _In_ const GrantToken_t* token,
    _In_ OSHandle_t* handle)
{
    g_attaches++;
    if (g_attachError != OS_EOK) {
        return g_attachError;
    }
    assert(g_active == 0);
    g_transfer.Token = *token;
    *handle = (OSHandle_t) { .ID = 99, .Type = OSHANDLE_SHM };
    g_active++;
    return OS_EOK;
}

oserr_t
SHMMap(
    _In_ OSHandle_t* handle,
    _In_ size_t offset,
    _In_ size_t length,
    _In_ unsigned int flags)
{
    assert(handle->ID == 99 && g_active == 1);
    g_mapOffset = offset;
    g_mapLength = length;
    if (g_mapError != OS_EOK) {
        return g_mapError;
    }
    if (!(g_transfer.Token.Options.Scope & GrantTokenScopeRead) ||
        ((flags & SHM_ACCESS_WRITE) && !(g_transfer.Token.Options.Scope & GrantTokenScopeWrite))) {
        return OS_EPERMISSIONS;
    }
    assert(offset + length <= sizeof(g_storage));
    if (g_terminateDuringMap) {
        g_terminateDuringMap = false;
        assert(PmTerminateProcess(77, 0) == OS_EOK);
        assert(g_deleted == 0);
    }
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return g_storage + g_mapOffset;
}
void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    if (handle->ID == 99) {
        assert(g_active == 1);
        g_active--;
        if (g_mutateInput) {
            memset(g_storage, 0xee, sizeof(g_storage));
        }
    } else {
        // A process handle. The processd exit test lists processes with
        // other handles than 77.
        assert(handle->ID != UUID_INVALID);
    }
}

static oserr_t
__TestCreateProcess(
    _In_  const struct gracht_security_context* caller,
    _In_  const char*                          path,
    _In_  const char*                          arguments,
    _In_  struct ProcessOptions*               options,
    _Out_ OSProcessIdentity_t*                 identityOut)
{
    struct sys_process_inheritance_input input;
    oserr_t status = __ValidateProcessInput(path, arguments, options);
    if (status != OS_EOK) {
        return status;
    }
    status = __ReadInheritanceInput(options->DataBuffer, options->InheritationBlockLength, &input);
    if (status != OS_EOK) {
        return status;
    }
    sys_process_inheritance_input_destroy(&input);
    assert(g_active == 0);
    assert(options->DataBuffer != g_storage);
    assert(options->EnvironmentBlockLength == sizeof(g_environment));
    assert(memcmp(options->DataBuffer, g_environment, sizeof(g_environment)) == 0);
    g_creates++;
    *identityOut = (OSProcessIdentity_t) { .Process = 77, .Principal = 0x123456780000002aULL };
    return __ValidateProcessInput(path, arguments, options);
}

int
sys_process_spawn_response(
    _In_ struct gracht_message* message,
    _In_ oserr_t                result,
    _In_ uuid_t                 handle,
    _In_ uint64_t               principal)
{
    assert(g_active == 0);
    g_response = result;
    g_spawnIdentity.Process = handle;
    g_spawnIdentity.Principal = principal;
    if (result != OS_EOK) {
        assert(handle == UUID_INVALID && principal == 0);
    }
    return 0;
}

int
sys_process_get_startup_information_response(struct gracht_message* message, oserr_t result,
                                            uuid_t handle, uint32_t bytes)
{
    assert(g_active == 0);
    g_response = result;
    g_written = bytes;
    if (result != OS_EOK) {
        assert(handle == UUID_INVALID && bytes == 0);
    }
    return 0;
}

static SHMTransfer_t
__Transfer(void)
{
    SHMTransfer_t transfer = {
        .Token = {
            .Version = GRANT_TOKEN_VERSION,
            .ControlHandleID = 5,
            .ExportGlobalHandleID = 5,
            .Options = {
                .ResourceType = GrantTokenResourceTypeSHM,
                .TargetType = GrantTokenTargetTypeAny,
                .TargetGlobalHandleID = UUID_INVALID,
                .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
                .Data.SHM = { .Length = PROCESS_STARTUP_MAX_SIZE }
            }
        },
        .RangeMode = SHMTransferRangeFull
    };
    return transfer;
}

static void
__TestInput(void)
{
    struct sys_process_configuration configuration = { 0 };
    SHMTransfer_t transfer = __Transfer();
    struct ProcessOptions options = { 0 };
    unsigned int before;

    configuration.environ_block_length = sizeof(g_environment);
    transfer.Token.Options.Scope = GrantTokenScopeRead;
    assert(__ToSysSHMTransfer(&transfer, &configuration.data_buffer) == OS_EOK);
    memcpy(g_storage, g_environment, sizeof(g_environment));
    g_mutateInput = true;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EOK && g_creates == 1);
    assert(g_spawnIdentity.Process == 77);
    assert(g_spawnIdentity.Principal == 0x123456780000002aULL);
    g_mutateInput = false;
    memcpy(g_storage, g_environment, sizeof(g_environment));

    before = g_attaches;
    configuration.capabilities = CAPABILITY_DEVICEIO_ACCESS;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EPERMISSIONS && g_attaches == before && g_creates == 1);
    assert(g_spawnIdentity.Process == UUID_INVALID && g_spawnIdentity.Principal == 0);
    configuration.capabilities = 0;
    sys_process_spawn_invocation(NULL, "", "", &configuration);
    assert(g_response == OS_EINVALPARAMS && g_attaches == before);

    before = g_attaches;
    configuration.inherit_block_length = 1;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EINVALPARAMS && g_attaches == before + 1);
    before = g_attaches;
    configuration.inherit_block_length = 0;
    configuration.environ_block_length = UINT32_MAX;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EBUFFER && g_attaches == before);
    configuration.environ_block_length = 1;
    configuration.data_buffer.requested_length = 1;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EINVALPARAMS && g_attaches == before);
    configuration.data_buffer.requested_length = 0;
    configuration.data_buffer.token.scope = GrantTokenScopeWrite;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EPERMISSIONS && g_attaches == before);
    configuration.data_buffer.token.scope = GrantTokenScopeRead;
    g_attachError = OS_EPERMISSIONS;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EPERMISSIONS && g_active == 0);
    g_attachError = OS_EOK;
    g_mapError = OS_EOOM;
    sys_process_spawn_invocation(NULL, "/test", "", &configuration);
    assert(g_response == OS_EOOM && g_active == 0);
    g_mapError = OS_EOK;

    // Validate the exact snapshot length before load-path lookup can read it.
    options.DataBuffer = "LDPATH=x";
    options.EnvironmentBlockLength = 8;
    assert(__ValidateProcessInput("/test", "", &options) == OS_EINVALPARAMS);
    options.EnvironmentBlockLength = 9;
    assert(__ValidateProcessInput("/test", "", &options) == OS_EINVALPARAMS);
    options.DataBuffer = "\0junk";
    options.EnvironmentBlockLength = 5;
    assert(__ValidateProcessInput("/test", "", &options) == OS_EINVALPARAMS);
    options.DataBuffer = "";
    options.EnvironmentBlockLength = 1;
    assert(__ValidateProcessInput("/test", "", &options) == OS_EOK);
    options.EnvironmentBlockLength = 0;
    options.DataBuffer = NULL;
    assert(__ValidateProcessInput("/test", "", &options) == OS_EOK);
}

static void
__TestOutput(void)
{
    Process_t* process = calloc(1, sizeof(*process));
    Process_t sizing = { 0 };
    struct sys_process_inheritance_input input = { 0 };
    SHMTransfer_t transfer = __Transfer();
    struct sys_shm_transfer wire;
    uint32_t length = 0;
    unsigned int before;
    uuid_t result = UUID_INVALID;
    size_t boundary;
    char* arguments;
    Process_t* referenced;
    int exitCode;

    process->handle.ID = 77;
    process->primary_thread_id = 88;
    process->references = 1;
    process->load_context = calloc(1, sizeof(*process->load_context));
    process->load_context->SecurityGroupID = 42;
    assert(PmPrepareStartupInformation(process, "", 1, "ab", 3, &input) == OS_EOK);
    length = process->startup_data.Length;
    hashtable_set(&g_processes, &(struct process_entry) { 77, process });
    hashtable_set(&g_threadmappings, &(struct thread_mapping) { 77, 88 });
    assert(PmReferenceProcessByThread(89, &referenced) == OS_ENOENT);
    assert(PmReferenceProcessByThread(88, &referenced) == OS_EOK && referenced == process);
    assert(process->references == 2);
    PmReleaseProcess(referenced);

    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    before = g_attaches;
    g_caller.source = GRACHT_SECURITY_NONE;
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EPERMISSIONS && g_attaches == before);
    g_caller.source = GRACHT_SECURITY_KERNEL;
    g_caller.principal = (UINT64_C(1) << 32) | 42;
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EPERMISSIONS && g_attaches == before);
    g_caller.principal = 43;
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EPERMISSIONS && g_attaches == before);
    g_caller.principal = 42;
    assert(PmGetProcessStartupInformation(NULL, 88, &transfer, &result, &length) == OS_EPERMISSIONS);
    process->references = INT_MAX;
    assert(PmGetProcessStartupInformation(&g_caller, 88, &transfer, &result, &length) == OS_EOVERFLOW);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOVERFLOW);
    process->references = 1;
    assert(g_attaches == before);
    g_spuriousWake = true;
    assert(PmJoinProcess(77, 1, &exitCode) == OS_ETIMEOUT && process->references == 1);
    assert(!g_spuriousWake);
    transfer.Token.Options.Scope = GrantTokenScopeRead;
    assert(PmGetProcessStartupInformation(&g_caller, 88, &transfer, &result, &length) == OS_EPERMISSIONS);
    assert(g_attaches == before && process->references == 1);
    transfer.Token.Options.Scope = GrantTokenScopeRead | GrantTokenScopeWrite;

    assert(length == sizeof(ProcessStartupInformation_t) + 3 + sizeof(uintptr_t) + 1);
    memset(g_storage, 0xa5, sizeof(g_storage));
    transfer.RangeMode = SHMTransferRangeSubrange;
    transfer.RelativeOffset = 4096;
    transfer.RequestedLength = length - 1;
    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EBUFFER && process->references == 1);
    assert(g_storage[4096] == 0xa5 && g_storage[4096 + length - 1] == 0xa5);
    transfer.RequestedLength = length;
    assert(__ToSysSHMTransfer(&transfer, &wire) == OS_EOK);
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EOK && g_written == length && g_mapOffset == 4096 && g_mapLength == length);
    assert(g_storage[4095] == 0xa5 && g_storage[4096 + length] == 0xa5);
    assert(memcmp(g_storage + 4096 + sizeof(ProcessStartupInformation_t), "ab", 3) == 0);
    assert(process->references == 1);

    sizing = *process;
    sizing.startup_data = (struct ProcessStartupData) { 0 };
    g_moduleError = OS_EOOM;
    assert(PmPrepareStartupInformation(&sizing, "", 1, "ab", 3, &input) == OS_EOOM);
    assert(sizing.startup_data.Buffer == NULL);
    // Once prepared, output neither enumerates modules nor depends on their changes.
    assert(PmGetProcessStartupInformation(&g_caller, 88, &transfer, &result, &length) == OS_EOK);
    g_moduleError = OS_EOK;
    g_modules = 63;
    assert(PmGetProcessStartupInformation(&g_caller, 88, &transfer, &result, &length) == OS_EOK);
    assert(length == process->startup_data.Length);
    g_modules = 2;
    boundary = PROCESS_STARTUP_MAX_SIZE - sizeof(ProcessStartupInformation_t) - sizeof(uintptr_t) - 1;
    arguments = calloc(1, PROCESS_STARTUP_MAX_SIZE);
    assert(PmPrepareStartupInformation(&sizing, "", 1, arguments, boundary, &input) == OS_EOK);
    assert(sizing.startup_data.Length == PROCESS_STARTUP_MAX_SIZE);
    free(sizing.startup_data.Buffer);
    sizing.startup_data = (struct ProcessStartupData) { 0 };
    assert(PmPrepareStartupInformation(&sizing, "", 1, arguments, boundary + 1, &input) == OS_EBUFFER);
    assert(PmPrepareStartupInformation(&sizing, "", 1, arguments, SIZE_MAX, &input) == OS_EBUFFER);
    free(arguments);

    g_terminateDuringMap = true;
    sys_process_get_startup_information_invocation(NULL, 88, &wire);
    assert(g_response == OS_EOK && g_deleted == 0 && g_active == 0);
    // The process reported its exit code but its thread still runs, so it
    // stays listed until the exit watcher finds the thread gone.
    assert(PmGetProcessByHandle(77) == process && process->state == ProcessState_TERMINATING);
    assert(g_conditionNotifies == 1 && g_watcherJobs == 1 && g_watcherEntry != NULL);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 0);
    g_threadLookupStatus = OS_ENOENT;
    assert(PmRemoveExitedProcesses() == 0 && g_deleted == 1 && g_threadLookups == 1);
    g_threadLookupStatus = OS_EOK;
    assert(PmGetProcessByHandle(77) == NULL);
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 0);
    assert(PmReferenceProcessByThread(88, &referenced) == OS_ENOENT);
}

static void
__TestJoinTermination(void)
{
    Process_t* process = calloc(1, sizeof(*process));
    int        exitCode;
    unsigned int before = g_deleted;

    process->handle.ID = 77;
    process->primary_thread_id = 88;
    process->references = 1;
    process->load_context = calloc(1, sizeof(*process->load_context));
    hashtable_set(&g_processes, &(struct process_entry) { 77, process });
    hashtable_set(&g_threadmappings, &(struct thread_mapping) { 77, 88 });
    g_terminateDuringWait = true;
    assert(PmJoinProcess(77, 1, &exitCode) == OS_EOK && exitCode == 123);
    assert(g_deleted == before);
    g_threadLookupStatus = OS_ENOENT;
    assert(PmRemoveExitedProcesses() == 0 && g_deleted == before + 1);
    g_threadLookupStatus = OS_EOK;
}

int
main(void)
{
    PmInitialize();
    __TestInput();
    __TestOutput();
    __TestJoinTermination();
    hashtable_destroy(&g_threadmappings);
    hashtable_destroy(&g_processes);
    hashtable_destroy(&g_processHistory);
    puts("Startup server: snapshot, ranges, caller identity, capacity and process lifetime passed");
    return 0;
}
