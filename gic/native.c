/**
 * QEMU virt-only interrupt exercise, compiled only with VALI_ARM64_GIC_SELFTEST.
 * Runs after SMP startup and before userspace. SPI 127 must be unused by devices.
 * Software-pending that SPI exercises real GIC routing/EOI without depending on
 * PCI enumeration; it does not validate an external device's electrical signal.
 */
// This opt-in test must execute its checks in Release builds as well.
#undef NDEBUG
#include <arch/utils.h>
#include <arch/interrupts.h>
#include <assert.h>
#include <component/cpu.h>
#include <debug.h>
#include <interrupts/interrupts.h>
#include <machine.h>
#include "../../kernel/arch/aarch64/common/private.h"
#include "../../kernel/arch/aarch64/common/device.h"

#define TEST_SPI 127
#define TEST_ROUNDS 4

static unsigned int g_ipis[ARM64_CPU_COUNT];
static unsigned int g_spis;
unsigned int g_arm64GicTestTimers[ARM64_CPU_COUNT];

static uint64_t
__Counter(void)
{
    uint64_t value;

    __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(value));
    return value;
}

static void
__WaitCount(
    _In_ unsigned int* count,
    _In_ unsigned int expected)
{
    uint64_t start = __Counter();
    uint64_t frequency = GetMachine()->Processor.PlatformData.CounterFrequency;

    while (__atomic_load_n(count, __ATOMIC_ACQUIRE) < expected) {
        if (__Counter() - start >= frequency) {
            FATAL(FATAL_SCOPE_KERNEL, "GIC self-test delivery timed out, expected %u", expected);
        }
        __asm__ volatile("yield" ::: "memory");
    }
}

static irqstatus_t
__Ipi(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void* context)
{
    (void)functions;
    (void)context;
    __atomic_fetch_add(&g_ipis[ArchGetProcessorCoreId()], 1, __ATOMIC_RELEASE);
    return IRQSTATUS_HANDLED;
}

static irqstatus_t
__Spi(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void* context)
{
    (void)functions;
    (void)context;
    __atomic_fetch_add(&g_spis, 1, __ATOMIC_RELEASE);
    return IRQSTATUS_HANDLED;
}

void
Arm64GicSelfTest(void)
{
    InterruptDescriptor_t descriptor = {0};
    InterruptControllerResource_t spi = {0};
    SystemInterrupt_t* interrupt;
    uintptr_t distributor;
    irqstate_t previous;
    uuid_t ipiId;
    uuid_t spiId;
    unsigned int ipiLine;
    unsigned int cpu;
    unsigned int round;
    unsigned int before;
    uint64_t deadline;
    uint64_t frequency = GetMachine()->Processor.PlatformData.CounterFrequency;

    descriptor.Pin = INTERRUPT_NONE;
    descriptor.Vectors[0] = INTERRUPT_NONE;
    descriptor.ResourceTable.Handler = __Ipi;
    ipiId = InterruptRegister(&descriptor, INTERRUPT_KERNEL | INTERRUPT_SOFT | INTERRUPT_EXCLUSIVE);
    assert(ipiId != UUID_INVALID);
    interrupt = InterruptGet(ipiId);
    assert(interrupt != NULL);
    ipiLine = interrupt->Routing.TableIndex;
    InterruptPut(interrupt);

    descriptor.Line = TEST_SPI;
    descriptor.ResourceTable.Handler = __Spi;
    descriptor.AcpiConform = INTERRUPT_ACPICONFORM_PRESENT; // Edge-triggered.
    spiId = InterruptRegister(&descriptor, INTERRUPT_KERNEL | INTERRUPT_EXCLUSIVE);
    assert(spiId != UUID_INVALID);
    spi.Line = TEST_SPI;
    spi.AcpiConform = descriptor.AcpiConform;
    distributor = Arm64MapDevice(g_arm64Platform.Gic.Distributor, g_arm64Platform.Gic.DistributorLength);
    assert(distributor != 0);
    previous = InterruptEnable();

    for (round = 1; round <= TEST_ROUNDS; round++) {
        // Waiting after every target detects misrouting and lost repeated SGIs.
        // More than eight CPUs also detects any remaining GICv2 target mask.
        for (cpu = 0; cpu < (unsigned int)GetMachine()->Processor.NumberOfCores; cpu++) {
            assert(ArchProcessorSendInterrupt(cpu, ipiLine) == OS_EOK);
            __WaitCount(&g_ipis[cpu], round);
            assert(__atomic_load_n(&g_ipis[cpu], __ATOMIC_ACQUIRE) == round);
        }

        assert(InterruptConfigure(&spi, 0) == OS_EOK);
        *(volatile uint32_t*)(distributor + ARM64_GICD_ISPENDR + (TEST_SPI / 32) * 4) = 1U << (TEST_SPI % 32);
        __asm__ volatile("dsb sy" ::: "memory");
        deadline = __Counter() + frequency / 1000;
        while (__Counter() < deadline) {
            __asm__ volatile("yield" ::: "memory");
        }
        assert(__atomic_load_n(&g_spis, __ATOMIC_ACQUIRE) == round - 1);
        assert(InterruptConfigure(&spi, 1) == OS_EOK);
        __WaitCount(&g_spis, round);
        assert(__atomic_load_n(&g_spis, __ATOMIC_ACQUIRE) == round);

        // Idle CPUs may stop their scheduling timer. Set a fresh physical timer
        // deadline and require its actual registered handler to run again.
        before = __atomic_load_n(&g_arm64GicTestTimers[0], __ATOMIC_ACQUIRE);
        deadline = __Counter() + frequency / 1000;
        __asm__ volatile("msr cntp_cval_el0, %0\nmsr cntp_ctl_el0, %1\nisb"
            :: "r"(deadline), "r"(1ULL) : "memory");
        __WaitCount(&g_arm64GicTestTimers[0], before + 1);
    }
    // Check every target after the final send as well. A broadcast error could
    // otherwise reach an earlier CPU after its per-send check had finished.
    for (cpu = 0; cpu < (unsigned int)GetMachine()->Processor.NumberOfCores; cpu++) {
        assert(__atomic_load_n(&g_ipis[cpu], __ATOMIC_ACQUIRE) == TEST_ROUNDS);
    }
    assert(__atomic_load_n(&g_spis, __ATOMIC_ACQUIRE) == TEST_ROUNDS);
    InterruptRestoreState(previous);
    assert(InterruptUnregister(spiId) == OS_EOK);
    assert(InterruptUnregister(ipiId) == OS_EOK);
    WARNING("GIC SELFTEST PASS: %u CPUs, repeated IPIs, SPI mask/unmask, physical timer",
        GetMachine()->Processor.NumberOfCores);
}
