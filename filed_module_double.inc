// A filesystem driver process (a module such as MFS), as filed sees it through
// interfaces/driver_ops.c. The double replaces the generated ctt_filesystem
// client calls and the gracht client, so the real driver_ops.c runs on top of
// it. Each sent request runs at once on an in-memory filesystem, and its
// answer is kept until driver_ops.c reads it.
//
// Every call can be made to fail at one step of the request (enum
// __ModuleFailure), with __ModuleFailCall. The double counts the driver
// handles each filesystem has open, the abandoned requests and the answers
// that were read, so a test can check that filed closed what it opened and
// read no answer it did not get.
//
// Include after filed_vfs_doubles.inc. The test needs the generated
// filesystem contract headers and the libddk conversion helpers.
#include <ctt_filesystem_service_client.h>
#include <os/convert/grants.h>

#define MODULE_MAX_FILESYSTEMS 4
#define MODULE_MAX_ENTRIES     32
#define MODULE_MAX_HANDLES     64
#define MODULE_MAX_PATH        128
#define MODULE_MAX_DATA        8192

enum __ModuleCall {
    MODULE_CALL_SETUP,
    MODULE_CALL_DESTROY,
    MODULE_CALL_FSSTAT,
    MODULE_CALL_OPEN,
    MODULE_CALL_CREATE,
    MODULE_CALL_CLOSE,
    MODULE_CALL_LINK,
    MODULE_CALL_UNLINK,
    MODULE_CALL_READLINK,
    MODULE_CALL_MOVE,
    MODULE_CALL_READ,
    MODULE_CALL_WRITE,
    MODULE_CALL_TRUNCATE,
    MODULE_CALL_SEEK,
    MODULE_CALL_COUNT
};

static const char* g_moduleCallNames[MODULE_CALL_COUNT] = {
    "setup", "destroy", "fsstat", "open", "create", "close", "link", "unlink",
    "readlink", "move", "read", "write", "truncate", "seek"
};

/**
 * @brief The step at which a request to the driver fails.
 * MODULE_FAIL_NONE:   the request is sent and answered.
 * MODULE_FAIL_SEND:   the request cannot be sent. The driver never sees it.
 * MODULE_FAIL_WAIT:   gracht_client_await fails. The driver does not run it.
 * MODULE_FAIL_STATUS: the driver sends an error message instead of an
 *                     answer, as gracht does when a call cannot be run.
 * MODULE_FAIL_READ:   the driver runs the call, but its answer cannot be
 *                     read (the _result call fails).
 */
enum __ModuleFailure {
    MODULE_FAIL_NONE,
    MODULE_FAIL_SEND,
    MODULE_FAIL_WAIT,
    MODULE_FAIL_STATUS,
    MODULE_FAIL_READ
};

static const char* g_moduleFailureNames[] = { "none", "send", "wait", "status", "read" };

/**
 * @brief One file or directory. Path is the full path inside the filesystem,
 * the way filed passes it, for example "/apps/file".
 */
struct __ModuleEntry {
    bool     Used;
    char     Path[MODULE_MAX_PATH];
    uint32_t Flags;
    uint32_t Permissions;
    uint8_t  Data[MODULE_MAX_DATA];
    size_t   Size;
};

/**
 * @brief One filesystem the driver set up. The root directory "/" is not
 * an entry, it always exists.
 */
struct __ModuleFileSystem {
    bool                 Used;
    char                 Label[32];
    int                  Handles;
    struct __ModuleEntry Entries[MODULE_MAX_ENTRIES];
};

/**
 * @brief An open file or directory. Position is the byte position in a
 * file, and for a directory whether its entries were read.
 */
struct __ModuleHandle {
    bool                       Used;
    struct __ModuleFileSystem* FileSystem;
    char                       Path[MODULE_MAX_PATH];
    uint64_t                   Position;
};

/**
 * @brief The answer of the last request of a thread, until driver_ops.c
 * reads it. State tells whether there is one. A thread has at most one
 * request waiting, like a usched job in filed.
 */
enum __ModuleAnswerState {
    MODULE_ANSWER_NONE,
    MODULE_ANSWER_PENDING,
    MODULE_ANSWER_SENT
};

