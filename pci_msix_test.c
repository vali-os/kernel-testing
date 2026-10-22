// Exercise the actual MSI-X engine with dropped writes and lost readbacks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned g_allocations;
static unsigned g_allocateCalls;
static unsigned g_failAllocation;

static void*
__MsixTestAllocate(size_t count, size_t size)
{
    void* result;

    if (++g_allocateCalls == g_failAllocation) {
        return NULL;
    }
    result = calloc(count, size);
    CHECK(result != NULL);
    g_allocations++;
    return result;
}

static void
__MsixTestFree(void* allocation)
{
    CHECK(allocation != NULL && g_allocations != 0);
    g_allocations--;
    free(allocation);
}

#include "../services/deviced/bus/pci/msi.c"
#define calloc __MsixTestAllocate
#define free __MsixTestFree
#include "../services/deviced/bus/pci/msix.c"
#undef calloc
#undef free

// The engine logs through the kernel log, which is not available on the host.
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }

static unsigned char g_config[256];
static unsigned char g_originalConfig[256];
static uint32_t g_table[2048][4];
static uint32_t g_originalTable[2048][4];
static struct PciBar g_bars[6];
static struct PciMsixState* g_state;
static unsigned g_writes;
static unsigned g_failWrite;
static int g_applyFailed;
static int g_badRead;
static uint32_t g_failedValue;
static int g_mapped;
static int g_failMap;
static int g_failUnmap;
static unsigned g_tableAccesses;

static uint32_t
__ConfigRead(void* context, size_t offset, size_t width)
{
    uint32_t value = 0;
    size_t i;

    (void)context;
    CHECK(offset + width <= sizeof(g_config));
    if (g_badRead) {
        g_badRead = 0;
        return g_failedValue ^ 1;
    }
    for (i = 0; i < width; i++) {
        value |= (uint32_t)g_config[offset + i] << (8 * i);
    }
    return value;
}

static void
__Put(size_t offset, uint32_t value, size_t width)
{
    size_t i;

    for (i = 0; i < width; i++) {
        g_config[offset + i] = value >> (8 * i);
    }
}

static int
__DropWrite(uint32_t value)
{
    if (++g_writes != g_failWrite) {
        return 0;
    }
    g_badRead = 1;
    g_failedValue = value;
    return !g_applyFailed;
}

static void
__ConfigWrite(void* context, size_t offset, uint32_t value, size_t width)
{
    (void)context;
    CHECK(width == 2 && (offset == 4 || offset == 0x42));
    if (!__DropWrite(value)) {
        __Put(offset, value, width);
    }
}

static oserr_t
__Map(void* context, const struct PciMsixRegion* region, void** mapping)
{
    (void)context;
    CHECK(!g_mapped && region->Bar == 0 && region->Offset == 0x1000);
    CHECK(region->Address == 0x80001000 && region->Length <= sizeof(g_table));
    if (g_failMap) {
        return OS_EOOM;
    }
    *mapping = g_table;
    g_mapped = 1;
    return OS_EOK;
}

static oserr_t
__Unmap(void* context, void* mapping, size_t length)
{
    (void)context;
    CHECK(g_mapped && mapping == g_table && length <= sizeof(g_table));
    if (g_failUnmap) {
        return OS_EUNKNOWN;
    }
    g_mapped = 0;
    return OS_EOK;
}

static uint32_t
__TableRead(void* mapping, size_t offset)
{
    CHECK(g_mapped && mapping == g_table && offset < sizeof(g_table) && !(offset & 3));
    g_tableAccesses++;
    if (g_badRead) {
        g_badRead = 0;
        return g_failedValue ^ 1;
    }
    CHECK(__ConfigRead(NULL, 4, 2) & 2);
    CHECK(__ConfigRead(NULL, 0x42, 2) & MSIX_ENABLE);
    return g_table[offset / 16][(offset % 16) / 4];
}

static void
__TableWrite(void* mapping, size_t offset, uint32_t value)
{
    uint32_t index = offset / 16;
    uint32_t field = (offset % 16) / 4;

    CHECK(g_mapped && mapping == g_table && offset < sizeof(g_table) && !(offset & 3));
    CHECK(__ConfigRead(NULL, 4, 2) & 2);
    CHECK((__ConfigRead(NULL, 0x42, 2) & 0xc000) == 0xc000);
    if (field != 3) {
        CHECK(g_table[index][3] & MSIX_VECTOR_MASK);
    }
    g_tableAccesses++;
    if (!__DropWrite(value)) {
        g_table[index][field] = value;
    }
}

