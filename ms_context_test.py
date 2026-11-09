"""Run the memory-context unit tests with a minimal host assertion harness."""
from pathlib import Path
import tempfile
from shm_device_test import run_host_test

HARNESS = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define assert_int_equal(a,b) assert((a) == (b))
#define assert_int_not_equal(a,b) assert((a) != (b))
#define assert_ptr_equal(a,b) assert((a) == (b))
#define assert_non_null(a) assert((a) != NULL)
#define assert_null(a) assert((a) == NULL)
#define assert_true(a) assert(a)
#define fail_msg(...) abort()
#define test_malloc malloc
#define test_free free
struct CMUnitTest { void (*Run)(void**); int (*Setup)(void**); };
#define cmocka_unit_test_setup(run,setup) {run,setup}
static int run_tests(const struct CMUnitTest* tests, size_t count,
                     int (*setup)(void**), int (*teardown)(void**))
{
    void* state = NULL;
    size_t i;
    assert(setup(&state) == 0);
    for (i = 0; i < count; i++) {
        assert(tests[i].Setup(&state) == 0);
        tests[i].Run(&state);
    }
    assert(teardown(&state) == 0);
    printf("Memory contexts: %zu lifetime and cleanup tests passed\n", count);
    return 0;
}
#define cmocka_run_group_tests(t,s,d) run_tests(t,sizeof(t)/sizeof(t[0]),s,d)
'''

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="vali-context-harness-") as directory:
        (Path(directory) / "cmocka.h").write_text(HARNESS)
        stubs = Path(directory) / "stubs.c"
        stubs.write_text(r'''
#include <debug.h>
#include <os/spinlock.h>
#include <assert.h>
#include <stdlib.h>
void DebugPanic(int scope, Context_t* context, const char* message, ...)
{
    (void)scope; (void)context; (void)message;
    abort();
}
void spinlock_init(spinlock_t* lock) { lock->current = 0; }
void spinlock_acquire(spinlock_t* lock) { assert(lock->current == 0); lock->current = 1; }
void spinlock_release(spinlock_t* lock) { lock->current = 0; }
''')
        # Existing memory-space destructors use a typed pointer through the generic
        # handle callback. Keep the address/UB checks, excluding that ABI cast.
        run_host_test("ms-context", "memoryspace.h", (
            "kernel/memory/space/context_test.c", "kernel/memory/space/context.c",
            "kernel/memory/space/create.c", "kernel/memory/space/struct.c", "librt/libds/list.c", str(stubs)
        ), extra_flags=("-I" + directory, '-DPRIxIN="zx"', "-Wno-unused-parameter", "-fno-sanitize=function"))