struct __ModuleAnswer {
    enum __ModuleAnswerState State;
    enum __ModuleCall        Call;
    enum __ModuleFailure     Failure;
    oserr_t                  Result;
    uintptr_t                Value;
    uint64_t                 Count;
    char                     Text[MODULE_MAX_PATH];
};

static struct __ModuleFileSystem g_moduleFileSystems[MODULE_MAX_FILESYSTEMS];
static struct __ModuleHandle     g_moduleHandles[MODULE_MAX_HANDLES];
static _Thread_local struct __ModuleAnswer g_moduleAnswer;
static pthread_mutex_t           g_moduleLock = PTHREAD_MUTEX_INITIALIZER;
static char                      g_moduleNextLabel[32] = "vali-data";

// The failure to inject. The call is failed once after FailSkip calls of the
// same kind passed.
static enum __ModuleCall    g_moduleFailCall;
static enum __ModuleFailure g_moduleFailure;
static int                  g_moduleFailSkip;

// What the double saw. Calls counts the requests the driver ran.
static int g_moduleCalls[MODULE_CALL_COUNT];
static int g_moduleAbandons;
static int g_moduleAnswersRead;

static void
__ModuleFailCall(
    _In_ enum __ModuleCall    call,
    _In_ enum __ModuleFailure failure,
    _In_ int                  skip)
{
    pthread_mutex_lock(&g_moduleLock);
    g_moduleFailCall = call;
    g_moduleFailure  = failure;
    g_moduleFailSkip = skip;
    pthread_mutex_unlock(&g_moduleLock);
}

// Returns the failure for this call, and disarms it once used.
static enum __ModuleFailure
__ModuleTakeFailure(
    _In_ enum __ModuleCall call)
{
    enum __ModuleFailure failure = MODULE_FAIL_NONE;

    if (g_moduleFailure != MODULE_FAIL_NONE && g_moduleFailCall == call) {
        if (g_moduleFailSkip == 0) {
            failure = g_moduleFailure;
            g_moduleFailure = MODULE_FAIL_NONE;
        } else {
            g_moduleFailSkip--;
        }
    }
    return failure;
}

static int
__ModuleOpenHandles(void)
{
    int count = 0;

    pthread_mutex_lock(&g_moduleLock);
    for (int i = 0; i < MODULE_MAX_HANDLES; i++) {
        count += g_moduleHandles[i].Used ? 1 : 0;
    }
    pthread_mutex_unlock(&g_moduleLock);
    return count;
}

static struct __ModuleFileSystem*
__ModuleFileSystem(
    _In_ uintptr_t fsctx)
{
    assert(fsctx > 0 && fsctx <= MODULE_MAX_FILESYSTEMS);
    assert(g_moduleFileSystems[fsctx - 1].Used);
    return &g_moduleFileSystems[fsctx - 1];
}

static struct __ModuleHandle*
__ModuleHandle(
    _In_ struct __ModuleFileSystem* fileSystem,
    _In_ uintptr_t                  fctx)
{
    assert(fctx > 0 && fctx <= MODULE_MAX_HANDLES);
    assert(g_moduleHandles[fctx - 1].Used);
    assert(g_moduleHandles[fctx - 1].FileSystem == fileSystem);
    return &g_moduleHandles[fctx - 1];
}

static struct __ModuleEntry*
__ModuleFind(
    _In_ struct __ModuleFileSystem* fileSystem,
    _In_ const char*                path)
{
    for (int i = 0; i < MODULE_MAX_ENTRIES; i++) {
        if (fileSystem->Entries[i].Used && !strcmp(fileSystem->Entries[i].Path, path)) {
            return &fileSystem->Entries[i];
        }
    }
    return NULL;
}

static bool
__ModuleIsRoot(
    _In_ const char* path)
{
    return !strcmp(path, "/");
}

