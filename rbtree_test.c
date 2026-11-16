// Exercise the production red-black tree: invariants, ordered navigation
// against a sorted oracle, and concurrent removal of the same keys.
#ifndef RBTREE_SOURCE
#define RBTREE_SOURCE "../librt/libds/rbtree.c"
#endif
#include RBTREE_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define KEY_SPACE   512
#define OPERATIONS  20000
#define THREADS     4
#define THREAD_KEYS 2000

#define KEY(n) ((void*)(uintptr_t)(n))

static rb_leaf_t g_leaves[KEY_SPACE + 1];
static int       g_present[KEY_SPACE + 1];

static int
__BlackHeight(
    rb_tree_t* tree,
    rb_leaf_t* leaf,
    uintptr_t  low,
    uintptr_t  high)
{
    int left;
    int right;
    uintptr_t key;
    if (IS_ITEM_NIL(tree, leaf)) {
        return 1;
    }

    key = (uintptr_t)leaf->key;
    CHECK(key > low && key < high);
    if (leaf->color == COLOR_RED) {
        CHECK(leaf->left->color == COLOR_BLACK);
        CHECK(leaf->right->color == COLOR_BLACK);
    }
    if (!IS_ITEM_NIL(tree, leaf->left)) {
        CHECK(leaf->left->parent == leaf);
    }
    if (!IS_ITEM_NIL(tree, leaf->right)) {
        CHECK(leaf->right->parent == leaf);
    }

    left  = __BlackHeight(tree, leaf->left, low, key);
    right = __BlackHeight(tree, leaf->right, key, high);
    CHECK(left == right);
    return left + (leaf->color == COLOR_BLACK ? 1 : 0);
}

static void
__CheckInvariants(
    rb_tree_t* tree)
{
    CHECK(tree->root->color == COLOR_BLACK);
    if (!IS_ITEM_NIL(tree, tree->root)) {
        CHECK(IS_ITEM_NIL(tree, tree->root->parent));
    }
    __BlackHeight(tree, tree->root, 0, UINTPTR_MAX);
}

static int
__OracleFloor(
    int key)
{
    for (int i = key; i >= 1; i--) {
        if (g_present[i]) {
            return i;
        }
    }
    return 0;
}

static int
__OracleCeiling(
    int key)
{
    for (int i = key; i <= KEY_SPACE; i++) {
        if (g_present[i]) {
            return i;
        }
    }
    return 0;
}

static int
__KeyOf(
    rb_leaf_t* leaf)
{
    return leaf != NULL ? (int)(uintptr_t)leaf->key : 0;
}

static void
__CheckNavigation(
    rb_tree_t* tree)
{
    rb_leaf_t* leaf;
    int        expected = __OracleCeiling(1);

    CHECK(__KeyOf(rb_tree_minimum(tree)) == expected);
    CHECK(__KeyOf(rb_tree_maximum(tree)) == __OracleFloor(KEY_SPACE));

    // Forward and backward walks must visit every present key exactly once.
    for (leaf = rb_tree_minimum(tree); leaf != NULL; leaf = rb_tree_next(tree, leaf)) {
        CHECK(__KeyOf(leaf) == expected);
        expected = (expected < KEY_SPACE) ? __OracleCeiling(expected + 1) : 0;
    }
    CHECK(expected == 0);

    expected = __OracleFloor(KEY_SPACE);
    for (leaf = rb_tree_maximum(tree); leaf != NULL; leaf = rb_tree_previous(tree, leaf)) {
        CHECK(__KeyOf(leaf) == expected);
        expected = (expected > 1) ? __OracleFloor(expected - 1) : 0;
    }
    CHECK(expected == 0);

    for (int key = 1; key <= KEY_SPACE; key++) {
        CHECK(__KeyOf(rb_tree_floor(tree, KEY(key))) == __OracleFloor(key));
        CHECK(__KeyOf(rb_tree_ceiling(tree, KEY(key))) == __OracleCeiling(key));
    }
}

static void
__TestEmpty(void)
{
    rb_tree_t tree;
    rb_tree_construct(&tree);
    CHECK(rb_tree_minimum(&tree) == NULL);
    CHECK(rb_tree_maximum(&tree) == NULL);
    CHECK(rb_tree_floor(&tree, KEY(5)) == NULL);
    CHECK(rb_tree_ceiling(&tree, KEY(5)) == NULL);
    CHECK(rb_tree_lookup(&tree, KEY(5)) == NULL);
    CHECK(rb_tree_remove(&tree, KEY(5)) == NULL);
}

static void
__TestRandomized(
    unsigned int seed)
{
    rb_tree_t tree;
    rb_leaf_t duplicate;
    srand(seed);
    rb_tree_construct(&tree);
    for (int i = 0; i <= KEY_SPACE; i++) {
        g_present[i] = 0;
    }

    for (int op = 0; op < OPERATIONS; op++) {
        int key = 1 + rand() % KEY_SPACE;
        if (g_present[key]) {
            CHECK(rb_tree_lookup(&tree, KEY(key)) == &g_leaves[key]);
            CHECK(rb_tree_remove(&tree, KEY(key)) == &g_leaves[key]);
            g_present[key] = 0;
        }
        else {
            RB_LEAF_INIT(&g_leaves[key], key, NULL);
            CHECK(rb_tree_append(&tree, &g_leaves[key]) == 0);
            RB_LEAF_INIT(&duplicate, key, NULL);
            CHECK(rb_tree_append(&tree, &duplicate) == -1);
            g_present[key] = 1;
        }
        if (op % 97 == 0) {
            __CheckInvariants(&tree);
            __CheckNavigation(&tree);
        }
    }
    __CheckInvariants(&tree);
    __CheckNavigation(&tree);
}

static rb_tree_t g_shared;
static rb_leaf_t g_shared_leaves[THREAD_KEYS + 1];
static int       g_removed[THREADS];

static void*
__RemoveAll(
    void* argument)
{
    int index = (int)(intptr_t)argument;
    for (int key = 1; key <= THREAD_KEYS; key++) {
        if (rb_tree_remove(&g_shared, KEY(key)) != NULL) {
            g_removed[index]++;
        }
    }
    return NULL;
}

// Every thread tries to remove every key. A key may only be unlinked once.
static void
__TestConcurrentRemove(void)
{
    pthread_t threads[THREADS];
    int       total = 0;

    rb_tree_construct(&g_shared);
    for (int key = 1; key <= THREAD_KEYS; key++) {
        RB_LEAF_INIT(&g_shared_leaves[key], key, NULL);
        CHECK(rb_tree_append(&g_shared, &g_shared_leaves[key]) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        CHECK(pthread_create(&threads[i], NULL, __RemoveAll, (void*)(intptr_t)i) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        total += g_removed[i];
    }
    CHECK(total == THREAD_KEYS);
    CHECK(rb_tree_minimum(&g_shared) == NULL);
}

int
main(void)
{
    __TestEmpty();
    for (unsigned int seed = 1; seed <= 8; seed++) {
        __TestRandomized(seed);
    }
    __TestConcurrentRemove();
    printf("rbtree: all checks passed\n");
    return 0;
}
