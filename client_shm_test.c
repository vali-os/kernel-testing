// Exercise USB client pools and IDE PIO with attachments that start unmapped.
#define __need_minmax
#define PRIuIN "zu"
#define PRIxIN "zx"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ddk/bytepool.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); abort(); } } while (0)

static bool g_failPool;
oserr_t __RealPoolCreate(void* buffer, long length, struct bytepool** pool);
#define bpool __RealPoolCreate
#include "../librt/libddk/utils/bytepool.c"
#undef bpool

oserr_t
bpool(
    _In_ void* buffer,
    _In_ long length,
    _Out_ struct bytepool** pool)
{
    return g_failPool ? OS_EOOM : __RealPoolCreate(buffer, length, pool);
}

#include "../librt/libddk/utils/bufferpool.c"
#include "../librt/libusb/functions.c"
#ifndef IDE_SOURCE
#define IDE_SOURCE "../modules/storage/ide/ide.c"
#endif
#include IDE_SOURCE

static _Alignas(4096) unsigned char g_bytes[8192];
static bool g_attached;
static bool g_mapped;
static bool g_failCreate;
static bool g_failAttach;
static bool g_failMap;
static bool g_deviceError;
static unsigned g_releases;
static unsigned g_maps;
static unsigned g_dataReads;
static unsigned g_dataWrites;
static unsigned g_access;
static size_t g_mapLength;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)level;
    (void)format;
}

oserr_t
SHMCreate(
    _In_ SHM_t* request,
    _Out_ OSHandle_t* handle)
{
    CHECK(request->Flags == SHM_COMMIT);
    CHECK(request->Conformity == OSMEMORYCONFORMITY_NONE);
    CHECK(request->Size == sizeof(g_bytes));
    if (g_failCreate) {
        return OS_EOOM;
    }
    CHECK(!g_attached);
    g_attached = g_mapped = true;
    *handle = (OSHandle_t){.ID = 7, .Payload = g_bytes};
    return OS_EOK;
}

oserr_t
SHMAttach(
    _In_ uuid_t id,
    _Out_ OSHandle_t* handle)
{
    CHECK(id == 7 && !g_attached);
    if (g_failAttach) {
        return OS_ENOENT;
    }
    g_attached = true;
    g_mapped = false;
    *handle = (OSHandle_t){.ID = id, .Payload = g_bytes};
    return OS_EOK;
}

size_t
SHMBufferCapacity(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 7 && g_attached);
    return sizeof(g_bytes);
}

size_t
SHMBufferLength(
    _In_ OSHandle_t* handle)
{
    CHECK(g_mapped);
    return SHMBufferCapacity(handle);
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 7 && g_attached && g_mapped);
    return g_bytes;
}

oserr_t
SHMMap(
    _InOut_ OSHandle_t* handle,
    _In_ size_t offset,
    _In_ size_t length,
    _In_ unsigned int access)
{
    CHECK(handle->ID == 7 && g_attached && !g_mapped);
    CHECK(offset == 0 && length <= sizeof(g_bytes));
    g_maps++;
    g_mapLength = length;
    g_access = access;
    if (g_failMap) {
        return OS_EOOM;
    }
    g_mapped = true;
    return OS_EOK;
}

void
OSHandleDestroy(
    _InOut_ OSHandle_t* handle)
{
    CHECK(handle->ID == 7 && g_attached);
    g_releases++;
    g_attached = g_mapped = false;
    memset(handle, 0, sizeof(*handle));
}

oserr_t
SHMGetSGTable(
    _In_ OSHandle_t* handle,
    _Out_ SHMSGTable_t* table,
    _In_ int count)
{
    // Suballocating CPU memory must not issue either physical-metrics syscall.
    CHECK(false);
    return OS_ENOTSUPPORTED;
}

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t width)
{
    CHECK(g_mapped);
    if (offset == IDE_REGISTER_STATUS) {
        return g_deviceError ? IDE_STATUS_ERR : IDE_STATUS_DRQ;
    }
    CHECK(offset == IDE_REGISTER_DATA && width == 2);
    g_dataReads++;
    return 0x5a5a;
}