static struct __ModuleEntry*
__ModuleAdd(
    _In_ struct __ModuleFileSystem* fileSystem,
    _In_ const char*                path,
    _In_ uint32_t                   flags,
    _In_ uint32_t                   permissions)
{
    for (int i = 0; i < MODULE_MAX_ENTRIES; i++) {
        struct __ModuleEntry* entry = &fileSystem->Entries[i];
        if (!entry->Used) {
            memset(entry, 0, sizeof(*entry));
            entry->Used = true;
            snprintf(entry->Path, sizeof(entry->Path), "%s", path);
            entry->Flags = flags;
            entry->Permissions = permissions;
            return entry;
        }
    }
    abort();
}

// The directory part of a path, as the driver sees it.
static void
__ModuleParent(
    _In_  const char* path,
    _Out_ char*       parent)
{
    const char* slash = strrchr(path, '/');

    assert(slash != NULL);
    if (slash == path) {
        strcpy(parent, "/");
        return;
    }
    memcpy(parent, path, slash - path);
    parent[slash - path] = '\0';
}

static uintptr_t
__ModuleOpen(
    _In_ struct __ModuleFileSystem* fileSystem,
    _In_ const char*                path)
{
    for (int i = 0; i < MODULE_MAX_HANDLES; i++) {
        if (!g_moduleHandles[i].Used) {
            g_moduleHandles[i].Used = true;
            g_moduleHandles[i].FileSystem = fileSystem;
            g_moduleHandles[i].Position = 0;
            snprintf(g_moduleHandles[i].Path, sizeof(g_moduleHandles[i].Path), "%s", path);
            fileSystem->Handles++;
            return (uintptr_t)i + 1;
        }
    }
    abort();
}

// The SHM buffer and the range a transfer selects.
static uint8_t*
__ModuleTransferBuffer(
    _In_  const struct ctt_fs_transfer_params* params,
    _Out_ size_t*                              lengthOut)
{
    SHMTransfer_t transfer;
    OSHandle_t    attachment;
    uint8_t*      buffer;

    assert(__FromSysSHMTransfer(&params->buffer, &transfer) == OS_EOK);
    assert(SHMAttachToken(&transfer.Token, &attachment) == OS_EOK);
    buffer = SHMBuffer(&attachment);
    OSHandleDestroy(&attachment);
    *lengthOut = __SHMTransferLength(&transfer);
    return buffer + transfer.RelativeOffset;
}

// Directory entries in the format __LoadNode parses. A directory handle
// returns all its entries on the first read, and nothing after.
static uint64_t
__ModuleReadDirectory(
    _In_ struct __ModuleHandle* handle,
    _In_ uint8_t*               buffer,
    _In_ size_t                 length)
{
    struct __ModuleFileSystem* fileSystem = handle->FileSystem;
    size_t                     used = 0;
    char                       parent[MODULE_MAX_PATH];

    if (handle->Position != 0) {
        return 0;
    }
    handle->Position = 1;

    for (int i = 0; i < MODULE_MAX_ENTRIES; i++) {
        struct __ModuleEntry*     entry = &fileSystem->Entries[i];
        struct VFSDirectoryEntry* out;
        const char*               name;
        size_t                    nameLength;

        if (!entry->Used) {
            continue;
        }
        __ModuleParent(entry->Path, parent);
        if (strcmp(parent, handle->Path)) {
            continue;
        }

        name = strrchr(entry->Path, '/') + 1;
        nameLength = strlen(name) + 1;
        assert(used + sizeof(struct VFSDirectoryEntry) + nameLength <= length);
        out = (struct VFSDirectoryEntry*)(buffer + used);
        memset(out, 0, sizeof(*out));
        out->NameLength = (uint32_t)nameLength;
        out->Flags = entry->Flags;
        out->Permissions = entry->Permissions;
        out->Size = entry->Size;
        memcpy(buffer + used + sizeof(*out), name, nameLength);
        used += sizeof(*out) + nameLength;
    }
    return used;
}

