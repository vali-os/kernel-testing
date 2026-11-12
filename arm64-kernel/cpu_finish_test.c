/** Ensure a thread parked by CpuCoreDeferFinish is finished at the next outer interrupt entry. */
#include <setjmp.h>
#include "../../kernel/components/cpu.c"

static SystemCpuCore_t g_core;
static SystemCpuCore_t g_otherCore;
static Context_t       g_context;
static unsigned int    g_finishCalls;
static uuid_t          g_finishedId;
static int             g_slotFreeAtFinish;

// Checks that expect an assertion to fail arm the panic stub and jump back.
static jmp_buf         g_panicExit;
static int             g_panicArmed;
static unsigned int    g_panics;

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

void
_assert_panic(
    _In_ const char* message)
{
    // Only checks that provoke an assertion on purpose may reach this.
    __Check(g_panicArmed, 90);
    g_panics++;
    longjmp(g_panicExit, 1);
}

uuid_t
ArchGetProcessorCoreId(void)
{
    return 1;
}

void
InterruptSetActiveStatus(
    _In_ int active)
{
}

void
InterruptsSetPriority(
    _In_ uint32_t priority)
{
}

void
ThreadingFinish(
    _In_ uuid_t threadId)
{
    // The slot must already be free, so the finish can never see its own
    // handle as still parked.
    g_slotFreeAtFinish = (g_core.PendingFinish == UUID_INVALID);
    g_finishedId = threadId;
    g_finishCalls++;
}

static void
__DeferOnCurrentCore(
    _In_ int threadHandle)
{
    CpuCoreDeferFinish(&g_core, (uuid_t)threadHandle);
}

static void
__DeferOnOtherCore(
    _In_ int threadHandle)
{
    CpuCoreDeferFinish(&g_otherCore, (uuid_t)threadHandle);
}

// Runs the operation and reports whether an assertion failed. The failed
// assertion must come before any effect of the operation.
static int
__Panics(
    _In_ void (*operation)(int),
    _In_ int    argument)
{
    g_panics = 0;
    g_panicArmed = 1;
    if (setjmp(g_panicExit) == 0) {
        operation(argument);
    }
    g_panicArmed = 0;
    return g_panics != 0;
}

void
_start(void)
{
    register long result __asm__("x0") = 0;
    register long number __asm__("x8") = 93;

    g_core.Id = 1;
    g_core.PendingFinish = UUID_INVALID;
    g_coreTable[1] = &g_core;

    // Nothing is parked, so an outer interrupt entry finishes nothing.
    CpuCoreEnterInterrupt(&g_context, 0);
    __Check(g_finishCalls == 0, 1);

    // ThreadingAdvance parks the outgoing thread while the outer interrupt
    // is still active.
    CpuCoreDeferFinish(&g_core, 7);
    __Check(g_core.PendingFinish == 7, 2);

    // A nested interrupt can still be using the outgoing stack, so the
    // thread stays parked until the outer interrupt has returned.
    CpuCoreEnterInterrupt(&g_context, 0);
    __Check(g_finishCalls == 0 && g_core.PendingFinish == 7, 3);
    CpuCoreExitInterrupt(&g_context, 0);
    __Check(g_finishCalls == 0 && g_core.PendingFinish == 7, 4);
    CpuCoreExitInterrupt(&g_context, 0);
    __Check(g_finishCalls == 0 && g_core.PendingFinish == 7, 5);

    // The next outer interrupt entry finishes the parked thread, once.
    CpuCoreEnterInterrupt(&g_context, 0);
    __Check(g_finishCalls == 1 && g_finishedId == 7, 6);
    __Check(g_slotFreeAtFinish && g_core.PendingFinish == UUID_INVALID, 7);
    CpuCoreExitInterrupt(&g_context, 0);

    // The slot is free again, and a later outer entry has nothing to finish.
    CpuCoreEnterInterrupt(&g_context, 0);
    __Check(g_finishCalls == 1, 8);
    CpuCoreExitInterrupt(&g_context, 0);

    // Only the running core may park a thread, as the slot is drained by that
    // core's own interrupt entry.
    __Check(__Panics(__DeferOnOtherCore, 23), 9);
    __Check(g_otherCore.PendingFinish == UUID_INVALID, 10);

    // There is one slot per core. A second thread parked before the drain
    // would overwrite the first and leave it unfinished, so it is refused.
    CpuCoreDeferFinish(&g_core, 21);
    __Check(__Panics(__DeferOnCurrentCore, 22), 11);
    __Check(g_core.PendingFinish == 21, 12);

    CpuCoreEnterInterrupt(&g_context, 0);
    __Check(g_finishCalls == 2 && g_finishedId == 21, 13);
    CpuCoreExitInterrupt(&g_context, 0);

    __asm__ volatile("svc 0" :: "r"(result), "r"(number) : "memory");
    __builtin_unreachable();
}
