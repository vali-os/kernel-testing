// Production inheritance and handle state machines, with kernel grant operations
// replaced by a deterministic reference/token model at the syscall boundary.
#define close __TestDescriptorClose
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <internal/_syscalls.h>
#include <os/grants.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)
static void* __TestMalloc(size_t size);
static void* __TestCalloc(size_t count, size_t size);
static void __TestFree(void* memory);
static oserr_t __Drop(uuid_t id);
static oserr_t __Install(const GrantToken_t* token, uuid_t destination, uuid_t* result);
#undef Syscall_HandleDestroy
#undef Syscall_HandleImport
#define Syscall_HandleDestroy(id) __Drop(id)
#define Syscall_HandleImport(token, destination, result) __Install(token, destination, result)
#define malloc __TestMalloc
#define calloc __TestCalloc
#define free __TestFree
#define g_ipcOps g_handleIpcOps
#include "../librt/libos/handles.c"
#undef g_ipcOps
#include "../librt/libc/stdio/core/core.c"
#include "../librt/libc/stdio/core/iod_bitmap.c"
#include "../librt/libc/stdio/core/inheritance.c"
#include "../librt/libc/stdio/core/export.c"
#ifndef INHERITANCE_IMPORT_SOURCE
#define INHERITANCE_IMPORT_SOURCE "../librt/libc/stdio/core/import.c"
#endif
#include INHERITANCE_IMPORT_SOURCE
#include "../librt/libc/io/close.c"
#undef close
#include "../services/processd/startup.c"
#include "../services/processd/process.c"
#include "../librt/libds/hashtable.c"
#undef malloc
#undef calloc
#undef free

