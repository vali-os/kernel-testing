// Reuse processd boundary fixtures; creation below runs the real constructor.
#define main __StartupServerTestMain
#include "process_startup_server_test.c"
#undef main

static unsigned int g_loaded;
static unsigned int g_started;
static bool         g_exitOnPublication;
static oserr_t      g_startError;

mstring_t*
mstr_new_u8(
    _In_ const char* value)
{
    return (mstring_t*)strdup(value);
}

mstring_t*
mstr_path_basename(
    _In_ mstring_t* value)
{
    return mstr_new_u8("test");
}

mstring_t*
mstr_path_dirname(
    _In_ mstring_t* value)
{
    return mstr_new_u8("/");
}

char*
mstr_u8(
    _In_ mstring_t* value)
{
    return strdup((const char*)value);
}

struct PEImageLoadContext*
PEImageLoadContextNew(
    _In_ uuid_t      scope,
    _In_ const char* paths)
{
    struct PEImageLoadContext* context = calloc(1, sizeof(*context));

    assert(context != NULL && strcmp(paths, "LDPATH=/boot") == 0);
    context->SecurityGroupID = 0xdeadbeefU;
    context->MemorySpace = 555;
    g_loaded++;
    return context;
}

oserr_t
PEImageLoad(
    _In_ struct PEImageLoadContext* context,
    _In_ mstring_t*                 path,
    _In_ bool                       dependency)
{
    free(path);
    return OS_EOK;
}

oserr_t
PEImageLoadContextModulePath(
    _In_  struct PEImageLoadContext* context,
    _In_  mstring_t*                 name,
    _Out_ mstring_t**                pathOut)
{
    *pathOut = (mstring_t*)"/test";
    return OS_EOK;
}

oserr_t
PEImageLoadContextModuleEntryPoint(
    _In_  struct PEImageLoadContext* context,
    _In_  mstring_t*                 name,
    _Out_ uintptr_t*                 entryOut)
{
    *entryOut = 0x1000;
    return OS_EOK;
}

oserr_t
OSHandleCreate(
    _In_  OSHandle_t* registration,
    _In_  uint32_t    access,
    _Out_ OSHandle_t* handleOut)
{
    *handleOut = (OSHandle_t) { .ID = 77 };
    return OS_EOK;
}

oserr_t
CapabilityGrant(
    _In_ Capabilities_t capabilities,
    _In_ uuid_t         memorySpace)
{
    assert(memorySpace == 555);
    return OS_EOK;
}

void
usched_cnd_init(
    _In_ struct usched_cnd* condition)
{
    memset(condition, 0, sizeof(*condition));
}

void
ThreadParametersInitialize(
    _In_ OSThreadParameters_t* parameters)
{
    memset(parameters, 0, sizeof(*parameters));
}

static void
__ExitAtPublication(void)
{
    // Simulate an immediately exiting child once the registry becomes visible.
    // Reading its load context after this unlock is a use-after-free.
    g_processUnlockHook = NULL;
    assert(PmTerminateProcess(77, 0) == OS_EOK);
}

oserr_t
Syscall_ThreadCreate(
    _In_  uintptr_t             entry,
    _In_  void*                 argument,
    _In_  OSThreadParameters_t* parameters,
    _Out_ uuid_t*               threadOut)
{
    assert(g_processesLock.references == 1);
    assert(parameters->MemorySpaceHandle == 555 && entry == 0x1000);
    g_started++;
    *threadOut = 88;
    if (g_exitOnPublication && g_startError == OS_EOK) {
        g_processUnlockHook = __ExitAtPublication;
    }
    return g_startError;
}

oserr_t
Syscall_ThreadDetach(
    _In_ uuid_t thread)
{
    assert(thread == 88);
    return OS_EOK;
}

static void
__TestSpawnIdentity(void)
{
    struct gracht_security_context caller = {
        .source = GRACHT_SECURITY_KERNEL,
        .principal = 42,
        .capabilities = CAPABILITY_GRANT_CAPABILITY
    };
    struct ProcessOptions options = {
        .DataBuffer = (void*)g_environment,
        .EnvironmentBlockLength = sizeof(g_environment),
        .Capabilities = CAPABILITY_DEVICEIO_ACCESS
    };
    OSProcessIdentity_t unchanged = { 444, 123, 456 };
    OSProcessIdentity_t identity = unchanged;
    uuid_t              process = 444;
    unsigned int        deleted;

    PmInitialize();
    assert(PmCreateProcess(NULL, "/test", "", &options, &identity) == OS_EPERMISSIONS);
    assert(memcmp(&identity, &unchanged, sizeof(identity)) == 0);
    assert(g_loaded == 0 && g_started == 0);

    assert(PmCreateProcess(&caller, "/test", "", &options, &identity) == OS_EOK);
    assert(identity.Process == 77 && identity.Reserved == 0 && identity.Principal == 0xdeadbeefULL);
    assert(identity.Principal != caller.principal && identity.Principal != identity.Process);
    assert(g_loaded == 1 && g_started == 1);
    assert(PmTerminateProcess(77, 0) == OS_EOK);

    // Trusted service bootstrap returns the process handle for service tracking.
    assert(PmCreateServiceProcess("/test", "", &options, &process) == OS_EOK && process == 77);
    assert(PmTerminateProcess(77, 0) == OS_EOK);

    g_startError = OS_EOOM;
    identity = unchanged;
    deleted = g_deleted;
    assert(PmCreateProcess(&caller, "/test", "", &options, &identity) == OS_EOOM);
    assert(memcmp(&identity, &unchanged, sizeof(identity)) == 0 && g_deleted == deleted + 1);
    process = 444;
    assert(PmCreateServiceProcess("/test", "", &options, &process) == OS_EOOM && process == 444);
    g_startError = OS_EOK;

    g_exitOnPublication = true;
    deleted = g_deleted;
    assert(PmCreateProcess(&caller, "/test", "", &options, &identity) == OS_EOK);
    assert(g_deleted == deleted + 1 && PmGetProcessByHandle(77) == NULL);
    assert(identity.Process == 77 && identity.Reserved == 0 && identity.Principal == 0xdeadbeefULL);
    hashtable_destroy(&g_threadmappings);
    hashtable_destroy(&g_processes);
    hashtable_destroy(&g_processHistory);
}

int
main(void)
{
    struct gracht_security_context caller = {
        .source = GRACHT_SECURITY_KERNEL,
        .principal = 42
    };

    assert(PmAuthorizeSpawn(NULL, 0) == OS_EOK);
    assert(PmAuthorizeSpawn(NULL, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.capabilities = CAPABILITY_DEVICEIO_ACCESS;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.capabilities = CAPABILITY_GRANT_CAPABILITY;
    // Grant authority permits delegating rights the sender does not itself hold.
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EOK);
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_ALL) == OS_EOK);
    assert(PmAuthorizeSpawn(&caller, (Capabilities_t)0x80000000U) == OS_EINVALPARAMS);
    caller.source = GRACHT_SECURITY_NONE;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.source = GRACHT_SECURITY_KERNEL;
    caller.principal = 0;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    __TestSpawnIdentity();
    puts("Process authorization and spawn identity: permissions, outputs, failure cleanup and early exit passed");
    return 0;
}
