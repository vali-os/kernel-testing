// Real DMA wrappers with controlled syscall and provider transport failures.
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#undef assert
#define assert(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#include <os/types/syscall.h>
#include <os/handle.h>
#include <internal/_syscalls.h>
#include <time.h>
typedef int errno_t;
#include <internal/_tls.h>

static struct thread_storage g_tls;
struct thread_storage* __tls_current(void) { return &g_tls; }
uuid_t usched_notification_handle(void) { assert(0); return 0; }
void usched_wait_async(void) { assert(0); }

_Static_assert(sizeof(struct OSDmaMappingResult) == 24, "DMA mapping result ABI");
_Static_assert(offsetof(struct OSDmaMappingResult, InlineSegment) == 8, "DMA inline segment ABI");

static oserr_t g_waitStatus;
static struct SHMAccessBeginParameters g_waitParameters;
static unsigned int g_expectedMapFlags;
static struct DMAMapRequirements g_expectedMapRequirements;
static int g_allocations;
static int g_failAfter = -1;

static void*
__Allocate(size_t size)
{
    void* result;

    // Fail a chosen bookkeeping allocation to check that no kernel owner leaks.
    if (g_failAfter == 0) {
        return NULL;
    }
    if (g_failAfter > 0) {
        g_failAfter--;
    }
    result = malloc(size);
    assert(result != NULL);
    g_allocations++;
    return result;
}

static void*
__Calloc(size_t count, size_t size)
{
    void* result = __Allocate(count * size);

    // Mirror calloc so the production wrapper gets its usual zeroed owners.
    if (result != NULL) {
        memset(result, 0, count * size);
    }
    return result;
}

static void
__Free(void* pointer)
{
    // Track wrapper allocations independently of the fake hardware storage.
    if (pointer != NULL) {
        g_allocations--;
        free(pointer);
    }
}

#define malloc __Allocate
#define calloc __Calloc
#define free __Free
#include "../librt/libddk/dma/dma.c"
#include "../librt/libddk/dma/context.c"
#undef free
#undef calloc
#undef malloc


static uint64_t g_session = 41;
static enum SHMDeviceCachePolicy g_cachePolicy = SHMDeviceCacheCoherent;
static int g_releaseFailure;
static int g_queryFailure;
static int g_releases;
static int g_queries;

oserr_t
OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest request, void* data, size_t length)
{
    struct OSDmaConfiguration* result = data;

    assert(device == 7);
    if (request == OSIOCTLREQUEST_DMA_QUERY) {
        g_queries++;
        assert(length == sizeof(*result));
        memset(result, 0, sizeof(*result));
        result->Session = g_session;
        result->Count = 1;
        result->CachePolicy = g_cachePolicy;
        result->Ranges[0].Length = UINT64_MAX;
        // A failed reply may leave output bytes behind; they are not a confirmed lease.
        return g_queryFailure ? OS_EPROTOCOL : OS_EOK;
    }
    assert(request == OSIOCTLREQUEST_DMA_RELEASE && length == sizeof(uint64_t));
    assert(*(uint64_t*)data == g_session);
    g_releases++;
    return g_releaseFailure ? OS_EUNKNOWN : OS_EOK;
}
#ifdef MOLLENOS
static int g_contextLocked;
void usched_call_once(struct usched_once_flag* flag, void (*fn)(void)) { static int done; (void)flag; if (!done) { done = 1; fn(); } }
void usched_mtx_init(struct usched_mtx* m, int type) { assert(m == &g_dmaContextLock && type == mtx_plain); }
void usched_mtx_lock(struct usched_mtx* m) { assert(m == &g_dmaContextLock && !g_contextLocked); g_contextLocked = 1; }
void usched_mtx_unlock(struct usched_mtx* m) { assert(m == &g_dmaContextLock && g_contextLocked); g_contextLocked = 0; }
#endif

static int g_calls;
static int g_logs;
static int g_unmaps;
static int g_detaches;
static int g_failRollback;
static int g_segmentCalls;
static int g_contexts;
static int g_mappings;
static int g_shmOwners;
static int g_deviceOwned;
static unsigned int g_persistentUses;
static int g_badCount;
static uint32_t g_resultSegmentCount = 1;
static int g_failSyscall = -1;
static oserr_t g_failStatus;
static size_t g_lastOffset;
static size_t g_lastLength;
static size_t g_transferOffset;
static size_t g_transferLength;
static unsigned char g_buffer[4096];