// Starts a request. Returns the failure to apply, or -1 when the request
// could not be sent. The driver runs the call when the failure is
// MODULE_FAIL_NONE or MODULE_FAIL_READ.
static int
__ModuleBegin(
    _In_  struct gracht_message_context* context,
    _In_  enum __ModuleCall              call,
    _Out_ bool*                          runOut)
{
    enum __ModuleFailure failure;

    pthread_mutex_lock(&g_moduleLock);
    assert(g_moduleAnswer.State == MODULE_ANSWER_NONE);
    failure = __ModuleTakeFailure(call);
    if (failure == MODULE_FAIL_SEND) {
        pthread_mutex_unlock(&g_moduleLock);
        *runOut = false;
        return -1;
    }

    memset(&g_moduleAnswer, 0, sizeof(g_moduleAnswer));
    g_moduleAnswer.State   = MODULE_ANSWER_PENDING;
    g_moduleAnswer.Call    = call;
    g_moduleAnswer.Failure = failure;
    context->message_id = (uint32_t)call + 1;
    *runOut = failure == MODULE_FAIL_NONE || failure == MODULE_FAIL_READ;
    if (*runOut) {
        g_moduleCalls[call]++;
    }
    return 0;
}

// Ends a request started with __ModuleBegin, with g_moduleLock held.
static int
__ModuleEnd(void)
{
    pthread_mutex_unlock(&g_moduleLock);
    return 0;
}

// gracht, as __GrachtAwaitCompleted uses it.
gracht_client_t* GetGrachtClient(void) { return (gracht_client_t*)&g_moduleFileSystems; }

int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                   flags)
{
    int status = 0;

    pthread_mutex_lock(&g_moduleLock);
    assert(g_moduleAnswer.State == MODULE_ANSWER_PENDING);
    assert(context->message_id == (uint32_t)g_moduleAnswer.Call + 1);
    if (g_moduleAnswer.Failure == MODULE_FAIL_WAIT) {
        status = -1;
    } else {
        g_moduleAnswer.State = MODULE_ANSWER_SENT;
    }
    pthread_mutex_unlock(&g_moduleLock);
    return status;
}

int
gracht_client_get_status(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    int status;

    pthread_mutex_lock(&g_moduleLock);
    assert(g_moduleAnswer.State == MODULE_ANSWER_SENT);
    status = g_moduleAnswer.Failure == MODULE_FAIL_STATUS ? GRACHT_MESSAGE_ERROR : GRACHT_MESSAGE_COMPLETED;
    pthread_mutex_unlock(&g_moduleLock);
    return status;
}

int
gracht_client_abandon(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    pthread_mutex_lock(&g_moduleLock);
    assert(g_moduleAnswer.State != MODULE_ANSWER_NONE);
    assert(context->message_id == (uint32_t)g_moduleAnswer.Call + 1);
    g_moduleAnswer.State = MODULE_ANSWER_NONE;
    g_moduleAbandons++;
    pthread_mutex_unlock(&g_moduleLock);
    return 0;
}

// Takes the answer of a call. Returns -1 when it cannot be read. The answer
// must have been waited for, which is the rule this double checks.
static int
__ModuleTakeAnswer(
    _In_  struct gracht_message_context* context,
    _In_  enum __ModuleCall              call,
    _Out_ struct __ModuleAnswer*         answerOut)
{
    int status = 0;

    pthread_mutex_lock(&g_moduleLock);
    assert(g_moduleAnswer.State == MODULE_ANSWER_SENT);
    assert(g_moduleAnswer.Call == call);
    assert(context->message_id == (uint32_t)call + 1);
    assert(g_moduleAnswer.Failure == MODULE_FAIL_NONE || g_moduleAnswer.Failure == MODULE_FAIL_READ);
    if (g_moduleAnswer.Failure == MODULE_FAIL_READ) {
        status = -1;
    } else {
        *answerOut = g_moduleAnswer;
        g_moduleAnswersRead++;
    }
    g_moduleAnswer.State = MODULE_ANSWER_NONE;
    pthread_mutex_unlock(&g_moduleLock);
    return status;
}

// The requests. Each runs the call on the in-memory filesystem when the
// driver gets to run it.

int
ctt_filesystem_setup(
    _In_ gracht_client_t*                  client,
    _In_ struct gracht_message_context*    context,
    _In_ const struct ctt_fs_setup_params* params)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_SETUP, &run)) {
        return -1;
    }
    g_moduleAnswer.Result = OS_EOOM;
    for (int i = 0; run && i < MODULE_MAX_FILESYSTEMS; i++) {
        if (!g_moduleFileSystems[i].Used) {
            memset(&g_moduleFileSystems[i], 0, sizeof(g_moduleFileSystems[i]));
            g_moduleFileSystems[i].Used = true;
            snprintf(g_moduleFileSystems[i].Label, sizeof(g_moduleFileSystems[i].Label), "%s", g_moduleNextLabel);
            g_moduleAnswer.Result = OS_EOK;
            g_moduleAnswer.Value = (uintptr_t)i + 1;
            break;
        }
    }
    return __ModuleEnd();
}

