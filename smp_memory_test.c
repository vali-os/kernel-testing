/* Kernel clones must return their own usable handle and own their mappings. */
#include "../kernel/memory/ms_create.c"
#include "../kernel/memory/ms_struct.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static SystemMachine_t g_machine;
static unsigned int g_allocations, g_destroyed, g_cloned;
static int g_failAllocation, g_failClone, g_failHandle;
static MemorySpace_t* g_published;
static HandleDestructorFn g_destructor;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
void DebugPanic(int scope, Context_t* context, const char* message, ...) { (void)scope; (void)context; (void)message; abort(); }
void* kmalloc(size_t size) { if (g_failAllocation) return NULL; g_allocations++; return calloc(1, size); }
void kfree(void* p) { CHECK(g_allocations > 0); g_allocations--; free(p); }
SystemMachine_t* GetMachine(void) { return &g_machine; }
size_t GetMemorySpacePageSize(void) { return 4096; }
void DynamicMemoryPoolConstruct(DynamicMemoryPool_t* pool, uintptr_t start, size_t length, size_t chunk) {
    (void)pool; (void)start; (void)length; (void)chunk;
}
void DynamicMemoryPoolDestroy(DynamicMemoryPool_t* pool) { (void)pool; }
oserr_t MmuCloneVirtualSpace(MemorySpace_t* parent, MemorySpace_t* child, int inherit) {
    CHECK(parent == NULL && inherit == 0);
    CHECK(child != NULL && child->Flags == MEMORY_SPACE_INHERIT);
    if (g_failClone) return OS_EOOM;
    g_cloned++; return OS_EOK;
}
oserr_t MmuDestroyVirtualSpace(MemorySpace_t* space) { CHECK(space != NULL); g_destroyed++; return OS_EOK; }
uuid_t CreateHandle(HandleType_t type, HandleDestructorFn destructor, void* resource) {
    CHECK(type == HandleTypeMemorySpace);
    if (g_failHandle) return UUID_INVALID;
    g_destructor = destructor; g_published = resource; return 42;
}
oserr_t DestroyHandle(uuid_t handle) { (void)handle; abort(); }
MemorySpace_t* GetCurrentMemorySpace(void) { abort(); }
MemorySpace_t* GetDomainMemorySpace(void) { abort(); }
uuid_t GetCurrentMemorySpaceHandle(void) { abort(); }
void* LookupHandleOfType(uuid_t handle, HandleType_t type) { (void)handle; (void)type; abort(); }
oserr_t AcquireHandleOfType(uuid_t handle, HandleType_t type, void** result) { (void)handle; (void)type; (void)result; abort(); }
uuid_t MSContextNew(void) { abort(); }
void MSContextDelete(struct MSContext* context) { CHECK(context == NULL); }
int main(void) {
    uuid_t handle = UUID_INVALID;
    CHECK(CreateMemorySpace(MEMORY_SPACE_INHERIT, &handle) == OS_EOK);
    CHECK(handle == 42 && g_published != NULL && g_cloned == 1);
    g_destructor(g_published);
    CHECK(g_allocations == 0 && g_destroyed == 1);
    g_failAllocation = 1;
    CHECK(CreateMemorySpace(MEMORY_SPACE_INHERIT, &handle) == OS_EOOM);
    g_failAllocation = 0; g_failClone = 1;
    CHECK(CreateMemorySpace(MEMORY_SPACE_INHERIT, &handle) == OS_EOOM);
    CHECK(g_allocations == 0 && g_destroyed == 1);
    g_failClone = 0; g_failHandle = 1;
    CHECK(CreateMemorySpace(MEMORY_SPACE_INHERIT, &handle) == OS_EOOM);
    CHECK(g_allocations == 0 && g_destroyed == 2 && handle == UUID_INVALID);
    puts("SMP memory spaces: independent handles, ownership and failure cleanup passed");
    return 0;
}