static struct {
    unsigned int Errors;
    unsigned int Warnings;
    unsigned int Traces;
    unsigned int Matches;
    enum OSSysLogLevel Level;
    const char* Expected;
} g_diagnostics;

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    va_list arguments;
    char    message[512];

    g_logs++;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_diagnostics.Errors++;
    } else if (level == OSSYSLOGLEVEL_WARNING) {
        g_diagnostics.Warnings++;
    } else if (level == OSSYSLOGLEVEL_TRACE) {
        g_diagnostics.Traces++;
    }
    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (g_diagnostics.Expected != NULL && level == g_diagnostics.Level &&
        strstr(message, g_diagnostics.Expected) != NULL) {
        g_diagnostics.Matches++;
    }
}

static void
__ExpectDiagnostic(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        expected)
{
    memset(&g_diagnostics, 0, sizeof(g_diagnostics));
    g_diagnostics.Level = level;
    g_diagnostics.Expected = expected;
}

static oserr_t
__ModelBegin(
    _In_ uuid_t                                 handle,
    _In_ const struct SHMAccessBeginParameters* parameters)
{
    g_waitParameters = *parameters;
    if (g_waitStatus != OS_EOK) {
        return g_waitStatus;
    }
    assert(handle == 202 && g_mappings == 1);
    switch (parameters->Kind) {
        case DMAAccessTransfer:
            g_lastOffset = parameters->Offset;
            g_lastLength = parameters->Length;
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_transferOffset = parameters->Offset;
            g_transferLength = parameters->Length;
            g_deviceOwned = 1;
            break;
        case DMAAccessUse:
            assert(g_expectedMapFlags == (DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT));
            if (g_persistentUses == UINT_MAX) {
                return OS_EOVERFLOW;
            }
            g_persistentUses++;
            break;
        default:
            return OS_EINVALPARAMS;
    }
    return OS_EOK;
}

static oserr_t
__ModelEnd(
    _In_ uuid_t                 handle,
    _In_ unsigned int operation,
    _In_ size_t                 length)
{
    assert(handle == 202 && g_mappings == 1);
    switch (operation) {
        case DMAAccessTransfer:
            g_lastOffset = g_transferOffset;
            g_lastLength = length;
            if (!g_deviceOwned || length > g_transferLength) {
                return OS_EINVALPARAMS;
            }
            g_deviceOwned = 0;
            break;
        case DMAAccessUse:
            assert(length == 0);
            assert(g_expectedMapFlags == (DMA_MAP_REQUIRE_DIRECT | DMA_MAP_PERSISTENT));
            assert(g_persistentUses != 0);
            g_persistentUses--;
            break;
        default:
            return OS_EINVALPARAMS;
    }
    return OS_EOK;
}