static struct PciMsixAccess g_access = {
    .Config = { NULL, __ConfigRead, __ConfigWrite },
    .Bars = g_bars,
    .Map = __Map,
    .Unmap = __Unmap,
    .Read = __TableRead,
    .Write = __TableWrite
};

static void
__Setup(void)
{
    unsigned i;

    CHECK(g_state == NULL && !g_mapped && !g_allocations);
    memset(g_config, 0, sizeof(g_config));
    memset(g_bars, 0, sizeof(g_bars));
    __Put(6, 0x10, 2);
    __Put(4, 4, 2);
    __Put(0x34, 0x40, 1);
    __Put(0x40, 0x11, 1);
    __Put(0x42, 3, 2);
    __Put(0x44, 0x1000, 4);
    __Put(0x48, 0x10000, 4);
    __Put(0x10, 0x40000000, 4);
    g_bars[0] = (struct PciBar) {
        .BusAddress = 0x40000000, .CpuAddress = 0x80000000,
        .Size = 0x20000, .Space = 2, .State = PciBarAssigned
    };
    for (i = 0; i < 2048; i++) {
        g_table[i][0] = 0xfee00000;
        g_table[i][1] = i;
        g_table[i][2] = 0x20 + i;
        g_table[i][3] = 0x40 | (i & 1);
    }
    memcpy(g_originalConfig, g_config, sizeof(g_config));
    memcpy(g_originalTable, g_table, sizeof(g_table));
    g_writes = g_failWrite = g_allocateCalls = g_failAllocation = g_tableAccesses = 0;
    g_applyFailed = g_badRead = g_failMap = g_failUnmap = 0;
}

static oserr_t
__Operation(unsigned operation)
{
    struct PciInterruptRequest request = {
        .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = operation, .Allocation = 1023
    };

    if (operation == PciInterruptReserve) {
        request.Count = 2;
    }
    return PciMsixControl(&g_access, &g_state, &request);
}

static oserr_t
__Program(unsigned index)
{
    struct PciInterruptRequest request = {
        .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptProgram,
        .Allocation = 1023, .Index = index,
        .AddressLow = 0xfee02000, .AddressHigh = 0x1234, .Data = 0xe0 + index
    };
    return PciMsixControl(&g_access, &g_state, &request);
}

static void
__Prepared(void)
{
    CHECK(__Operation(PciInterruptReserve) == OS_EOK);
    CHECK(__Program(0) == OS_EOK && __Program(1) == OS_EOK);
}

static void
__Restored(void)
{
    g_failWrite = 0;
    g_failMap = g_failUnmap = 0;
    CHECK(__Operation(PciInterruptRelease) == OS_EOK);
    CHECK(__Operation(PciInterruptRelease) == OS_EOK);
    CHECK(!g_mapped && !g_state && !g_allocations);
    CHECK(memcmp(g_config, g_originalConfig, sizeof(g_config)) == 0);
    CHECK(memcmp(g_table, g_originalTable, sizeof(g_table)) == 0);
}

