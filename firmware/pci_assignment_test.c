// Exercise actual BAR placement and configuration writes with modeled PCI functions.
#include "../../services/deviced/bus/pci/bars.c"
#include "../../services/deviced/bus/pci/assignment.c"
#include "../../services/deviced/bus/pci/io.c"
#include "../../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t g_config[8][64];
static unsigned int g_writes;
static unsigned int g_rejectBar;
static int g_badRollback;
static int g_rejectCommand;
static struct PciAddressWindow g_windows[16];
static PciHost_t g_host;
static PciDevice_t g_nodes[8];

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)level;
    (void)format;
}

static size_t
__Read(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t width)
{
    CHECK(slot < 8 && reg + width <= 256);
    return (g_config[slot][reg / 4] >> ((reg & 3) * 8)) & (width == 2 ? 0xffff : UINT32_MAX);
}

static void
__Write(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t value,
    _In_ size_t width)
{
    g_writes++;
    if (reg == 4 && g_rejectCommand && value != 0) {
        return;
    }
    if (reg >= 0x10 && reg < 0x28) {
        CHECK((g_config[slot][1] & 7) == 0);
        if (g_rejectBar == reg) {
            g_rejectBar = 0;
            g_config[slot][reg / 4] = 0xdead0000;
            return;
        }
        if (g_badRollback && value == 0) {
            return;
        }
    }
    if (width == 2) {
        g_config[slot][reg / 4] = (g_config[slot][reg / 4] & 0xffff0000) | value;
    } else {
        g_config[slot][reg / 4] = value;
    }
}

static const struct PciHostOperations g_operations = {
    .Read = __Read,
    .Write = __Write
};

static void
__Reset(void)
{
    unsigned int i;

    memset(g_config, 0, sizeof(g_config));
    memset(g_nodes, 0, sizeof(g_nodes));
    memset(&g_host, 0, sizeof(g_host));
    memset(g_windows, 0, sizeof(g_windows));
    g_writes = 0;
    g_rejectBar = 0;
    g_badRollback = 0;
    g_rejectCommand = 0;
    g_host.Operations = &g_operations;
    g_host.Resources.Windows = g_windows;
    g_host.Resources.Count = 1;
    g_host.Resources.Present = 1;
    g_host.Resources.AssignBars = 1;
    g_host.RootDevice = &g_nodes[0];
    g_host.Identification.BusEnd = 255;
    g_windows[0] = (struct PciAddressWindow) {
        .Space = PciAddressMemory32, .BusBase = 0xc0000000,
        .PhysicalBase = 0x600000000ULL, .Length = 0x4000000
    };
    for (i = 0; i < 8; i++) {
        g_nodes[i].Host = &g_host;
        g_nodes[i].Slot = i;
        list_construct(&g_nodes[i].Children);
        ELEMENT_INIT(&g_nodes[i].ChildHeader, 0, &g_nodes[i]);
        g_config[i][1] = 7;
    }
}

static void
__Child(
    _In_ unsigned int node,
    _In_ unsigned int parent)
{
    g_nodes[node].Parent = &g_nodes[parent];
    list_append(&g_nodes[parent].Children, &g_nodes[node].ChildHeader);
}

static void
__Bar(
    _In_ unsigned int node,
    _In_ unsigned int index,
    _In_ uint32_t space,
    _In_ uint32_t attributes,
    _In_ uint64_t address,
    _In_ uint64_t size)
{
    PciDescribeBar(&g_host, space, attributes, address, size, &g_nodes[node].Resources.Bars[index]);
    g_config[node][4 + index] = (uint32_t)address | attributes;
    if (space == 3) {
        g_config[node][5 + index] = address >> 32;
    }
}

static void
__TestPi4(void)
{
    unsigned int writes;

    __Reset();
    __Child(1, 0);
    g_nodes[1].IsBridge = 1;
    g_config[1][0x20 / 4] = 0xc3f0c000;
    __Child(2, 1);
    __Child(3, 1);
    // The later function's assigned BAR must be reserved before assigning node 2.
    __Bar(3, 0, 2, 0, 0xc0000000, 0x2000);
    __Bar(2, 0, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);
    CHECK(g_config[2][4] == 0xc0002000);
    CHECK(g_config[3][4] == 0xc0000000);
    CHECK(g_nodes[2].Resources.Bars[0].CpuAddress == 0x600002000ULL);
    CHECK(g_config[2][1] == 7);
    writes = g_writes;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK && g_writes == writes);
}