int
ctt_filesystem_destroy(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_DESTROY, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        assert(fileSystem->Handles == 0);
        fileSystem->Used = false;
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_fsstat(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_FSSTAT, &run)) {
        return -1;
    }
    if (run) {
        snprintf(g_moduleAnswer.Text, sizeof(g_moduleAnswer.Text), "%s", __ModuleFileSystem(fsctx)->Label);
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_open(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const char*                    path)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_OPEN, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        if (__ModuleIsRoot(path) || __ModuleFind(fileSystem, path) != NULL) {
            g_moduleAnswer.Value = __ModuleOpen(fileSystem, path);
            g_moduleAnswer.Result = OS_EOK;
        } else {
            g_moduleAnswer.Result = OS_ENOENT;
        }
    }
    return __ModuleEnd();
}

int
ctt_filesystem_create(
    _In_ gracht_client_t*                 client,
    _In_ struct gracht_message_context*   context,
    _In_ const uintptr_t                  fsctx,
    _In_ const uintptr_t                  fctx,
    _In_ const struct ctt_fs_open_params* params)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_CREATE, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleHandle*     directory = __ModuleHandle(fileSystem, fctx);
        char                       path[MODULE_MAX_PATH];

        snprintf(path, sizeof(path), "%s%s%s", directory->Path,
                 __ModuleIsRoot(directory->Path) ? "" : "/", params->name);
        if (__ModuleFind(fileSystem, path) != NULL) {
            g_moduleAnswer.Result = OS_EEXISTS;
        } else {
            __ModuleAdd(fileSystem, path, params->flags, params->permissions);
            g_moduleAnswer.Value = __ModuleOpen(fileSystem, path);
            g_moduleAnswer.Result = OS_EOK;
        }
    }
    return __ModuleEnd();
}

int
ctt_filesystem_close(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const uintptr_t                fctx)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_CLOSE, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        __ModuleHandle(fileSystem, fctx)->Used = false;
        fileSystem->Handles--;
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_link(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const uintptr_t                fctx,
    _In_ const char*                    name,
    _In_ const char*                    target,
    _In_ const uint8_t                  symbolic)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_LINK, &run)) {
        return -1;
    }
    if (run) {
        g_moduleAnswer.Result = OS_ENOTSUPPORTED;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_unlink(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const char*                    path)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_UNLINK, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleEntry* entry = __ModuleFind(__ModuleFileSystem(fsctx), path);
        if (entry == NULL) {
            g_moduleAnswer.Result = OS_ENOENT;
        } else {
            entry->Used = false;
            g_moduleAnswer.Result = OS_EOK;
        }
    }
    return __ModuleEnd();
}

int
ctt_filesystem_readlink(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const char*                    path)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_READLINK, &run)) {
        return -1;
    }
    if (run) {
        snprintf(g_moduleAnswer.Text, sizeof(g_moduleAnswer.Text), "%s.target", path);
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

// Moves or copies one entry, the way MFS does it: the target must not exist,
// and its directory must.
int
ctt_filesystem_move(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const char*                    source,
    _In_ const char*                    target,
    _In_ const uint8_t                  copy)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_MOVE, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleEntry*      from = __ModuleFind(fileSystem, source);
        char                       parent[MODULE_MAX_PATH];

        __ModuleParent(target, parent);
        if (from == NULL) {
            g_moduleAnswer.Result = OS_ENOENT;
        } else if (__ModuleFind(fileSystem, target) != NULL) {
            g_moduleAnswer.Result = OS_EEXISTS;
        } else if (!__ModuleIsRoot(parent) && __ModuleFind(fileSystem, parent) == NULL) {
            g_moduleAnswer.Result = OS_ENOENT;
        } else if (copy) {
            struct __ModuleEntry* to = __ModuleAdd(fileSystem, target, from->Flags, from->Permissions);
            memcpy(to->Data, from->Data, from->Size);
            to->Size = from->Size;
            g_moduleAnswer.Result = OS_EOK;
        } else {
            snprintf(from->Path, sizeof(from->Path), "%s", target);
            g_moduleAnswer.Result = OS_EOK;
        }
    }
    return __ModuleEnd();
}

