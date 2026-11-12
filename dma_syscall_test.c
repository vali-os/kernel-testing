// Exercise the production access syscalls with an instrumented SHM backend.
// Translation and cache operations themselves are covered by shm_device_test.
#include "../kernel/api/dma.c"
#include "../kernel/api/shm.c"
#include "../kernel/api/handles.c"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static oserr_t
__TestBeginTransfer(uuid_t mapping, size_t offset, size_t length)
{
    struct SHMAccessBeginParameters request = { .Kind = DMAAccessTransfer,
        .Mode = SHMWaitNonBlocking, .Offset = offset, .Length = length };
    return ScShmAccessBegin(NULL, mapping, &request);
}

static oserr_t
__TestBeginUse(uuid_t mapping)
{
    struct SHMAccessBeginParameters request = { .Kind = DMAAccessUse, .Mode = SHMWaitNonBlocking };
    return ScShmAccessBegin(NULL, mapping, &request);
}

static oserr_t
__TestBeginCpu(uuid_t mapping, enum SHMCpuAccess access)
{
    struct SHMAccessBeginParameters request = { .Mode = SHMWaitNonBlocking };
    request.Kind = SHMAccessCpu;
    request.Write = access == SHMCpuAccessWrite;
    if (access != SHMCpuAccessRead && access != SHMCpuAccessWrite) {
        request.Kind = (unsigned int)-1;
    }
    return ScShmAccessBegin(NULL, mapping, &request);
}

struct SHMDeviceContext { int References; struct SHMDeviceMapping* Quarantine; };
struct SHMDeviceMapping { struct SHMDeviceContext* Context; struct SHMDeviceMapping* Next; int Owned; unsigned int Uses; bool Persistent; struct SHMAccess Access; };
void __TestHandlesInitialize(void);
void __TestHandlesDrain(void);
void __TestHandlesFinish(void);
uuid_t __TestNextHandleId(uuid_t next);
unsigned int __TestHandleAllocationCalls(void);

static MSContextSecurity_t g_security = { .GlobalHandleID = 10 };
static int g_live, g_contexts, g_maps, g_buffers, g_locked;
static struct SHMRecord* g_releaseRecord;
static int g_copyFailure, g_engineFailure, g_oom;
static int g_contextFailure, g_capabilityFailure;
static bool g_dmaForkWait;
static bool g_checkCopyVisibility;
static bool g_segmentsUnavailable;
static unsigned int g_copyCalls, g_failCopyCall;
static unsigned int g_directMapCalls;
static unsigned int g_persistentMapCalls;
static unsigned int g_mapCalls, g_allocateCalls, g_contextCalls;
static int g_hostErrno;
int*
__errno(void)
{
    return &g_hostErrno;
}
static struct DMASegment g_segments[] = {
    { 0x100001000ULL, 4096 },
    { 0x200001000ULL, 4096 }
};
static uint32_t g_segmentCount = 1;

static struct {
    unsigned int Errors;
    unsigned int Warnings;
    unsigned int Traces;
    unsigned int Matches;
    enum OSSysLogLevel Level;
    const char* Expected;
} g_diagnostics;

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    va_list arguments;
    char    message[512];

    CHECK(!g_locked);
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

void* kmalloc(size_t length) { if (g_oom) return NULL; g_live++; return calloc(1, length); }
void kfree(void* p) { CHECK(p != NULL && !g_locked); g_live--; free(p); }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) { CHECK(atomic_exchange(&lock->Current, 1) == 0); }
void SpinlockReleaseIrq(Spinlock_t* lock) { CHECK(atomic_exchange(&lock->Current, 0) == 1); }
int
list_cmp_default(
    _In_ void* left,
    _In_ void* right)
{
    return left != right;
}

