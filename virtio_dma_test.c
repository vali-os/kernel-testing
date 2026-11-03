/** Exercise real split rings and block requests with translated, bounced DMA. */
#define __need_minmax
#define PRIuIN "zu"
#define PRIxIN "zx"
#include "../modules/virtio/common/queue.c"
#include "../modules/virtio/blk/request.c"
int read(int descriptor, void* data, unsigned int length);
struct ioset_event {
    unsigned int events;
    union {
        void* context;
    } data;
};
#define IOSETSYN 1
#ifndef VIRTIO_BLK_MAIN_SOURCE
#define VIRTIO_BLK_MAIN_SOURCE "../modules/virtio/blk/main.c"
#endif
#include VIRTIO_BLK_MAIN_SOURCE
#include "../librt/libds/list.c"
#include "virtio_dma_model.h"
#include <stdio.h>
#include <stdarg.h>
#define CHECK(x) DMA_CHECK(x)

static BusDevice_t g_bus = {.Base.Id = 7};
static unsigned g_replies;
static oserr_t g_result;
static size_t g_completed;
static bool g_resetFailure;
static bool g_notifyFailure;
static bool g_cancelMonitor;
static unsigned g_monitorSleeps;
static uint16_t g_queueEnabled;
static uint64_t g_descriptorAddress;
static uint64_t g_usedAddress;
static uint64_t g_availableAddress;
static unsigned g_lockDepth;
static VirtioBlkDevice_t* g_destroyOnLock;
static unsigned g_eventReads;

int
read(
    _In_ int descriptor,
    _Out_ void* data,
    _In_ unsigned int length)
{
    CHECK(descriptor == 42);
    memset(data, 0, length);
    g_eventReads++;
    return length;
}

oserr_t
VirtioBlkDeviceHandleConfigurationChange(
    _InOut_ VirtioBlkDevice_t* device)
{
    CHECK(0);
    return OS_EUNKNOWN;
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
spinlock_init(
    _InOut_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_acquire(
    _InOut_ spinlock_t* lock)
{
    (void)lock;
}
void
spinlock_release(
    _InOut_ spinlock_t* lock)
{
    (void)lock;
}
void
ReadVolatileMemory(
    _In_ const volatile void* source,
    _Out_ volatile void* target,
    _In_ size_t length)
{
    memcpy((void*)target, (const void*)source, length);
}
void
WriteVolatileMemory(
    _Out_ volatile void* target,
    _InOut_ void* source,
    _In_ size_t length)
{
    memcpy((void*)target, source, length);
}
oserr_t
VirtioPciRegionRead(
    _In_ const VirtioPciRegion_t* region,
    _In_ uint32_t offset,
    _In_ size_t width,
    _InOut_ uint64_t* value)
{
    (void)region;
    (void)width;
    *value = 0;
    if (offset == offsetof(VirtioPciCommonConfiguration_t, NumQueues)) {
        *value = 1;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueSize)) {
        *value = 8;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueEnable)) {
        *value = g_queueEnabled;
    }
    return OS_EOK;
}
oserr_t
VirtioPciRegionWrite(
    _In_ const VirtioPciRegion_t* region,
    _In_ uint32_t offset,
    _In_ uint64_t value,
    _In_ size_t width)
{
    (void)width;
    if (region->Length == 32) {
        return g_notifyFailure ? OS_EUNKNOWN : OS_EOK;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueEnable)) {
        g_queueEnabled = value;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueDesc)) {
        g_descriptorAddress = value;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueDriver)) {
        g_availableAddress = value;
    }
    if (offset == offsetof(VirtioPciCommonConfiguration_t, QueueDevice)) {
        g_usedAddress = value;
    }
    return OS_EOK;
}
oserr_t
VirtioPciReset(
    _InOut_ VirtioPciTransport_t* transport)
{
    if (g_resetFailure) {
        return OS_ETIMEOUT;
    }
    transport->ResetGeneration++;
    g_queueEnabled = 0;
    return OS_EOK;
}
oserr_t
VirtioBlkRecoverDevice(
    _InOut_ VirtioBlkDevice_t* device)
{
    return VirtioBlkResetRequestQueue(device);
}
oserr_t
VirtioBlkDeviceDestroy(
    _InOut_ VirtioBlkDevice_t* device)
{
    (void)device;
    CHECK(0);
    return OS_EUNKNOWN;
}
void
gracht_server_defer_message(
    _InOut_ struct gracht_message* message,
    _Out_ struct gracht_message* out)
{
    memcpy(out, message, GRACHT_MESSAGE_DEFERRABLE_SIZE(message));
}
int
ctt_storage_transfer_response(
    _InOut_ struct gracht_message* message,
    _In_ const oserr_t status,
    _In_ const size_t sectors)
{
    (void)message;
    g_replies++;
    g_result = status;
    g_completed = sectors;
    return 0;
}
void
usched_mtx_lock(
    _InOut_ struct usched_mtx* lock)
{
    (void)lock;
    CHECK(g_lockDepth++ == 0);
    // Complete yielding teardown before a waiting event obtains the lock.
    if (g_destroyOnLock != NULL) {
        list_remove(&g_devices, &g_destroyOnLock->Header);
        free(g_destroyOnLock);
        g_destroyOnLock = NULL;
    }
}
void
usched_mtx_unlock(
    _InOut_ struct usched_mtx* lock)
{
    (void)lock;
    CHECK(--g_lockDepth == 0);
}
bool
usched_is_cancelled(
    _In_ const void* token)
{
    (void)token;
    return g_cancelMonitor;
}
int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    (void)until;
    // A one-shot kernel failure clears while no new client request or device
    // interrupt occurs. The real monitor must still deliver the pending reply.
    g_dmaFailEnd = 0;
    g_cancelMonitor = true;
    g_monitorSleeps++;
    return 0;
}

