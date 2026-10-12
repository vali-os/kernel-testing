"""Test boot reservation selection without an allocator backend."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("memory-region-test", "component/memory.h",
                  ("testing/memory_region_test.c", "kernel/memory/memory_region.c"))
