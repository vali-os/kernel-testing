/** Exercise the real handle table through growth, rollback and memory pressure. */
static void
__TestHandleIndex(
    _In_ const struct OSDmaOpenParameters* parameters,
    _In_ const struct OSDmaMapParameters*  map)
{
    uuid_t                    handles[128] = { 0 };
    uuid_t                    extra = UUID_INVALID;
    uuid_t                    generation;
    struct __DmaRecord*        held;
    struct __DmaRecord*        acquired;
    struct OSDmaMappingResult result = { 0 };
    unsigned int              i;
    size_t                    capacity;
    int                       calls;
    int                       unlockedFrees;

    CHECK(g_dmaRecords.capacity == 0 && g_indexAllocations == 0);
    // Either allocation required to construct the table can fail cleanly.
    for (i = 0; i < 2; i++) {
        g_indexFailAfter = (int)i;
        CHECK(ScDmaOpen(7, parameters, &extra) == OS_EOOM);
        CHECK(extra == UUID_INVALID && g_live == 0 && g_contexts == 0);
        CHECK(g_indexAllocations == 0 && g_dmaRecords.capacity == 0);
    }
    g_indexFailAfter = -1;

    for (i = 0; i < 12; i++) {
        CHECK(ScDmaOpen(7, parameters, &handles[i]) == OS_EOK);
    }
    CHECK(g_dmaRecords.element_count == g_dmaRecords.grow_count);
    g_indexFailAfter = 0;
    CHECK(ScDmaOpen(7, parameters, &extra) == OS_EOOM && extra == UUID_INVALID);
    CHECK(ScDmaMapShm(handles[0], 99, map, &result) == OS_EOOM);
    CHECK(result.Handle == UUID_INVALID && g_maps == 0 && g_live == 12);
    CHECK(g_dmaRecords.element_count == 12);
    g_indexFailAfter = -1;

    // Roll back an inserted but unpublished record after a failed user copy.
    g_copyFailure = 1;
    CHECK(ScDmaMapShm(handles[0], 99, map, &result) == OS_EINVALPARAMS);
    CHECK(g_maps == 0 && g_live == 12 && g_dmaRecords.element_count == 12);
    g_copyFailure = 0;
    CHECK(__Begin(handles[0], 0, &held) == OS_EOK);
    for (i = 12; i < 128; i++) {
        CHECK(ScDmaOpen(7, parameters, &handles[i]) == OS_EOK);
    }
    CHECK(held->Handle == handles[0] && held->Context != NULL);
    CHECK(__Begin(handles[0], 0, &acquired) == OS_EBUSY);
    __End(held);

    // Lookup requires no allocation, including after several table resizes.
    g_oom = 1;
    g_indexFailAfter = 0;
    for (i = 0; i < 128; i++) {
        CHECK(__Begin(handles[i], 0, &acquired) == OS_EOK);
        CHECK(acquired->Handle == handles[i] && acquired->Owner == g_security.Handle);
        __End(acquired);
    }
    g_oom = 0;

    // Every removal, including crossing shrink thresholds, must be allocation
    // free and keep capacity until the final record detaches the whole index.
    capacity = g_dmaRecords.capacity;
    calls = g_indexAllocationCalls;
    unlockedFrees = g_indexUnlockedFrees;
    g_indexFailAfter = 0;
    for (i = 0; i < 128; i++) {
        CHECK(ScDmaClose(handles[i]) == OS_EOK);
        CHECK(ScDmaClose(handles[i]) == OS_ENOENT);
        CHECK(g_indexAllocationCalls == calls);
        if (i != 127) {
            CHECK(g_dmaRecords.capacity == capacity);
            CHECK(__Begin(handles[127], 0, &acquired) == OS_EOK);
            __End(acquired);
        }
    }
    g_indexFailAfter = -1;
    CHECK(g_dmaRecords.capacity == 0 && g_indexAllocations == 0);
    CHECK(g_live == 0 && g_contexts == 0);
    CHECK(g_indexUnlockedFrees == unlockedFrees + 2);
    CHECK(g_mutexAcquisitions > 0 && !g_locked);

    // Long-lived handles can collide with later generations in the same bucket.
    // Equality must select the full ID through insertions, removal and shrinking.
    for (i = 0; i < 16; i++) {
        g_dmaGeneration += 255;
        CHECK(ScDmaOpen(7, parameters, &handles[i]) == OS_EOK);
    }
    for (i = 16; i != 0; i--) {
        CHECK(__Begin(handles[i - 1], 0, &acquired) == OS_EOK);
        CHECK(acquired->Handle == handles[i - 1]);
        __End(acquired);
        CHECK(ScDmaClose(handles[i - 1]) == OS_EOK);
    }
    CHECK(g_live == 0 && g_indexAllocations == 0);

    // Reserved entries are invisible until setup and copy-out both succeed.
    CHECK(__Create(&held) == OS_EOK);
    CHECK(__Begin(held->Handle, 0, &acquired) == OS_ENOENT);
    __Remove(held);
    CHECK(g_live == 0 && g_indexAllocations == 0);

    generation = g_dmaGeneration;
    g_dmaGeneration = UINT32_MAX;
    CHECK(ScDmaOpen(7, parameters, &extra) == OS_EOVERFLOW);
    CHECK(extra == UUID_INVALID && g_live == 0 && g_indexAllocations == 0);
    g_dmaGeneration = generation;
    puts("DMA index: growth, stable records, rollback, allocation-free removal and unlocked destruction passed");
}

