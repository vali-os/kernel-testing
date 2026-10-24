// Exercise real scheduler allocation and chaining with controlled SHM backing.
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
static SHM_t                g_options[4];
static int                  g_createCalls;
static int                  g_segmentCalls;
static int                  g_failCreateAt;
static bool                 g_failSegments;

oserr_t
SHMCreate(
    _In_  SHM_t*      options,
    _Out_ OSHandle_t* handleOut)
{
    int i;

    if (++g_createCalls == g_failCreateAt) {
        return OS_EOOM;
    }
    CHECK(options->Size <= sizeof(g_memory[0]));
    for (i = 0; i < 4 && g_live[i]; i++) { }
    CHECK(i < 4);
    g_live[i] = true;
    g_options[i] = *options;
    memset(g_memory[i], 0xa5, sizeof(g_memory[i]));
    *handleOut = (OSHandle_t) {
        .ID = i + 1,
        .Type = OSHANDLE_SHM,
        .Payload = g_memory[i]
    };
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return handle->Payload;
}

oserr_t
SHMGetSGTable(
    _In_ OSHandle_t*   handle,
    _In_ SHMSGTable_t* table,
    _In_ int           maxCount)
{
    (void)maxCount;
    g_segmentCalls++;
    CHECK(SHM_KIND(g_options[handle->ID - 1].Flags) == SHM_DEVICE);
    if (g_failSegments) {
        return OS_EOOM;
    }
    table->Entries = malloc(sizeof(*table->Entries));
    CHECK(table->Entries != NULL);
    table->Count = 1;
    table->Entries[0].Address = 0x200000 + handle->ID * 4096;
    table->Entries[0].Length = g_options[handle->ID - 1].Size;
    return OS_EOK;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    if (handle->ID == UUID_INVALID) {
        return;
    }
    CHECK(handle->ID <= 4 && g_live[handle->ID - 1]);
    g_live[handle->ID - 1] = false;
    memset(handle, 0, sizeof(*handle));
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
        CHECK(g_options[0].Flags == (SHM_COMMIT | SHM_PRIVATE));
        CHECK(g_options[0].Conformity == OSMEMORYCONFORMITY_NONE);
        CHECK(g_segmentCalls == calls && pool->ElementPoolDMATable.Count == 0);
        CHECK(pool->ElementPoolDMATable.Entries == NULL);
    } else {
        CHECK(g_options[0].Flags == (SHM_DEVICE | SHM_PRIVATE));
        CHECK(g_options[0].Conformity == OSMEMORYCONFORMITY_LOW);
        CHECK(g_segmentCalls == calls + 1 && pool->ElementPoolDMATable.Count == 1);
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
    UsbSchedulerDestroy(scheduler);
    __CheckReleased();
}

static void
__TestCleanup(void)
{
    UsbSchedulerSettings_t settings;
    UsbScheduler_t*        scheduler = NULL;
    int                   i;

    UsbSchedulerSettingsCreate(&settings, 1, 1, 0, USB_SCHEDULER_SOFTWARE_ELEMENTS);
    __AddPool(&settings);
    __AddPool(&settings);
    for (i = 1; i <= 2; i++) {
        g_failCreateAt = g_createCalls + i;
        CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOOM);
        CHECK(scheduler == NULL);
        __CheckReleased();
    }
    g_failCreateAt = 0;
    settings.Flags = 0;
    g_failSegments = true;
    CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOOM);
    CHECK(scheduler == NULL);
    __CheckReleased();

    // Software pools never request an SG table, even if that operation fails.
    settings.Flags = USB_SCHEDULER_SOFTWARE_ELEMENTS;
    CHECK(UsbSchedulerInitialize(&settings, &scheduler) == OS_EOK);
    UsbSchedulerDestroy(scheduler);
    __CheckReleased();
    g_failSegments = false;
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
