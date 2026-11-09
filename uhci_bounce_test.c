// Real UHCI packet planning, common DMA transitions and bounce pool. The host
// models memory and DMA calls; device addresses deliberately differ from pointers.
#define __need_minmax
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            printf("line %d: %s\n", __LINE__, #x);                                                 \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static int g_heap, g_heapCalls, g_failHeap;
static void*
__TestCalloc(
    _In_    size_t count,
    _In_    size_t size)
{
    void* value;
    if (++g_heapCalls == g_failHeap) {
        return NULL;
    }
    value = calloc(count, size);
    if (value) {
        g_heap++;
    }
    return value;
}

static void
__TestFree(
    _InOut_ void* value)
{
    if (value) {
        g_heap--;
        free(value);
    }
}
#define calloc __TestCalloc
#define free __TestFree
#include "../modules/serial/usb/common/transfer/bounce.c"
#undef calloc
#undef free
#include "../modules/serial/usb/common/transfer/dma.c"
#undef __TRACE
#include "../modules/serial/usb/uhci/transfer.c"
#include "../librt/libds/list.c"

struct DMAContext {
    int Unused;
};
struct DMABuffer {
    uint8_t* Data;
    struct DMASegment Segment;
};
struct DMAMapping {
    bool Active;
};
static struct DMAContext g_context;
static uint8_t g_client[262144];
static struct DMASegment g_segments[8];
static uint32_t g_segmentCount;
static int g_buffers, g_allocates, g_freeCalls, g_maps, g_attached, g_sessions;
static int g_cpu, g_begins, g_ends, g_cpuBegins, g_cpuEnds;
static int g_failAllocate, g_failFree, g_failMap, g_failAttach, g_failOpen;
static int g_failCpuBegin, g_failCpuEnd, g_failClose, g_failRelease, g_failBegin, g_failEnd;
static int g_lock;

void
SystemDebug(
    _In_    enum OSSysLogLevel level,
    _In_    const char* format,
    ...)
{
    (void)level;
    (void)format;
}

void
spinlock_init(
    _InOut_ spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

void
spinlock_acquire(
    _InOut_ spinlock_t* lock)
{
    CHECK(!lock->current);
    lock->current = 1;
    g_lock++;
}

void
spinlock_release(
    _InOut_ spinlock_t* lock)
{
    CHECK(lock->current && g_lock);
    lock->current = 0;
    g_lock--;
}

oserr_t
DMAAllocate(
    _In_    struct DMAContext* context,
    _In_    const struct DMARequirements* req,
    _Out_   struct DMABuffer** out)
{
    CHECK(context == &g_context && !*out && !g_lock && req->Contiguous);
    g_allocates++;
    if (g_failAllocate) {
        return OS_EOOM;
    }
    *out = calloc(1, sizeof(**out));
    (*out)->Data = calloc(1, req->Length);
    (*out)->Segment = (struct DMASegment){0x200000 + 0x100000 * g_allocates, req->Length};
    g_buffers++;
    return OS_EOK;
}

oserr_t
DMAFree(
    _InOut_ struct DMABuffer** buffer)
{
    CHECK(!g_lock);
    if (!*buffer) {
        return OS_EOK;
    }
    g_freeCalls++;
    if (g_failFree) {
        return OS_EUNKNOWN;
    }
    free((*buffer)->Data);
    free(*buffer);
    *buffer = NULL;
    g_buffers--;
    return OS_EOK;
}

void*
DMABufferData(
    _In_    const struct DMABuffer* buffer)
{
    return buffer->Data;
}

const struct DMASegment*
DMABufferSegments(
    _In_    const struct DMABuffer* buffer,
    _Out_   uint32_t* count)
{
    *count = 1;
    return &buffer->Segment;
}

oserr_t
DMAMapSHM(
    _In_    struct DMAContext* context,
    _In_    const OSHandle_t* buffer,
    _In_    size_t offset,
    _In_    size_t length,
    _In_    enum DMADirection direction,
    _In_    const struct DMAMapRequirements* requirements,
    _Out_   struct DMAMapping** out)
{
    CHECK(context == &g_context && buffer->ID == 42 && !requirements && !*out);
    CHECK(offset + length <= sizeof(g_client));
    (void)direction;
    *out = calloc(1, sizeof(**out));
    g_maps++;
    return OS_EOK;
}

const struct DMASegment*
DMAMappingSegments(
    _In_    const struct DMAMapping* mapping,
    _Out_   uint32_t* count)
{
    (void)mapping;
    *count = g_segmentCount;
    return g_segments;
}

oserr_t
DMAUnmap(
    _InOut_ struct DMAMapping** mapping)
{
    if (!*mapping) {
        return OS_EOK;
    }
    CHECK(!(*mapping)->Active);
    free(*mapping);
    *mapping = NULL;
    g_maps--;
    return OS_EOK;
}

oserr_t
DMABeginTransfer(
    _InOut_ struct DMAMapping* mapping,
    _In_    size_t offset,
    _In_    size_t length,
    _In_    enum SHMWaitMode mode,
    _In_    const OSTimestamp_t* deadline)
{
    CHECK(!g_cpu && !g_lock && !mapping->Active && !offset && length &&
          mode == SHMWaitNonBlocking && !deadline);
    g_begins++;
    if (g_failBegin) {
        return OS_EBUSY;
    }
    mapping->Active = true;
    return OS_EOK;
}

oserr_t
DMAEndTransfer(
    _InOut_ struct DMAMapping* mapping,
    _In_    size_t length)
{
    (void)length;
    CHECK(mapping->Active && !g_cpu && !g_lock);
    g_ends++;
    if (g_failEnd) {
        return OS_EUNKNOWN;
    }
    mapping->Active = false;
    return OS_EOK;
}

oserr_t
SHMAttach(
    _In_    uuid_t id,
    _InOut_ OSHandle_t* handle)
{
    CHECK(id == 42 && !g_lock);
    if (g_failAttach) {
        return OS_EOOM;
    }
    *handle = (OSHandle_t){.ID = 42, .Type = OSHANDLE_SHM};
    g_attached++;
    return OS_EOK;
}

oserr_t
SHMMap(
    _InOut_ OSHandle_t* handle,
    _In_    size_t offset,
    _In_    size_t length,
    _In_    unsigned int access)
{
    CHECK(handle->ID == 42 && offset == 0 && length <= sizeof(g_client) && !g_lock);
    (void)access;
    return g_failMap ? OS_EOOM : OS_EOK;
}

oserr_t
SHMRelease(
    _InOut_ OSHandle_t* handle)
{
    if (!handle->ID) {
        return OS_EOK;
    }
    CHECK(!g_lock);
    if (g_failRelease) {
        return OS_EUNKNOWN;
    }
    handle->ID = UUID_INVALID;
    g_attached--;
    return OS_EOK;
}

void*
SHMBuffer(
    _InOut_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    return g_client;
}

size_t
SHMBufferCapacity(
    _InOut_ OSHandle_t* handle)
{
    CHECK(handle->ID == 42);
    return sizeof(g_client);
}

size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
SHMCreate(
    _InOut_ SHM_t* options,
    _InOut_ OSHandle_t* handle)
{
    (void)options;
    (void)handle;
    CHECK(false);
    return OS_EUNKNOWN;
}

oserr_t
SHMAccessOpen(
    _In_    const OSHandle_t* buffer,
    _In_    size_t offset,
    _In_    size_t length,
    _InOut_ struct SHMAccessSession* session)
{
    CHECK(buffer->ID == 42 && !session->Handle && offset + length <= sizeof(g_client) && !g_lock);
    if (g_failOpen) {
        return OS_EOOM;
    }
    session->Handle = 12;
    g_sessions++;
    return OS_EOK;
}

oserr_t
SHMAccessBeginCpu(
    _InOut_ struct SHMAccessSession* session,
    _In_    enum SHMCpuAccess access,
    _In_    enum SHMWaitMode mode,
    _In_    const OSTimestamp_t* deadline)
{
    CHECK(session->Handle == 12 && !g_lock && !g_cpu && mode == SHMWaitNonBlocking && !deadline);
    (void)access;
    g_cpuBegins++;
    if (g_failCpuBegin) {
        return OS_EBUSY;
    }
    g_cpu = 1;
    return OS_EOK;
}

oserr_t
SHMAccessEndCpu(
    _InOut_ struct SHMAccessSession* session)
{
    CHECK(session->Handle == 12 && !g_lock && g_cpu);
    g_cpuEnds++;
    if (g_failCpuEnd) {
        return OS_EUNKNOWN;
    }
    g_cpu = 0;
    return OS_EOK;
}

oserr_t
SHMAccessClose(
    _InOut_ struct SHMAccessSession* session)
{
    if (!session->Handle) {
        return OS_EOK;
    }
    CHECK(!g_cpu && !g_lock);
    if (g_failClose) {
        return OS_EUNKNOWN;
    }
    session->Handle = UUID_INVALID;
    g_sessions--;
    return OS_EOK;
}

static void
__CheckEmpty(void)
{
    CHECK(!g_buffers && !g_maps && !g_attached && !g_sessions && !g_cpu && !g_heap && !g_lock);
}

static struct UsbTransferBouncePool*
__Pool(
    _In_    unsigned int slots)
{
    struct UsbTransferBouncePool* pool = NULL;
    CHECK(UsbTransferBouncePoolCreate(&g_context, slots, &pool) == OS_EOK);
    return pool;
}

static UsbManagerTransfer_t
__Transfer(
    _InOut_ struct UsbTransferBouncePool* pool,
    _In_    bool control,
    _In_    bool input)
{
    UsbManagerTransfer_t t = {0};
    t.Type = control ? USBTRANSFER_TYPE_CONTROL : USBTRANSFER_TYPE_BULK;
    t.Direction = input ? USBTRANSFER_DIRECTION_IN : USBTRANSFER_DIRECTION_OUT;
    t.MaxPacketSize = 64;
    t.SHMHandle = (OSHandle_t){.ID = 42, .Type = OSHANDLE_SHM};
    t.Dma.Context = &g_context;
    t.Dma.PacketPool = pool;
    t.Dma.BufferOffset = 1;
    t.Dma.BufferLength = control ? 85 : 128;
    g_segments[0] = (struct DMASegment){0x100ffe, 2};
    g_segments[1] = (struct DMASegment){0x600000, 12};
    g_segments[2] = (struct DMASegment){0x900000, 256};
    g_segmentCount = 3;
    return t;
}

static void
__Destroy(
    _InOut_ UsbManagerTransfer_t* t)
{
    CHECK(UsbTransferDmaDestroy(t) == OS_EOK);
    free(t->Elements);
    t->Elements = NULL;
}

static void
__TestCopies(void)
{
    struct UsbTransferBouncePool* pool = __Pool(4);
    UsbManagerTransfer_t t;
    struct UsbTransferPacketBounce* s;
    int input;
    int ends;
    int calls;
    int heaps;
    size_t i;

    for (input = 0; input < 2; input++) {
        memset(g_client, 0x31, sizeof(g_client));
        t = __Transfer(pool, true, input);
        CHECK(UsbTransferDmaPrepare(&t) == OS_EOK);
        s = t.Dma.Packets;
        CHECK(s && s->Count == 2 && t.ElementCount == 4);
        CHECK(t.Elements[0].Length == 8 && t.Elements[1].Length == 64 &&
              t.Elements[2].Length == 13);
        CHECK(t.Elements[3].Length == 0 && !t.Elements[3].NeedsBounce);
        calls = g_allocates;
        heaps = g_heapCalls;
        CHECK(UsbTransferBounceReserve(&t) == OS_EOK);
        CHECK(g_allocates == calls && g_heapCalls == heaps && pool->Available == 2);
        CHECK(UsbTransferDmaBegin(&t) == OS_EOK);
        CHECK(__BounceData(s, &s->Packets[0])[0] == 0x31);
        CHECK(t.Elements[0].Data.Address ==
            pool->Address + s->Packets[0].SlotIndex * 64);
        CHECK(t.Elements[2].Data.Address == 0x900000 + 58);
        if (!input) {
            CHECK(__BounceData(s, &s->Packets[1])[63] == 0x31);
        }
        memset(__BounceData(s, &s->Packets[0]), 0xff, 8);
        memset(__BounceData(s, &s->Packets[1]), 0xab, 64);
        calls = g_begins;
        CHECK(UsbTransferDmaBegin(&t) == OS_EOK && g_begins == calls);
        CHECK(pool->Available == 2); // A later TD batch uses the same slots.
        g_failEnd = 1;
        CHECK(UsbTransferDmaComplete(&t, 25) == OS_EUNKNOWN && pool->Available == 2);
        g_failEnd = 0;
        if (input) {
            g_failCpuBegin = 1;
        }
        CHECK(UsbTransferDmaComplete(&t, 25) == (input ? OS_EBUSY : OS_EOK));
        ends = g_ends;
        g_failCpuBegin = 0;
        if (input) {
            g_failCpuEnd = 1;
            CHECK(UsbTransferDmaComplete(&t, 16) == OS_EUNKNOWN && pool->Available == 2);
            CHECK(g_client[9] == 0xab && g_client[25] == 0xab && g_client[26] == 0x31);
            memset(__BounceData(s, &s->Packets[1]), 0xdd, 64);
            g_failCpuEnd = 0;
        }
        CHECK(UsbTransferDmaComplete(&t, 25) == OS_EOK && g_ends == ends);
        CHECK(pool->Available == 4);
        CHECK(UsbTransferDmaComplete(&t, 25) == OS_EOK);
        for (i = 0; i < 100; i++) {
            CHECK(g_client[i] == (input && i >= 9 && i < 26 ? 0xab : 0x31));
        }
        __Destroy(&t);
    }
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK);
    __CheckEmpty();
}

static void
__TestDirectAndCancel(void)
{
    struct UsbTransferBouncePool* pool = __Pool(2);
    UsbManagerTransfer_t t = __Transfer(pool, false, false);
    int calls = g_allocates;

    g_segments[0] = (struct DMASegment){0x100fff, 128};
    g_segmentCount = 1;
    CHECK(UsbTransferDmaPrepare(&t) == OS_EOK && !t.Dma.Packets);
    CHECK(t.ElementCount == 3 && t.Elements[2].Length == 0);
    CHECK(!t.Elements[0].NeedsBounce && !t.Elements[1].NeedsBounce);
    CHECK(UsbTransferDmaBegin(&t) == OS_EOK);
    CHECK(UsbTransferDmaComplete(&t, 128) == OS_EOK);
    CHECK(g_allocates == calls);
    __Destroy(&t);
    t = __Transfer(pool, false, true);
    CHECK(UsbTransferDmaPrepare(&t) == OS_EOK);
    CHECK(UsbTransferBounceReserve(&t) == OS_EOK);
    CHECK(UsbTransferDmaBegin(&t) == OS_EOK);
    memset(g_client, 0x55, sizeof(g_client));
    CHECK(UsbTransferDmaDestroy(&t) == OS_EBUSY);
    CHECK(UsbTransferDmaComplete(&t, 0) == OS_EOK);
    CHECK(g_client[1] == 0x55 && pool->Available == 2);
    __Destroy(&t);
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK);
    __CheckEmpty();
}

