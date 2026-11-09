// Failed initial mappings must remain reachable until rollback succeeds.
static void
__TestMapCleanup(
    _In_ uuid_t viewID,
    _In_ uuid_t otherID)
{
    struct MSContext*    context = __MSContextGet(&g_space);
    struct MSAllocation* allocation;
    SHMHandle_t          view;
    SHMHandle_t          other;
    SHMHandle_t          original;
    vaddr_t              address;
    vaddr_t              nextAddress;
    size_t               handles;
    int                  mode;

    assert(context->Allocations.head == NULL);
    handles = g_handles.element_count;
    for (mode = 0; mode < 3; mode++) {
        assert(SHMAttach(viewID, &view) == OS_EOK);
        original = view;
        address = g_nextAddress;

        // The MMU installs one page before failing. Rollback has a distinct error.
        g_failMap = true;
        g_failUnmapAddress = address;
        assert(ScSHMMap(&view, 0, 2 * PAGE_SIZE, SHM_ACCESS_READ) == OS_EDEVFAULT);
        g_failMap = false;
        assert(memcmp(&view, &original, sizeof(view)) == 0);
        assert(context->Allocations.count == 1);
        allocation = MSAllocationLookup(context, address);
        assert(allocation != NULL && allocation->SHM.CleanupPending);
        assert(__Page(address)->Flags != 0);

        // Failed retries must preserve the allocation without creating another.
        nextAddress = g_nextAddress;
        assert(SHMViewCleanup(view.ID) == OS_EDEVFAULT);
        assert(ScSHMDetach(&view) == OS_EDEVFAULT);
        assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EDEVFAULT);
        assert(memcmp(&view, &original, sizeof(view)) == 0);
        assert(context->Allocations.count == 1 && g_nextAddress == nextAddress);
        g_failUnmapAddress = 0;

        // Cleanup for another view must leave this allocation available for retry.
        assert(SHMAttach(otherID, &other) == OS_EOK);
        assert(SHMDetach(&other) == OS_EOK);
        assert(context->Allocations.count == 1);

        if (mode == 0) {
            assert(SHMViewCleanup(view.ID) == OS_EOK);
            assert(context->Allocations.head == NULL);
            assert(SHMViewCleanup(view.ID) == OS_EOK);
        } else if (mode == 1) {
            assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
            assert(context->Allocations.count == 1);
            assert(MSAllocationLookup(context, address) == NULL);
        }
        assert(ScSHMDetach(&view) == OS_EOK);
        assert(view.ID == UUID_INVALID && view.Buffer == NULL);
        assert(__Page(address)->Flags == 0);
        assert(context->Allocations.head == NULL);
        __DrainJanitor();
        assert(g_handles.element_count == handles);
    }
}

// Failed replacement mappings must remain reachable through map and detach.
static void
__TestRemapCleanup(
    _In_ uuid_t viewID,
    _In_ uuid_t otherID)
{
    struct MSContext* context = __MSContextGet(&g_space);
    SHMHandle_t       view;
    SHMHandle_t       other;
    SHMHandle_t       original;
    vaddr_t          replacement;
    vaddr_t          nextAddress;
    size_t           handles;
    int              mode;

    assert(context->Allocations.head == NULL);
    handles = g_handles.element_count;
    for (mode = 0; mode < 4; mode++) {
        assert(SHMAttach(viewID, &view) == OS_EOK);
        assert(SHMMap(&view, 100, 500, SHM_ACCESS_READ) == OS_EOK);
        original = view;
        replacement = g_nextAddress;

        // Distinct errors prove that replacement cleanup failure is reported.
        g_failUnmap = true;
        g_failUnmapAddress = replacement;
        assert(ScSHMMap(&view, PAGE_SIZE, PAGE_SIZE, SHM_ACCESS_READ) == OS_EDEVFAULT);
        assert(memcmp(&view, &original, sizeof(view)) == 0);
        assert(__Page(replacement)->Flags != 0);
        assert(context->Allocations.count == 2);

        // Neither retry may lose the record or allocate another replacement.
        nextAddress = g_nextAddress;
        assert(ScSHMDetach(&view) == OS_EDEVFAULT);
        assert(SHMMap(&view, PAGE_SIZE, PAGE_SIZE, SHM_ACCESS_READ) == OS_EDEVFAULT);
        assert(memcmp(&view, &original, sizeof(view)) == 0);
        assert(g_nextAddress == nextAddress && context->Allocations.count == 2);
        g_failUnmap = false;

        // Another view of the same backing must not clean this replacement.
        assert(SHMAttach(otherID, &other) == OS_EOK);
        assert(SHMDetach(&other) == OS_EOK);
        assert(context->Allocations.count == 2);
        g_failUnmapAddress = 0;

        if (mode == 1) {
            // Cleanup can succeed while removal of the old mapping still fails.
            g_failUnmapAddress = (vaddr_t)view.Buffer - 100;
            assert(SHMDetach(&view) == OS_EDEVFAULT);
            assert(memcmp(&view, &original, sizeof(view)) == 0);
            assert(context->Allocations.count == 1);
            g_failUnmapAddress = 0;
        } else if (mode == 2) {
            // Map retries cleanup before publishing the next replacement.
            assert(SHMMap(&view, PAGE_SIZE, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
            assert(view.Offset == PAGE_SIZE && view.Length == PAGE_SIZE);
            assert(context->Allocations.count == 1);
        } else if (mode == 3) {
            // Detach must clean the replacement even with no primary mapping.
            assert(SHMUnmap(&view, (vaddr_t)view.Buffer, view.Length) == OS_EOK);
            assert(view.Buffer == NULL && context->Allocations.count == 1);
        }

        assert(ScSHMDetach(&view) == OS_EOK);
        assert(view.ID == UUID_INVALID && view.Buffer == NULL);
        assert(__Page(replacement)->Flags == 0);
        assert(__Page((vaddr_t)original.Buffer)->Flags == 0);
        assert(context->Allocations.head == NULL);
        __DrainJanitor();
        assert(g_handles.element_count == handles);
    }

    // Successful rollback reports the original error and needs no later cleanup.
    assert(SHMAttach(viewID, &view) == OS_EOK);
    assert(SHMMap(&view, 0, PAGE_SIZE, SHM_ACCESS_READ) == OS_EOK);
    original = view;
    replacement = g_nextAddress;
    g_failUnmapAddress = (vaddr_t)view.Buffer;
    assert(SHMMap(&view, PAGE_SIZE, PAGE_SIZE, SHM_ACCESS_READ) == OS_EDEVFAULT);
    assert(memcmp(&view, &original, sizeof(view)) == 0);
    assert(__Page(replacement)->Flags == 0 && context->Allocations.count == 1);
    g_failUnmapAddress = 0;
    assert(SHMDetach(&view) == OS_EOK);
    assert(context->Allocations.head == NULL);
    __DrainJanitor();
    assert(g_handles.element_count == handles);
}
