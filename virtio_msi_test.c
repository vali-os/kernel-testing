// Exercise production Virtio routing against a banked common-register model.
#define __need_minmax
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER) && (defined(__i386__) || defined(__x86_64__))
#include <immintrin.h>
// The host test does not use the driver build's precompiled intrinsic headers.
void _ReadBarrier(void);
void _WriteBarrier(void);
void _ReadWriteBarrier(void);
#endif
#include "../modules/virtio/common/pci.c"
#include "../modules/virtio/common/queue.c"
#include "virtio_dma_model.h"

#define CHECK(x) do { if (!(x)) { printf( "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define FIELD(name) offsetof(VirtioPciCommonConfiguration_t, name)

static uint8_t g_registers[64];
static uint16_t g_vectors[2];
static uint16_t g_enabled[2];
static uint16_t g_selected;
static int g_rejectQueue;
static int g_rejectConfig;
static size_t g_failWrite;
static unsigned int g_vectorWrites;
static DeviceIo_t* g_releaseFailure;
static unsigned int g_barReleases;
static unsigned int g_registerReads;
static bool g_captureIoAccesses;
static size_t g_ioOffsets[2];
static size_t g_ioWidths[2];
static size_t g_ioValues[2];
static bool g_ioWrites[2];
static unsigned int g_ioAccessCount;

static void
__RecordDeviceIoAccess(
    _In_ size_t offset,
    _In_ size_t width,
    _In_ size_t value,
    _In_ bool   write)
{
    if (!g_captureIoAccesses) {
        return;
    }

    CHECK(g_ioAccessCount < 2);
    g_ioOffsets[g_ioAccessCount] = offset;
    g_ioWidths[g_ioAccessCount] = width;
    g_ioValues[g_ioAccessCount] = value;
    g_ioWrites[g_ioAccessCount] = write;
    g_ioAccessCount++;
}

size_t
DeviceIoRead(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t width)
{
    size_t value = 0;
    (void)io;
    g_registerReads++;
    __RecordDeviceIoAccess(offset, width, 0, false);
    if (offset == FIELD(QueueMsiXVector)) {
        return g_vectors[g_selected];
    }
    if (offset == FIELD(QueueEnable)) {
        return g_enabled[g_selected];
    }
    memcpy(&value, &g_registers[offset], width);
    return value;
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t value,
    _In_ size_t width)
{
    (void)io;
    __RecordDeviceIoAccess(offset, width, value, true);
    if (offset == g_failWrite) {
        return OS_EUNKNOWN;
    }
    if (offset == FIELD(QueueSelect)) {
        CHECK(value < 2);
        g_selected = (uint16_t)value;
    } else if (offset == FIELD(QueueMsiXVector)) {
        CHECK(width == 2 && !g_enabled[g_selected]);
        g_vectors[g_selected] = g_rejectQueue ? UINT16_MAX : (uint16_t)value;
        g_vectorWrites++;
        return OS_EOK;
    } else if (offset == FIELD(QueueEnable)) {
        g_enabled[g_selected] = (uint16_t)value;
    } else if (offset == FIELD(ConfigMsiXVector)) {
        CHECK(width == 2 && !(g_registers[FIELD(DeviceStatus)] & VIRTIO_STATUS_DRIVER_OK));
        if (g_rejectConfig) {
            value = UINT16_MAX;
        }
        g_vectorWrites++;
    } else if (offset == FIELD(DeviceStatus) && value == 0) {
        g_vectors[0] = g_vectors[1] = UINT16_MAX;
        g_enabled[0] = g_enabled[1] = 0;
        memset(&g_registers[FIELD(ConfigMsiXVector)], 0xff, 2);
    }
    memcpy(&g_registers[offset], &value, width);
    return OS_EOK;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)level;
    (void)format;
}
void
usched_job_yield(
    void)
{
}
void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

// These entry points belong to transport discovery and DMA allocation, which
// this register-level test does not invoke. Fail if the test starts using them.
void
spinlock_init(
    _In_ spinlock_t* lock)
{
    abort();
}
oserr_t
OSDeviceIOCtl(
    _In_ uuid_t id,
    _In_ enum OSIOCtlRequest request,
    _In_ void* buffer,
    _In_ size_t length)
{
    abort();
}
oserr_t
DeviceIoAcquire(
    _In_ DeviceIo_t* io)
{
    abort();
}
oserr_t
DeviceIoRelease(
    _In_ DeviceIo_t* io)
{
    if (io == g_releaseFailure) {
        return OS_EUNKNOWN;
    }
    g_barReleases++;
    return OS_EOK;
}

