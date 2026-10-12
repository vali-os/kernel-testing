"""Run the real range allocator under ASan/UBSan without memory-system stubs."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("memory-stack-test", "utils/memory_stack.h",
                  ("testing/memory_stack_test.c", "kernel/utils/memory_stack.c"))
