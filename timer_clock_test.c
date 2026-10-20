/* Real clock selection, RTC programming and APIC/TSC calibration with fake hardware. */
#define LIBC_KERNEL
#include "../kernel/components/timer.c"
#include "../kernel/arch/x86/components/rtc.c"
#include "../kernel/arch/x86/components/tsc.c"
#include "../kernel/arch/x86/interrupts/apic/apictimer.c"
#include "../librt/libds/list.c"
#include "../librt/libc/time/common/month_lengths.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static SystemMachine_t g_machine;
Cmos_t g_cmos;
static uint8_t g_registers[128];
static uint64_t g_referenceTick, g_referenceFrequency = 1024, g_apicStart;
static uint32_t g_initial, g_current;
static clock_t g_elapsed, g_deadline;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
void DebugPanic(int scope, Context_t* context, const char* message, ...) { abort(); }
void* kmalloc(size_t n) { return calloc(1, n); }
void kfree(void* p) { free(p); }
SystemMachine_t* GetMachine(void) { return &g_machine; }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) { (void)lock; }
void SpinlockReleaseIrq(Spinlock_t* lock) { (void)lock; }
irqstate_t InterruptDisable(void) { return 1; }
irqstate_t InterruptRestoreState(irqstate_t state) { return state; }
uint8_t CmosRead(uint8_t reg) { return g_registers[reg]; }
void CmosWrite(uint8_t reg, uint8_t value) { g_registers[reg] = value; }
bool HPETIsPresent(void) { return false; }
bool HPETIsEmulatingLegacyController(void) { return false; }
oserr_t HPETComparatorStart(int a, uint64_t b, int c, int d) { abort(); }
oserr_t HPETComparatorStop(int a) { abort(); }
uuid_t InterruptRegister(InterruptDescriptor_t* d, unsigned int f) { return 1; }
oserr_t CpuHasFeatures(unsigned int a, unsigned int b) { return OS_EOK; }
void _rdtsc(uint64_t* value) { *value = g_referenceTick * 1000000; }
uint32_t ApicReadLocal(size_t reg) {
    if (reg == APIC_INITIAL_COUNT) return g_initial;
    CHECK(reg == APIC_CURRENT_COUNT);
    return g_initial == UINT32_MAX ? UINT32_MAX - (uint32_t)((g_referenceTick - g_apicStart) * 1000000) : g_current;
}
void ApicWriteLocal(size_t reg, uint32_t value) {
    if (reg == APIC_INITIAL_COUNT) { g_initial = value; g_apicStart = g_referenceTick; }
}
oserr_t ThreadingAdvance(int preemptive, clock_t elapsed, clock_t* deadline) {
    CHECK(*deadline == 20 * NSEC_PER_MSEC);
    g_elapsed = elapsed; *deadline = g_deadline; return OS_EOK;
}
oserr_t ThreadCreate(const char* n, ThreadEntry_t e, void* a, unsigned int f, uuid_t m, size_t k, size_t u, uuid_t* h) { abort(); }
static void ReferenceRead(void* context, UInteger64_t* tick) { tick->QuadPart = ++g_referenceTick; }
static void ReferenceFrequency(void* context, UInteger64_t* frequency) { frequency->QuadPart = g_referenceFrequency; }

int main(void) {
    list_construct(&g_machine.SystemTimers.Timers);
    for (unsigned rate = 3; rate <= 15; rate++) {
        UInteger64_t frequency = { .QuadPart = 32768U >> (rate - 1) };
        CHECK(__RTCConfigure(&g_cmos, &frequency) == OS_EOK);
        CHECK((g_registers[CMOS_REGISTER_STATUS_A] & 15) == rate);
        CHECK(g_cmos.Frequency == (32768U >> (rate - 1)));
    }
    UInteger64_t frequency = { .QuadPart = 1000 };
    CHECK(__RTCConfigure(&g_cmos, &frequency) == OS_EOK && frequency.QuadPart == 1024);
    CHECK(SystemTimerRegister("rtc", &g_rtcOperations, SystemTimeAttributes_IRQ, &g_cmos) == OS_EOK);
    SystemTimerRefreshTimers();
    CHECK(g_cmos.RtcEnabled && g_machine.SystemTimers.Clock->Frequency.QuadPart == 1024);
    g_cmos.Ticks = 1536;
    tick_t timestamp;
    SystemTimerGetTimestamp(&timestamp);
    CHECK(timestamp == 1500000000);
    SystemTimerRefreshTimers();
    CHECK(g_cmos.RtcEnabled && g_machine.SystemTimers.Clock->Frequency.QuadPart == 1024);
    SystemTimerGetTimestamp(&timestamp);
    CHECK(timestamp == 1500000000);
    CHECK(__FractionToNanoseconds(1499999999, 1500000000) == 999999999);
    CHECK(__FractionToNanoseconds(UINT64_MAX - 1, UINT64_MAX) == 999999999);
    CHECK(__FractionToNanoseconds(UINT64_MAX / 2, UINT64_MAX) == 499999999);
    // An update flag alone must not resynchronize the wall clock every second.
    g_registers[CMOS_REGISTER_STATUS_C] = CMOSC_IRQ_UPDATE | CMOSC_IRQ_PERIODIC;
    RtcInterrupt(NULL, NULL);
    CHECK(g_cmos.Ticks == 1537);
    // Non-invariant TSCs are not registered; invariant ones calibrate against
    // the actual 1024-Hz reference interval, not an assumed 100 ms.
    TscInitialize();
    CHECK(list_count(&g_machine.SystemTimers.Timers) == 1);
    SystemTimer_t reference = { .Operations = { .Read = ReferenceRead, .GetFrequency = ReferenceFrequency } };
    g_machine.SystemTimers.Clock = &reference;
    g_machine.Processor.PlatformData.Flags = X86_CPU_FLAG_INVARIANT_TSC;
    TscInitialize();
    CHECK(g_tscFrequency == 1024000000 && list_count(&g_machine.SystemTimers.Timers) == 2);
    ApicTimerRecalibrate(NULL);
    CHECK(g_lapicTimer.Frequency == 1024000000);
    CHECK(__DeadlineToTicks(20000000) == 20480000);
    CHECK(__DeadlineToTicks(1) == 2 && __DeadlineToTicks(0) == 0);
    CHECK(__DeadlineToTicks(INT64_MAX) == UINT32_MAX);
    g_initial = 1536000; g_current = 0; g_deadline = 1;
    ApicTimerHandler(NULL, NULL);
    CHECK(g_elapsed == 1500000 && g_initial == 2);
    g_referenceFrequency = 0;
    CHECK(__Calibrate() == OS_ENOTSUPPORTED);
    while (g_machine.SystemTimers.Timers.head) {
        element_t* node = g_machine.SystemTimers.Timers.head;
        list_remove(&g_machine.SystemTimers.Timers, node); free(node->value);
    }
    puts("Clock: RTC rates, retained fallback, exact timestamps, invariant TSC and APIC deadlines passed");
    return 0;
}