MSContextSecurity_t* MemorySpaceGetSecurityContext(void) { return &g_security; }
oserr_t CapabilityVerify(Capabilities_t c)
{
    CHECK(c == CAPABILITY_DEVICEIO_ACCESS);
    return g_capabilityFailure ? OS_EPERMISSIONS : OS_EOK;
}
static oserr_t __CopyDmaUser(void* user, void* kernel, size_t length, bool toUser)
{
    CHECK(!g_locked);
    g_copyCalls++;
    if (g_copyCalls == g_failCopyCall) return OS_EINVALPARAMS;
    if (toUser && g_checkCopyVisibility) {
        // Open and map replies put the kernel handle first in the result.
        CHECK(ScResourceRelease(*(uuid_t*)kernel) == OS_EBUSY);
        CHECK(ScDestroyHandle(*(uuid_t*)kernel) == OS_EINVALPARAMS);
    }
    if (!user || (toUser && g_copyFailure)) return OS_EINVALPARAMS;
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}

oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       kernelBuffer,
    _In_  const void* userBuffer,
    _In_  size_t      length)
{
    return __CopyDmaUser((void*)userBuffer, kernelBuffer, length, false);
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       userBuffer,
    _In_  const void* kernelBuffer,
    _In_  size_t      length)
{
    return __CopyDmaUser(userBuffer, (void*)kernelBuffer, length, true);
}
oserr_t SHMDeviceContextCreate(const struct SHMDeviceRange* ranges, uint32_t count,
    enum SHMDeviceCachePolicy policy, uint64_t limit, struct SHMDeviceContext** out)
{
    g_contextCalls++;
    CHECK(count == 1 && ranges[0].DeviceBase == 0x100000000ULL);
    CHECK(policy == SHMDeviceCacheNonCoherent && limit == UINT64_MAX);
    if (g_contextFailure) return OS_ENOTSUPPORTED;
    *out = calloc(1, sizeof(**out)); (*out)->References = 1; g_contexts++; return OS_EOK;
}
void SHMDeviceContextRelease(struct SHMDeviceContext** context)
{
    if (g_releaseRecord != NULL) CHECK(g_releaseRecord->Object == NULL);
    if (*context && --(*context)->References == 0) { free(*context); g_contexts--; }
    *context = NULL;
}
oserr_t
SHMDeviceContextClose(struct SHMDeviceContext** context)
{
    if ((*context)->References != 1) return OS_EBUSY;
    SHMDeviceContextRelease(context);
    return OS_EOK;
}
static struct DMAMapRequirements g_expectedMapRequirements;

oserr_t
SHMDeviceMap(
    _In_    struct SHMDeviceContext*         context,
    _In_    uuid_t                           shm,
    _In_    size_t                           offset,
    _In_    size_t                           length,
    _In_    enum DMADirection                direction,
    _In_    enum SHMDeviceMappingKind        kind,
    _In_    const struct DMAMapRequirements* requirements,
    _InOut_ struct SHMDeviceMapping**        out)
{
    CHECK(!g_locked);
    g_mapCalls++;
    if (kind == SHMDeviceMappingStreamingDirect) {
        g_directMapCalls++;
    } else if (kind == SHMDeviceMappingPersistent) {
        g_persistentMapCalls++;
    } else {
        CHECK(kind == SHMDeviceMappingStreamingBounce);
    }
    CHECK(requirements != NULL);
    CHECK(requirements->AddressAlignment == g_expectedMapRequirements.AddressAlignment);
    CHECK(requirements->LengthAlignment == g_expectedMapRequirements.LengthAlignment);
    CHECK(shm == 99 && offset == 16 && length == 64 && direction == DMAFromDevice);
    if (g_engineFailure) {
        return OS_EOOM;
    }
    *out = calloc(1, sizeof(**out));
    (*out)->Context = context;
    (*out)->Persistent = kind == SHMDeviceMappingPersistent;
    (*out)->Access.Offset = offset;
    (*out)->Access.Length = length;
    context->References++;
    g_maps++;
    return OS_EOK;
}

