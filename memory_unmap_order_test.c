/* Physical pages must not be recycled until remote translations are gone. */
#include "../kernel/memory/ms_unmap.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int g_cleared, g_synchronized, g_freed, g_noPages, g_failAllocation;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) {}
void* kmalloc(size_t n) { return g_failAllocation ? NULL : malloc(n); }
void kfree(void* p) { free(p); }
size_t GetMemorySpacePageSize(void) { return 4096; }
void MutexLock(Mutex_t* mutex) { abort(); }
void MutexUnlock(Mutex_t* mutex) { abort(); }
struct MSAllocation* MSAllocationLookup(struct MSContext* context, vaddr_t address) { abort(); }
oserr_t MSAllocationFree(struct MSContext* context, vaddr_t address, size_t length, struct MSAllocation** original) { abort(); }
int DynamicMemoryPoolContains(DynamicMemoryPool_t* pool, uintptr_t address) { abort(); }
void DynamicMemoryPoolFree(DynamicMemoryPool_t* pool, uintptr_t address) { abort(); }
int StaticMemoryPoolContains(StaticMemoryPool_t* pool, uintptr_t address) { abort(); }
void StaticMemoryPoolFree(StaticMemoryPool_t* pool, uintptr_t address) { abort(); }
SystemMachine_t* GetMachine(void) { abort(); }
void* LookupHandleOfType(uuid_t handle, HandleType_t type) { abort(); }
oserr_t ArchMmuClearVirtualPages(MemorySpace_t* space, vaddr_t address, int count, paddr_t* pages, int* freed, int* cleared) {
    CHECK(count == 1 && !g_freed);
    pages[0] = 0x12345000; *freed = *cleared = !g_noPages;
    g_cleared++; return OS_EOK;
}
void MSSync(MemorySpace_t* space, uintptr_t address, size_t length) {
    CHECK(g_cleared == 1 && !g_freed);
    g_synchronized++;
}
void FreePhysicalMemory(int count, const uintptr_t* pages) {
    CHECK(g_synchronized == 1 && count == 1 && pages[0] == 0x12345000);
    g_freed++;
}
int main(void) {
    CHECK(__ClearPhysicalPages(NULL, 0x10001000, 4096) == OS_EOK);
    CHECK(g_synchronized == 1 && g_freed == 1);
    g_synchronized = g_freed = g_cleared = 0; g_noPages = 1;
    CHECK(__ClearPhysicalPages(NULL, 0x10001000, 4096) == OS_EOK);
    CHECK(g_cleared == 1 && !g_synchronized && !g_freed);
    g_failAllocation = 1;
    CHECK(__ClearPhysicalPages(NULL, 0x10001000, 4096) == OS_EOOM);
    CHECK(g_cleared == 1 && !g_freed);
    puts("Unmap: shootdown before page reuse, absent mappings and allocation failure passed");
    return 0;
}
