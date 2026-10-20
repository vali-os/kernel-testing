/* Real shootdown and IRQ spinlock code: waiters must acknowledge without scheduling. */
#define LIBC_KERNEL
#include "../kernel/memory/ms_sync.c"
#include "../kernel/sync/spinlock.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
__declspec(dllimport) void* __stdcall CreateThread(void*, size_t, unsigned long (__stdcall *)(void*), void*, unsigned long, unsigned long*);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
__declspec(dllimport) int __stdcall CloseHandle(void*);
__declspec(dllimport) void __stdcall Sleep(unsigned long);
static SystemMachine_t g_machine;
static SystemCpuCore_t g_cores[4];
static _Thread_local unsigned int g_coreId;
static _Thread_local irqstate_t g_irq = 1;
static _Atomic(int) g_ipi[4], g_flushes[4], g_ready, g_done;
static Spinlock_t g_outerLock = OS_SPINLOCK_INIT;
static int g_mode, g_expectedPanic, g_failSend;
static InterruptHandler_t g_handler;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) {}
void DebugPanic(int scope, Context_t* context, const char* message, ...) {
    CHECK(g_expectedPanic);
    CHECK(atomic_load(&g_syncActive));
    puts("Memory sync: failed delivery/missing acknowledgement stops without releasing the request");
    exit(0);
}
SystemMachine_t* GetMachine(void) { return &g_machine; }
SystemCpuCore_t* CpuCoreCurrent(void) { return &g_cores[g_coreId]; }
SystemCpuCore_t* CpuCoreNext(SystemCpuCore_t* core) { return core->Link; }
SystemCpuState_t CpuCoreState(SystemCpuCore_t* core) { return core->State; }
uuid_t CpuCoreId(SystemCpuCore_t* core) { return core->Id; }
irqstate_t InterruptDisable(void) { irqstate_t old = g_irq; g_irq = 0; return old; }
irqstate_t InterruptRestoreState(irqstate_t state) { irqstate_t old = g_irq; g_irq = state; return old; }
uuid_t InterruptRegister(InterruptDescriptor_t* desc, unsigned int flags) { g_handler = desc->ResourceTable.Handler; return 42; }
oserr_t ArchProcessorSendInterrupt(uuid_t target, uuid_t interrupt) {
    CHECK(interrupt == 42 && target != g_coreId);
    if (g_failSend) return OS_EUNKNOWN;
    atomic_store(&g_ipi[target], 1); return OS_EOK;
}
void CpuInvalidateMemoryCache(void* address, size_t size) {
    CHECK(!g_irq && address == (void*)0x10001000 && size == 4096);
    atomic_fetch_add(&g_flushes[g_coreId], 1);
}
static void Poll(void) {
    if (atomic_exchange(&g_ipi[g_coreId], 0)) g_handler(NULL, NULL);
    MemorySpaceSyncProcessPending();
}
static unsigned long __stdcall Worker(void* argument) {
    g_coreId = (unsigned int)(uintptr_t)argument;
    atomic_fetch_add(&g_ready, 1);
    if (g_mode == 0) {
        SpinlockAcquireIrq(&g_outerLock);
        SpinlockReleaseIrq(&g_outerLock);
        // The IPI may arrive after polling already completed the request.
        Poll();
    } else {
        for (int i = 0; i < 100; i++) MSSync(NULL, 0x10001000, 4096);
        atomic_fetch_add(&g_done, 1);
        while (atomic_load(&g_done) != 4) { Poll(); Sleep(0); }
    }
    CHECK(g_irq == 1 && atomic_load(&g_cores[g_coreId].MemorySync) == NULL);
    return 0;
}
int main(int argc, char** argv) {
    MemorySpaceSyncProcessPending();
    MSSync(NULL, 0x10001000, 4096); // single CPU, before registration
    CHECK(g_irq == 1 && atomic_load(&g_flushes[0]) == 1);
    for (unsigned i = 0; i < 4; i++) {
        g_cores[i].Id = i; g_cores[i].State = CpuStateRunning;
        g_cores[i].Link = i == 3 ? NULL : &g_cores[i + 1];
    }
    g_machine.Processor.Cores = &g_cores[0];
    atomic_store(&g_machine.NumberOfActiveCores, 4);
    MemorySpaceSyncInitialize();
    if (argc > 1) {
        g_expectedPanic = 1; g_failSend = strcmp(argv[1], "--send-failure") == 0;
        MSSync(NULL, 0x10001000, 4096);
        CHECK(0);
    }
    void* workers[3];
    SpinlockAcquireIrq(&g_outerLock);
    for (unsigned i = 1; i < 4; i++) workers[i - 1] = CreateThread(NULL, 0, Worker, (void*)(uintptr_t)i, 0, NULL);
    while (atomic_load(&g_ready) != 3) Sleep(0);
    MSSync(NULL, 0x10001000, 4096);
    CHECK(g_irq == 0);
    for (int i = 1; i < 4; i++) CHECK(atomic_load(&g_flushes[i]) == 1);
    SpinlockReleaseIrq(&g_outerLock);
    for (int i = 0; i < 3; i++) { CHECK(WaitForSingleObject(workers[i], 10000) == 0); CloseHandle(workers[i]); }
    for (int i = 0; i < 4; i++) atomic_store(&g_flushes[i], 0);
    g_mode = 1;
    for (unsigned i = 1; i < 4; i++) workers[i - 1] = CreateThread(NULL, 0, Worker, (void*)(uintptr_t)i, 0, NULL);
    for (int i = 0; i < 100; i++) MSSync(NULL, 0x10001000, 4096);
    atomic_fetch_add(&g_done, 1);
    while (atomic_load(&g_done) != 4) { Poll(); Sleep(0); }
    for (int i = 0; i < 3; i++) { CHECK(WaitForSingleObject(workers[i], 10000) == 0); CloseHandle(workers[i]); }
    for (int i = 0; i < 4; i++) CHECK(atomic_load(&g_flushes[i]) == 400);
    CHECK(g_irq == 1 && atomic_load(&g_syncActive) == 0);
    puts("Memory sync: IRQ-disabled lock waiters, delayed IPIs and 400 concurrent shootdowns passed");
    return 0;
}
