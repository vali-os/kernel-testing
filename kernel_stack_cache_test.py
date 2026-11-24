"""Run the kernel stack cache under ASan/UBSan with host stacks.

--mutation-check also builds copies of kernel/threads/stack_cache.c with one
known mistake each, and requires every copy to fail at an assertion.
"""
import argparse
from pathlib import Path
import tempfile

from shm_device_test import run_host_test

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel/threads/stack_cache.c"
SOURCES = ("testing/kernel_stack_cache_test.c", SOURCE)

MUTATIONS = (
    ("keep one stack more than the limit",
     "if (g_cachedCount < KERNEL_STACK_CACHE_LIMIT) {",
     "if (g_cachedCount <= KERNEL_STACK_CACHE_LIMIT) {"),
    ("cache stacks of every size",
     "    if (stackSize != THREADING_KERNEL_STACK_SIZE) {\n        return OS_ENOTSUPPORTED;",
     "    if (stackSize == 0) {\n        return OS_ENOTSUPPORTED;"),
    ("hand out cached stacks for every size",
     "    if (stackSize != THREADING_KERNEL_STACK_SIZE) {\n        return OS_ENOENT;",
     "    if (stackSize == 0) {\n        return OS_ENOENT;"),
    ("return the list entry instead of the stack top",
     "*stackTopOut = __TopFromEntry(entry);",
     "*stackTopOut = __TopFromEntry(entry) - sizeof(struct __CachedStack);"),
    ("keep the count when a stack is taken",
     "        g_cachedCount--;\n",
     ""),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    run_host_test("kernel-stack-cache-test", "stack_cache.h", SOURCES, ("-D_GNU_SOURCE",))
    if not args.mutation_check:
        return

    original = (ROOT / SOURCE).read_text()
    for name, old, new in MUTATIONS:
        assert original.count(old) == 1, name
        with tempfile.TemporaryDirectory(prefix="vali-stack-cache-mutation-") as directory:
            mutant = Path(directory) / "stack_cache.c"
            mutant.write_text(original.replace(old, new))
            sources = (SOURCES[0], str(mutant))
            run_host_test("kernel-stack-cache-mutant", "stack_cache.h", sources,
                          ("-D_GNU_SOURCE",), mutation=name)
            print("Rejected mutation:", name, flush=True)


if __name__ == "__main__":
    main()
