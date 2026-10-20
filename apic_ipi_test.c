#include "../kernel/arch/x86/interrupts/apic/apicipi.c"
#include <component/timer.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static irqstate_t   g_irqState;
static unsigned int g_disableCalls;
static unsigned int g_restoreCalls;
static unsigned int g_busyReads[2];
static unsigned int g_reads[2];
static unsigned int g_writes;
static unsigned int g_clockReads;
static int          g_sent;
static int          g_x2Apic;
static UInteger64_t g_ipi;

irqstate_t
InterruptDisable(void)
{
    irqstate_t previous = g_irqState;
    g_irqState = 0;
    g_disableCalls++;
    return previous;
}

irqstate_t
InterruptRestoreState(irqstate_t state)
{
    irqstate_t previous = g_irqState;
    CHECK(g_irqState == 0);
    g_irqState = state;
    g_restoreCalls++;
    return previous;
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 0;
}

int
ApicIsX2Apic(void)
{
    CHECK(g_irqState == 0);
    return g_x2Apic;
}

uint32_t
ApicReadLocal(size_t reg)
{
    CHECK(g_irqState == 0 && reg == APIC_ICR_LOW);
    CHECK(++g_reads[g_sent] <= __APIC_DELIVERY_SPIN_LIMIT);
    return g_reads[g_sent] <= g_busyReads[g_sent]
            ? APIC_DELIVERY_BUSY : APIC_LEVEL_ASSERT;
}

void
ApicWriteLocal(size_t reg, uint32_t value)
{
    CHECK(g_irqState == 0 && !g_x2Apic && !g_sent);
    CHECK(g_reads[0] > g_busyReads[0]);
    if (g_writes == 0) {
        CHECK(reg == APIC_ICR_HIGH && value == g_ipi.u.HighPart);
    } else {
        CHECK(g_writes == 1 && reg == APIC_ICR_LOW && value == g_ipi.u.LowPart);
        g_sent = 1;
    }
    g_writes++;
}

void
ApicWriteLocal64(size_t reg, uint64_t value)
{
    CHECK(g_irqState == 0 && g_x2Apic && !g_sent && g_writes == 0);
    CHECK(g_reads[0] > g_busyReads[0]);
    CHECK(reg == APIC_ICR_LOW && value == g_ipi.QuadPart);
    g_sent = 1;
    g_writes++;
}

void
SystemTimerGetWallClockTime(OSTimestamp_t* time)
{
    g_clockReads++;
    time->Seconds = 0;
    time->Nanoseconds = 0;
}

void
SystemTimerStall(OSTimestamp_t* deadline)
{
    (void)deadline;
    // A frozen RTC/PIT would hang here. Fail immediately instead.
    CHECK(0);
}

static void
CheckDelivery(unsigned int busyBefore, unsigned int busyAfter, irqstate_t initialState)
{
    oserr_t expected = busyBefore >= __APIC_DELIVERY_SPIN_LIMIT
            || busyAfter >= __APIC_DELIVERY_SPIN_LIMIT ? OS_ETIMEOUT : OS_EOK;

    g_irqState = initialState;
    g_disableCalls = g_restoreCalls = 0;
    g_busyReads[0] = busyBefore;
    g_busyReads[1] = busyAfter;
    g_reads[0] = g_reads[1] = 0;
    g_writes = g_clockReads = 0;
    g_sent = 0;
    g_ipi.u.HighPart = g_x2Apic ? 0x12345678 : APIC_DESTINATION(3);
    g_ipi.u.LowPart = APIC_LEVEL_ASSERT | APIC_VECTOR(0x40);

    CHECK(__SendIPI(&g_ipi) == expected);
    CHECK(g_irqState == initialState);
    CHECK(g_disableCalls == 1 && g_restoreCalls == 1);
    CHECK(g_clockReads == 0);
    if (busyBefore >= __APIC_DELIVERY_SPIN_LIMIT) {
        CHECK(g_reads[0] == __APIC_DELIVERY_SPIN_LIMIT);
        CHECK(g_reads[1] == 0 && g_writes == 0 && !g_sent);
    } else {
        CHECK(g_reads[0] == busyBefore + 1);
        CHECK(g_reads[1] == (busyAfter >= __APIC_DELIVERY_SPIN_LIMIT
                ? __APIC_DELIVERY_SPIN_LIMIT : busyAfter + 1));
        CHECK(g_writes == (g_x2Apic ? 1 : 2) && g_sent);
    }
}

int
main(void)
{
    for (g_x2Apic = 0; g_x2Apic <= 1; g_x2Apic++) {
        for (irqstate_t state = 0; state <= 1; state++) {
            CheckDelivery(0, 0, state);
            CheckDelivery(3, 0, state);
            CheckDelivery(0, 3, state);
            CheckDelivery(3, 5, state);
            CheckDelivery(__APIC_DELIVERY_SPIN_LIMIT - 1,
                          __APIC_DELIVERY_SPIN_LIMIT - 1, state);
            CheckDelivery(__APIC_DELIVERY_SPIN_LIMIT, 0, state);
            CheckDelivery(0, __APIC_DELIVERY_SPIN_LIMIT, state);
        }
    }
    puts("APIC IPI: frozen clock, bounded pre/post delivery, xAPIC/x2APIC writes and IRQ restoration passed");
    return 0;
}