static void
__TestLimits(void)
{
    __Reset();
    __Child(1, 0);
    g_windows[0].BusBase = 0x100001001ULL;
    g_windows[0].Length = 0x10000;
    __Bar(1, 0, 3, 4, 0, 0x4000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);
    CHECK(g_config[1][4] == 0x4004 && g_config[1][5] == 1);
    CHECK(g_nodes[1].Resources.Bars[0].BusAddress == 0x100004000ULL);
    __Bar(1, 2, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOOM);
    CHECK(g_config[1][6] == 0);

    __Reset();
    __Child(1, 0);
    g_windows[0].BusBase = UINT64_MAX - 0xfff;
    g_windows[0].Length = 0x1000;
    __Bar(1, 0, 3, 4, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);
    __Bar(1, 2, 3, 4, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOOM);

    __Reset();
    __Child(1, 0);
    g_windows[0].Length = 0x800;
    __Bar(1, 0, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOOM && g_writes == 0);
    g_windows[0].Length = 0x1000;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);

    __Reset();
    __Child(1, 0);
    g_windows[0].Attributes = PCI_WINDOW_PREFETCHABLE;
    __Bar(1, 0, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOOM && g_writes == 0);
    __Bar(1, 0, 2, 8, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);

    __Reset();
    __Child(1, 0);
    g_windows[0].Space = PciAddressIo;
    g_windows[0].BusBase = 0x1000;
    __Bar(1, 0, 1, 1, 0, 0x100);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK && g_config[1][4] == 0x1001);
}

static void
__TestBridges(void)
{
    __Reset();
    __Child(1, 0);
    __Child(2, 0);
    g_nodes[2].IsBridge = 1;
    g_config[2][0x20 / 4] = 0xc000c000; // sibling reserves the first MiB
    g_config[2][0x24 / 4] = 0xfff0; // disabled prefetch window
    __Bar(1, 0, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK && g_config[1][4] == 0xc0100000);

    __Reset();
    __Child(1, 0);
    __Child(2, 1);
    g_nodes[1].IsBridge = 1;
    g_config[1][0x20 / 4] = 0xc010c010;
    __Bar(2, 0, 2, 0, 0, 0x1000);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK && g_config[2][4] == 0xc0100000);
    __Bar(2, 1, 2, 0, 0, 0x1000);
    g_config[1][0x20 / 4] = 0xfff0; // disabled forwarding
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOOM);
}

static void
__TestFailures(void)
{
    unsigned int writes;

    __Reset();
    __Child(1, 0);
    __Bar(1, 0, 3, 4, 0, 0x1000);
    g_rejectBar = 0x14;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EUNKNOWN);
    CHECK(g_config[1][4] == 4 && g_config[1][5] == 0 && g_config[1][1] == 7);
    CHECK(g_nodes[1].Resources.Bars[0].State == PciBarUnassigned);
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EOK);

    __Reset();
    __Child(1, 0);
    __Bar(1, 0, 2, 0, 0, 0x1000);
    g_rejectBar = 0x10;
    g_badRollback = 1;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EUNKNOWN);
    CHECK(g_host.Status.State == PciHostConfigurationUnknown && g_config[1][1] == 0);
    writes = g_writes;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_ENOTSUPPORTED && g_writes == writes);

    __Reset();
    __Child(1, 0);
    __Bar(1, 0, 2, 0, 0, 0x1000);
    g_rejectCommand = 1;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_EUNKNOWN);
    CHECK(g_host.Status.State == PciHostConfigurationUnknown && g_config[1][1] == 0);
    CHECK(g_nodes[1].Resources.Bars[0].State == PciBarAssigned);
    writes = g_writes;
    CHECK(PciAssignHostBars(&g_nodes[0]) == OS_ENOTSUPPORTED && g_writes == writes);
}

int
main(void)
{
    __TestPi4();
    __TestLimits();
    __TestBridges();
    __TestFailures();
    puts("PCI BAR assignment: placement, bridge routing, widths, preservation and failure recovery passed");
    return 0;
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
