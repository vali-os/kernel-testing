/* Reproduce a remote wake after FutexWait's value check, before queue insertion. */
#include "../kernel/sync/futex.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
__declspec(dllimport) void* __stdcall CreateThread(void*, size_t, unsigned long (__stdcall *)(void*), void*, unsigned long, unsigned long*);
__declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void*, unsigned long);
__declspec(dllimport) int __stdcall CloseHandle(void*);
__declspec(dllimport) void __stdcall Sleep(unsigned long);

static _Atomic(int) g_futex, g_beforeInsert, g_wakeAttempted, g_queued;
static _Thread_local int g_irqEnabled = 1;
static element_t g_waiter;
static int g_failAllocation, g_failBlock, g_forkResult = OS_EFORKED;
static int g_forkCalls;
static MemorySpace_t g_space;

void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
void* kmalloc(size_t size) { return g_failAllocation ? NULL : malloc(size); }
void kfree(void* p) { free(p); }
irqstate_t InterruptDisable(void) { int old = g_irqEnabled; g_irqEnabled = 0; return old; }
irqstate_t InterruptRestoreState(irqstate_t state) { int old = g_irqEnabled; g_irqEnabled = state; return old; }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) {
    int old = InterruptDisable();
    while (atomic_exchange(&lock->Current, 1)) { Sleep(0); }
    lock->IrqState = old;
}
void SpinlockReleaseIrq(Spinlock_t* lock) {
    int old = lock->IrqState;
    atomic_store(&lock->Current, 0);
    InterruptRestoreState(old);
}
uuid_t ArchGetProcessorCoreId(void) { return 0; }
SystemCpuCore_t* GetProcessorCore(uuid_t id) { (void)id; return (SystemCpuCore_t*)1; }
Thread_t* CpuCoreCurrentThread(SystemCpuCore_t* core) { (void)core; return (Thread_t*)1; }
SchedulerObject_t* ThreadSchedulerHandle(Thread_t* thread) { (void)thread; return (SchedulerObject_t*)&g_waiter; }
MemorySpace_t* GetCurrentMemorySpace(void) { return &g_space; }
oserr_t GetMemorySpaceMapping(MemorySpace_t* space, vaddr_t address, int count, uintptr_t* result) {
    (void)space; CHECK(count == 1); *result = address; return OS_EOK;
}
oserr_t ThreadFork(OSAsyncContext_t* context) { (void)context; g_forkCalls++; return g_forkResult; }
oserr_t SchedulerBlock(list_t* queue, OSTimestamp_t* deadline) {
    (void)deadline;
    if (g_failBlock) { return OS_EUNKNOWN; }
    atomic_store(&g_beforeInsert, 1);
    for (int i = 0; !atomic_load(&g_wakeAttempted) && i < 5000; i++) { Sleep(1); }
    CHECK(atomic_load(&g_wakeAttempted));
    // Give the waker a chance to expose the old check/enqueue gap.
    Sleep(20);
    ELEMENT_INIT(&g_waiter, 0, &g_waiter);
    list_append(queue, &g_waiter);
    return OS_EOK;
}
oserr_t SchedulerQueueObject(SchedulerObject_t* object) {
    CHECK(object == (SchedulerObject_t*)&g_waiter); atomic_store(&g_queued, 1); return OS_EOK;
}
void ArchThreadYield(void) {
    for (int i = 0; !atomic_load(&g_queued) && i < 5000; i++) { Sleep(1); }
    CHECK(atomic_load(&g_queued));
}
oserr_t SchedulerGetTimeoutReason(void) { return OS_EOK; }
static unsigned long __stdcall Wake(void* context) {
    (void)context;
    while (!atomic_load(&g_beforeInsert)) { Sleep(0); }
    atomic_store(&g_futex, 1);
    atomic_store(&g_wakeAttempted, 1);
    CHECK(FutexWake(&g_futex, 1, 0) == OS_EOK);
    return 0;
}
int main(void) {
    FutexInitialize();
    g_failAllocation = 1;
    CHECK(FutexWait(NULL, &g_futex, 0, 0, NULL, 0, 0, NULL) == OS_EOOM);
    CHECK(g_irqEnabled == 1);
    g_failAllocation = 0;
    for (int i = 0; i < 30; i++) {
        void* thread;
        atomic_store(&g_futex, 0); atomic_store(&g_beforeInsert, 0);
        atomic_store(&g_wakeAttempted, 0); atomic_store(&g_queued, 0);
        thread = CreateThread(NULL, 0, Wake, NULL, 0, NULL);
        CHECK(thread != NULL);
        CHECK(FutexWait(i & 1 ? (OSAsyncContext_t*)1 : NULL, &g_futex, 0, 0, NULL, 0, 0, NULL) == OS_EOK);
        CHECK(WaitForSingleObject(thread, 10000) == 0);
        CloseHandle(thread);
        CHECK(g_irqEnabled == 1);
    }
    CHECK(g_forkCalls == 15);
    CHECK(FutexWait(NULL, &g_futex, 0, 0, NULL, 0, 0, NULL) == OS_EINTERRUPTED);
    CHECK(g_irqEnabled == 1);
    atomic_store(&g_futex, 0);
    g_failBlock = 1;
    CHECK(FutexWait(NULL, &g_futex, 0, 0, NULL, 0, 0, NULL) == OS_EUNKNOWN);
    CHECK(g_irqEnabled == 1);
    g_forkResult = OS_EUNKNOWN;
    CHECK(FutexWait((OSAsyncContext_t*)1, &g_futex, 0, 0, NULL, 0, 0, NULL) == OS_EFORKED);
    CHECK(g_irqEnabled == 1);
    FutexItem_t* item = FutexGetNodeLocked(__GetBucket((uintptr_t)&g_futex), (uintptr_t)&g_futex, NULL);
    CHECK(item != NULL && atomic_load(&item->Waiters) == 0);
    CHECK(list_count(&item->BlockQueue) == 0);
    free(item);
    puts("Futex SMP: 30 check/enqueue races, async rechecks, mismatch and failure cleanup passed");
    return 0;
}
