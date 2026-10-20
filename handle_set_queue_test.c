/* Partial I/O-event drains must not retain links into the producer queue. */
#include "../kernel/handle_set.c"
#include "../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct handle_set g_set;
void LogAppendMessage(enum OSSysLogLevel level, const char* format, ...) {}
void* kmalloc(size_t n) { return calloc(1, n); }
void kfree(void* p) { free(p); }
void SpinlockConstruct(Spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void SpinlockAcquireIrq(Spinlock_t* lock) {}
void SpinlockReleaseIrq(Spinlock_t* lock) {}
void* LookupHandleOfType(uuid_t handle, HandleType_t type) { CHECK(type == HandleTypeSet); return &g_set; }
uuid_t CreateHandle(HandleType_t type, HandleDestructorFn destroy, void* p) { abort(); }
oserr_t FutexWait(OSAsyncContext_t* a, _Atomic(int)* f, int v, int flags, _Atomic(int)* f2, int n, int op, OSTimestamp_t* t) { abort(); }
oserr_t FutexWake(_Atomic(int)* f, int n, int flags) { return OS_EOK; }
int hashtable_construct(hashtable_t* h, size_t c, size_t e, hashtable_hashfn fn, hashtable_cmpfn cmp) { abort(); }
void* hashtable_get(hashtable_t* h, const void* k) { abort(); }
void* hashtable_set(hashtable_t* h, const void* k) { abort(); }
void* hashtable_remove(hashtable_t* h, const void* k) { abort(); }
void rb_tree_construct(rb_tree_t* t) { abort(); }
int rb_tree_append(rb_tree_t* t, rb_leaf_t* l) { abort(); }
rb_leaf_t* rb_tree_lookup(rb_tree_t* t, void* k) { abort(); }
rb_leaf_t* rb_tree_minimum(rb_tree_t* t) { abort(); }
rb_leaf_t* rb_tree_remove(rb_tree_t* t, void* k) { abort(); }
static void CheckList(list_t* list, int count) {
    int seen = 0;
    element_t* previous = NULL;
    foreach(node, list) {
        CHECK(node->previous == previous && ++seen <= count);
        previous = node;
    }
    CHECK(seen == count && list->count == count && list->tail == previous);
}
int main(void) {
    list_t source = LIST_INIT, destination = LIST_INIT;
    element_t nodes[4] = {0};
    for (int i = 0; i < 4; i++) list_append(&source, &nodes[i]);
    list_splice(&source, 2, &destination);
    CheckList(&source, 2); CheckList(&destination, 2);
    list_splice(&source, 1, &destination);
    CheckList(&source, 1); CheckList(&destination, 3);
    list_splice(&source, 99, &destination);
    CheckList(&source, 0); CheckList(&destination, 4);
    list_splice(&destination, -1, &source);
    list_splice(&destination, 4, &destination);
    CheckList(&source, 0); CheckList(&destination, 4);
    list_splice(&source, 1, &destination);
    CheckList(&source, 0); CheckList(&destination, 4);

    struct handleset_element items[3] = {0};
    list_construct(&g_set.events);
    for (int i = 0; i < 3; i++) {
        items[i].set = &g_set;
        items[i].Configuration = 1;
        items[i].Context.context = (void*)(uintptr_t)(i + 1);
        ELEMENT_INIT(&items[i].event_header, 0, &items[i]);
        ELEMENT_INIT(&items[i].set_header, 0, &items[i]);
        MarkHandleCallback(0, &items[i].set_header, (void*)1);
    }
    struct ioset_event events[3] = {0};
    int count;
    CHECK(WaitForHandleSet(1, NULL, events, 1, 0, NULL, &count) == OS_EOK && count == 1);
    CHECK(events[0].data.context == (void*)1 && events[0].events == 1);
    CheckList(&g_set.events, 2);
    CHECK(atomic_load(&g_set.events_pending) == 2);
    // Reuse the consumed intrusive node while older events remain pending.
    MarkHandleCallback(0, &items[0].set_header, (void*)1);
    CheckList(&g_set.events, 3);
    CHECK(WaitForHandleSet(1, NULL, events, 3, 1, NULL, &count) == OS_EOK && count == 3);
    CheckList(&g_set.events, 1);
    CHECK(atomic_load(&g_set.events_pending) == 1);
    CHECK(WaitForHandleSet(1, NULL, events, 1, 0, NULL, &count) == OS_EOK && count == 1);
    CheckList(&g_set.events, 0);
    CHECK(events[0].data.context == (void*)1);
    CHECK(WaitForHandleSet(1, NULL, events, 0, 0, NULL, &count) == OS_EINVALPARAMS);
    CHECK(WaitForHandleSet(1, NULL, events, 1, 1, NULL, &count) == OS_EOK && count == 1);
    puts("I/O events: partial splice, list boundaries, requeue, pending counts and poll capacity passed");
    return 0;
}