SCTYPE
_syscall(SCTYPE function, SCTYPE arg0, SCTYPE arg1, SCTYPE arg2, SCTYPE arg3, SCTYPE arg4)
{
    const struct DMARequirements* requirements = (const void*)(uintptr_t)arg1;
    const struct OSDmaMapParameters* parameters = (const void*)(uintptr_t)arg2;
    struct OSDmaAllocationResult* allocation = (void*)(uintptr_t)arg2;
    struct OSDmaMappingResult* mapping = (void*)(uintptr_t)arg3;

    // Use the real syscall macros and runtime argument forwarding. Success is
    // supplied by the model; production registry coverage is a separate test.
    g_calls++;
    assert(arg4 == 0);
    if (function == g_failSyscall) {
        return g_failStatus;
    }
    switch (function) {
        case 84:
            assert(arg0 == 0 && arg3 == 0);
            return __ModelBegin((uuid_t)arg1, (void*)(uintptr_t)arg2);
        case 85:
            assert(arg3 == 0);
            return __ModelEnd((uuid_t)arg0, (unsigned int)arg1, (size_t)arg2);
        case 60:
            assert(g_mappings == 0 && g_shmOwners == 1);
            assert(((SHMHandle_t*)(uintptr_t)arg0)->ID == 303);
            g_shmOwners--;
            g_detaches++;
            break;
        case 79:
            assert(arg0 == 7 && arg3 == 0);
            assert(*(const uint64_t*)(uintptr_t)arg1 == UINT64_MAX);
            assert(((const struct OSDmaOpenParameters*)(uintptr_t)arg1)->Configuration.CachePolicy == g_cachePolicy);
            *(uuid_t*)(uintptr_t)arg2 = 101;
            g_contexts++;
            break;
        case 80:
            assert(arg0 == 101 && arg3 == 0 && g_contexts == 1 && g_mappings == 0);
            assert(requirements->Length == 256 && requirements->Alignment == 64);
            assert(requirements->Boundary == 0x10000);
            assert(requirements->Contiguous == (g_resultSegmentCount == 1));
            allocation->Buffer = (SHMHandle_t) { .ID = 303, .Buffer = g_buffer, .Length = 4096, .Capacity = 4096 };
            allocation->Mapping.Handle = 202;
            allocation->Mapping.SegmentCount = g_resultSegmentCount;
            allocation->Mapping.InlineSegment = (struct DMASegment) { 0x100001234ULL, 4096 };
            g_shmOwners++;
            g_mappings++;
            break;
        case 81:
            assert(arg0 == 101 && g_contexts == 1 && g_mappings == 0);
            assert(arg1 == 303 && parameters->Offset == 128 && parameters->Length == 512);
            assert(parameters->Direction == DMAFromDevice);
            assert(parameters->Flags == g_expectedMapFlags);
            assert(parameters->Requirements.AddressAlignment == g_expectedMapRequirements.AddressAlignment);
            assert(parameters->Requirements.LengthAlignment == g_expectedMapRequirements.LengthAlignment);
            mapping->Handle = 202;
            mapping->SegmentCount = g_resultSegmentCount;
            mapping->InlineSegment = (struct DMASegment) { 0 };
            if (g_resultSegmentCount == 1) {
                mapping->InlineSegment = (struct DMASegment) { 0x100001234ULL, 512 };
            }
            g_mappings++;
            break;
        case 82:
            assert(arg0 == 202 && g_mappings == 1 && arg2 == 2);
            ((struct DMASegment*)(uintptr_t)arg1)[0] = (struct DMASegment) { 0x100001234ULL, g_shmOwners ? 2048 : 256 };
            ((struct DMASegment*)(uintptr_t)arg1)[1] = (struct DMASegment) { 0x200001234ULL, g_shmOwners ? 2048 : 256 };
            *(uint32_t*)(uintptr_t)arg3 = g_badCount ? 3 : 2;
            g_segmentCalls++;
            break;
        case 86:
            if (arg0 == 101) {
                assert(g_contexts == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
                if (g_mappings != 0) return OS_EBUSY;
                g_contexts--;
                break;
            }
            if (g_persistentUses != 0) return OS_EBUSY;
            if (g_failRollback) return OS_EUNKNOWN;
            assert(arg0 == 202 && g_mappings == 1 && arg1 == 0 && arg2 == 0 && arg3 == 0);
            if (g_deviceOwned) {
                return OS_EBUSY;
            }
            g_unmaps++;
            g_mappings--;
            break;
        default:
            assert(0);
    }
    return OS_EOK;
}

static void
__TestBuffer(void)
{
    struct DMAContext* context = NULL;
    struct DMAContext* duplicate = NULL;
    struct DMABuffer* buffer = NULL;
    struct DMABuffer* saved;
    struct DMARequirements requirements = { 256, 64, 0x10000, true };
    const struct DMASegment* segments;
    uint32_t count;
    int calls;
    int allocations;
    int unmaps;
    int detaches = g_detaches;
    int queries = g_queries;
    int releases = g_releases;

    calls = g_calls;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    assert(g_queries == queries + 1 && g_calls == calls + 1);
    assert(DMAOpen(7, UINT64_MAX, &duplicate) == OS_EBUSY && duplicate == NULL);
    assert(g_queries == queries + 1);
    g_failAfter = 0;
    calls = g_calls;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EOOM && buffer == NULL);
    assert(g_calls == calls);
    g_failAfter = -1;
    g_failSyscall = 80;
    g_failStatus = OS_ENOENT;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_ENOENT && buffer == NULL);
    g_failSyscall = -1;

    calls = g_calls;
    allocations = g_allocations;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EOK);
    assert(g_calls == calls + 1 && g_allocations == allocations + 1);
    assert(DMABufferData(buffer) == g_buffer && DMABufferSize(buffer) == 4096);
    calls = g_calls;
    g_failAfter = 0;
    segments = DMABufferSegments(buffer, &count);
    assert(count == 1 && segments[0].Address == 0x100001234ULL);
    assert(segments[0].Length == 4096 && g_calls == calls);
    g_failAfter = -1;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EBUSY);
    assert(DMAClose(&context) == OS_EBUSY && !context->Closing);
    assert(g_calls == calls);

    // Unmap failure retains both owners. Detach failure retains the attachment
    // after unmap, and a retry must neither unmap twice nor close its context.
    saved = buffer;
    g_failSyscall = 86;
    g_failStatus = OS_EUNKNOWN;
    assert(DMAFree(&buffer) == OS_EUNKNOWN && buffer == saved && g_shmOwners == 1);
    g_failSyscall = 60;
    assert(DMAFree(&buffer) == OS_EUNKNOWN && buffer == saved && g_mappings == 0);
    assert(DMAClose(&context) == OS_EBUSY);
    unmaps = g_unmaps;
    g_failSyscall = -1;
    calls = g_calls;
    assert(DMAFree(&buffer) == OS_EOK && buffer == NULL);
    assert(g_unmaps == unmaps && g_calls == calls + 1 && g_shmOwners == 0);
    assert(g_detaches == detaches + 1);
    assert(DMAFree(&buffer) == OS_EOK && DMAFree(NULL) == OS_EOK);

    // Kernel close failure and then provider release failure are independently
    // retryable. Neither permits child creation through the closing object.
    g_failSyscall = 86;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAClose: Syscall_ResourceRelease device 7, context 101 failed:");
    assert(DMAClose(&context) == OS_EUNKNOWN && context->Closing);
    assert(g_diagnostics.Matches == 1);
    calls = g_calls;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EBUSY && g_calls == calls);
    g_failSyscall = -1;
    g_releaseFailure = 1;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAClose: OSDeviceIOCtl DMA_RELEASE device 7, session 41 failed:");
    assert(DMAClose(&context) == OS_EUNKNOWN && g_contexts == 0);
    assert(g_diagnostics.Matches == 1);
    calls = g_calls;
    g_releaseFailure = 0;
    assert(DMAClose(&context) == OS_EOK && context == NULL && g_calls == calls);
    assert(g_releases == releases + 2 && g_allocations == 0);
    assert(DMAClose(&context) == OS_EOK && DMAClose(NULL) == OS_EOK);
}