int
ctt_filesystem_read(
    _In_ gracht_client_t*                     client,
    _In_ struct gracht_message_context*       context,
    _In_ const uintptr_t                      fsctx,
    _In_ const uintptr_t                      fctx,
    _In_ const struct ctt_fs_transfer_params* params)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_READ, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleHandle*     handle = __ModuleHandle(fileSystem, fctx);
        struct __ModuleEntry*      entry = __ModuleFind(fileSystem, handle->Path);
        size_t                     length;
        uint8_t*                   buffer = __ModuleTransferBuffer(params, &length);

        if (entry == NULL || (entry->Flags & FILE_FLAG_DIRECTORY)) {
            g_moduleAnswer.Count = __ModuleReadDirectory(handle, buffer, length);
        } else {
            size_t count = entry->Size - (size_t)handle->Position;
            if (count > length) {
                count = length;
            }
            memcpy(buffer, &entry->Data[handle->Position], count);
            handle->Position += count;
            g_moduleAnswer.Count = count;
        }
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_write(
    _In_ gracht_client_t*                     client,
    _In_ struct gracht_message_context*       context,
    _In_ const uintptr_t                      fsctx,
    _In_ const uintptr_t                      fctx,
    _In_ const struct ctt_fs_transfer_params* params)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_WRITE, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleHandle*     handle = __ModuleHandle(fileSystem, fctx);
        struct __ModuleEntry*      entry = __ModuleFind(fileSystem, handle->Path);
        size_t                     length;
        uint8_t*                   buffer = __ModuleTransferBuffer(params, &length);

        assert(entry != NULL && handle->Position + length <= MODULE_MAX_DATA);
        memcpy(&entry->Data[handle->Position], buffer, length);
        handle->Position += length;
        if (handle->Position > entry->Size) {
            entry->Size = (size_t)handle->Position;
        }
        g_moduleAnswer.Count = length;
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_truncate(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const uintptr_t                fctx,
    _In_ const uint64_t                 size)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_TRUNCATE, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleEntry*      entry = __ModuleFind(fileSystem, __ModuleHandle(fileSystem, fctx)->Path);

        assert(entry != NULL && size <= MODULE_MAX_DATA);
        entry->Size = (size_t)size;
        g_moduleAnswer.Result = OS_EOK;
    }
    return __ModuleEnd();
}

int
ctt_filesystem_seek(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uintptr_t                fsctx,
    _In_ const uintptr_t                fctx,
    _In_ const uint64_t                 position)
{
    bool run;

    if (__ModuleBegin(context, MODULE_CALL_SEEK, &run)) {
        return -1;
    }
    if (run) {
        struct __ModuleFileSystem* fileSystem = __ModuleFileSystem(fsctx);
        struct __ModuleHandle*     handle = __ModuleHandle(fileSystem, fctx);
        struct __ModuleEntry*      entry = __ModuleFind(fileSystem, handle->Path);

        // A position past the end of a file is refused. The answer still
        // carries a position, which filed must not use.
        g_moduleAnswer.Value = (uintptr_t)position;
        if (entry != NULL && position > entry->Size) {
            g_moduleAnswer.Result = OS_EINVALPARAMS;
        } else {
            handle->Position = position;
            g_moduleAnswer.Result = OS_EOK;
        }
    }
    return __ModuleEnd();
}

// The answers.

int
ctt_filesystem_setup_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uintptr_t* fsctx_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_SETUP, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *fsctx_out = answer.Value;
    return 0;
}

int
ctt_filesystem_destroy_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_DESTROY, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_fsstat_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, struct ctt_fsstat* stats_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_FSSTAT, &answer)) {
        return -1;
    }
    memset(stats_out, 0, sizeof(*stats_out));
    *err_out = answer.Result;
    stats_out->label = strdup(answer.Text);
    assert(stats_out->label != NULL);
    stats_out->block_size = 512;
    return 0;
}

