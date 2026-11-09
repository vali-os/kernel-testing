/** Exercise event counter mappings at every slab-aligned offset within a page. */
#include "../../kernel/sync/userevent.c"

static MemorySpace_t g_space;
static atomic_int g_page[1024] __attribute__((aligned(4096)));
static size_t g_offset;
static oserr_t g_translationStatus;
static oserr_t g_mappingStatus;
static int g_freed;
static int g_mapped;

static void
__Check(
    _In_ int condition,
    _In_ int code)
{
    register long value __asm__("x0") = code;
    register long number __asm__("x8") = 93;

    if (!condition) {
        __asm__ volatile("svc 0" :: "r"(value), "r"(number) : "memory");
        __builtin_unreachable();
    }
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

size_t
GetMemorySpacePageSize(void)
{
    return sizeof(g_page);
}

void*
MemoryCacheAllocate(
    _In_ MemoryCache_t* cache)
{
    return &g_page[g_offset / sizeof(atomic_int)];
}

void
MemoryCacheFree(
    _In_ MemoryCache_t* cache,
    _In_ void*         object)
{
    __Check(object == &g_page[g_offset / sizeof(atomic_int)], 1);
    g_freed++;
}

oserr_t
GetMemorySpaceMapping(
    _In_  MemorySpace_t* space,
    _In_  vaddr_t        address,
    _In_  int            pageCount,
    _Out_ uintptr_t*     physical)
{
    __Check(space == &g_space, 2);
    __Check(address == (vaddr_t)&g_page[g_offset / sizeof(atomic_int)] && pageCount == 1, 3);
    *physical = 0x12345000 + g_offset;
    return g_translationStatus;
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mapping)
{
    __Check(space == &g_space, 4);
    // The mapping API accepts page bases, regardless of the counter's offset.
    __Check(options->Pages[0] == 0x12345000, 5);
    __Check(options->Length == sizeof(g_page), 6);
    __Check(options->Flags == (MAPPING_COMMIT | MAPPING_DOMAIN |
        MAPPING_USERSPACE | MAPPING_PERSISTENT), 7);
    __Check(options->PlacementFlags == (MAPPING_PHYSICAL_FIXED | MAPPING_VIRTUAL_PROCESS), 8);
    *mapping = 0x8100028000ULL;
    g_mapped++;
    return g_mappingStatus;
}

void
_start(void)
{
    UserEvent_t event;
    oserr_t     status;
    register long result __asm__("x0");
    register long syscallNumber __asm__("x8");

    for (g_offset = 0; g_offset < sizeof(g_page); g_offset += sizeof(void*)) {
        atomic_store((atomic_int*)&g_page[g_offset / sizeof(atomic_int)], 42);
        status = AllocateSyncAddress(&event);
        __Check(status == OS_EOK, 9);
        __Check(event.KernelMapping == (atomic_int*)&g_page[g_offset / sizeof(atomic_int)], 10);
        __Check((uintptr_t)event.UserspaceMapping == 0x8100028000ULL + g_offset, 11);
        __Check(atomic_load(event.KernelMapping) == 0 && g_freed == 0, 12);
    }

    g_offset = 0x80;
    g_mapped = 0;
    g_translationStatus = OS_ENOENT;
    __Check(AllocateSyncAddress(&event) == OS_ENOENT, 13);
    __Check(g_freed == 1 && g_mapped == 0, 14);
    g_translationStatus = OS_EOK;
    g_mappingStatus = OS_EOOM;
    __Check(AllocateSyncAddress(&event) == OS_EOOM, 15);
    __Check(g_freed == 2 && g_mapped == 1, 16);

    result = 0;
    syscallNumber = 93;
    __asm__ volatile("svc 0" :: "r"(result), "r"(syscallNumber) : "memory");
    __builtin_unreachable();
}