static void
__TestStreaming(
    _In_ bool direct)
{
    struct DMAContext* context = NULL;
    struct DMAMapping* mapping = NULL;
    struct DMAMapping* saved;
    OSHandle_t buffer = { .ID = 303, .Type = OSHANDLE_SHM };
    const struct DMASegment* segments;
    const struct DMASegment* cached;
    uint32_t count;
    int calls;
    int logs;
    int queries;
    int allocations;
    int i;
    oserr_t (*map)(struct DMAContext*, const OSHandle_t*, size_t, size_t,
        enum DMADirection, const struct DMAMapRequirements*, struct DMAMapping**) = direct ? DMAMapSHMDirect : DMAMapSHM;

    g_expectedMapFlags = direct ? DMA_MAP_REQUIRE_DIRECT : 0;
    g_expectedMapRequirements = (struct DMAMapRequirements){2, 2};
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    g_resultSegmentCount = 2;
    // Failure at either local allocation, the map syscall, or the segment query
    // publishes nothing. Every successful kernel creation must be rolled back.
    for (i = 0; i < 2; i++) {
        g_failAfter = i;
        assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EOOM);
        assert(mapping == NULL && g_mappings == 0 && g_allocations == 1);
    }
    g_failAfter = -1;
    g_failSyscall = 81;
    g_failStatus = OS_EINCOMPLETE;
    assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EINCOMPLETE);
    g_failSyscall = 82;
    g_failStatus = OS_EBUFFER;
    assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EBUFFER);
    assert(mapping == NULL && g_mappings == 0);
    g_failSyscall = -1;
    g_badCount = 1;
    assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EUNKNOWN);
    assert(mapping == NULL && g_mappings == 0);
    g_badCount = 0;

    calls = g_calls;
    assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EOK);
    assert(g_calls == calls + 2 && g_shmOwners == 0);
    segments = DMAMappingSegments(mapping, &count);
    assert(count == 2 && segments[0].Address == 0x100001234ULL);
    assert(segments[1].Address == 0x200001234ULL);
    cached = segments;
    calls = g_calls;
    assert(map(context, &buffer, 128, 512, DMAFromDevice, &g_expectedMapRequirements, &mapping) == OS_EBUSY);
    assert(DMAClose(&context) == OS_EBUSY && g_calls == calls);
    assert(DMABeginTransfer(mapping, 0, 0, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(DMABeginTransfer(mapping, 500, 20, SHMWaitNonBlocking, NULL) == OS_EINVALPARAMS);
    assert(DMAEndTransfer(mapping, 0) == OS_EINVALPARAMS);
    assert(g_calls == calls + 1);

    g_failSyscall = 84;
    g_failStatus = OS_EUNKNOWN;
    assert(DMABeginTransfer(mapping, 16, 256, SHMWaitNonBlocking, NULL) == OS_EUNKNOWN);
    assert(!g_deviceOwned);
    g_failSyscall = -1;
    assert(DMABeginTransfer(mapping, 16, 256, SHMWaitNonBlocking, NULL) == OS_EOK);
    assert(g_lastOffset == 16 && g_lastLength == 256);
    saved = mapping;
    calls = g_calls;
    assert(DMABeginTransfer(mapping, 0, 512, SHMWaitNonBlocking, NULL) == OS_EBUSY);
    assert(DMAUnmap(&mapping) == OS_EBUSY && mapping == saved);
    assert(DMAEndTransfer(mapping, 257) == OS_EINVALPARAMS && g_calls == calls + 3);
    g_failSyscall = 85;
    assert(DMAEndTransfer(mapping, 7) == OS_EUNKNOWN && g_deviceOwned && g_transferLength == 256);
    g_failSyscall = -1;
    assert(DMAEndTransfer(mapping, 7) == OS_EOK);
    assert(g_lastOffset == 16 && g_lastLength == 7);

    // Reusing a mapping must do exactly two syscalls per transfer, with no RPC,
    // heap traffic, segment query or logging on the successful hot path.
    calls = g_calls;
    queries = g_queries;
    allocations = g_allocations;
    logs = g_logs;
    g_failAfter = 0;
    for (i = 0; i < 1000; i++) {
        assert(DMAMappingSegments(mapping, &count) == cached && count == 2);
        assert(DMABeginTransfer(mapping, 32, 128, SHMWaitNonBlocking, NULL) == OS_EOK);
        assert(DMAEndTransfer(mapping, 0) == OS_EOK);
    }
    g_failAfter = -1;
    assert(g_calls == calls + 2000 && g_queries == queries);
    assert(g_allocations == allocations && g_logs == logs && g_lastOffset == 32);
    assert(g_lastLength == 0);
    g_failSyscall = 86;
    assert(DMAUnmap(&mapping) == OS_EUNKNOWN && mapping == saved);
    g_failSyscall = -1;
    assert(DMAUnmap(&mapping) == OS_EOK && mapping == NULL);
    assert(DMAUnmap(&mapping) == OS_EOK && DMAUnmap(NULL) == OS_EOK);
    assert(DMAClose(&context) == OS_EOK && g_allocations == 0);
    g_resultSegmentCount = 1;
    g_expectedMapFlags = 0;
    g_expectedMapRequirements = (struct DMAMapRequirements){0};
}

static void
__TestInlineAndRollback(void)
{
    struct DMAContext* context = NULL;
    struct __DmaResource* pending;
    struct DMAMapping* mapping = NULL;
    struct DMABuffer* allocation = NULL;
    struct DMARequirements requirements = { 256, 64, 0x10000, true };
    OSHandle_t buffer = { .ID = 303, .Type = OSHANDLE_SHM };
    const struct DMASegment* segments;
    uint32_t count;
    int calls;
    int releases;
    int i;

    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    calls = g_calls;
    g_failSyscall = 82;
    g_failStatus = OS_EUNKNOWN;
    assert(DMAMapSHM(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EOK);
    assert(g_calls == calls + 1);
    calls = g_calls;
    g_failAfter = 0;
    segments = DMAMappingSegments(mapping, &count);
    assert(count == 1 && segments[0].Length == 512 && g_calls == calls);
    g_failAfter = -1;
    g_failSyscall = -1;
    assert(DMAUnmap(&mapping) == OS_EOK);
    assert(DMAClose(&context) == OS_EOK && g_allocations == 0);

    // If rollback itself fails, context close owns the retry. Exercise both a
    // streaming mapping and a buffer with a separate attachment to release.
    for (i = 0; i < 2; i++) {
        assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
        g_resultSegmentCount = 2;
        g_failSyscall = 82;
        g_failRollback = 1;
        __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "__DmaDeferCleanup: device 7, context 101, mapping 202 deferring resource cleanup");
        if (i == 0) {
            assert(DMAMapSHM(context, &buffer, 128, 512, DMAFromDevice, NULL, &mapping) == OS_EUNKNOWN);
            assert(mapping == NULL);
        } else {
            requirements.Contiguous = false;
            assert(DMAAllocate(context, &requirements, &allocation) == OS_EUNKNOWN);
            assert(allocation == NULL && g_shmOwners == 1);
        }
        assert(g_mappings == 1 && context->Pending != NULL);
        assert(g_diagnostics.Matches == 1);
        pending = context->Pending;
        calls = g_calls;
        releases = g_releases;
        __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAClose: __DmaCollect device 7, context 101 failed:");
        assert(DMAClose(&context) == OS_EUNKNOWN && context != NULL);
        assert(g_diagnostics.Matches == 1);
        assert(context->Pending == pending && !context->Closing);
        assert(g_calls == calls + 1 && g_releases == releases && g_contexts == 1);
        g_failRollback = 0;
        g_failSyscall = -1;
        assert(DMAClose(&context) == OS_EOK && context == NULL);
        assert(g_allocations == 0 && g_mappings == 0 && g_shmOwners == 0);
    }
    g_resultSegmentCount = 1;
}

static void
__TestFragmentedBuffer(void)
{
    struct DMAContext* context = NULL;
    struct DMABuffer* buffer = NULL;
    struct DMARequirements requirements = { 256, 64, 0x10000, false };
    const struct DMASegment* segments;
    uint32_t count;
    int calls;
    int queries;
    int segmentCalls;

    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    g_resultSegmentCount = 2;
    calls = g_calls;
    queries = g_queries;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EOK);
    assert(g_calls == calls + 2 && g_queries == queries);
    calls = g_calls;
    segmentCalls = g_segmentCalls;
    g_failAfter = 0;
    segments = DMABufferSegments(buffer, &count);
    assert(count == 2 && segments[1].Address == 0x200001234ULL);
    assert(DMABufferSegments(buffer, &count) == segments);
    assert(g_calls == calls && g_segmentCalls == segmentCalls);
    g_failAfter = -1;
    assert(DMAFree(&buffer) == OS_EOK && buffer == NULL);
    assert(g_calls == calls + 2 && g_shmOwners == 0);
    assert(DMAClose(&context) == OS_EOK && g_allocations == 0);
    g_resultSegmentCount = 1;
}