const OSHandleOps_t g_handleIpcOps = {0}, g_eventOps = {0}, g_hqueueOps = {0}, g_shmOps = {0}, g_socketOps = {0};
stdio_ops_t g_fmemOps, g_memstreamOps, g_evtOps, g_iosetOps, g_ipcOps;
stdio_ops_t g_fileOps, g_pipeOps, g_netOps;
static int g_errno;
static int g_failAfter = -1;
static int g_allocations;
static int g_creates;
static int g_revokes;
static int g_imports;
static int g_drops;
static int g_failToken = -1;
static int g_failImport = -1;
static bool g_failDrop;
static bool g_consumed[2048];
static bool g_live[4096];
static unsigned int g_scope[2048];
static void* __TestMalloc(size_t size)
{
    void* memory;
    if (g_failAfter == 0) return NULL;
    if (g_failAfter > 0) g_failAfter--;
    memory = malloc(size);
    CHECK(memory != NULL);
    g_allocations++;
    return memory;
}
static void* __TestCalloc(size_t count, size_t size)
{
    void* memory = __TestMalloc(count * size);
    if (memory != NULL) memset(memory, 0, count * size);
    return memory;
}
static void __TestFree(void* memory)
{
    if (memory != NULL) { CHECK(g_allocations > 0); g_allocations--; free(memory); }
}
int* __errno(void) { return &g_errno; }
void* dsalloc(size_t size) { return __TestMalloc(size); }
void dsfree(void* memory) { __TestFree(memory); }
void spinlock_init(spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void spinlock_acquire(spinlock_t* lock) { (void)lock; }
void spinlock_release(spinlock_t* lock) { (void)lock; }
int OsErrToErrNo(oserr_t error) { return error == OS_EOK ? 0 : -1; }
SCTYPE syscall2(SCTYPE number, SCTYPE a, SCTYPE b) { return OS_EOK; }
static unsigned int g_socketAdoptions;
static unsigned int g_pipeAdoptions;
size_t SHMBufferCapacity(OSHandle_t* handle) { return 4096; }
oserr_t SHMAdopt(uuid_t id, OSHandle_t* handle)
{
    g_pipeAdoptions++;
    return OSHandleWrap(id, OSHANDLE_SHM, NULL, true, handle);
}
oserr_t SHMMap(OSHandle_t* handle, size_t offset, size_t length, unsigned int access)
{
    CHECK(handle->Type == OSHANDLE_SHM && offset == 0 && length == 4096);
    CHECK(access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    return OS_EOK;
}
oserr_t OSSocketAcquire(uuid_t id, OSHandle_t* handle)
{
    g_socketAdoptions++;
    return OSHandleWrap(id, OSHANDLE_SOCKET, NULL, true, handle);
}
oserr_t OSGrantTokenCreate(uuid_t source, const GrantTokenOptions_t* options, GrantToken_t* token)
{
    uuid_t id;
    CHECK(source >= 100 && source <= 103);
    CHECK(options->ResourceType == (source == 102 ? GrantTokenResourceTypeSHM : GrantTokenResourceTypeProvider));
    if (source == 102) {
        CHECK(options->Data.SHM.Offset == 0 && options->Data.SHM.Length == 4096);
        CHECK(options->Scope == (GrantTokenScopeRead | GrantTokenScopeWrite));
    }
    CHECK(options->TargetType == GrantTokenTargetTypeAny);
    CHECK(options->Flags == GrantTokenFlagsSingleUse);
    if (g_failToken == g_creates) return OS_EOOM;
    id = ++g_creates;
    CHECK(id < 2048);
    g_scope[id] = options->Scope;
    g_consumed[id] = false;
    *token = (GrantToken_t){.ControlHandleID=id, .ExportGlobalHandleID=id,
        .Version=GRANT_TOKEN_VERSION, .Options=*options};
    return OS_EOK;
}
oserr_t OSGrantTokenRevoke(uuid_t control)
{
    CHECK(control > 0 && control <= (uuid_t)g_creates);
    g_revokes++;
    return OS_EOK;
}
static oserr_t __Install(const GrantToken_t* token, uuid_t destination, uuid_t* result)
{
    CHECK(destination == 99);
    CHECK(!g_consumed[token->ExportGlobalHandleID]);
    if (g_failImport == g_imports) return OS_EOOM;
    g_consumed[token->ExportGlobalHandleID] = true;
    *result = 2000 + g_imports++;
    CHECK(*result < 4096);
    g_live[*result] = true;
    return OS_EOK;
}
static oserr_t __Drop(uuid_t id)
{
    CHECK(id < 4096 && g_live[id]);
    if (g_failDrop) return OS_EBUSY;
    g_live[id] = false;
    g_drops++;
    return OS_EOK;
}
oserr_t PEModuleEntryPoints(struct PEImageLoadContext* context, uintptr_t* entries, int* count)
{
    entries[0] = 1;
    *count = 1;
    return OS_EOK;
}
static void __Init(void)
{
    OSHandlesInitialize();
    CHECK(hashtable_construct(&g_stdioObjects, 0, sizeof(struct stdio_object_entry), stdio_hash, stdio_cmp) == 0);
    CHECK(stdio_bitmap_initialize() == 0);
}
static void __Finish(void)
{
    CHECK(g_stdioObjects.element_count == 0 && g_osHandles.element_count == 0);
    hashtable_destroy(&g_stdioObjects);
    hashtable_destroy(&g_osHandles);
    __TestFree(g_stdioHandles);
}
static void __Close(int fd)
{
    CHECK(__TestDescriptorClose(fd) == 0);
}
static void __File(int fd, uuid_t id, unsigned int flags)
{
    stdio_handle_t* descriptor;
    OSHandle_t handle;
    CHECK(stdio_handle_create(fd, 0, flags, FILE_SIGNATURE, NULL, &descriptor) == 0);
    descriptor->XTFlags = flags | __IO_OPEN;
    descriptor->Shared = true;
    if (OSHandlesFind(id, &handle) == OS_EOK) {
        CHECK(OSHandlesAcquire(&handle) == OS_EOK);
    } else {
        g_live[id] = true;
        CHECK(OSHandleWrap(id, OSHANDLE_NULL, NULL, true, &handle) == OS_EOK);
    }
    stdio_handle_set_handle(descriptor, &handle);
}

static void __TestFlow(void)
{
    char inputBytes[4096];
    struct InheritanceOptions options = { .Flags=PROCESS_INHERIT_STDOUT|PROCESS_INHERIT_STDERR,
        .StdOutHandle=7, .StdErrHandle=7 };
    struct inheritance_export exported;
    struct sys_process_inheritance_input input;
    struct sys_process_inheritance_output output;
    struct PEImageLoadContext context = { .MemorySpace=99 };
    Process_t process = { .load_context=&context };
    ProcessStartupInformation_t header;
    struct ProcessOptions processOptions = { 0 };
    gracht_buffer_t wire;
    char* childBytes;
    int before;
    uint32_t length;

    __Init();
    __File(7, 100, __IO_WRITE | __IO_APPEND | __IO_TEXTMODE);
    CHECK(CRTWriteInheritanceBlock(&options, inputBytes, sizeof(inputBytes), &exported) == OS_EOK);
    CHECK(exported.count == 1);
    CHECK(__ReadInheritanceInput(inputBytes, exported.length, &input) == OS_EOK);
    CHECK(input.resources_count == 1 && input.descriptors_count == 2);
    CHECK(input.resources[0].scope == __FILE_WRITE_ACCESS);
    CHECK(input.descriptors[0].descriptor == 1 && input.descriptors[1].descriptor == 2);
    memcpy(inputBytes + exported.length, "LDPATH=/boot\0", sizeof("LDPATH=/boot\0"));
    processOptions.DataBuffer = inputBytes;
    processOptions.InheritationBlockLength = exported.length;
    processOptions.EnvironmentBlockLength = sizeof("LDPATH=/boot\0");
    CHECK(__ValidateProcessInput("/test", "", &processOptions) == OS_EOK);
    CHECK(strcmp(__GetLoadPaths(&processOptions), "LDPATH=/boot") == 0);
    memset(inputBytes, 0xee, exported.length);
    before = g_imports;
    CHECK(PmPrepareStartupInformation(&process, __ProcOptsEnvironmentBlock(&processOptions),
        processOptions.EnvironmentBlockLength, "x", PROCESS_STARTUP_MAX_SIZE, &input)
        == OS_EBUFFER && g_imports == before);
    CHECK(PmPrepareStartupInformation(&process, __ProcOptsEnvironmentBlock(&processOptions),
        processOptions.EnvironmentBlockLength, "x", 2, &input)
        == OS_EOK && g_imports == before + 1);
    sys_process_inheritance_input_destroy(&input);
    CRTReleaseInheritanceBlock(&exported);
    __Close(7);
    __Finish();
    CHECK(g_live[2000 + before]);

    __Init();
    memcpy(&header, process.startup_data.Buffer, sizeof(header));
    childBytes = (char*)process.startup_data.Buffer + sizeof(header) + header.ArgumentsLength;
    length = header.InheritationLength;
    wire = (gracht_buffer_t){.data=childBytes, .limit=length};
    deserialize_sys_process_inheritance_output(&wire, &output);
    CHECK(!wire.error && output.resources_count == 1 && output.descriptors_count == 2);
    CHECK(output.resources[0] == (uuid_t)(2000 + before));
    sys_process_inheritance_output_destroy(&output);
    // A malformed length must not register or close any installed reference.
    CHECK(CRTReadInheritanceBlock(childBytes, length - 1) == OS_EINVALPARAMS);
    CHECK(CRTReadInheritanceBlock(childBytes, length + 1) == OS_EINVALPARAMS);
    CHECK(g_osHandles.element_count == 0);
    __File(1, 100, __IO_READ);
    CHECK(CRTReadInheritanceBlock(childBytes, length) == OS_EEXISTS);
    CHECK(stdio_handle_get(1)->OSHandle.ID == 100 && g_live[2000 + before]);
    __Close(1);
    CHECK(CRTReadInheritanceBlock(childBytes, length) == OS_EOK);
    CHECK(stdio_handle_get(1)->OSHandle.ID == stdio_handle_get(2)->OSHandle.ID);
    CHECK(!(stdio_handle_get(1)->XTFlags & (__IO_READ | __IO_PERSISTANT)));
    CHECK(stdio_handle_get(1)->XTFlags & __IO_APPEND);
    CHECK(CRTReadInheritanceBlock(childBytes, length) == OS_EEXISTS);
    before = g_drops;
    __Close(1);
    CHECK(g_drops == before);
    __Close(2);
    CHECK(g_drops == before + 1);
    __Finish();
    __TestFree(process.startup_data.Buffer);
}

static void __TestFailures(void)
{
    char bytes[4096];
    struct InheritanceOptions options = { .Flags=PROCESS_INHERIT_FILES };
    struct inheritance_export exported;
    struct sys_process_inheritance_input input;
    struct sys_process_inheritance_output output;
    struct PEImageLoadContext context = { .MemorySpace=99 };
    Process_t process = { .load_context=&context };
    gracht_buffer_t wire;
    uuid_t childIDs[] = {3500, 3501};
    struct sys_inherited_descriptor descriptors[] = {
        {.descriptor=0, .resource_index=0, .signature=FILE_SIGNATURE, .flags=__IO_READ},
        {.descriptor=2, .resource_index=1, .signature=FILE_SIGNATURE, .flags=__IO_WRITE}
    };
    int baseline;
    int before;
    int failure;
    uint32_t length;
    oserr_t status;

    __Init();
    __File(7, 100, __IO_READ);
    __File(8, 101, __IO_WRITE);
    before = g_creates;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, 1, &exported) == OS_EBUFFER);
    CHECK(g_creates == before && exported.count == 0);
    stdio_handle_get(8)->Signature = EVENT_SIGNATURE;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_ENOTSUPPORTED);
    CHECK(g_creates == before);
    stdio_handle_get(8)->Signature = FILE_SIGNATURE;
    baseline = g_allocations;
    for (failure = 0; failure < 5; failure++) {
        g_failAfter = failure;
        CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_EOOM);
        g_failAfter = -1;
        CHECK(g_allocations == baseline && g_creates == before);
    }
    g_failToken = g_creates + 1;
    before = g_revokes;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_EOOM);
    CHECK(g_revokes == before + 1);
    g_failToken = -1;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_EOK);
    length = exported.length;
    CHECK(__ReadInheritanceInput(bytes, length + 1, &input) == OS_EINVALPARAMS);
    CHECK(__ReadInheritanceInput(bytes, length - 1, &input) == OS_EINVALPARAMS);
    CHECK(__ReadInheritanceInput(bytes, length, &input) == OS_EOK);
    input.descriptors[1].descriptor = input.descriptors[0].descriptor;
    wire = (gracht_buffer_t){.data=bytes, .limit=sizeof(bytes)};
    serialize_sys_process_inheritance_input(&wire, &input);
    sys_process_inheritance_input_destroy(&input);
    CHECK(__ReadInheritanceInput(bytes, length, &input) == OS_EINVALPARAMS);
    CRTReleaseInheritanceBlock(&exported);
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_EOK);
    CHECK(__ReadInheritanceInput(bytes, exported.length, &input) == OS_EOK);
    g_failImport = g_imports + 1;
    before = g_imports;
    CHECK(PmPrepareStartupInformation(&process, NULL, 0, "x", 2, &input)
        == OS_EOOM && g_imports == before + 1);
    sys_process_inheritance_input_destroy(&input);
    CHECK(process.startup_data.Buffer == NULL);
    // The real processd caller tears down the unstarted group on this path.
    CHECK(__Drop(2000 + before) == OS_EOK);
    g_failImport = -1;
    CRTReleaseInheritanceBlock(&exported);
    __Close(7);
    __Close(8);
    __Finish();

    output = (struct sys_process_inheritance_output){.resources_count=2, .resources=childIDs,
        .descriptors_count=2, .descriptors=descriptors};
    wire = (gracht_buffer_t){.data=bytes, .limit=sizeof(bytes)};
    serialize_sys_process_inheritance_output(&wire, &output);
    length = wire.index;
    // Sweep every decode, wrap and descriptor allocation. Exit releases grants
    // which adoption did not wrap; successful local rollback balances the rest.
    for (failure = 0; failure < 9; failure++) {
        __Init();
        baseline = g_allocations;
        g_live[3500] = g_live[3501] = true;
        g_failAfter = failure;
        status = CRTReadInheritanceBlock(bytes, length);
        g_failAfter = -1;
        if (status == OS_EOK) {
            __Close(0);
            __Close(2);
        } else {
            CHECK(status == OS_EOOM);
            CHECK(g_stdioObjects.element_count == 0 && g_osHandles.element_count == 0);
            if (g_live[3500]) CHECK(__Drop(3500) == OS_EOK);
            if (g_live[3501]) CHECK(__Drop(3501) == OS_EOK);
        }
        CHECK(g_allocations == baseline);
        CHECK(stdio_bitmap_allocate(0) == 0);
        CHECK(stdio_bitmap_allocate(2) == 2);
        stdio_bitmap_free(0);
        stdio_bitmap_free(2);
        __Finish();
    }
}

