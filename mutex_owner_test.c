/* Contention must not resolve a mutex owner through the locked handle table. */
#include "../kernel/sync/mutex.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static SystemMachine_t machine;
static Mutex_t mutex;
static int attempts, releaseAt;
uuid_t ThreadCurrentHandle(void) {
    if (++attempts == releaseAt) atomic_store(&mutex.Owner, UUID_INVALID);
    return 7;
}
void* LookupHandleOfType(uuid_t handle, HandleType_t type) { CHECK(0); return NULL; }
SchedulerObject_t* ThreadSchedulerHandle(Thread_t* t) { CHECK(0); return NULL; }
uuid_t SchedulerObjectGetAffinity(SchedulerObject_t* s) { CHECK(0); return 0; }
SystemCpuCore_t* CpuCoreCurrent(void) { CHECK(0); return NULL; }
uuid_t CpuCoreId(SystemCpuCore_t* c) { CHECK(0); return 0; }
SystemMachine_t* GetMachine(void) { return &machine; }
irqstate_t InterruptDisable(void) { return 0; }
irqstate_t InterruptRestoreState(irqstate_t s) { return 0; }
void SpinlockConstruct(Spinlock_t* l) { memset(l, 0, sizeof(*l)); }
void SpinlockAcquire(Spinlock_t* l) {}
void SpinlockRelease(Spinlock_t* l) {}
void SpinlockAcquireIrq(Spinlock_t* l) {}
void SpinlockReleaseIrq(Spinlock_t* l) {}
oserr_t SchedulerBlock(list_t* l, OSTimestamp_t* d) { CHECK(0); return OS_EUNKNOWN; }
oserr_t SchedulerQueueObject(SchedulerObject_t* s) { CHECK(0); return OS_EUNKNOWN; }
oserr_t SchedulerGetTimeoutReason(void) { CHECK(0); return OS_EUNKNOWN; }
void ArchThreadYield(void) { CHECK(0); }
int main(void) {
    atomic_store(&machine.NumberOfActiveCores, 4);
    MutexConstruct(&mutex, 0);
    atomic_store(&mutex.Owner, 9);
    releaseAt = 5;
    CHECK(__TrySpinOnOwner(&mutex, 9) == OS_EOK);
    CHECK(mutex.Owner == 7 && attempts == 5);
    attempts = 0; releaseAt = 0; mutex.Owner = 9;
    CHECK(__TrySpinOnOwner(&mutex, 9) == OS_EUNKNOWN);
    CHECK(attempts == MUTEX_SPINS && mutex.Owner == 9);
    puts("Mutex contention: no owner handle lookup, bounded retry passed");
    return 0;
}