static void
__TestOpenRollback(void)
{
    struct DMAContext* context = NULL;
    struct DMAContext* duplicate = NULL;
    struct DMABuffer* buffer = NULL;
    struct DMARequirements requirements = { .Length = 256 };
    int calls;
    int releases;
    char diagnostic[256];

    // A clean rollback publishes no owner and does not need a later open.
    g_failSyscall = 79;
    g_failStatus = OS_EOOM;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAOpen: Syscall_DmaOpen device 7, session 41 failed:");
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOOM);
    assert(g_diagnostics.Matches == 1);
    assert(context == NULL && g_dmaContexts == NULL && g_allocations == 0);

    // A failed provider release must stay reachable even when initialization is
    // abandoned. The retained object allows only close, with no kernel handle.
    g_releaseFailure = 1;
    snprintf(diagnostic, sizeof(diagnostic),
        "DMAOpen: OSDeviceIOCtl DMA_RELEASE device 7, session 41 rollback failed: %u (open status %u)",
        OS_EUNKNOWN, OS_EOOM);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, diagnostic);
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOOM);
    assert(g_diagnostics.Matches == 1 && g_diagnostics.Errors == 2);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, NULL);
    assert(context != NULL && context->Closing && context->Handle == UUID_INVALID);
    calls = g_calls;
    assert(DMAAllocate(context, &requirements, &buffer) == OS_EBUSY);
    assert(buffer == NULL && g_calls == calls);
    assert(DMAOpen(7, UINT64_MAX, &duplicate) == OS_EBUSY && duplicate == NULL);
    assert(DMAClose(&context) == OS_EUNKNOWN && context != NULL);
    assert(g_calls == calls);
    releases = g_releases;
    g_releaseFailure = 0;
    assert(DMAClose(&context) == OS_EOK && context == NULL);
    assert(g_releases == releases + 1 && g_calls == calls);
    assert(g_dmaContexts == NULL && g_allocations == 0);
    g_failSyscall = -1;

    // Successful cleanup releases the device for a completely new context.
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    assert(DMAClose(&context) == OS_EOK && context == NULL);
}

