#ifndef TEST_HANDLE_SET_STUBS_H
#define TEST_HANDLE_SET_STUBS_H

// Handle-table suites without notification queues do not link handle_set.c.
// Routing suites define TEST_HANDLE_SETS and exercise the real cleanup hook.
#ifndef TEST_HANDLE_SETS
void
HandleSetsRemoveGrant(
    _In_ HandleGrantsTable_t* table,
    _In_ uuid_t              handle)
{
    (void)table;
    (void)handle;
}
#endif

#endif