static void
__TestContractSemantics(void)
{
    struct sys_inherited_descriptor descriptor = {
        .descriptor = 7, .resource_index = 0, .signature = FILE_SIGNATURE, .flags = __IO_READ
    };
    struct sys_grant_token resource = {
        .version = GRANT_TOKEN_VERSION, .export_id = 1,
        .resource_type = GrantTokenResourceTypeProvider,
        .target_type = GrantTokenTargetTypeAny, .flags = GrantTokenFlagsSingleUse,
        .scope = __FILE_READ_ACCESS
    };
    struct sys_process_inheritance_input source = {
        .resources_count = 1, .resources = &resource,
        .descriptors_count = 1, .descriptors = &descriptor
    };
    struct sys_process_inheritance_input decoded;
    struct sys_process_inheritance_output output;
    uuid_t          childID = 3500;
    char            bytes[512];
    gracht_buffer_t wire;
    int            variant;
    oserr_t        expected;

    // Processd treats libc descriptor metadata as opaque. The child rejects an
    // unsupported signature, a disallowed mode flag and an out-of-range number.
    for (variant = 0; variant < 3; variant++) {
        descriptor.signature = variant == 0 ? EVENT_SIGNATURE : FILE_SIGNATURE;
        descriptor.flags = variant == 1 ? __IO_PRIORITY : __IO_READ;
        descriptor.descriptor = variant == 2 ? INTERNAL_MAXFILES : 7;
        wire = (gracht_buffer_t) { .data = bytes, .limit = sizeof(bytes) };
        serialize_sys_process_inheritance_input(&wire, &source);
        CHECK(!wire.error);
        CHECK(__ReadInheritanceInput(bytes, wire.index, &decoded) == OS_EOK);
        sys_process_inheritance_input_destroy(&decoded);
        expected = variant == 0 ? OS_ENOTSUPPORTED : OS_EINVALPARAMS;
        output = __InheritanceOutput(&source);
        output.resources = &childID;
        wire = (gracht_buffer_t) { .data = bytes, .limit = sizeof(bytes) };
        serialize_sys_process_inheritance_output(&wire, &output);
        CHECK(CRTReadInheritanceBlock(bytes, wire.index) == expected);
    }

    // Descriptor modes are not compared with token scope before import. The
    // kernel limits access through the installed reference.
    descriptor.descriptor = 7;
    resource.scope = __FILE_WRITE_ACCESS;
    wire = (gracht_buffer_t) { .data = bytes, .limit = sizeof(bytes) };
    serialize_sys_process_inheritance_input(&wire, &source);
    CHECK(__ReadInheritanceInput(bytes, wire.index, &decoded) == OS_EOK);
    sys_process_inheritance_input_destroy(&decoded);
    resource.scope = __FILE_READ_ACCESS;

    descriptor.resource_index = 1;
    wire = (gracht_buffer_t) { .data = bytes, .limit = sizeof(bytes) };
    serialize_sys_process_inheritance_input(&wire, &source);
    CHECK(__ReadInheritanceInput(bytes, wire.index, &decoded) == OS_EINVALPARAMS);
    descriptor.resource_index = 0;
    resource.resource_type = UINT32_MAX;
    wire = (gracht_buffer_t) { .data = bytes, .limit = sizeof(bytes) };
    serialize_sys_process_inheritance_input(&wire, &source);
    CHECK(__ReadInheritanceInput(bytes, wire.index, &decoded) == OS_EINVALPARAMS);
}