static void
__TestDefaultTableShrinking(void)
{
    hashtable_t table = { 0 };
    struct __DmaEntry entry = { 0 };
    size_t capacity;
    unsigned int i;

    // The DMA opt-out must not change the default behavior of other users.
    CHECK(hashtable_construct(&table, 16, sizeof(entry), __HandleHash, __HandleCompare) == 0);
    CHECK(table.flags == 0);
    for (i = 1; i <= 128; i++) {
        entry.Handle = i;
        hashtable_set(&table, &entry);
    }
    capacity = table.capacity;
    for (i = 1; i < 128; i++) {
        entry.Handle = i;
        CHECK(hashtable_remove(&table, &entry) != NULL);
    }
    CHECK(table.capacity < capacity);
    entry.Handle = 128;
    CHECK(hashtable_get(&table, &entry) != NULL);
    hashtable_destroy(&table);
    CHECK(g_indexAllocations == 0);
}

static struct OSDmaOpenParameters g_reopenParameters;
static uuid_t g_reopenedHandle;

static void
__OpenDuringIndexDestruction(void)
{
    CHECK(g_dmaRecords.capacity == 0 && !g_locked);
    CHECK(ScDmaOpen(7, &g_reopenParameters, &g_reopenedHandle) == OS_EOK);
}

static void
__TestDetachedIndex(
    _In_ struct OSDmaOpenParameters parameters)
{
    uuid_t handle = UUID_INVALID;
    struct __DmaRecord* record;

    CHECK(ScDmaOpen(7, &parameters, &handle) == OS_EOK);
    g_reopenParameters = parameters;
    g_beforeIndexFree = __OpenDuringIndexDestruction;
    // Simulate a new syscall after unlock but before old storage is freed.
    CHECK(ScDmaClose(handle) == OS_EOK);
    CHECK(g_dmaRecords.element_count == 1 && g_indexAllocations == 2);
    CHECK(__Begin(g_reopenedHandle, 0, &record) == OS_EOK);
    __End(record);
    CHECK(ScDmaClose(g_reopenedHandle) == OS_EOK);
    CHECK(g_dmaRecords.capacity == 0 && g_indexAllocations == 0);
    CHECK(g_live == 0 && g_contexts == 0);
}
