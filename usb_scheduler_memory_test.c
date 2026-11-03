// Exercise real scheduler allocation and chaining with controlled DMA backing.
#include <stdlib.h>
#include <stdint.h>
static void* g_aligned[8];
static void* g_backing[8];
static void* __TestAlignedAlloc(size_t alignment, size_t length)
{
    int i;
    for (i = 0; i < 8 && g_aligned[i] != NULL; i++) { }
    if (i == 8) abort();
    g_backing[i] = malloc(length + alignment);
    g_aligned[i] = (void*)(((uintptr_t)g_backing[i] + alignment - 1) & ~(alignment - 1));
    return g_aligned[i];
}
static void __TestFree(void* memory)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (memory != NULL && memory == g_aligned[i]) {
            free(g_backing[i]);
            g_aligned[i] = NULL;
            return;
        }
    }
    free(memory);
}
#define aligned_alloc __TestAlignedAlloc
#define free __TestFree
#include "../modules/serial/usb/common/scheduler.c"
#include "../modules/serial/usb/common/scheduler_settings.c"
#include "../modules/serial/usb/common/scheduler_arena.c"
#include "../modules/serial/usb/common/scheduler_chain.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/** Software metadata with the same link and arena fields as controller pools. */
struct __TestElement {
    reg32_t BreadthLink;
    reg32_t DepthLink;
    UsbSchedulerObject_t Object;
    uint32_t Payload;
};

static _Alignas(4096) uint8_t g_memory[4][4096];
static bool                 g_live[4];
static struct DMARequirements g_options[4];
static int g_createCalls, g_segmentCalls, g_failCreateAt, g_failFree;

struct DMABuffer {
    int Index;
    struct DMASegment Segment;
};
static struct DMABuffer g_buffers[4];

oserr_t DMAAllocate(struct DMAContext* context, const struct DMARequirements* options, struct DMABuffer** out)
{
    int i;
    (void)context;
    if (++g_createCalls == g_failCreateAt) return OS_EOOM;
    CHECK(options->Length <= 4096 && options->Contiguous);
    for (i = 0; i < 4 && g_live[i]; i++) { }
    CHECK(i < 4);
    g_live[i] = true;
    g_options[i] = *options;
    g_buffers[i].Index = i;
    g_buffers[i].Segment = (struct DMASegment) { .Address = 0x201000 + i * 4096, .Length = 4096 };
    memset(g_memory[i], 0xa5, 4096);
    *out = &g_buffers[i];
    return OS_EOK;
}
void* DMABufferData(const struct DMABuffer* buffer) { return g_memory[buffer->Index]; }
const struct DMASegment* DMABufferSegments(const struct DMABuffer* buffer, uint32_t* count)
{ g_segmentCalls++; *count = 1; return &buffer->Segment; }
oserr_t DMAFree(struct DMABuffer** buffer)
{
    if (*buffer == NULL) return OS_EOK;
    if (g_failFree) return OS_EUNKNOWN;
    CHECK(g_live[(*buffer)->Index]);
    g_live[(*buffer)->Index] = false;
    *buffer = NULL;
    return OS_EOK;
}

void
spinlock_init(
    _In_ spinlock_t* lock)
{
    (void)lock;
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

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)level;
    (void)format;
}

oserr_t
UsbPeriodicSchedulerResetInternalData(
    _In_ UsbPeriodicScheduler_t* scheduler,
    _In_ int                     resetFramelist)
{
    // This fixture tests pool ownership, without a periodic schedule.
    CHECK((scheduler->Settings->Flags & USB_SCHEDULER_PERIODIC) == 0);
    (void)resetFramelist;
    return OS_EOK;
}

static void
__AddPool(
    _In_ UsbSchedulerSettings_t* settings)
{
    UsbSchedulerSettingsAddPool(
        settings,
        sizeof(struct __TestElement),
        64,
        4,
        1,
        offsetof(struct __TestElement, BreadthLink),
        offsetof(struct __TestElement, DepthLink),
        offsetof(struct __TestElement, Object)
    );
}

static void
__CheckReleased(void)
{
    int i;

    for (i = 0; i < 4; i++) {
        CHECK(!g_live[i]);
    }
}