static void
__Setup(
    _InOut_ VirtioBlkDevice_t* device)
{
    memset(device, 0, sizeof(*device));
    device->Transport.Device = &g_bus;
    device->Transport.DriverFeatures = VIRTIO_F_VERSION_1;
    device->Transport.Regions[VIRTIO_PCI_CAP_COMMON_CFG - 1].Length = 64;
    device->Transport.Regions[VIRTIO_PCI_CAP_NOTIFY_CFG - 1].Length = 32;
    device->Descriptor.SectorSize = 512;
    device->Descriptor.SectorCount = 1000;
    device->SegmentMax = 2;
    device->SizeMax = 512;
    g_queueEnabled = 0;
    CHECK(DMAOpen(7, UINT64_MAX, &device->Transport.DmaContext) == OS_EOK);
    CHECK(VirtioSplitQueueCreate(&device->Transport, 0, 8, &device->RequestQueue) == OS_EOK);
    CHECK(g_descriptorAddress > UINT32_MAX && g_usedAddress > UINT32_MAX &&
          g_availableAddress > UINT32_MAX);
}

static void
__Teardown(
    _InOut_ VirtioBlkDevice_t* device)
{
    CHECK(VirtioBlkResetRequestQueue(device) == OS_EOK);
    CHECK(!device->RequestQueue && !device->CleanupRequests);
    CHECK(device->Transport.ActiveQueues == 0);
    CHECK(DMAClose(&device->Transport.DmaContext) == OS_EOK);
    CHECK(g_dmaLiveBuffers == 0 && g_dmaLiveMappings == 0);
}

static void
__Complete(
    _InOut_ VirtioBlkDevice_t* device,
    _In_ uint8_t status)
{
    VirtioSplitQueue_t* queue = device->RequestQueue;
    uint16_t head = queue->Available->Ring[queue->UsedIndex % queue->QueueSize];
    VirtioBlkRequest_t* request = queue->Contexts[head];
    uint16_t descriptor = queue->Descriptors[head].Next;
    uint32_t written = 1;
    VirtioSplitDescriptor_t* entry;

    CHECK(request && request->DeviceOwned);
    while (queue->Descriptors[descriptor].Flags & VIRTIO_SPLIT_DESC_F_NEXT) {
        entry = &queue->Descriptors[descriptor];
        if (entry->Flags & VIRTIO_SPLIT_DESC_F_WRITE) {
            memset(__TestDmaPointer(entry->Address), 0x5a, entry->Length);
            written += entry->Length;
        } else {
            CHECK(*(uint8_t*)__TestDmaPointer(entry->Address) == 0x3c);
            CHECK(*((uint8_t*)__TestDmaPointer(entry->Address) + entry->Length - 1) == 0x3c);
        }
        descriptor = entry->Next;
    }
    *(uint8_t*)__TestDmaPointer(queue->Descriptors[descriptor].Address) = status;
    queue->Used->Ring[queue->UsedIndex % queue->QueueSize] =
        (VirtioSplitUsedElement_t){head, written};
    queue->Used->RingIndex++;
}