static void
__TestFifo(void)
{
    struct UsbTransferBouncePool* pool = __Pool(2);
    UsbManagerTransfer_t first = __Transfer(pool, false, false);
    UsbManagerTransfer_t second = __Transfer(pool, true, false);
    UsbManagerTransfer_t third = __Transfer(pool, false, false);
    int calls;

    CHECK(UsbTransferDmaPrepare(&first) == OS_EOK);
    CHECK(UsbTransferDmaPrepare(&second) == OS_EOK);
    CHECK(UsbTransferDmaPrepare(&third) == OS_EOK);
    CHECK(UsbTransferBounceReserve(&first) == OS_EOK);
    CHECK(UsbTransferBounceReserve(&second) == OS_EBUSY);
    CHECK(UsbTransferBounceReserve(&third) == OS_EBUSY);
    CHECK(pool->Available == 1 && pool->Waiters.count == 2);
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EBUSY);
    CHECK(UsbTransferBounceAbort(&first) == OS_EOK);
    CHECK(UsbTransferBounceCanRetry(&second) && !UsbTransferBounceCanRetry(&third));
    CHECK(UsbTransferBounceReserve(&third) == OS_EBUSY);
    CHECK(pool->Available == 2); // A smaller later request cannot overtake.
    __Destroy(&second);          // Cancelling the head makes the next waiter eligible.
    CHECK(UsbTransferBounceCanRetry(&third));
    CHECK(UsbTransferBounceReserve(&third) == OS_EOK);
    calls = g_begins;
    g_failCpuBegin = 1;
    CHECK(UsbTransferDmaBegin(&third) == OS_EBUSY && g_begins == calls);
    CHECK(pool->Available == 2);
    g_failCpuBegin = 0;
    CHECK(UsbTransferBounceReserve(&third) == OS_EOK);
    g_failBegin = 1;
    CHECK(UsbTransferDmaBegin(&third) == OS_EBUSY && pool->Available == 2);
    g_failBegin = 0;
    __Destroy(&first);
    __Destroy(&third);
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK);
    __CheckEmpty();
}

