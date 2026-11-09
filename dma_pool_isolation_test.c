// Real byte allocator and DMA pool: active hub buffers must not share cache lines.
#define __need_minmax
#include "../librt/libddk/utils/bytepool.c"
#include "../librt/libddk/utils/bufferpool.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static _Alignas(4096) uint8_t g_storage[8192];
static size_t g_line;
static oserr_t g_queryStatus;

oserr_t
OSSystemQuery(
    _In_ enum OSSystemQueryRequest request,
    _Out_ void* buffer,
    _In_ size_t size,
    _Out_ size_t* queried)
{
    CHECK(request == OSSYSTEMQUERY_CACHEINFO && size == sizeof(OSSystemCacheInfo_t));
    ((OSSystemCacheInfo_t*)buffer)->DataCacheLineSizeBytes = g_line;
    *queried = size;
    return g_queryStatus;
}

void*
SHMBuffer(_In_ OSHandle_t* handle)
{
    return g_storage;
}

size_t
SHMBufferLength(_In_ OSHandle_t* handle)
{
    return sizeof(g_storage);
}

void
SystemDebug(_In_ enum OSSysLogLevel level, _In_ const char* format, ...)
{
}

static void
__CheckActiveLines(
    _In_ void* buffer,
    _In_ size_t length)
{
    const uint8_t* bytes = buffer;
    size_t rounded = (length + g_line - 1) & ~(g_line - 1);
    size_t i;

    CHECK(((uintptr_t)buffer & (g_line - 1)) == 0);
    for (i = 0; i < rounded; i++) {
        CHECK(bytes[i] == 0xA5);
    }
}

static void
__TestOverlappingHubActivity(
    _In_ size_t line)
{
    OSHandle_t shm = { .ID = 7 };
    struct dma_pool* pool = NULL;
    void* interrupt;
    void* scan;
    void* reset;
    void* status;
    void* extra[128];
    size_t count;
    size_t i;
    size_t interruptSize;
    size_t scanSize;
    size_t resetSize;

    g_line = line;
    memset(g_storage, 0, sizeof(g_storage));
    CHECK(dma_pool_create(&shm, &pool) == OS_EOK);
    CHECK(dma_pool_allocate(pool, 256, &interrupt) == OS_EOK);
    CHECK(dma_pool_allocate(pool, 12, &scan) == OS_EOK);
    CHECK(dma_pool_allocate(pool, 8, &reset) == OS_EOK);
    interruptSize = (256 + line - 1) & ~(line - 1);
    scanSize = (12 + line - 1) & ~(line - 1);
    resetSize = (8 + line - 1) & ~(line - 1);
    memset(interrupt, 0xA5, interruptSize);
    memset(scan, 0xA5, scanSize);
    memset(reset, 0xA5, resetSize);
    CHECK((uint8_t*)interrupt + interruptSize <= (uint8_t*)scan ||
        (uint8_t*)scan + scanSize <= (uint8_t*)interrupt);
    CHECK((uint8_t*)scan + scanSize <= (uint8_t*)reset ||
        (uint8_t*)reset + resetSize <= (uint8_t*)scan);
    CHECK((uint8_t*)reset + resetSize <= (uint8_t*)interrupt ||
        (uint8_t*)interrupt + interruptSize <= (uint8_t*)reset);
    CHECK(dma_pool_handle(pool) == 7);
    CHECK(g_storage + dma_pool_offset(pool, scan) == scan);

    // The event worker allocates/frees status requests while scan/reset and
    // the interrupt pipe are active. Check the whole reserved lines, not just data.
    for (i = 0; i < 100; i++) {
        CHECK(dma_pool_allocate(pool, 12, &status) == OS_EOK);
        memset(status, 0x5A, 12);
        CHECK(dma_pool_free(pool, status) == OS_EOK);
        __CheckActiveLines(interrupt, 256);
        __CheckActiveLines(scan, 12);
        __CheckActiveLines(reset, 8);
    }
    CHECK(dma_pool_allocate(pool, SIZE_MAX, &status) == OS_EOOM);
    CHECK(dma_pool_allocate(pool, 0, &status) == OS_EINVALPARAMS);
    count = 0;
    while (count < 128 && dma_pool_allocate(pool, 64, &extra[count]) == OS_EOK) {
        count++;
    }
    CHECK(count < 128);
    for (i = 0; i < count; i++) {
        CHECK(dma_pool_free(pool, extra[i]) == OS_EOK);
    }
    __CheckActiveLines(interrupt, 256);
    __CheckActiveLines(scan, 12);
    __CheckActiveLines(reset, 8);
    CHECK(dma_pool_free(pool, scan) == OS_EOK);
    CHECK(dma_pool_free(pool, reset) == OS_EOK);
    CHECK(dma_pool_free(pool, interrupt) == OS_EOK);
    CHECK(dma_pool_allocate(pool, 4096, &status) == OS_EOK);
    CHECK(dma_pool_free(pool, status) == OS_EOK);
    CHECK(dma_pool_destroy(pool) == OS_EOK);
}

int
main(void)
{
    OSHandle_t shm = { .ID = 7 };
    struct dma_pool* pool = NULL;

    g_queryStatus = OS_ENOTSUPPORTED;
    CHECK(dma_pool_create(&shm, &pool) == OS_ENOTSUPPORTED && pool == NULL);
    g_queryStatus = OS_EOK;
    g_line = 0;
    CHECK(dma_pool_create(&shm, &pool) == OS_EINVALPARAMS);
    g_line = 63;
    CHECK(dma_pool_create(&shm, &pool) == OS_EINVALPARAMS);
    __TestOverlappingHubActivity(32);
    __TestOverlappingHubActivity(64);
    __TestOverlappingHubActivity(128);
    __TestOverlappingHubActivity(256);
    puts("DMA pool: isolated hub scan/reset/interrupt buffers, allocator metadata and reuse passed");
    return 0;
}
