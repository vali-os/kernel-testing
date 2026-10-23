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

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t width)
{
    size_t value = 0;
    (void)io;
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
WriteDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t value,
    _In_ size_t width)
{
    (void)io;
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
SystemDebug(
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
AcquireDeviceIo(
    _In_ DeviceIo_t* io)
{
    abort();
}
oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* io)
{
    abort();
}
size_t
MemoryPageSize(
    void)
{
    abort();
}
oserr_t
IoctlDeviceEx(
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
    VirtioSplitQueue_t queue = { 0 };
    queue.Transport = transport;
    queue.QueueIndex = index;
    queue.QueueSize = 8;
    return __ProgramQueue(&queue);
}

int
main(
    void)
{
    VirtioPciTransport_t transport;
    DeviceIo_t io = { 0 };
    unsigned int pass;

    __Setup(&transport, &io);
    transport.MsixEnabled = 1;
    for (pass = 0; pass < 2; pass++) {
        CHECK(__Queue(&transport, 0) == OS_EOK);
        CHECK(__Queue(&transport, 1) == OS_EOK);
        CHECK(g_enabled[0] && g_enabled[1]);
        CHECK(g_vectors[0] == 0 && g_vectors[1] == 0);
        CHECK(VirtioPciFinishInitialization(&transport) == OS_EOK);
        CHECK(ReadDeviceIo(&io, FIELD(ConfigMsiXVector), 2) == 0);
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
    puts("Virtio MSI-X: shared routes, reset restoration, legacy mode and rejected routes passed");
    return 0;
}
