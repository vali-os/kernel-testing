/* Heap locking must not sleep or be preempted while an IRQ caller waits. */
#define LIBC_KERNEL
#include "../kernel/memory/heap.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static irqstate_t irq = 1;
static int busy, polls, depth;
static SystemMachine_t machine;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) {}
void DebugPanic(int scope, Context_t* context, const char* message, ...) { abort(); }
irqstate_t InterruptDisable(void) { irqstate_t old = irq; irq = 0; return old; }
irqstate_t InterruptRestoreState(irqstate_t state) { irqstate_t old = irq; irq = state; return old; }
void MemorySpaceSyncProcessPending(void) { CHECK(!irq); polls++; busy = 0; }
void MutexLock(Mutex_t* m) { CHECK(0); } // the old heap's sleeping path is forbidden
oserr_t MutexTryLock(Mutex_t* m) { CHECK(!irq); if (busy) return OS_EBUSY; depth++; return OS_EOK; }
void MutexUnlock(Mutex_t* m) { CHECK(!irq && depth > 0); depth--; }
void MutexConstruct(Mutex_t* m, unsigned flags) {}
void SpinlockConstruct(Spinlock_t* l) { memset(l, 0, sizeof(*l)); }
void SpinlockAcquireIrq(Spinlock_t* l) { l->IrqState = InterruptDisable(); }
void SpinlockReleaseIrq(Spinlock_t* l) { InterruptRestoreState(l->IrqState); }
SystemMachine_t* GetMachine(void) { return &machine; }
uuid_t ArchGetProcessorCoreId(void) { return 0; }
size_t GetMemorySpacePageSize(void) { return 4096; }
MemorySpace_t* GetCurrentMemorySpace(void) { return NULL; }
oserr_t MemorySpaceMap(MemorySpace_t* ms, struct MemorySpaceMapOptions* o, vaddr_t* out) { return OS_EOOM; }
oserr_t MemorySpaceUnmap(MemorySpace_t* ms, vaddr_t p, size_t size) { abort(); }
oserr_t GetMemorySpaceMapping(MemorySpace_t* ms, uintptr_t p, int count, uintptr_t* out) { abort(); }
int main(void)
{
    MemoryCache_t cache = {.Flags = HEAP_SINGLE_SLAB, .ObjectSize = 32, .ObjectCount = 2};
    uintptr_t data[8] = {0};
    uint8_t bitmap = 0;
    MemorySlab_t slab = {.Address = data, .FreeBitmap = &bitmap, .NumberOfFreeObjects = 2};
    ELEMENT_INIT(&slab.Header, 0, &slab);
    list_construct(&cache.FreeSlabs); list_construct(&cache.PartialSlabs); list_construct(&cache.FullSlabs);
    list_append(&cache.FreeSlabs, &slab.Header);
    cache.NumberOfFreeObjects = 2;
    busy = 1;
    void* a = MemoryCacheAllocate(&cache);
    CHECK(a == data && irq == 1 && polls == 1 && depth == 0);
    void* b = MemoryCacheAllocate(&cache);
    CHECK(b == (char*)data + 32 && irq == 1);
    CHECK(MemoryCacheAllocate(&cache) == NULL && irq == 1 && !depth);
    MemoryCacheFree(&cache, a);
    CHECK(irq == 1 && !depth && cache.NumberOfFreeObjects == 1);
    irq = 0;
    CHECK(MemoryCacheAllocate(&cache) == a && !irq && !depth);
    MemoryCacheFree(&cache, a);
    CHECK(!irq && !depth);
    irq = 1;
    CHECK(__CacheContainsAddress(&cache, (uintptr_t)b) && irq && !depth);
    MemoryCache_t empty = {0}; // slab allocation failure restores IRQs too
    CHECK(MemoryCacheAllocate(&empty) == NULL && irq && !depth);
    struct { MemoryAtomicCache_t cache; uintptr_t slots[2]; } local = {{0, 2}, {0}};
    cache.AtomicCaches = (uintptr_t)&local;
    MemoryCacheFree(&cache, a);
    CHECK(irq && local.cache.Available == 1 && local.slots[0] == (uintptr_t)a);
    CHECK(MemoryCacheAllocate(&cache) == a && irq && local.cache.Available == 0);
    puts("Heap IRQ ownership, contention, failure and local-cache regressions passed");
    return 0;
}