static void
__TestBlockHandoff(
    void)
{
    VirtioBlkDevice_t device;
    OSHandle_t memory = {0};
    struct gracht_message message = {0};
    unsigned begins = g_dmaBegins;
    unsigned ends = g_dmaEnds;
    unsigned replies = g_replies;
    unsigned maps = g_dmaMaps;
    unsigned i;

    __Setup(&device);
    CHECK(SHMCreate(&(SHM_t){.Flags = SHM_COMMIT | SHM_CLEAN, .Size = 8192, .Access = 3},
                    &memory) == OS_EOK);
    CHECK(VirtioBlkDeviceTransfer(
              &device, &message, __STORAGE_OPERATION_READ, 0, memory.ID, 17, 8) == OS_EOK);
    for (i = 0; i < 4; i++) {
        __Complete(&device, VIRTIO_BLK_S_OK);
        CHECK(VirtioBlkDeviceHandleInterrupt(&device) == OS_EOK);
        if (i < 3) {
            CHECK(((uint8_t*)SHMBuffer(&memory))[17] == 0);
        }
    }
    CHECK(g_replies == replies + 1 && g_result == OS_EOK && g_completed == 8);
    CHECK(g_dmaBegins == begins + 1 && g_dmaEnds == ends + 1 && g_dmaMaps == maps + 1);
    CHECK(((uint8_t*)SHMBuffer(&memory))[16] == 0 && ((uint8_t*)SHMBuffer(&memory))[17] == 0x5a);
    CHECK(((uint8_t*)SHMBuffer(&memory))[17 + 4096] == 0);

    // Writes must be visible in the separate device view before publication,
    // with the same single handoff across all four descriptor chains.
    memset(SHMBuffer(&memory), 0x3c, 8192);
    CHECK(VirtioBlkDeviceTransfer(
              &device, &message, __STORAGE_OPERATION_WRITE, 0, memory.ID, 17, 8) == OS_EOK);
    for (i = 0; i < 4; i++) {
        __Complete(&device, VIRTIO_BLK_S_OK);
        CHECK(VirtioBlkDeviceHandleInterrupt(&device) == OS_EOK);
    }
    CHECK(g_replies == replies + 2 && g_result == OS_EOK && g_completed == 8);
    CHECK(g_dmaBegins == begins + 2 && g_dmaEnds == ends + 2 && g_dmaMaps == maps + 2);

    // A later device error copies back only earlier successful read chunks.
    memset(SHMBuffer(&memory), 0, 8192);
    CHECK(VirtioBlkDeviceTransfer(
              &device, &message, __STORAGE_OPERATION_READ, 0, memory.ID, 17, 8) == OS_EOK);
    __Complete(&device, VIRTIO_BLK_S_OK);
    CHECK(VirtioBlkDeviceHandleInterrupt(&device) == OS_EOK);
    __Complete(&device, VIRTIO_BLK_S_IOERR);
    CHECK(VirtioBlkDeviceHandleInterrupt(&device) == OS_EOK);
    CHECK(g_replies == replies + 3 && g_result != OS_EOK && g_completed == 2);
    CHECK(((uint8_t*)SHMBuffer(&memory))[17 + 1023] == 0x5a);
    CHECK(((uint8_t*)SHMBuffer(&memory))[17 + 1024] == 0);
    CHECK(SHMRelease(&memory) == OS_EOK);
    __Teardown(&device);
}

static void
__TestCompletionRetry(
    void)
{
    VirtioBlkDevice_t device;
    OSHandle_t memory = {0};
    struct gracht_message message = {0};
    unsigned replies = g_replies;

    __Setup(&device);
    CHECK(SHMCreate(&(SHM_t){.Flags = SHM_COMMIT | SHM_CLEAN, .Size = 4096, .Access = 3},
                    &memory) == OS_EOK);
    CHECK(VirtioBlkDeviceTransfer(
              &device, &message, __STORAGE_OPERATION_READ, 0, memory.ID, 0, 1) == OS_EOK);
    __Complete(&device, VIRTIO_BLK_S_OK);
    g_dmaFailEnd = 1;
    CHECK(VirtioBlkDeviceHandleInterrupt(&device) == OS_EOK);
    CHECK(device.CleanupRequests && g_replies == replies && g_dmaLiveMappings == 1);
    CHECK(((uint8_t*)SHMBuffer(&memory))[0] == 0);
    ELEMENT_INIT(&device.Header, 0, &device);
    list_append(&g_devices, &device.Header);
    g_cancelMonitor = false;
    __CleanupMonitor(NULL, NULL);
    list_remove(&g_devices, &device.Header);
    CHECK(g_monitorSleeps == 1 && g_replies == replies + 1);
    CHECK(!device.CleanupRequests && ((uint8_t*)SHMBuffer(&memory))[0] == 0x5a);
    __Teardown(&device);
    CHECK(SHMRelease(&memory) == OS_EOK);
}

