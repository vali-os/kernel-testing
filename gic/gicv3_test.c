// Compile the production backend against a register model. MMIO commands use
// set/clear semantics, while counter-driven waits can be forced to time out.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <component/cpu.h>
#include <machine.h>
#include <log.h>
#include "../../kernel/components/cpu_private.h"
#include "../../kernel/arch/aarch64/common/private.h"

#define __VALI_ARM64_GICV3_REGISTERS_H__
static uint8_t g_dist[0x10000] __attribute__((aligned(8)));
static uint8_t g_redists[3][0x20000] __attribute__((aligned(8)));
static SystemMachine_t g_machine;
static SystemCpuCore_t g_cores[3];
static unsigned int g_current;
struct Arm64Platform g_arm64Platform;
static uint64_t g_sre, g_ctlr, g_sgi, g_eoi, g_iar, g_counter, g_features;
static int g_stuckWake, g_stuckRwp, g_denySre, g_denyMap;

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

SystemCpuCore_t*
GetProcessorCore(
    _In_ uuid_t id)
{
    assert(id < 3);
    return &g_cores[id];
}

SystemCpuCore_t*
CpuCoreCurrent(void)
{
    return &g_cores[g_current];
}

PlatformCpuCoreBlock_t*
CpuCorePlatformBlock(
    _In_ SystemCpuCore_t* core)
{
    return &core->PlatformData;
}

void
LogAppendMessage(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
}

uintptr_t
Arm64MapDevice(
    _In_ uint64_t physical,
    _In_ uint64_t length)
{
    if (g_denyMap) {
        return 0;
    }
    if (physical == 0x8000000) {
        assert(length == sizeof(g_dist));
        return (uintptr_t)g_dist;
    }
    assert(physical >= 0x80a0000 && physical < 0x8100000);
    assert((physical - 0x80a0000) % 0x20000 == 0 && length == 0x20000);
    return (uintptr_t)g_redists[(physical - 0x80a0000) / 0x20000];
}

static uint32_t
__GicRead32(
    _In_ uintptr_t    base,
    _In_ unsigned int offset)
{
    if (offset == 0x14 && g_stuckWake) {
        return 4;
    }
    if (!offset && g_stuckRwp) {
        return base == (uintptr_t)g_dist ? (1U << 31) : 8;
    }
    return *(uint32_t*)(base + offset);
}

static void
__GicWrite32(
    _In_ uintptr_t    base,
    _In_ unsigned int offset,
    _In_ uint32_t     value)
{
    if (offset >= 0x100 && offset < 0x180) {
        *(uint32_t*)(base + offset) |= value;
    } else if (offset >= 0x180 && offset < 0x200) {
        *(uint32_t*)(base + offset - 0x80) &= ~value;
    } else {
        *(uint32_t*)(base + offset) = value;
    }
}

static uint64_t
__GicRead64(
    _In_ uintptr_t    base,
    _In_ unsigned int offset)
{
    return *(uint64_t*)(base + offset);
}

static void
__GicWrite64(
    _In_ uintptr_t    base,
    _In_ unsigned int offset,
    _In_ uint64_t     value)
{
    *(uint64_t*)(base + offset) = value;
}

static uint64_t
__ReadSystem(
    _In_ const char* name)
{
    if (!strcmp(name, "cntfrq_el0")) {
        return 100;
    }
    if (!strcmp(name, "cntpct_el0")) {
        return ++g_counter;
    }
    if (!strcmp(name, "id_aa64pfr0_el1")) {
        return g_features;
    }
    if (!strcmp(name, "icc_sre_el1")) {
        return g_sre;
    }
    if (!strcmp(name, "icc_ctlr_el1")) {
        return g_ctlr;
    }
    if (!strcmp(name, "icc_iar1_el1")) {
        return g_iar;
    }
    assert(!"unexpected system register read");
    return 0;
}

static void
__WriteSystem(
    _In_ const char* name,
    _In_ uint64_t    value)
{
    if (!strcmp(name, "icc_sre_el1") && !g_denySre) {
        g_sre = value;
    } else if (!strcmp(name, "icc_ctlr_el1")) {
        g_ctlr = value;
    } else if (!strcmp(name, "icc_sgi1r_el1")) {
        g_sgi = value;
    } else if (!strcmp(name, "icc_eoir1_el1")) {
        g_eoi = value;
    }
}