static void __TestMixedResources(void)
{
    char bytes[4096];
    struct InheritanceOptions options = { .Flags = PROCESS_INHERIT_FILES };
    struct inheritance_export exported;
    struct sys_process_inheritance_input input;
    struct sys_process_inheritance_output output;
    uuid_t ids[] = {3500, 3501, 3502};
    gracht_buffer_t wire;
    unsigned int i;

    __Init();
    __File(7, 100, __IO_READ);
    __File(8, 102, __IO_READ | __IO_NONBLOCK | __IO_PIPE);
    stdio_handle_get(8)->Signature = PIPE_SIGNATURE;
    __File(9, 103, __IO_RW);
    __File(10, 103, __IO_RW);
    stdio_handle_get(9)->Signature = NET_SIGNATURE;
    stdio_handle_get(10)->Signature = NET_SIGNATURE;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_EOK);
    CHECK(__ReadInheritanceInput(bytes, exported.length, &input) == OS_EOK);
    CHECK(input.resources_count == 3 && input.descriptors_count == 4);
    for (i = 7; i <= 10; i++) __Close(i);
    output = __InheritanceOutput(&input);
    output.resources = ids;
    wire = (gracht_buffer_t){.data = bytes, .limit = sizeof(bytes)};
    serialize_sys_process_inheritance_output(&wire, &output);
    for (i = 0; i < 3; i++) g_live[ids[i]] = true;
    CHECK(CRTReadInheritanceBlock(bytes, wire.index) == OS_EOK);
    CHECK(g_pipeAdoptions == 1 && g_socketAdoptions == 1);
    CHECK(stdio_handle_get(8)->XTFlags & __IO_NONBLOCK);
    CHECK(stdio_handle_get(8)->XTFlags & __IO_PIPE);
    CHECK(stdio_handle_get(9)->OSHandle.ID == stdio_handle_get(10)->OSHandle.ID);
    for (i = 7; i <= 10; i++) __Close(i);
    sys_process_inheritance_input_destroy(&input);
    CRTReleaseInheritanceBlock(&exported);
    __Finish();
}