static void
__TestPool(
    _In_ bool softwareElements)
{
    UsbSchedulerSettings_t settings;
    UsbScheduler_t*        scheduler = NULL;
    UsbSchedulerPool_t*    pool;
    struct __TestElement*  first;
    struct __TestElement*  second;
    struct __TestElement*  third;
    uint8_t*              element;
    uintptr_t             physical;
    int                   calls = g_segmentCalls;

    UsbSchedulerSettingsCreate(&settings, 1, 1, 0,
        softwareElements ? USB_SCHEDULER_SOFTWARE_ELEMENTS : 0);
    __AddPool(&settings);
    CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOK);
    pool = &scheduler->Settings.Pools[0];
    CHECK(pool->ElementPool[sizeof(struct __TestElement)] == 0);
    CHECK((uintptr_t)pool->ElementPool % 4096 == 0);
    if (softwareElements) {
        CHECK(g_segmentCalls == calls && pool->ElementPoolDMA == NULL);
    } else {
        CHECK(g_options[0].Contiguous && g_options[0].Alignment == 4096);
        CHECK(g_segmentCalls == calls + 1 && pool->ElementPoolDMA != NULL);
    }

    CHECK(UsbSchedulerAllocateElement(scheduler, 0, &element) == OS_EOK);
    first = (struct __TestElement*)element;
    CHECK(first->Object.Index == 1 && first->Payload == 0);
    CHECK(UsbSchedulerAllocateElement(scheduler, 0, &element) == OS_EOK);
    second = (struct __TestElement*)element;
    CHECK(second->Object.Index == 2);
    physical = UsbSchedulerGetDma(pool, element);
    CHECK(physical == (softwareElements ? 0 : 0x201000 + 2 * 64));
    CHECK(UsbSchedulerChainElement(scheduler, 0, (uint8_t*)first, 0, element,
        USB_ELEMENT_NO_INDEX, USB_CHAIN_DEPTH) == OS_EOK);
    CHECK(first->Object.DepthIndex == second->Object.Index);
    CHECK(first->DepthLink == physical);
    // Appending through the tail preserves the root and both software and
    // hardware links, which lets xHCI avoid searching the existing chain.
    CHECK(UsbSchedulerAllocateElement(scheduler, 0, &element) == OS_EOK);
    third = (struct __TestElement*)element;
    physical = UsbSchedulerGetDma(pool, element);
    CHECK(UsbSchedulerChainElement(scheduler, 0, (uint8_t*)second, 0, element,
        USB_ELEMENT_NO_INDEX, USB_CHAIN_DEPTH) == OS_EOK);
    CHECK(first->Object.DepthIndex == second->Object.Index);
    CHECK(second->Object.DepthIndex == third->Object.Index && second->DepthLink == physical);
    CHECK(third->Object.DepthIndex == USB_ELEMENT_NO_INDEX);
    CHECK(UsbSchedulerUnchainElement(scheduler, 0, (uint8_t*)second, 0, element,
        USB_CHAIN_DEPTH) == OS_EOK);
    CHECK(second->Object.DepthIndex == USB_ELEMENT_NO_INDEX);
    UsbTransferArenaFreeElement(&scheduler->TransferArena, element);
    element = (uint8_t*)second;
    CHECK(UsbSchedulerUnchainElement(scheduler, 0, (uint8_t*)first, 0, element,
        USB_CHAIN_DEPTH) == OS_EOK);
    CHECK(first->Object.DepthIndex == USB_ELEMENT_NO_INDEX);
    UsbTransferArenaFreeElement(&scheduler->TransferArena, (uint8_t*)first);
    CHECK(UsbSchedulerAllocateElement(scheduler, 0, &element) == OS_EOK);
    CHECK(element == (uint8_t*)first && first->Payload == 0);
    CHECK(UsbSchedulerDestroy(&scheduler) == OS_EOK);
    __CheckReleased();
}

static void
__TestCleanup(void)
{
    UsbSchedulerSettings_t settings;
    UsbScheduler_t*        scheduler = NULL;
    int                   i;

    UsbSchedulerSettingsCreate(&settings, 1, 1, 0, USB_SCHEDULER_FRAMELIST);
    __AddPool(&settings);
    __AddPool(&settings);
    for (i = 1; i <= 3; i++) {
        g_failCreateAt = g_createCalls + i;
        CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOOM);
        CHECK(scheduler != NULL);
        g_failFree = 1;
        if (i > 1) {
            CHECK(UsbSchedulerDestroy(&scheduler) == OS_EUNKNOWN);
            CHECK(scheduler != NULL);
        }
        g_failFree = 0;
        CHECK(UsbSchedulerDestroy(&scheduler) == OS_EOK && scheduler == NULL);
        __CheckReleased();
    }
    g_failCreateAt = 0;
    CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOK);
    g_failFree = 1;
    CHECK(UsbSchedulerDestroy(&scheduler) == OS_EUNKNOWN && scheduler != NULL);
    g_failFree = 0;
    CHECK(UsbSchedulerDestroy(&scheduler) == OS_EOK);
    __CheckReleased();
    settings.Flags = USB_SCHEDULER_SOFTWARE_ELEMENTS;
    CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOK);
    CHECK(UsbSchedulerDestroy(&scheduler) == OS_EOK);
    __CheckReleased();
}

int
main(void)
{
    __TestPool(true);
    __TestPool(false);
    __TestCleanup();
    puts("USB scheduler memory: cached metadata, hardware DMA, chaining, reuse and failure cleanup passed");
    return 0;
}
