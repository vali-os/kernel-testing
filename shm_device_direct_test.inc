#include <unistd.h>

// Captures what the kernel code under test logs. In host builds WARNING
// prints to stdout, ERROR prints to stderr and TRACE prints nothing, so an
// empty capture means nothing above trace level was logged.
struct __LogCapture {
    int  Pipe[2];
    int  SavedOutput;
    int  SavedError;
    char Text[4096];
};

static void
__LogCaptureBegin(
    _Out_ struct __LogCapture* capture)
{
    fflush(stdout);
    fflush(stderr);
    assert(pipe(capture->Pipe) == 0);
    capture->SavedOutput = dup(STDOUT_FILENO);
    capture->SavedError = dup(STDERR_FILENO);
    assert(capture->SavedOutput >= 0 && capture->SavedError >= 0);
    assert(dup2(capture->Pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
    assert(dup2(capture->Pipe[1], STDERR_FILENO) == STDERR_FILENO);
    assert(close(capture->Pipe[1]) == 0);
}

// Restores stdout and stderr and reads the captured text. The text is printed
// again so a failing case still shows what was logged.
static const char*
__LogCaptureEnd(
    _InOut_ struct __LogCapture* capture)
{
    size_t  length = 0;
    ssize_t bytesRead;

    fflush(stdout);
    fflush(stderr);
    assert(dup2(capture->SavedOutput, STDOUT_FILENO) == STDOUT_FILENO);
    assert(dup2(capture->SavedError, STDERR_FILENO) == STDERR_FILENO);
    assert(close(capture->SavedOutput) == 0 && close(capture->SavedError) == 0);
    do {
        bytesRead = read(capture->Pipe[0], &capture->Text[length], sizeof(capture->Text) - 1 - length);
        assert(bytesRead >= 0);
        length += (size_t)bytesRead;
    } while (bytesRead > 0 && length < sizeof(capture->Text) - 1);
    assert(close(capture->Pipe[0]) == 0);
    capture->Text[length] = '\0';
    fputs(capture->Text, stdout);
    return capture->Text;
}

// Counts how often text occurs in the captured log.
static unsigned int
__CountLogText(
    _In_ const char* log,
    _In_ const char* text)
{
    unsigned int count = 0;

    for (const char* found = strstr(log, text); found != NULL; found = strstr(found + 1, text)) {
        count++;
    }
    return count;
}

// Direct-only mappings use the original committed pages even when a bounce
// pool exists. Failures must leave that pool, source views and references alone.
// A refusal because the device cannot reach the pages (OS_ENOENT) or because
// they break the cache or alignment rules (OS_ENOTSUPPORTED) is an expected
// answer. Drivers fall back to a bounce mapping on it, so it is not logged
// above trace level.
static void
__ExpectDirectFailure(
    _In_ struct SHMDeviceContext* context,
    _In_ size_t                   offset,
    _In_ size_t                   length,
    _In_ enum DMADirection        direction,
    _In_ oserr_t                  expected)
{
    struct SHMDeviceMapping* mapping = NULL;
    struct __LogCapture capture;
    unsigned int live = atomic_load(&g_liveAllocations);
    size_t freePages = MemoryStackFreeBlockCount(&g_pool.Stack);
    int views = g_mapCalls;
    const char* log;

    __LogCaptureBegin(&capture);
    assert(__TestMap(context, 0x20, offset, length, direction, SHMDeviceMappingStreamingDirect, NULL, &mapping) == expected);
    log = __LogCaptureEnd(&capture);
    if (expected == OS_ENOENT || expected == OS_ENOTSUPPORTED) {
        assert(log[0] == '\0');
    }
    assert(mapping == NULL && g_bufferReferences == 1 && !g_bufferLocked);
    assert(atomic_load(&g_liveAllocations) == live && g_liveViews == 0);
    assert(g_mapCalls == views && g_bounceMemory == NULL);
    assert(MemoryStackFreeBlockCount(&g_pool.Stack) == freePages && g_poolLockHeld == 0);
}

static void
__TestDirectMappings(void)
{
    struct SHMDeviceRange range = { 0x700000, 0x100000000ULL, 0x10000 };
    const paddr_t pages[] = { 0x700000, 0x702000 };
    const paddr_t unreachable[] = { 0x700000, 0x800000 };
    const paddr_t missing[] = { 0x700000, 0 };
    struct SHMDeviceContext* context;
    struct SHMDeviceContext* limited;
    struct SHMDeviceMapping* mapping = NULL;
    const struct DMASegment* segments;
    uint32_t count;
    unsigned int allocations;
    unsigned int i;
    int views;
    enum DMADirection direction;
    enum SHMDeviceCachePolicy policy;
    struct __LogCapture capture;
    const char* log;

    for (policy = SHMDeviceCacheNonCoherent; policy <= SHMDeviceCacheCoherent; policy++) {
        context = __CreateContext(&range, 1, policy, UINT64_MAX);
        __CreateBuffer(pages, 2, 0, 0x2000, 0);
        for (direction = DMAToDevice; direction <= DMABidirectional; direction++) {
            assert(__TestMap(context, 0x20, 0xf00, 0x200, direction, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
            segments = __TestMappingSegments(mapping, &count);
            assert(count == 2 && segments[0].Address == 0x100000f00ULL);
            assert(segments[0].Length == 0x100 && segments[1].Address == 0x100002000ULL);
            assert(segments[1].Length == 0x100 && g_bounceMemory == NULL && g_liveViews == 0);
            allocations = g_allocationCalls;
            views = g_mapCalls;
            assert(__TestMap(context, 0x20, 0, 64, direction, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EBUSY);
            assert(__TestSyncForDevice(mapping, 0, 0x200, NULL) == OS_EOK);
            assert(__TestUnmap(&mapping) == OS_EBUSY);
            assert(__TestSyncForCpu(mapping, 0x200) == OS_EOK);
            if (policy == SHMDeviceCacheNonCoherent) {
                assert(g_cacheCallCount == (direction == DMAToDevice ? 2 : 4));
                assert(g_cacheCalls[0].Address == 0x700f00 && g_cacheCalls[1].Address == 0x702000);
                assert(g_cacheCalls[0].Operation == (direction == DMAToDevice ? __CacheClean : __CacheCleanInvalidate));
                if (direction != DMAToDevice) {
                    assert(g_cacheCalls[2].Operation == __CacheInvalidate);
                }
            } else {
                assert(g_cacheCallCount == 0);
            }
            g_cacheCallCount = 0;
            assert(g_allocationCalls == allocations && g_mapCalls == views);
            assert(__TestMappingSegments(mapping, &count) == segments && count == 2);
            assert(__TestUnmap(&mapping) == OS_EOK);

            // Unaligned device reads are safe; writes need exclusive cache lines
            // only on noncoherent cached memory.
            if (policy == SHMDeviceCacheNonCoherent && direction != DMAToDevice) {
                // Cache rejection precedes SHM record allocation. An unrelated
                // heap failure must not hide this direct-only validation error.
                g_failAtCall = g_allocationCalls + 2;
                __ExpectDirectFailure(context, 1, 64, direction, OS_ENOTSUPPORTED);
                g_failAtCall = 0;
                __ExpectDirectFailure(context, 0, 63, direction, OS_ENOTSUPPORTED);
            } else {
                assert(__TestMap(context, 0x20, 1, 63, direction, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
                assert(g_bounceMemory == NULL);
                assert(__TestUnmap(&mapping) == OS_EOK);
            }
        }
        for (i = 1; i <= 3; i++) {
            g_failAtCall = g_allocationCalls + i;
            __ExpectDirectFailure(context, 0, 64, DMAToDevice, OS_EOOM);
            g_failAtCall = 0;
        }
        __ExpectDirectFailure(context, 0, 0, DMAToDevice, OS_EINVALPARAMS);
        __ExpectDirectFailure(context, 1, SIZE_MAX, DMAToDevice, OS_EINVALPARAMS);
        __ExpectDirectFailure(context, 0, 64, (enum DMADirection)99, OS_EINVALPARAMS);
        assert(DestroyHandle(0x20) == OS_EOK);

        // The first page translates, but a later one cannot. A reachable pool
        // must not silently substitute storage for this request.
        __CreateBuffer(unreachable, 2, 0, 0x2000, 0);
        __ExpectDirectFailure(context, 0, 0x2000, DMAToDevice, OS_ENOENT);
        if (policy == SHMDeviceCacheNonCoherent) {
            // An unreachable cache-sharing range still reports reachability,
            // even though direct DMA would also violate the cache-line rule.
            g_failAtCall = g_allocationCalls + 2;
            __ExpectDirectFailure(context, 1, 0x1fff, DMAFromDevice, OS_ENOENT);
            g_failAtCall = 0;
        }
        // The first bounce mapping of a context is logged once, with the
        // reason. It is what tells an operator that the device bounces, since
        // the refusals before it are only traced. Later ones are not logged.
        __LogCaptureBegin(&capture);
        assert(__TestMap(context, 0x20, 0, 0x2000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
        log = __LogCaptureEnd(&capture);
        assert(__CountLogText(log, "SHMDeviceMappingPrepare: context") == 1);
        assert(__CountLogText(log, "bounces: range is not reachable by the device, limit 0xffffffffffffffff") == 1);
        assert(g_bounceMemory != NULL);
        assert(__TestUnmap(&mapping) == OS_EOK);
        __LogCaptureBegin(&capture);
        assert(__TestMap(context, 0x20, 0, 0x2000, DMAToDevice, SHMDeviceMappingStreamingBounce, NULL, &mapping) == OS_EOK);
        log = __LogCaptureEnd(&capture);
        assert(log[0] == '\0');
        assert(g_bounceMemory != NULL);
        assert(__TestUnmap(&mapping) == OS_EOK);
        assert(DestroyHandle(0x20) == OS_EOK);
        __CreateBuffer(missing, 2, 0, 0x2000, 0);
        __ExpectDirectFailure(context, 0, 0x2000, DMAToDevice, OS_EINCOMPLETE);
        assert(DestroyHandle(0x20) == OS_EOK);
        SHMDeviceContextRelease(&context);
    }

    context = __CreateContext(&range, 1, SHMDeviceCacheNonCoherent, UINT64_MAX);
    __CreateBuffer(pages, 2, 0, 0x2000, SHM_COMMIT);
    g_buffer->Allocation.Owner = &g_machine.PhysicalMemory;
    g_buffer->Allocation.Cached = false;
    assert(__TestMap(context, 0x20, 1, 63, DMAFromDevice, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    assert(__TestSyncForDevice(mapping, 0, 63, NULL) == OS_EOK);
    assert(__TestSyncForCpu(mapping, 63) == OS_EOK);
    __ExpectCacheCalls(NULL, 0);
    assert(g_bounceMemory == NULL && g_liveViews == 0);
    assert(__TestUnmap(&mapping) == OS_EOK);
    assert(DestroyHandle(0x20) == OS_EOK);
    SHMDeviceContextRelease(&context);

    context = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT64_MAX);
    limited = __CreateContext(&range, 1, SHMDeviceCacheCoherent, UINT32_MAX);
    __CreateBuffer(pages, 2, 0, 0x2000, 0);
    __ExpectDirectFailure(limited, 0, 64, DMAToDevice, OS_ENOENT);
    SHMDeviceContextRelease(&limited);
    // Direct mapping must work without the device pool and outlive both caller
    // references until unmap.
    g_pool.Length = 0;
    assert(__TestMap(context, 0x20, 0, 0x2000, DMABidirectional, SHMDeviceMappingStreamingDirect, NULL, &mapping) == OS_EOK);
    SHMDeviceContextRelease(&context);
    assert(DestroyHandle(0x20) == OS_EINCOMPLETE);
    assert(__TestSyncForDevice(mapping, 0, 64, NULL) == OS_EOK);
    assert(__TestSyncForCpu(mapping, 0) == OS_EOK);
    assert(__TestUnmap(&mapping) == OS_EOK && g_buffer == NULL);
    __UsePool(0x708000, false);
}