oserr_t
SHMDeviceBeginUse(
    _In_ struct SHMDeviceMapping*    mapping,
    _In_ const struct SHMAccessWait* wait)
{
    (void)wait;
    CHECK(!g_locked);
    if (!mapping->Persistent) {
        return OS_EINVALPARAMS;
    }
    if (g_engineFailure) {
        return OS_EUNKNOWN;
    }
    mapping->Uses++;
    mapping->Access.Phase = SHMAccessActive;
    mapping->Access.Holder = SHMAccessHolderDevice;
    return OS_EOK;
}

oserr_t
SHMDeviceEndUse(
    _In_ struct SHMDeviceMapping* mapping)
{
    CHECK(!g_locked);
    if (!mapping->Persistent || mapping->Uses == 0) {
        return OS_EINVALPARAMS;
    }
    if (g_engineFailure) {
        return OS_EUNKNOWN;
    }
    mapping->Uses--;
    mapping->Access.Phase = mapping->Uses != 0 ? SHMAccessActive : SHMAccessIdle;
    mapping->Access.Holder = mapping->Uses != 0 ? SHMAccessHolderDevice : SHMAccessHolderNone;
    return OS_EOK;
}

oserr_t SHMDeviceAllocate(struct SHMDeviceContext* context,
    const struct DMARequirements* req, SHMHandle_t* buffer, struct SHMDeviceMapping** out)
{
    g_allocateCalls++;
    CHECK(req->Length == 4096);
    if (g_engineFailure) return OS_EOOM;
    *out = calloc(1, sizeof(**out)); (*out)->Context = context; context->References++; g_maps++;
    buffer->ID = 99; g_buffers++; return OS_EOK;
}
oserr_t SHMDetach(SHMHandle_t* buffer) { CHECK(buffer->ID == 99); g_buffers--; return OS_EOK; }
const struct DMASegment*
SHMDeviceMappingSegments(
    _In_  const struct SHMDeviceMapping* mapping,
    _Out_ uint32_t*                      count)
{
    CHECK(mapping != NULL);
    *count = g_segmentsUnavailable ? 0 : g_segmentCount;
    return g_segmentsUnavailable ? NULL : g_segments;
}
oserr_t
SHMDeviceSyncForDevice(
    _In_ struct SHMDeviceMapping*    mapping,
    _In_ size_t                      offset,
    _In_ size_t                      length,
    _In_ const struct SHMAccessWait* wait)
{
    CHECK(offset == 0 && length == 64);
    if (mapping->Persistent) return OS_EINVALPARAMS;
    if (mapping->Owned) return OS_EBUSY;
    if (g_dmaForkWait) {
        CHECK(wait->Blocking && wait->Async != NULL);
        mapping->Access.Phase = SHMAccessWaiting;
        return OS_EFORKED;
    }
    mapping->Owned = 1;
    return OS_EOK;
}
oserr_t SHMDeviceSyncForCpu(struct SHMDeviceMapping* m, size_t length)
{ CHECK(length <= 64); if (m->Persistent) return OS_EINVALPARAMS; if (!m->Owned) return OS_EINVALPARAMS; m->Owned = 0; return OS_EOK; }
oserr_t SHMDeviceUnmap(struct SHMDeviceMapping** mapping)
{
    if (g_releaseRecord != NULL) CHECK(g_releaseRecord->Object == NULL);
    CHECK(!g_locked);
    if (*mapping == NULL) return OS_EOK;
    if ((*mapping)->Owned || (*mapping)->Uses != 0) return OS_EBUSY;
    SHMDeviceContextRelease(&(*mapping)->Context); free(*mapping); *mapping = NULL; g_maps--; return OS_EOK;
}

#include "dma_record_test.inc"
#include "dma_range_syscall_test.inc"