static void
__TestOverflow(void)
{
    struct UsbTransferBouncePool* pool = __Pool(1);
    UsbManagerTransfer_t transfer = __Transfer(pool, true, true);
    struct UsbTransferPacketBounce* bounce;
    uint64_t address;
    size_t i;
    int calls;

    memset(g_client, 0x31, sizeof(g_client));
    CHECK(UsbTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(UsbTransferBounceReserve(&transfer) == OS_EOK);
    bounce = transfer.Dma.Packets;
    CHECK(bounce->Overflow != NULL && pool->Available == 1);
    address = bounce->Overflow->Segment.Address;
    CHECK(transfer.Elements[0].Data.Address == address);
    CHECK(transfer.Elements[1].Data.Address == address + USB_BOUNCE_SLOT_SIZE);

    // Overflow memory follows the same copy and completion rules as pool slots.
    // A second reservation must reuse it without another DMA allocation.
    calls = g_allocates;
    CHECK(UsbTransferBounceReserve(&transfer) == OS_EOK && g_allocates == calls);
    CHECK(UsbTransferDmaBegin(&transfer) == OS_EOK);
    CHECK(__BounceData(bounce, &bounce->Packets[0])[0] == 0x31);
    memset(__BounceData(bounce, &bounce->Packets[1]), 0xab, 64);
    CHECK(UsbTransferDmaComplete(&transfer, 85) == OS_EOK);
    for (i = 0; i < 100; i++) {
        CHECK(g_client[i] == (i >= 9 && i < 73 ? 0xab : 0x31));
    }
    CHECK(pool->Available == 1);
    __Destroy(&transfer);
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK);
    __CheckEmpty();
}