#define GIC_READ_SYSTEM(name, value) ((value) = __ReadSystem(#name))
#define GIC_WRITE_SYSTEM(name, value) __WriteSystem(#name, value)
#define GIC_BARRIER() ((void)0)
#define GIC_RELAX() ((void)0)
#include "../../kernel/arch/aarch64/common/gicv3.c"

static void
__Reset(void)
{
    static struct Arm64GicRegion region = {0x80a0000, 0x60000};
    unsigned int i;
    uint64_t affinities[] = {0x100, 0x203, 0x1200340211ULL};

    memset(g_dist, 0, sizeof(g_dist));
    memset(g_redists, 0, sizeof(g_redists));
    memset(g_cores, 0, sizeof(g_cores));
    memset(&g_arm64Platform, 0, sizeof(g_arm64Platform));
    g_machine.Processor.NumberOfCores = 3;
    g_machine.Processor.Cores = g_cores;
    g_arm64Platform.Gic.Version = 3;
    g_arm64Platform.Gic.Distributor = 0x8000000;
    g_arm64Platform.Gic.Regions = &region;
    g_arm64Platform.Gic.RegionCount = 1;
    g_arm64Platform.TimerInterrupt = 30;
    g_stuckWake = g_stuckRwp = g_denySre = g_denyMap = 0;
    g_sre = g_counter = g_sgi = g_eoi = 0;
    g_ctlr = ICC_CTLR_RSS | ICC_CTLR_EOIMODE;
    g_features = 1ULL << 24;
    g_current = 0;
    g_needsRangeSelector = 0;
    __GicWrite32((uintptr_t)g_dist, GIC_PIDR2, 0x30);
    __GicWrite32((uintptr_t)g_dist, ARM64_GICD_TYPER, 3 | GICD_RSS);
    __GicWrite32((uintptr_t)g_dist, ARM64_GICD_CTLR, GICD_DS);
    for (i = 0; i < 3; i++) {
        g_cores[i].PlatformData.Affinity = affinities[i];
        // Hardware discovery order differs from CPU-table order.
        __GicWrite32((uintptr_t)g_redists[i], GIC_PIDR2, 0x30);
        __GicWrite64((uintptr_t)g_redists[i], GICR_TYPER,
            (uint64_t)__AffinityValue(affinities[2-i]) << 32 | (i == 2 ? GICR_LAST : 0));
    }
}