static void
__TestFailureDiagnostics(
    _In_ const struct OSDmaOpenParameters* parameters,
    _In_ const struct OSDmaMapParameters*  map)
{
    struct DMARequirements        requirements = { .Length = 4096 };
    struct OSDmaMappingResult     result = { .Handle = 0xdead };
    struct OSDmaAllocationResult  allocation = { .Mapping.Handle = 0xdead };
    struct DMASegment            segment = { 0xdead, 0xbeef };
    struct SHMRecord*           held;
    uuid_t                       context = 0xdead;
    uint32_t                     count;
    unsigned int                 copies;
    unsigned int                 maps;
    unsigned int                 allocations;
    unsigned int                 contexts;
    HandleGrantsTable_t* ownerTable = g_security.HandleTable;
    HandleGrantsTable_t* otherTable = CreateHandleGrantsTable();

    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "ScDmaOpen: CapabilityVerify device 7 failed:");
    contexts = g_contextCalls;
    g_capabilityFailure = 1;
    CHECK(ScDmaOpen(7, parameters, &context) == OS_EPERMISSIONS);
    CHECK(context == 0xdead && g_contextCalls == contexts && g_diagnostics.Matches == 1);
    g_capabilityFailure = 0;
    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "ScDmaOpen: invalid device ID");
    CHECK(ScDmaOpen(UUID_INVALID, parameters, &context) == OS_EINVALPARAMS);
    CHECK(context == 0xdead && g_contextCalls == contexts && g_diagnostics.Matches == 1);

    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaOpen: MemorySpaceCopyFromUser parameters");
    CHECK(ScDmaOpen(7, NULL, &context) == OS_EINVALPARAMS);
    CHECK(context == 0xdead && g_contextCalls == contexts && g_diagnostics.Matches == 1);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaOpen: SHMDeviceContextCreate device 7");
    g_contextFailure = 1;
    copies = g_copyCalls;
    CHECK(ScDmaOpen(7, parameters, &context) == OS_ENOTSUPPORTED);
    CHECK(context == 0xdead && g_copyCalls == copies + 1 && g_diagnostics.Matches == 1);
    __TestHandlesDrain();
    CHECK(g_live == 0 && g_contexts == 0);
    g_contextFailure = 0;

    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, NULL);
    CHECK(ScDmaOpen(7, parameters, &context) == OS_EOK);
    CHECK(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 0);
#if defined(__TRACE) && defined(__OSCONFIG_LOGGING_KTRACE)
    CHECK(g_diagnostics.Traces != 0);
#else
    CHECK(g_diagnostics.Traces == 0);