static void
__TestCleanupFailures(
    void)
{
    VirtioBlkDevice_t device;
    OSHandle_t memory = {0};
    struct gracht_message message = {0};
    unsigned replies;
    unsigned stage;

    for (stage = 0; stage < 4; stage++) {
        __Setup(&device);
        CHECK(SHMCreate(&(SHM_t){.Flags = SHM_COMMIT | SHM_CLEAN, .Size = 4096, .Access = 3},
                        &memory) == OS_EOK);
        replies = g_replies;
        CHECK(VirtioBlkDeviceTransfer(
                  &device, &message, __STORAGE_OPERATION_READ, 0, memory.ID, 0, 1) == OS_EOK);
        g_resetFailure = true;
        CHECK(VirtioBlkResetRequestQueue(&device) == OS_ETIMEOUT);
        CHECK(g_dmaLiveMappings == 1 && g_replies == replies);
        g_resetFailure = false;
        g_dmaFailEnd = stage == 0;
        g_dmaFailUnmap = stage == 1;
        g_dmaFailFree = stage == 2;
        g_dmaFailClose = stage == 3;
        if (stage < 3) {
            CHECK(VirtioBlkResetRequestQueue(&device) != OS_EOK);
            CHECK(device.CleanupRequests && g_replies == replies);
        } else {
            CHECK(VirtioBlkResetRequestQueue(&device) == OS_EOK);
            CHECK(DMAClose(&device.Transport.DmaContext) != OS_EOK);
            CHECK(device.Transport.DmaContext);
        }
        g_dmaFailEnd = g_dmaFailUnmap = g_dmaFailFree = g_dmaFailClose = 0;
        __Teardown(&device);
        CHECK(g_replies == replies + 1 && g_result == OS_ECANCELLED);
        CHECK(((uint8_t*)SHMBuffer(&memory))[0] == 0);
        CHECK(SHMRelease(&memory) == OS_EOK);
    }
}

static void
__TestSetupRollback(
    void)
{
    VirtioBlkDevice_t device;
    OSHandle_t memory = {0};
    struct gracht_message message = {0};
    unsigned replies = g_replies;

    __Setup(&device);
    CHECK(SHMCreate(&(SHM_t){.Flags = SHM_COMMIT | SHM_CLEAN, .Size = 4096, .Access = 3},
                    &memory) == OS_EOK);
    g_dmaFailMap = 1;
    g_dmaFailFree = 1;
    CHECK(VirtioBlkDeviceTransfer(
              &device, &message, __STORAGE_OPERATION_READ, 0, memory.ID, 0, 1) == OS_EOOM);
    CHECK(device.CleanupRequests && !device.CleanupRequests->Deferred);
    CHECK(g_replies == replies && g_dmaLiveMappings == 0);
    g_dmaFailFree = 0;
    CHECK(VirtioBlkRetryRequests(&device) == OS_EOK);
    CHECK(!device.CleanupRequests && g_replies == replies);
    __Teardown(&device);
    CHECK(SHMRelease(&memory) == OS_EOK);
}

static void
__TestQueueRollback(
    void)
{
    VirtioBlkDevice_t device;
    VirtioSplitQueue_t* queue = NULL;

    __Setup(&device);
    CHECK(VirtioBlkResetRequestQueue(&device) == OS_EOK);
    g_dmaFailAllocate = 2;
    g_dmaFailFree = 1;
    CHECK(VirtioSplitQueueCreate(&device.Transport, 0, 8, &queue) == OS_EOOM);
    CHECK(queue && device.Transport.ActiveQueues == 1 && g_dmaLiveBuffers == 1);
    CHECK(VirtioSplitQueueDestroy(queue) != OS_EOK);
    CHECK(device.Transport.ActiveQueues == 1);
    g_dmaFailFree = 0;
    CHECK(VirtioSplitQueueDestroy(queue) == OS_EOK);
    CHECK(!device.Transport.ActiveQueues && !g_dmaLiveBuffers);
    __Teardown(&device);
}

int
main(
    void)
{
    VirtioBlkDevice_t* device = calloc(1, sizeof(*device));
    struct ioset_event event = {.events = IOSETSYN, .data.context = (void*)(uintptr_t)17};

    CHECK(device != NULL);
    device->Descriptor.DeviceID = 17;
    device->EventDescriptor = 42;
    ELEMENT_INIT(&device->Header, 0, device);
    list_append(&g_devices, &device->Header);
    CHECK(OnEvent(&event) == OS_EOK && g_eventReads == 1);
    g_destroyOnLock = device;
    CHECK(OnEvent(&event) == OS_ENOENT && g_eventReads == 1);
    __TestBlockHandoff();
    __TestCompletionRetry();
    __TestCleanupFailures();
    __TestQueueRollback();
    __TestSetupRollback();
    puts("Virtio DMA: translated rings, ordinary SHM, one handoff across chunks, lone-response "
         "retry, rollback and teardown passed");
    return 0;
}
