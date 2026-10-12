"""Exercise the real system memory component and both region types."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("system-memory-test", "component/memory.h",
                  ("testing/system_memory_test.c", "kernel/components/memory.c",
                   "kernel/utils/memory_stack.c"))