oserr_t
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t value,
    _In_ size_t width)
{
    CHECK(g_mapped);
    if (offset == IDE_REGISTER_DATA && width == 2) {
        CHECK(value == 0x3c3c);
        g_dataWrites++;
    }
    return OS_EOK;
}

static void
__TestUsbPool(
    void)
{
    void* allocation;
    unsigned releases = g_releases;

    g_failCreate = true;
    CHECK(UsbInitialize() == OS_EOOM && !g_attached);
    g_failCreate = false;
    g_failPool = true;
    CHECK(UsbInitialize() == OS_EOOM && !g_attached);
    CHECK(g_releases == releases + 1);
    g_failPool = false;

    CHECK(UsbInitialize() == OS_EOK);
    CHECK(dma_pool_allocate(UsbRetrievePool(), 128, &allocation) == OS_EOK);
    memset(allocation, 0x4c, 128);
    CHECK(dma_pool_handle(UsbRetrievePool()) == 7);
    CHECK(g_bytes[dma_pool_offset(UsbRetrievePool(), allocation)] == 0x4c);
    CHECK(dma_pool_free(UsbRetrievePool(), allocation) == OS_EOK);
    UsbCleanup();
    CHECK(!g_attached && !UsbRetrievePool());
    CHECK(g_releases == releases + 2);
    UsbCleanup();
    CHECK(g_releases == releases + 2);
}

static void
__TestIde(
    void)
{
    BusDevice_t bus = {0};
    IdeController_t controller = {.Device = &bus};
    IdeDevice_t device = {.Controller = &controller};
    unsigned releases;
    unsigned maps;

    memset(g_bytes, 0, sizeof(g_bytes));
    CHECK(IdeDeviceRead(&device, 0, 2, 7, 17) == OS_EOK);
    CHECK(g_access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    CHECK(g_mapLength == 1041 && g_dataReads == 512);
    CHECK(g_bytes[16] == 0 && g_bytes[17] == 0x5a);
    CHECK(g_bytes[1040] == 0x5a && g_bytes[1041] == 0 && !g_attached);

    memset(g_bytes, 0x3c, sizeof(g_bytes));
    CHECK(IdeDeviceWrite(&device, 0, 2, 7, 17) == OS_EOK);
    CHECK(g_access == SHM_ACCESS_READ && g_dataWrites == 512 && !g_attached);

    releases = g_releases;
    maps = g_maps;
    g_failAttach = true;
    CHECK(IdeDeviceRead(&device, 0, 1, 7, 0) == OS_ENOENT);
    CHECK(g_releases == releases && g_maps == maps);
    g_failAttach = false;
    CHECK(IdeDeviceRead(&device, 0, 1, 7, SIZE_MAX) == OS_EINVALPARAMS);
    CHECK(IdeDeviceWrite(&device, 0, 1, 7, 8192) == OS_EINVALPARAMS);
    CHECK(g_releases == releases + 2 && g_maps == maps && !g_attached);
    CHECK(IdeDeviceRead(&device, 0, SIZE_MAX, 7, 0) == OS_ENOTSUPPORTED);
    CHECK(g_releases == releases + 2);

    g_failMap = true;
    CHECK(IdeDeviceRead(&device, 0, 1, 7, 0) == OS_EOOM);
    CHECK(g_releases == releases + 3 && !g_attached);
    g_failMap = false;
    g_deviceError = true;
    CHECK(IdeDeviceRead(&device, 0, 1, 7, 0) == OS_EDEVFAULT);
    CHECK(g_releases == releases + 4 && !g_attached);
    g_deviceError = false;
}

int
main(
    void)
{
    __TestUsbPool();
    __TestIde();
    puts("Client SHM: ordinary USB pool, no SG query, PIO CPU views, bounds and failure cleanup passed");
    return 0;
}