int
ctt_filesystem_open_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uintptr_t* fctx_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_OPEN, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *fctx_out = answer.Value;
    return 0;
}

int
ctt_filesystem_create_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uintptr_t* fctx_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_CREATE, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *fctx_out = answer.Value;
    return 0;
}

int
ctt_filesystem_close_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_CLOSE, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_link_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_LINK, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_unlink_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_UNLINK, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_readlink_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, char* link_out, const uint32_t link_max_length)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_READLINK, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    snprintf(link_out, link_max_length, "%s", answer.Text);
    return 0;
}

int
ctt_filesystem_read_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uint64_t* read_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_READ, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *read_out = answer.Count;
    return 0;
}

int
ctt_filesystem_write_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uint64_t* written_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_WRITE, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *written_out = answer.Count;
    return 0;
}

int
ctt_filesystem_move_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_MOVE, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_truncate_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_TRUNCATE, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    return 0;
}

int
ctt_filesystem_seek_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* err_out, uint64_t* result_out)
{
    struct __ModuleAnswer answer;

    if (__ModuleTakeAnswer(context, MODULE_CALL_SEEK, &answer)) {
        return -1;
    }
    *err_out = answer.Result;
    *result_out = answer.Value;
    return 0;
}

// Disks driven by the double. A disk is registered and mounted the way
// VFSStorageRegisterAndSetupPartition does it, with the real driver_ops.c as
// its interface.

extern struct VFSOperations g_driverOps;

// A registered disk with one partition, mounted at /storage/<serial>/<label>.
struct __TestDisk {
    const char*        Serial;
    const char*        Label;
    struct VFSStorage* Storage;
    FileSystem_t*      FileSystem;
    struct VFS*        VFS;
};


static void
__RegisterDisk(
    _In_ struct __TestDisk* disk)
{
    UInteger64_t sector = { .QuadPart = 2048 };
    guid_t       guid = GUID_EMPTY;
    char         path[64];

    disk->Storage = calloc(1, sizeof(struct VFSStorage));
    assert(disk->Storage != NULL);
    disk->Storage->Protocol.StorageType = VFSSTORAGE_TYPE_DEVICE;
    snprintf(&disk->Storage->Stats.Serial[0], sizeof(disk->Storage->Stats.Serial), "%s", disk->Serial);
    snprintf(path, sizeof(path), "/storage/%s", disk->Serial);
    __MakeDirectory(path);

    snprintf(g_moduleNextLabel, sizeof(g_moduleNextLabel), "%s", disk->Label);
    disk->FileSystem = FileSystemNew(
        disk->Storage, 0, &sector,
        VFSIdentifierAllocate(disk->Storage), &guid
    );
    assert(disk->FileSystem != NULL);
    assert(VFSFileSystemConnectInterface(disk->FileSystem, VFSInterfaceNew(1, &g_driverOps)) == OS_EOK);
    assert(VFSFileSystemMount(disk->FileSystem, NULL) == OS_EOK);
    disk->VFS = disk->FileSystem->VFS;
}

static void
__UnregisterDisk(
    _In_ struct __TestDisk* disk)
{
    char path[64];

    assert(VFSFileSystemUnmount(disk->FileSystem, 1) == OS_EOK);
    assert(VFSFileSystemDisconnectInterface(disk->FileSystem, 1) == OS_EOK);
    FileSystemDestroy(disk->FileSystem);
    snprintf(path, sizeof(path), "/storage/%s", disk->Serial);
    assert(VFSNodeUnlink(g_rootScope, path) == OS_EOK);
    free(disk->Storage);
}

// The driver's copy of a disk.
static struct __ModuleFileSystem*
__DriverOf(
    _In_ struct __TestDisk* disk)
{
    return __ModuleFileSystem((uintptr_t)disk->FileSystem->Data);
}

