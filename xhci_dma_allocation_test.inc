/** Independent scratchpad pages and retryable partial allocation/release. */
static void
__TestScratchpadPages(void)
{
    struct XhciScratchpad* pages = NULL;
    reg64_t addresses[18] = { 0 };
    size_t i;
    uint32_t segmentCount;
    int allocations = g_allocateCalls;

    CHECK(XHCI_HCSPARAMS2_MAXSCRATCHPADS(0) == 0);
    CHECK(XHCI_HCSPARAMS2_MAXSCRATCHPADS(1U << 27) == 1);
    CHECK(XHCI_HCSPARAMS2_MAXSCRATCHPADS(1U << 21) == 32);
    CHECK(XHCI_HCSPARAMS2_MAXSCRATCHPADS((31U << 21) | (31U << 27)) == 1023);
    CHECK(XHCI_HCSPARAMS2_MAXSCRATCHPADS(1U << 26) == 0);

    g_scatteredAllocations = true;
    CHECK(XhciScratchpadsAllocate((struct DMAContext*)42, 18, addresses, &pages) == OS_EOK);
    CHECK(g_allocateCalls == allocations + 18 && g_maps == 18);
    for (i = 0; i < 18; i++) {
        CHECK(addresses[i] == DMABufferSegments(pages[i].Buffer, &segmentCount)[0].Address);
        CHECK((addresses[i] & (XHCI_PAGE_SIZE - 1)) == 0);
        if (i != 0) CHECK(addresses[i] - addresses[i - 1] == 0x20000);
    }
    CHECK(XhciScratchpadsFree(&pages, 18) == OS_EOK);
    CHECK(pages == NULL && g_maps == 0);
    CHECK(XhciScratchpadsFree(&pages, 18) == OS_EOK);

    g_failAllocateAfter = 3;
    CHECK(XhciScratchpadsAllocate((struct DMAContext*)42, 18, addresses, &pages) == OS_EOOM);
    CHECK(pages != NULL && g_maps == 3);
    g_failAllocateAfter = -1;
    g_failBufferFree = pages[1].Buffer;
    CHECK(XhciScratchpadsFree(&pages, 18) == OS_EBUSY);
    CHECK(pages[0].Buffer == NULL);
    CHECK(pages[1].Buffer == g_failBufferFree && g_maps == 2);
    g_failBufferFree = NULL;
    CHECK(XhciScratchpadsFree(&pages, 18) == OS_EOK);
    CHECK(pages == NULL && g_maps == 0);

    // A segment setup failure publishes no buffer; only the scratchpad array remains.
    g_failSegments = 1;
    CHECK(XhciScratchpadsAllocate((struct DMAContext*)42, 18, addresses, &pages) == OS_EUNKNOWN);
    CHECK(pages[0].Buffer == NULL && g_maps == 0);
    g_failSegments = 0;
    CHECK(XhciScratchpadsFree(&pages, 18) == OS_EOK);
    CHECK(pages == NULL && g_maps == 0);
    g_scatteredAllocations = false;
    puts("xHCI scratchpads: scattered pages, partial allocation and release retries passed");
}
