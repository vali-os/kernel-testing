// SHM_CLEAN initialization with real SHM and VM code over host-backed pages.
static void
__AssertPageContents(
    _In_ const unsigned char* page,
    _In_ unsigned char        value)
{
    size_t i;

    for (i = 0; i < PAGE_SIZE; i++) {
        assert(page[i] == value);
    }
}

static void
__TestCleanMapping(
    _In_ struct SHMBuffer* backing)
{
    struct SHMReference source = {
        .Buffer = backing,
        .Length = backing->Length,
        .Scope = GrantTokenScopeRead | GrantTokenScopeWrite
    };
    SHMHandle_t view;
    paddr_t     savedPages[2] = { backing->Pages[2], backing->Pages[3] };
    unsigned int access;
    int          mode;

    g_testCleanPages = true;
    backing->Flags |= SHM_CLEAN;
    for (mode = 0; mode < 3; mode++) {
        g_cleanPageCount = 0;
        backing->Pages[2] = 0;
        backing->Pages[3] = (paddr_t)g_cleanPages[3];
        memset(g_cleanPages[3], 0x5a, PAGE_SIZE);
        assert(SHMViewCreate(&source, 2 * PAGE_SIZE, 2 * PAGE_SIZE,
                             GrantTokenScopeRead, &view) == OS_EOK);
        access = SHM_ACCESS_READ;
        if (mode == 0) {
            access |= SHM_ACCESS_COMMIT;
        }
        assert(SHMMap(&view, 0, 2 * PAGE_SIZE, access) == OS_EOK);
        if (mode == 1) {
            assert(SHMCommitFault(backing->ID, (vaddr_t)view.Buffer,
                                  (vaddr_t)view.Buffer) == OS_EOK);
        } else if (mode == 2) {
            assert(SHMCommit(view.ID, view.Buffer, view.Buffer, PAGE_SIZE) == OS_EOK);
        }
        assert(g_cleanPageCount == 1 && backing->PageMapping == 0);
        __AssertPageContents((void*)backing->Pages[2], 0);
        __AssertPageContents(g_cleanPages[3], 0x5a);
        assert(__Page((vaddr_t)view.Buffer)->Physical == backing->Pages[2]);
        assert(__Page((vaddr_t)view.Buffer)->Flags & MAPPING_READONLY);

        // Mapping the same backing again must preserve data already written.
        memset((void*)backing->Pages[2], 0x3c, PAGE_SIZE);
        assert(SHMMap(&view, 0, 2 * PAGE_SIZE,
                      SHM_ACCESS_READ | SHM_ACCESS_COMMIT) == OS_EOK);
        assert(g_cleanPageCount == 1);
        __AssertPageContents((void*)backing->Pages[2], 0x3c);
        __AssertPageContents(g_cleanPages[3], 0x5a);
        assert(!(__RecalculateFlags(backing, SHM_ACCESS_READ) & MAPPING_CLEAN));
        assert(SHMDetach(&view) == OS_EOK);
        __DrainJanitor();
    }

    // A failed zeroing map must free the new page without publishing it.
    g_cleanPageCount = 0;
    g_cleanPageFrees = 0;
    backing->Pages[2] = 0;
    g_failMap = true;
    assert(SHMEnsurePhysicalPages(backing, 2, 1) == OS_EOOM);
    g_failMap = false;
    assert(backing->Pages[2] == 0 && backing->PageMapping == 0);
    assert(g_cleanPageFrees == 1);

    // Failed removal keeps the initialized page mapped until cleanup can retry.
    g_failUnmap = true;
    assert(SHMEnsurePhysicalPages(backing, 2, 1) == OS_EBUSY);
    assert(backing->Pages[2] != 0 && backing->PageMapping != 0);
    __AssertPageContents((void*)backing->Pages[2], 0);
    assert(SHMEnsurePhysicalPages(backing, 2, 1) == OS_EBUSY);
    assert(g_cleanPageCount == 2 && g_cleanPageFrees == 1);
    g_failUnmap = false;
    assert(SHMEnsurePhysicalPages(backing, 2, 1) == OS_EOK);
    assert(backing->PageMapping == 0 && g_cleanPageCount == 2);

    // Buffers without SHM_CLEAN keep the allocator's contents.
    backing->Flags &= ~SHM_CLEAN;
    backing->Pages[2] = 0;
    assert(SHMEnsurePhysicalPages(backing, 2, 1) == OS_EOK);
    __AssertPageContents((void*)backing->Pages[2], 0xa5);
    backing->Pages[2] = savedPages[0];
    backing->Pages[3] = savedPages[1];
    g_testCleanPages = false;
}