static void __TestSharedRequirement(void)
{
    char bytes[512];
    struct InheritanceOptions options = { .Flags = PROCESS_INHERIT_FILES };
    struct inheritance_export exported;
    int before;

    // Inheritance only exports descriptors whose copies share one handle.
    __Init();
    __File(8, 102, __IO_READ | __IO_PIPE);
    stdio_handle_get(8)->Signature = PIPE_SIGNATURE;
    stdio_handle_get(8)->Shared = false;
    before = g_creates;
    CHECK(CRTWriteInheritanceBlock(&options, bytes, sizeof(bytes), &exported) == OS_ENOTSUPPORTED);
    CHECK(g_creates == before);
    stdio_handle_get(8)->Shared = true;
    __Close(8);
    __Finish();
}

int main(void)
{
    __TestContractSemantics();
    __TestFlow();
    __TestFailures();
    __TestMixedResources();
    __TestSharedRequirement();
    __Init();
    __File(7, 100, __IO_READ);
    g_failDrop = true;
    CHECK(__TestDescriptorClose(7) == -1);
    CHECK(stdio_handle_get(7) != NULL && g_live[100]);
    g_failDrop = false;
    __Close(7);
    __Finish();
    CHECK(g_allocations == 0 && g_creates == g_revokes);
    puts("Inheritance: mixed resources, aliases, close retry, bounds and allocation rollback passed");
    return 0;
}