#endif
    g_security.GlobalHandleID = 11;
    g_security.HandleTable = otherTable;
    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, NULL);
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    CHECK(g_diagnostics.Warnings == 0);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable;
    maps = g_mapCalls;
    allocations = g_allocateCalls;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaMapShm: MemorySpaceCopyFromUser parameters");
    CHECK(ScDmaMapShm(context, 99, NULL, &result) == OS_EINVALPARAMS);
    CHECK(result.Handle == 0xdead && g_mapCalls == maps && g_diagnostics.Matches == 1);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaAllocate: MemorySpaceCopyFromUser requirements");
    CHECK(ScDmaAllocate(context, NULL, &allocation) == OS_EINVALPARAMS);
    CHECK(allocation.Mapping.Handle == 0xdead && g_allocateCalls == allocations && g_diagnostics.Matches == 1);

    g_oom = 1;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "__CreateMappingRecord: SHMRecordCreate context");
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOOM);
    CHECK(result.Handle == 0xdead && g_mapCalls == maps && g_diagnostics.Matches == 1);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "__CreateMappingRecord: SHMRecordCreate context");
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EOOM);
    CHECK(allocation.Mapping.Handle == 0xdead && g_allocateCalls == allocations && g_diagnostics.Matches == 1);
    g_oom = 0;

    g_engineFailure = 1;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "SHM 99, offset 16, length 64, direction 1 failed:");
    copies = g_copyCalls;
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOOM);
    CHECK(result.Handle == 0xdead && g_copyCalls == copies + 1 && g_diagnostics.Matches == 1);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaAllocate: SHMDeviceAllocate context");
    copies = g_copyCalls;
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EOOM);
    CHECK(allocation.Mapping.Handle == 0xdead && g_copyCalls == copies + 1 && g_diagnostics.Matches == 1);
    g_engineFailure = 0;

    g_copyFailure = 1;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "__PublishMapping: MemorySpaceCopyToUser mapping");
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EINVALPARAMS);
    CHECK(result.Handle == 0xdead && g_diagnostics.Matches == 1);
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "__PublishMapping: MemorySpaceCopyToUser mapping");
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EINVALPARAMS);
    CHECK(allocation.Mapping.Handle == 0xdead && g_diagnostics.Matches == 1);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_buffers == 0 && g_live == 1);
    g_copyFailure = 0;

    CHECK(SHMRecordBegin(context, &g_contextOps, &held) == OS_EOK);
    CHECK(((struct SHMDeviceContext*)held->Object)->References == 1);
    SHMRecordEnd(held);
    CHECK(ScDmaMapShm(context, 99, map, &result) == OS_EOK);
    count = 0xdead;
    g_failCopyCall = g_copyCalls + 1;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaGetSegments: MemorySpaceCopyToUser count");
    CHECK(ScDmaGetSegments(result.Handle, &segment, 1, &count) == OS_EINVALPARAMS);
    CHECK(count == 0xdead && segment.Address == 0xdead && segment.Length == 0xbeef);
    CHECK(g_diagnostics.Matches == 1);
    g_failCopyCall = g_copyCalls + 2;
    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, "ScDmaGetSegments: MemorySpaceCopyToUser 1 segments");
    CHECK(ScDmaGetSegments(result.Handle, &segment, 1, &count) == OS_EINVALPARAMS);
    CHECK(count == 1 && segment.Address == 0xdead && segment.Length == 0xbeef);
    CHECK(g_diagnostics.Matches == 1);
    g_failCopyCall = 0;

    // Release-only mappings expose no usable addresses. The syscall must
    // reject both query forms before writing even the count to userspace.
    g_segmentsUnavailable = true;
    count = 0xdead;
    copies = g_copyCalls;
    CHECK(ScDmaGetSegments(result.Handle, NULL, 0, &count) == OS_EBUSY);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 1, &count) == OS_EBUSY);
    CHECK(g_copyCalls == copies && count == 0xdead);
    CHECK(segment.Address == 0xdead && segment.Length == 0xbeef);
    g_segmentsUnavailable = false;

    __ExpectDiagnostic(OSSYSLOGLEVEL_ERROR, NULL);
    copies = g_copyCalls;
    CHECK(ScDmaGetSegments(result.Handle, NULL, 0, &count) == OS_EOK && count == 1);
    CHECK(g_copyCalls == copies + 1);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 0, &count) == OS_EBUFFER && count == 1);
    CHECK(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 0);
    CHECK(ScResourceRelease(context) == OS_EBUSY);
    CHECK(ScResourceRelease(0xdead) == OS_ENOENT);
    CHECK(g_diagnostics.Errors == 0 && g_diagnostics.Warnings == 1);

    CHECK(__TestBeginTransfer(result.Handle, 0, 64) == OS_EOK);
    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "ScShmAccessBegin: handle");
    CHECK(__TestBeginTransfer(result.Handle, 0, 64) == OS_EBUSY);
    CHECK(g_diagnostics.Matches == 1);
    CHECK(SHMRecordBegin(result.Handle, &g_mappingOps, &held) == OS_EOK);
    SHMRecordEnd(held);
    g_releaseRecord = held;
    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "ScResourceRelease: resource");
    CHECK(ScResourceRelease(result.Handle) == OS_EBUSY && g_maps == 1);
    CHECK(held->Object != NULL);
    CHECK(g_diagnostics.Matches == 1);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 64) == OS_EOK);
    __ExpectDiagnostic(OSSYSLOGLEVEL_WARNING, "ScShmAccessEnd: handle");
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 64) == OS_EINVALPARAMS);
    CHECK(g_diagnostics.Matches == 1);
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    g_releaseRecord = NULL;
    CHECK(SHMRecordBegin(context, &g_contextOps, &held) == OS_EOK);
    SHMRecordEnd(held);
    g_releaseRecord = held;
    CHECK(ScResourceRelease(context) == OS_EOK);
    g_releaseRecord = NULL;
    __TestHandlesDrain();
    CHECK(g_live == 0 && g_contexts == 0 && g_maps == 0 && g_buffers == 0 && !g_locked);
    DestroyHandleGrantsTable(otherTable);
    puts("DMA diagnostics: per-call failures, unchanged outputs, quiet queries and cleanup state passed");
}