static void
__TestValidation(void)
{
    struct PciMsixLayout layout;
    struct PciInterruptRequest request = {
        .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptReserve,
        .Allocation = 1023, .Count = 2
    };
    unsigned value;

    __Setup();
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK);
    CHECK(layout.Count == 4 && layout.Table.Length == 64 && layout.Pending.Length == 8);
    CHECK(!g_writes && !g_mapped);
    __Put(0x42, 2047, 2);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK);
    CHECK(layout.Count == 2048 && layout.Table.Length == 32768 && layout.Pending.Length == 256);
    __Put(0x42, 64, 2);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK && layout.Pending.Length == 16);
    __Put(0x48, 0x1fff8, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Put(0x48, 0x1fff0, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK);
    for (value = 1; value <= 7; value++) {
        __Setup();
        __Put(0x44, 0x1000 | value, 4);
        CHECK(PciMsixControl(&g_access, &g_state, &request) == OS_EINVALPARAMS);
        CHECK(!g_state && !g_writes && !g_mapped);
    }
    __Setup();
    __Put(0x44, 0x1ffc0, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK);
    __Put(0x44, 0x1ffc8, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    __Put(0x48, 0x1010, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    g_bars[2] = g_bars[0];
    __Put(0x18, 0x40000000, 4);
    __Put(0x48, 0x1002, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    g_bars[0].State = PciBarUnassigned;
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    g_bars[0].Space = 1;
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    __Put(0x10, 0x50000000, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    g_bars[0].CpuAddress = UINTPTR_MAX - 7;
    CHECK(PciMsixProbe(&g_access, &layout) == OS_ENOTSUPPORTED);
    __Setup();
    g_bars[0].Space = 3;
    g_bars[0].BusAddress |= 1ULL << 32;
    __Put(0x10, 0x40000004, 4);
    __Put(0x14, 1, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EOK);
    g_bars[1] = g_bars[0];
    __Put(0x44, 0x1001, 4);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EINVALPARAMS);
    __Setup();
    __Put(0x42, 0x8003, 2);
    CHECK(PciMsixProbe(&g_access, &layout) == OS_EBUSY);
    CHECK(!g_writes && !g_mapped && !g_state);
    puts("MSI-X layout: BAR identity/type, 64-bit pairs, extents, PBA rounding and alias overlap passed");
}

static void
__TestFailures(void)
{
    unsigned operation;
    unsigned total;
    unsigned fail;
    unsigned applied;
    unsigned retry;
    unsigned cases = 0;

    for (operation = PciInterruptReserve; operation <= PciInterruptRelease; operation++) {
        __Setup();
        if (operation != PciInterruptReserve) {
            CHECK(__Operation(PciInterruptReserve) == OS_EOK);
        }
        if (operation >= PciInterruptEnable) {
            CHECK(__Program(0) == OS_EOK && __Program(1) == OS_EOK);
        }
        if (operation >= PciInterruptDisable) {
            CHECK(__Operation(PciInterruptEnable) == OS_EOK);
        }
        g_writes = 0;
        CHECK((operation == PciInterruptProgram ? __Program(0) : __Operation(operation)) == OS_EOK);
        total = g_writes;
        __Restored();
        for (fail = 1; fail <= total; fail++) {
            for (applied = 0; applied < 2; applied++) {
                for (retry = 0; retry < 2; retry++) {
                    __Setup();
                    if (operation != PciInterruptReserve) {
                        CHECK(__Operation(PciInterruptReserve) == OS_EOK);
                    }
                    if (operation >= PciInterruptEnable) {
                        CHECK(__Program(0) == OS_EOK && __Program(1) == OS_EOK);
                    }
                    if (operation >= PciInterruptDisable) {
                        CHECK(__Operation(PciInterruptEnable) == OS_EOK);
                    }
                    g_writes = 0;
                    g_failWrite = fail;
                    g_applyFailed = applied;
                    CHECK((operation == PciInterruptProgram ? __Program(0) : __Operation(operation)) == OS_EUNKNOWN);
                    CHECK(g_state != NULL && g_allocations == 2);
                    g_failWrite = 0;
                    if (retry) {
                        CHECK((operation == PciInterruptProgram ? __Program(0) : __Operation(operation)) == OS_EOK);
                    }
                    __Restored();
                    cases++;
                }
            }
        }
    }
    printf("MSI-X lifecycle: %u dropped/uncertain writes, retry and complete snapshot restoration passed\n", cases);
}

static void
__TestLifecycle(void)
{
    struct PciInterruptRequest request;
    unsigned i;
    unsigned accesses;

    for (i = 1; i <= 2; i++) {
        __Setup();
        g_failAllocation = i;
        CHECK(__Operation(PciInterruptReserve) == OS_EOOM);
        CHECK(!g_state && !g_allocations && !g_writes);
    }
    __Setup();
    g_failMap = 1;
    CHECK(__Operation(PciInterruptReserve) == OS_EOOM);
    CHECK(g_state != NULL && !g_writes && !g_mapped);
    __Restored();
    __Setup();
    CHECK(__Operation(PciInterruptReserve) == OS_EOK);
    CHECK(__Operation(PciInterruptReserve) == OS_EOK);
    CHECK(__Operation(PciInterruptEnable) == OS_EBUSY);
    for (i = 0; i < 4; i++) {
        CHECK(g_table[i][3] & 1);
    }
    CHECK(__Program(0) == OS_EOK);
    CHECK(__Operation(PciInterruptEnable) == OS_EBUSY);
    CHECK(__Program(2) == OS_EINVALPARAMS);
    CHECK(__Program(1) == OS_EOK);
    request = (struct PciInterruptRequest) {
        .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptProgram, .Allocation = 1023,
        .AddressLow = 0xfee02000, .Data = 0xaa };
    CHECK(PciMsixControl(&g_access, &g_state, &request) == OS_EINVALPARAMS);
    request.Operation = PciInterruptRelease;
    request.Allocation++;
    request.AddressLow = request.Data = 0;
    CHECK(PciMsixControl(&g_access, &g_state, &request) == OS_EBUSY);
    for (i = 0; i < 8; i++) {
        CHECK(__Operation(PciInterruptEnable) == OS_EOK);
        CHECK(__Operation(PciInterruptEnable) == OS_EOK);
        CHECK(!(g_table[0][3] & 1) && !(g_table[1][3] & 1));
        CHECK((g_table[2][3] & 1) && (g_table[3][3] & 1));
        CHECK(__Program(0) == OS_EBUSY);
        CHECK(__Operation(PciInterruptDisable) == OS_EOK);
    }
    g_failUnmap = 1;
    CHECK(__Operation(PciInterruptRelease) == OS_EUNKNOWN);
    CHECK(g_state != NULL && g_mapped);
    CHECK(__Operation(PciInterruptEnable) == OS_EBUSY);
    CHECK(__Operation(PciInterruptReserve) == OS_EBUSY);
    CHECK(__Program(0) == OS_EBUSY);
    accesses = g_tableAccesses;
    CHECK(__Operation(PciInterruptDisable) == OS_EOK);
    CHECK(__Operation(PciInterruptRelease) == OS_EUNKNOWN);
    CHECK(g_tableAccesses == accesses);
    __Restored();
    __Setup();
    __Prepared();
    // A final Enable write may have taken effect even though readback failed.
    g_failWrite = g_writes + 4;
    g_applyFailed = 1;
    CHECK(__Operation(PciInterruptEnable) == OS_EUNKNOWN);
    CHECK(__Program(0) == OS_EBUSY);
    g_failWrite = 0;
    CHECK(__Operation(PciInterruptDisable) == OS_EOK);
    CHECK(__Program(0) == OS_EOK);
    __Restored();
    puts("MSI-X ownership: allocation/map failures, unused masks, uncertain enable and unmap retry passed");
}

static void
__TestCounts(void)
{
    const unsigned counts[] = { 1, 65, 2048 };
    struct PciInterruptRequest request;
    unsigned count;
    unsigned i;
    unsigned j;

    for (j = 0; j < sizeof(counts) / sizeof(counts[0]); j++) {
        __Setup();
        count = counts[j];
        __Put(0x42, count - 1, 2);
        memcpy(g_originalConfig, g_config, sizeof(g_config));
        request = (struct PciInterruptRequest) {
            .Strategy = INTERRUPT_STRATEGY_MSIX, .Operation = PciInterruptReserve, .Allocation = 1023, .Count = count + 1
        };
        CHECK(PciMsixControl(&g_access, &g_state, &request) == OS_EINVALPARAMS);
        CHECK(!g_state && !g_writes);
        request.Count = count;
        CHECK(PciMsixControl(&g_access, &g_state, &request) == OS_EOK);
        for (i = 0; i < count; i++) {
            CHECK(__Program(i) == OS_EOK);
        }
        CHECK(__Operation(PciInterruptEnable) == OS_EOK);
        CHECK(__Operation(PciInterruptDisable) == OS_EOK);
        __Restored();
    }
    puts("MSI-X counts: 1, 65 and 2048 entries programmed, enabled and restored passed");
}

int
main(void)
{
    __TestValidation();
    __TestLifecycle();
    __TestFailures();
    __TestCounts();
    return 0;
}