size_t
MemoryPageSize(
    void)
{
    abort();
}
oserr_t
DeviceIoctlEx(
    _In_ uuid_t id,
    _In_ int direction,
    _In_ unsigned int offset,
    _In_ size_t* value,
    _In_ size_t width)
{
    abort();
}
oserr_t
SHMCreate(
    _In_ SHM_t* shm,
    _In_ OSHandle_t* handle)
{
    abort();
}
oserr_t
SHMGetSGTable(
    _In_ OSHandle_t* handle,
    _In_ SHMSGTable_t* table,
    _In_ int count)
{
    abort();
}
void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    abort();
}
void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    abort();
}
void
ReadVolatileMemory(
    _In_ const volatile void* source,
    _In_ volatile void* target,
    _In_ size_t size)
{
    abort();
}
void
WriteVolatileMemory(
    _In_ volatile void* target,
    _In_ void* source,
    _In_ size_t size)
{
    abort();
}

static void
__Setup(
    _In_ VirtioPciTransport_t* transport,
    _In_ DeviceIo_t* io)
{
    memset(g_registers, 0, sizeof(g_registers));
    memset(transport, 0, sizeof(*transport));
    g_vectors[0] = g_vectors[1] = UINT16_MAX;
    g_enabled[0] = g_enabled[1] = 0;
    g_selected = 0;
    g_rejectQueue = g_rejectConfig = 0;
    g_failWrite = SIZE_MAX;
    g_vectorWrites = 0;
    memset(&g_registers[FIELD(ConfigMsiXVector)], 0xff, 2);
    transport->Regions[VIRTIO_PCI_CAP_COMMON_CFG - 1] = (VirtioPciRegion_t) { io, 0, sizeof(g_registers) };
    transport->Regions[VIRTIO_PCI_CAP_NOTIFY_CFG - 1] = (VirtioPciRegion_t) { io, 0, 4 };
}

static oserr_t
__Queue(
    _In_ VirtioPciTransport_t* transport,
    _In_ uint16_t index)
{
    struct DMABuffer buffer = { .Count = 1, .Segments = {{0x100000000ULL, 4096}} };
    VirtioSplitQueue_t queue = {
        .DescriptorRegion = &buffer, .AvailableRegion = &buffer, .UsedRegion = &buffer
    };
    queue.Transport = transport;
    queue.QueueIndex = index;
    queue.QueueSize = 8;
    return __ProgramQueue(&queue);
}

static void
__TestTransportCleanup(void)
{
    BusDevice_t device = {0};
    VirtioPciTransport_t transport = { .Device = &device, .AcquiredBars = 3 };
    unsigned int reads;

    CHECK(DMAOpen(7, UINT64_MAX, &transport.DmaContext) == OS_EOK);
    g_releaseFailure = &device.IoSpaces[1];
    CHECK(VirtioPciTransportDestroy(&transport) == OS_EUNKNOWN);
    CHECK(transport.Closing && transport.DmaContext == NULL);
    CHECK(transport.AcquiredBars == 2 && g_barReleases == 1);
    // The block initialization error path reports FAILED after transport
    // rollback. A partially closed transport must not touch its released BAR.
    transport.Regions[VIRTIO_PCI_CAP_COMMON_CFG - 1] = (VirtioPciRegion_t) {
        &device.IoSpaces[0], 0, sizeof(g_registers)
    };
    reads = g_registerReads;
    VirtioPciSetFailed(&transport);
    CHECK(g_registerReads == reads);
    CHECK(VirtioPciTransportDestroy(&transport) == OS_EUNKNOWN);
    CHECK(g_barReleases == 1);
    g_releaseFailure = NULL;
    CHECK(VirtioPciTransportDestroy(&transport) == OS_EOK);
    CHECK(transport.Device == NULL && g_barReleases == 2);
}