static void
__TestDirectMapFlags(
    _In_ uuid_t context)
{
    struct OSDmaMapParameters map = {
        .Offset = 16, .Length = 64, .Direction = DMAFromDevice,
        .Flags = DMA_MAP_REQUIRE_DIRECT
    };
    struct OSDmaMappingResult result = { .Handle = 0xdead };
    unsigned int calls = g_mapCalls;
    unsigned int directCalls = g_directMapCalls;

    map.Flags |= 0x80000000U;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_mapCalls == calls && g_directMapCalls == directCalls && g_live == 1);
    CHECK(result.Handle == 0xdead);
    map.Flags = DMA_MAP_REQUIRE_DIRECT;
    g_engineFailure = 1;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOOM);
    CHECK(g_mapCalls == calls + 1 && g_directMapCalls == directCalls + 1);
    __TestHandlesDrain();
    CHECK(result.Handle == 0xdead && g_maps == 0 && g_live == 1);
    g_engineFailure = 0;
    g_copyFailure = 1;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_live == 1 && result.Handle == 0xdead);
    g_copyFailure = 0;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    CHECK(g_directMapCalls == directCalls + 3 && g_mapCalls == calls + 3);
    CHECK(__TestBeginTransfer(result.Handle, 0, 64) == OS_EOK);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 64) == OS_EOK);
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    __TestHandlesDrain();
    CHECK(g_maps == 0 && g_live == 1);
}

#include "dma_persistent_syscall_test.inc"

