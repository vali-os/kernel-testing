/** Retain janitor work across real hash-table relocation. No scheduler timing
 * is needed: tests decide when queued destructors run, while using the same
 * handle, hash-table and intrusive queue code as the kernel.
 */
#include "../kernel/handle.c"
#include <stdio.h>
#include <stdlib.h>

static unsigned g_destroyed[1024];
static size_t g_live;
static bool g_failRecords, g_failTable;
void *kmalloc(size_t size)
{
    if (g_failRecords)
        return NULL;
    void *p = malloc(size);
    if (p)
        g_live++;
    return p;
}
void kfree(void *p)
{
    if (p)
    {
        assert(g_live);
        g_live--;
        free(p);
    }
}
void *dsalloc(size_t size)
{
    return g_failTable ? NULL : malloc(size);
}
void dsfree(void *p)
{
    free(p);
}

static void DestroyResource(void *value)
{
    unsigned index = (unsigned)(uintptr_t)value;
    assert(index < 1024 && !g_destroyed[index]);
    g_destroyed[index]++;
}

static void DrainJanitor(void)
{
    element_t *entry;
    while ((entry = queue_pop(&g_cleanQueue)) != NULL)
    {
        __CleanupHandle((struct ResourceHandle *)entry);
    }
}

/** Destructors may allocate more handles before returning to janitor cleanup. */
static void DestroyWithChurn(void *unused)
{
    (void)unused;
    uuid_t temporary[128];
    for (unsigned i = 0; i < 128; ++i)
    {
        temporary[i] = CreateHandle(HandleTypeGeneric, NULL, NULL);
        assert(temporary[i] != UUID_INVALID);
    }
    for (unsigned i = 0; i < 128; ++i)
        assert(DestroyHandle(temporary[i]) == OS_EOK);
}

int main(void)
{
    uuid_t handles[512];
    assert(InitializeHandles() == OS_EOK);
    handles[0] = CreateHandle(HandleTypeThread, DestroyResource, (void *)(uintptr_t)1);
    assert(handles[0] != UUID_INVALID);
    assert(DestroyHandle(handles[0]) == OS_EOK);
    // An acquired record must survive table movement even outside the lock.
    uuid_t pinnedId = CreateHandle(HandleTypeGeneric, NULL, (void *)(uintptr_t)1000);
    struct ResourceHandle *pinned = __AcquireHandle(pinnedId);
    assert(pinned != NULL);
    size_t originalCapacity = g_handles.capacity;
    // Rehash the table while the janitor still owns its first queue entry.
    for (unsigned i = 1; i < 512; ++i)
    {
        handles[i] = CreateHandle(HandleTypeSHM, DestroyResource, (void *)(uintptr_t)(i + 1));
        assert(handles[i] != UUID_INVALID);
    }
    assert(g_handles.capacity > originalCapacity);
    assert(pinned->ID == pinnedId && pinned->Resource == (void *)(uintptr_t)1000);
    assert(DestroyHandle(pinnedId) == OS_EINCOMPLETE);
    assert(DestroyHandle(pinnedId) == OS_EOK);
    DrainJanitor();
    assert(g_destroyed[1] == 1);
    for (unsigned i = 1; i < 512; ++i)
    {
        assert(!g_destroyed[i + 1]);
        assert(LookupHandleOfType(handles[i], HandleTypeSHM) == (void *)(uintptr_t)(i + 1));
        assert(DestroyHandle(handles[i]) == OS_EOK);
        assert(DestroyHandle(handles[i]) == OS_ENOENT);
    }
    // A failed optional table shrink must not retain dangling index entries.
    g_failTable = true;
    DrainJanitor();
    g_failTable = false;
    for (unsigned i = 1; i <= 512; ++i)
        assert(g_destroyed[i] == 1);
    assert(g_handles.element_count == 0 && !g_live);
    uuid_t reentrant = CreateHandle(HandleTypeGeneric, DestroyWithChurn, NULL);
    assert(DestroyHandle(reentrant) == OS_EOK);
    DrainJanitor();
    assert(g_handles.element_count == 0 && !g_live);
    g_failRecords = true;
    assert(CreateHandle(HandleTypeGeneric, NULL, NULL) == UUID_INVALID);
    g_failRecords = false;
    // Fill precisely to the next growth boundary, then fail its allocation.
    size_t count = g_handles.grow_count;
    uuid_t *growth = malloc(count * sizeof(*growth));
    assert(growth);
    for (size_t i = 0; i < count; ++i)
    {
        growth[i] = CreateHandle(HandleTypeGeneric, NULL, NULL);
        assert(growth[i] != UUID_INVALID);
    }
    g_failTable = true;
    assert(CreateHandle(HandleTypeGeneric, NULL, NULL) == UUID_INVALID);
    assert(g_live == count && g_handles.element_count == count);
    g_failTable = false;
    for (size_t i = 0; i < count; ++i)
        assert(DestroyHandle(growth[i]) == OS_EOK);
    DrainJanitor();
    assert(g_handles.element_count == 0 && !g_live);
    free(growth);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("handle lifetime: pending cleanup survives table growth and shrink");
    return 0;
}
