// Use the real handle implementation while keeping its allocation accounting
// separate from the access payload fault-injection allocator.
#include <stdlib.h>
#include <assert.h>
#define kmalloc __HandleTestAllocate
#define kfree __HandleTestFree
#include "../kernel/handle.c"
#include "handle_set_stubs.h"
#undef kmalloc
#undef kfree

static unsigned int g_handleAllocations;
static unsigned int g_handleAllocationCalls;

void*
__HandleTestAllocate(size_t size)
{
    void* memory = calloc(1, size);
    g_handleAllocationCalls++;
    if (memory != NULL) {
        g_handleAllocations++;
    }
    return memory;
}

void
__HandleTestFree(void* memory)
{
    if (memory != NULL) {
        assert(g_handleAllocations != 0);
        g_handleAllocations--;
        free(memory);
    }
}

void* dsalloc(size_t size) { g_handleAllocationCalls++; return malloc(size); }
void dsfree(void* memory) { free(memory); }

void
__TestHandlesDrain(void)
{
    element_t* entry;

    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

void
__TestHandlesInitialize(void)
{
    assert(InitializeHandles() == OS_EOK);
}

void
__TestHandlesFinish(void)
{
    __TestHandlesDrain();
    assert(g_handles.element_count == 0 && g_handleAllocations == 0);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
}

uuid_t
__TestNextHandleId(uuid_t next)
{
    return atomic_exchange(&g_nextHandleId, next);
}

unsigned int
__TestHandleAllocationCalls(void)
{
    return g_handleAllocationCalls;
}