static void
__TestContextDiagnostics(void)
{
    struct DMAContext* context = NULL;
    struct DMAContext* duplicate = NULL;
    int calls = g_calls;
    int queries = g_queries;
    int releases = g_releases;

    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "DMAOpen: missing context output or invalid device");
    assert(DMAOpen(7, UINT64_MAX, NULL) == OS_EINVALPARAMS);
    assert(DMAOpen(UUID_INVALID, UINT64_MAX, &context) == OS_EINVALPARAMS);
    assert(g_diagnostics.Matches == 2 && context == NULL);
    assert(g_calls == calls && g_queries == queries && g_releases == releases);

    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAOpen: context allocation failed for device 7");
    g_failAfter = 0;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOOM);
    assert(g_diagnostics.Matches == 1 && context == NULL);
    assert(g_calls == calls && g_queries == queries && g_releases == releases);
    assert(g_allocations == 0 && g_dmaContexts == NULL);
    g_failAfter = -1;

    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "DMAOpen: OSDeviceIOCtl DMA_QUERY device 7 failed:");
    g_queryFailure = 1;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EPROTOCOL);
    assert(g_diagnostics.Matches == 1 && context == NULL);
    assert(g_calls == calls && g_queries == queries + 1 && g_releases == releases);
    assert(g_allocations == 0 && g_contexts == 0 && g_dmaContexts == NULL);
    g_queryFailure = 0;

    // A failed open must release the registry lock and allow a fresh attempt.
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, NULL);
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EOK);
    assert(context != NULL && context->Handle == 101 && context->Session == g_session);
    assert(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 0);