int main(void)
{
    struct OSDmaOpenParameters parameters = { .AddressLimit = UINT64_MAX,
        .Configuration = { .Count = 1, .CachePolicy = SHMDeviceCacheNonCoherent,
            .Ranges = { { .DeviceBase = 0x100000000ULL, .Length = 0x10000 } } } };
    struct OSDmaMapParameters map = { .Offset = 16, .Length = 64, .Direction = DMAFromDevice };
    struct OSDmaMappingResult result = { 0 };
    struct OSDmaAllocationResult allocation = { 0 };
    struct DMARequirements requirements = { .Length = 4096 };
    struct DMASegment segment;
    HandleGrantsTable_t* ownerTable;
    HandleGrantsTable_t* otherTable;
    uuid_t context = 0;
    uint32_t count = 0;

    __TestHandlesInitialize();
    g_security.HandleTable = CreateHandleGrantsTable();
    CHECK(g_security.HandleTable != NULL);
    ownerTable = g_security.HandleTable;
    otherTable = CreateHandleGrantsTable();
    CHECK(otherTable != NULL);
    g_oom = 1;
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EOOM && context == 0);
    g_oom = 0; g_copyFailure = 1;
    __TestHandlesDrain();
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_contexts == 0 && g_live == 0);
    g_copyFailure = 0;
    CHECK(ScDmaOpen(7, &parameters, &context) == OS_EOK);
    __TestDirectMapFlags(context);
    __TestPersistentSyscalls(context);
    g_security.GlobalHandleID = 11;
    g_security.HandleTable = otherTable;
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_ENOENT);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable; g_copyFailure = 1;
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EINVALPARAMS);
    __TestHandlesDrain();
    CHECK(g_buffers == 0 && g_maps == 0 && g_contexts == 1 && g_live == 1);
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EINVALPARAMS && g_maps == 0);
    g_copyFailure = 0; g_engineFailure = 1;
    __TestHandlesDrain();
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOOM);
    __TestHandlesDrain();
    CHECK(g_live == 1);
    g_engineFailure = 0;
    map.Requirements = (struct DMAMapRequirements){2, 2};
    g_expectedMapRequirements = map.Requirements;
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    map.Requirements = (struct DMAMapRequirements){0};
    g_expectedMapRequirements = map.Requirements;
    CHECK(result.SegmentCount == 1 && result.InlineSegment.Address == g_segments[0].Address);
    CHECK(result.InlineSegment.Length == g_segments[0].Length);
    CHECK(ScResourceRelease(context) == OS_EBUSY);
    CHECK(ScResourceRelease(context) == OS_EBUSY);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 0, &count) == OS_EBUFFER && count == 1);
    CHECK(ScDmaGetSegments(result.Handle, &segment, 1, &count) == OS_EOK && segment.Address == g_segments[0].Address);
    g_security.GlobalHandleID = 11;
    g_security.HandleTable = otherTable;
    CHECK(ScResourceRelease(result.Handle) == OS_ENOENT);
    g_security.GlobalHandleID = 10;
    g_security.HandleTable = ownerTable;
    CHECK(__TestBeginTransfer(result.Handle, 0, 64) == OS_EOK);
    CHECK(ScResourceRelease(result.Handle) == OS_EBUSY);
    CHECK(ScShmAccessEnd(result.Handle, DMAAccessTransfer, 7) == OS_EOK);
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    CHECK(ScDmaAllocate(context, &requirements, &allocation) == OS_EOK);
    CHECK(allocation.Mapping.SegmentCount == 1);
    CHECK(allocation.Mapping.InlineSegment.Address == g_segments[0].Address);
    CHECK(allocation.Mapping.InlineSegment.Length == g_segments[0].Length);
    CHECK(ScResourceRelease(allocation.Mapping.Handle) == OS_EOK);
    CHECK(SHMDetach(&allocation.Buffer) == OS_EOK);
    // Multi-segment replies must initialize the unused inline bytes rather
    // than copy kernel stack contents to the caller.
    g_segmentCount = 2;
    memset(&result, 0xff, sizeof(result));
    CHECK(ScDmaMapShm(context, 99, &map, &result) == OS_EOK);
    CHECK(result.SegmentCount == 2);
    CHECK(result.InlineSegment.Address == 0 && result.InlineSegment.Length == 0);
    CHECK(ScResourceRelease(result.Handle) == OS_EOK);
    g_segmentCount = 1;
    CHECK(ScResourceRelease(context) == OS_EOK);
    CHECK(ScResourceRelease(context) == OS_ENOENT);
    __TestHandlesDrain();
    CHECK(g_live == 0 && g_maps == 0 && g_contexts == 0 && g_buffers == 0);
    __TestDmaCleanupOrder(&parameters, &map);
    __TestDmaAsyncLifecycle(&parameters, &map);
    __TestRangeSyscalls(&parameters);
    __TestHandleIndex(&parameters, &map);
    __TestCustomRecordCleanup();
    __TestCustomAccessDispatch();
    __TestFailureDiagnostics(&parameters, &map);
    DestroyHandleGrantsTable(g_security.HandleTable);
    DestroyHandleGrantsTable(otherTable);
    __TestHandlesFinish();
    puts("DMA syscalls: ownership, live-map close, copy rollback, translation and busy unmap passed");
    return 0;
}