int
main(void)
{
    unsigned int count;
    unsigned int i;
    uint32_t token;
    InterruptControllerResource_t irq = {.Line = 45};
    struct Arm64GicRegion regions[2] = {
        {0x80a0000, 0x20000},
        {0x80c0000, 0x40000}
    };

    __Reset();
    assert(g_arm64GicV3.Initialize(&count) == OS_EOK && count == 128);
    assert(!(g_ctlr & ICC_CTLR_EOIMODE));
    assert(g_sre & 1);
    assert(g_cores[0].PlatformData.Gic.Redistributor == (uintptr_t)g_redists[2]);
    assert(__GicRead64((uintptr_t)g_dist, GICD_IROUTER + 45 * 8) == 0x100);
    assert((__GicRead32((uintptr_t)g_dist, ARM64_GICD_CTLR) & (GICD_DS | GICD_ARE | 3)) == (GICD_DS | GICD_ARE | 2));
    for (i = 0; i < 3; i++) {
        g_current = i;
        assert(g_arm64GicV3.InitializeCore() == OS_EOK);
        g_arm64GicV3.EnablePrivate(30);
        assert(__GicRead32(g_cores[i].PlatformData.Gic.Redistributor + GIC_FRAME_SIZE, 0x100) & (1U << 30));
    }
    assert(g_arm64GicV3.Configure(&irq, 1) == OS_EOK);
    assert(__GicRead32((uintptr_t)g_dist, 0x104) & (1U << 13));
    assert(__GicRead32((uintptr_t)g_dist, 0xc08) & (2U << 26));
    irq.AcpiConform = INTERRUPT_ACPICONFORM_TRIGGERMODE;
    assert(g_arm64GicV3.Configure(&irq, 0) == OS_EOK);
    assert(!(__GicRead32((uintptr_t)g_dist, 0x104) & (1U << 13)));
    assert(!(__GicRead32((uintptr_t)g_dist, 0xc08) & (3U << 26)));
    assert(g_arm64GicV3.Send(&g_cores[2].PlatformData, 7) == OS_EOK);
    assert(g_sgi == ((0x12ULL << 48) | (1ULL << 44) | (0x34ULL << 32) | (7ULL << 24) | (2ULL << 16) | 2));
    g_iar = 8193;
    token = g_arm64GicV3.Acknowledge();
    assert(g_arm64GicV3.Decode(token) == 8193); // Never alias LPI 8193 to scheduler SGI 1.
    g_arm64GicV3.Complete(token);
    assert(g_eoi == 8193);

    __Reset();
    g_stuckWake = 1;
    assert(g_arm64GicV3.Initialize(&count) != OS_EOK && g_counter >= 100);
    __Reset();
    g_stuckRwp = 1;
    assert(g_arm64GicV3.Initialize(&count) != OS_EOK && g_counter >= 100);
    __Reset();
    g_denySre = 1;
    assert(g_arm64GicV3.Initialize(&count) != OS_EOK);
    __Reset();
    g_denyMap = 1;
    assert(g_arm64GicV3.Initialize(&count) == OS_EOOM);
    __Reset();
    g_features = 0;
    assert(g_arm64GicV3.Initialize(&count) == OS_ENOTSUPPORTED);
    __Reset();
    g_ctlr &= ~ICC_CTLR_RSS;
    assert(g_arm64GicV3.Initialize(&count) == OS_ENOTSUPPORTED);
    __Reset();
    __GicWrite32((uintptr_t)g_dist, 4, 3);
    assert(g_arm64GicV3.Initialize(&count) == OS_ENOTSUPPORTED);
    __Reset();
    __GicWrite32((uintptr_t)g_dist, GIC_PIDR2, 0x40);
    assert(g_arm64GicV3.Initialize(&count) == OS_ENOTSUPPORTED);
    __Reset();
    __GicWrite64((uintptr_t)g_redists[1], GICR_TYPER, __GicRead64((uintptr_t)g_redists[0], GICR_TYPER));
    assert(g_arm64GicV3.Initialize(&count) == OS_EINVALPARAMS);
    __Reset();
    __GicWrite64((uintptr_t)g_redists[0], GICR_TYPER, GICR_LAST);
    assert(g_arm64GicV3.Initialize(&count) == OS_ENOENT);

    __Reset();
    g_arm64Platform.Gic.RegionCount = 2;
    g_arm64Platform.Gic.Regions = regions;
    __GicWrite64((uintptr_t)g_redists[0], GICR_TYPER,
        __GicRead64((uintptr_t)g_redists[0], GICR_TYPER) | GICR_LAST);
    assert(g_arm64GicV3.Initialize(&count) == OS_EOK);

    __Reset();
    regions[0].Length = 0x10000;
    g_arm64Platform.Gic.RegionCount = 1;
    g_arm64Platform.Gic.Regions = regions;
    assert(g_arm64GicV3.Initialize(&count) == OS_EINVALPARAMS);

    __Reset();
    g_arm64Platform.Gic.RegionCount = 0;
    for (i = 0; i < 3; i++) {
        g_cores[i].PlatformData.Gic.RedistributorPhysical = 0x80a0000 + (2 - i) * 0x20000;
    }
    assert(g_arm64GicV3.Initialize(&count) == OS_EOK);
    __Reset();
    g_arm64Platform.Gic.RegionCount = 0;
    g_cores[0].PlatformData.Gic.RedistributorPhysical = 0x80a0000;
    assert(g_arm64GicV3.Initialize(&count) == OS_EINVALPARAMS);
    puts("GICv3 discovery, register sequencing, timeout and affinity tests: PASS");
    return 0;
}