#ifdef __TRACE
    assert(g_diagnostics.Traces != 0);
#else
    assert(g_diagnostics.Traces == 0);
#endif
    calls = g_calls;
    queries = g_queries;
    assert(DMAOpen(7, UINT64_MAX, &context) == OS_EBUSY);
    assert(DMAOpen(7, UINT64_MAX, &duplicate) == OS_EBUSY && duplicate == NULL);
    assert(g_calls == calls && g_queries == queries && g_releases == releases);
    assert(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 0);
    assert(DMAClose(&context) == OS_EOK && context == NULL);
    assert(DMAClose(&context) == OS_EOK && DMAClose(NULL) == OS_EOK);
    assert(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 0);
    assert(g_allocations == 0 && g_contexts == 0 && g_dmaContexts == NULL);
#ifdef MOLLENOS
    assert(!g_contextLocked);
#endif
    puts("DMA contexts: query/allocation diagnostics, quiet duplicates and retryable registry lifetime passed");
}

#include "ddk_dma_persistent_test.inc"

int
main(void)
{
    // The driver API and syscall budgets must be identical for coherent and
    // non-coherent providers; cache policy belongs to the kernel, not callers.
    for (g_cachePolicy = SHMDeviceCacheNonCoherent;
         g_cachePolicy <= SHMDeviceCacheCoherent;
         g_cachePolicy++) {
        __TestContextDiagnostics();
        __TestOpenRollback();
        __TestBuffer();
        __TestStreaming(false);
        __TestStreaming(true);
        __TestPersistentUses();
        __TestInlineAndRollback();
        __TestFragmentedBuffer();
    }
    puts("DDK DMA: typed owners, retryable cleanup, completed prefixes and exact syscall/RPC budgets passed");
    return 0;
}