// A path on a disk, as the caller of VFSNodeMove passes it.
static const char*
__On(
    _In_ struct __TestDisk* disk,
    _In_ const char*        path)
{
    static _Thread_local char buffers[4][128];
    static _Thread_local int  next;
    char*                     buffer = buffers[next++ % 4];

    snprintf(buffer, sizeof(buffers[0]), "/storage/%s/%s%s", disk->Serial, disk->Label, path);
    return buffer;
}

static oserr_t
__Lookup(
    _In_ const char* cpath)
{
    struct VFSNode* node;
    mstring_t*      path = mstr_path_new_u8(cpath);
    oserr_t         oserr;

    assert(path != NULL);
    oserr = VFSNodeGet(g_rootScope, path, 1, &node);
    if (oserr == OS_EOK) {
        VFSNodePut(node);
    }
    mstr_delete(path);
    return oserr;
}

// Creates a file through the VFS and gives it its data in the driver.
static void
__CreateFile(
    _In_ struct __TestDisk* disk,
    _In_ const char*        path,
    _In_ const char*        data)
{
    struct __ModuleEntry* entry;
    uuid_t                handle;
    oserr_t               oserr;

    oserr = VFSNodeOpen(g_rootScope, __On(disk, path), __FILE_CREATE | __FILE_READ_ACCESS | __FILE_WRITE_ACCESS,
                        FILE_PERMISSION_READ | FILE_PERMISSION_WRITE, &handle);
    if (oserr != OS_EOK) {
        fprintf(stderr, "create %s: %u\n", __On(disk, path), oserr);
    }
    assert(oserr == OS_EOK);
    assert(VFSNodeClose(g_rootScope, handle) == OS_EOK);
    pthread_mutex_lock(&g_moduleLock);
    entry = __ModuleFind(__DriverOf(disk), path);
    assert(entry != NULL);
    entry->Size = strlen(data);
    memcpy(entry->Data, data, entry->Size);
    pthread_mutex_unlock(&g_moduleLock);
}

static void
__CreateDirectory(
    _In_ struct __TestDisk* disk,
    _In_ const char*        path)
{
    __MakeDirectory(__On(disk, path));
}

// Checks a file in the driver, and what a lookup finds. data is NULL for a
// file that must not exist.
static void
__ExpectFile(
    _In_ struct __TestDisk* disk,
    _In_ const char*        path,
    _In_ const char*        data)
{
    struct __ModuleEntry* entry;

    pthread_mutex_lock(&g_moduleLock);
    entry = __ModuleFind(__DriverOf(disk), path);
    if ((data == NULL) != (entry == NULL)) {
        fprintf(stderr, "%s on disk %s: %s\n", path, disk->Serial, entry == NULL ? "missing" : "present");
    }
    if (data == NULL) {
        assert(entry == NULL);
    } else {
        assert(entry != NULL);
        assert(entry->Size == strlen(data) && !memcmp(entry->Data, data, entry->Size));
        assert(FILE_FLAG_TYPE(entry->Flags) == FILE_FLAG_FILE);
    }
    pthread_mutex_unlock(&g_moduleLock);
    assert(__Lookup(__On(disk, path)) == (data == NULL ? OS_ENOENT : OS_EOK));
}

// Checks that no node lock is held below node, including the mounted VFSs.
static void
__ExpectUnlocked(
    _In_ struct VFSNode* node);

static void
__ExpectChildUnlocked(
    _In_ int         index,
    _In_ const void* element,
    _In_ void*       userContext)
{
    __ExpectUnlocked(((const struct __VFSChild*)element)->Node);
}

static void
__ExpectUnlocked(
    _In_ struct VFSNode* node)
{
    assert(node->Lock.readers == 0 && node->Lock.sync_object.references == 0);
    assert(node->HandlesLock.references == 0 && node->MountsLock.references == 0);
    if (__NodeIsMountPoint(node)) {
        __ExpectUnlocked(((struct VFS*)node->TypeData)->Root);
    }
    hashtable_enumerate(&node->Children, __ExpectChildUnlocked, NULL);
}

static void
__ExpectNoOperations(
    _In_ struct VFS* vfs)
{
    usched_mtx_lock(&vfs->StateLock);
    assert(vfs->State == VFS_STATE_ACTIVE && vfs->Operations == 0);
    usched_mtx_unlock(&vfs->StateLock);
}