static void
__TestSplitDeviceIo(void)
{
    DeviceIo_t io = { 0 };
    VirtioPciRegion_t region = {
        .IoSpace = &io,
        .Offset = 40,
        .Length = 8
    };
    uint64_t expected = 0x89abcdef01234008ULL;
    uint32_t low = (uint32_t)expected;
    uint32_t high = (uint32_t)(expected >> 32);
    uint64_t value;
    oserr_t  oserr;

    memcpy(&g_registers[region.Offset], &low, sizeof(low));
    memcpy(&g_registers[region.Offset + sizeof(low)], &high, sizeof(high));
    g_ioAccessCount = 0;
    g_captureIoAccesses = true;
    oserr = VirtioPciRegionRead(&region, 0, sizeof(value), &value);
    g_captureIoAccesses = false;
    CHECK(oserr == OS_EOK && value == expected);
    CHECK(g_ioAccessCount == 2);
    CHECK(g_ioOffsets[0] == region.Offset && g_ioOffsets[1] == region.Offset + 4);
    CHECK(g_ioWidths[0] == 4 && g_ioWidths[1] == 4);
    CHECK(!g_ioWrites[0] && !g_ioWrites[1]);

    g_ioAccessCount = 0;
    g_captureIoAccesses = true;
    oserr = VirtioPciRegionWrite(&region, 0, expected, sizeof(value));
    g_captureIoAccesses = false;
    CHECK(oserr == OS_EOK && g_ioAccessCount == 2);
    CHECK(g_ioOffsets[0] == region.Offset && g_ioOffsets[1] == region.Offset + 4);
    CHECK(g_ioWidths[0] == 4 && g_ioWidths[1] == 4);
    CHECK(g_ioValues[0] == low && g_ioValues[1] == high);
    CHECK(g_ioWrites[0] && g_ioWrites[1]);

    g_failWrite = region.Offset + 4;
    g_ioAccessCount = 0;
    g_captureIoAccesses = true;
    oserr = VirtioPciRegionWrite(&region, 0, expected, sizeof(value));
    g_captureIoAccesses = false;
    g_failWrite = SIZE_MAX;
    CHECK(oserr == OS_EUNKNOWN && g_ioAccessCount == 2);
    CHECK(g_ioOffsets[0] == region.Offset && g_ioOffsets[1] == region.Offset + 4);
}

int
main(
    void)
{
    VirtioPciTransport_t transport;
    DeviceIo_t io = { 0 };
    unsigned int pass;

    __TestTransportCleanup();
    __TestSplitDeviceIo();
    __Setup(&transport, &io);
    transport.MsixEnabled = 1;
    for (pass = 0; pass < 2; pass++) {
        CHECK(__Queue(&transport, 0) == OS_EOK);
        CHECK(__Queue(&transport, 1) == OS_EOK);
        CHECK(g_enabled[0] && g_enabled[1]);
        CHECK(g_vectors[0] == 0 && g_vectors[1] == 0);
        CHECK(VirtioPciFinishInitialization(&transport) == OS_EOK);
        CHECK(DeviceIoRead(&io, FIELD(ConfigMsiXVector), 2) == 0);
        CHECK(g_registers[FIELD(DeviceStatus)] & VIRTIO_STATUS_DRIVER_OK);
        CHECK(VirtioPciReset(&transport) == OS_EOK);
        CHECK(transport.MsixEnabled && g_vectors[0] == UINT16_MAX);
    }
    CHECK(g_vectorWrites == 6);

    __Setup(&transport, &io);
    CHECK(__Queue(&transport, 0) == OS_EOK);
    CHECK(VirtioPciFinishInitialization(&transport) == OS_EOK);
    CHECK(g_vectorWrites == 0 && g_vectors[0] == UINT16_MAX);

    __Setup(&transport, &io);
    transport.MsixEnabled = 1;
    g_rejectQueue = 1;
    CHECK(__Queue(&transport, 0) == OS_EDEVFAULT);
    CHECK(!g_enabled[0] && transport.ActiveQueues == 0);
    g_rejectQueue = 0;
    g_failWrite = FIELD(QueueMsiXVector);
    CHECK(__Queue(&transport, 0) == OS_EUNKNOWN && !g_enabled[0]);
    g_failWrite = SIZE_MAX;
    g_rejectConfig = 1;
    CHECK(VirtioPciFinishInitialization(&transport) == OS_EDEVFAULT);
    CHECK(!(g_registers[FIELD(DeviceStatus)] & VIRTIO_STATUS_DRIVER_OK));
    g_rejectConfig = 0;
    g_failWrite = FIELD(ConfigMsiXVector);
    CHECK(VirtioPciFinishInitialization(&transport) == OS_EUNKNOWN);
    CHECK(!(g_registers[FIELD(DeviceStatus)] & VIRTIO_STATUS_DRIVER_OK));
    puts("Virtio PCI: split 64-bit I/O, MSI-X routes, reset and legacy mode passed");
    return 0;
}

oserr_t SHMAttach(uuid_t id, OSHandle_t* handle) { abort(); }
size_t SHMBufferCapacity(OSHandle_t* handle) { abort(); }
oserr_t SHMRelease(OSHandle_t* handle) { abort(); }