static void
__TestFailures(void)
{
    struct UsbTransferBouncePool* pool = NULL;
    UsbManagerTransfer_t t;
    int* failures[] = {&g_failAttach, &g_failMap, &g_failOpen};
    unsigned int i;

    g_failHeap = g_heapCalls + 1;
    CHECK(UsbTransferBouncePoolCreate(&g_context, 2, &pool) == OS_EOOM && !pool);
    g_failHeap = 0;
    g_failAllocate = 1;
    CHECK(UsbTransferBouncePoolCreate(&g_context, 2, &pool) == OS_EOOM && pool);
    g_failAllocate = 0;
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK);
    pool = __Pool(1);
    t = __Transfer(pool, false, false);
    g_failHeap = g_heapCalls + 1;
    CHECK(UsbTransferDmaPrepare(&t) == OS_EOOM && !t.Dma.Packets);
    g_failHeap = 0;
    __Destroy(&t);
    for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        t = __Transfer(pool, false, false);
        *failures[i] = 1;
        CHECK(UsbTransferDmaPrepare(&t) == OS_EOOM);
        *failures[i] = 0;
        __Destroy(&t);
    }
    t = __Transfer(pool, true, false);
    CHECK(UsbTransferDmaPrepare(&t) == OS_EOK && t.Dma.Packets->Count == 2);
    g_failAllocate = 1;
    CHECK(UsbTransferBounceReserve(&t) == OS_EOOM && pool->Waiters.count == 0);
    g_failAllocate = 0;
    CHECK(UsbTransferBounceReserve(&t) == OS_EOK && t.Dma.Packets->Overflow);
    CHECK(pool->Available == 1);
    g_failCpuEnd = 1;
    CHECK(UsbTransferDmaBegin(&t) == OS_EUNKNOWN && g_cpu);
    CHECK(UsbTransferDmaDestroy(&t) == OS_EUNKNOWN);
    g_failCpuEnd = 0;
    g_failClose = 1;
    CHECK(UsbTransferDmaDestroy(&t) == OS_EUNKNOWN && !g_cpu);
    g_failClose = 0;
    g_failRelease = 1;
    CHECK(UsbTransferDmaDestroy(&t) == OS_EUNKNOWN);
    g_failRelease = 0;
    g_failFree = 1;
    CHECK(UsbTransferDmaDestroy(&t) == OS_EUNKNOWN);
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EBUSY);
    g_failFree = 0;
    __Destroy(&t);
    g_failFree = 1;
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EUNKNOWN && pool);
    g_failFree = 0;
    CHECK(UsbTransferBouncePoolDestroy(&pool) == OS_EOK && !pool);
    __CheckEmpty();
}
int
main(void)
{
    __TestCopies();
    __TestDirectAndCancel();
    __TestFifo();
    __TestOverflow();
    __TestFailures();
    CHECK(g_cpuBegins > 0 && g_cpuEnds > 0 && g_freeCalls > 0);
    puts("UHCI bounce: packet geometry, exact copies, FIFO, one DMA handoff, overflow and retry "
         "cleanup passed");
    return 0;
}
