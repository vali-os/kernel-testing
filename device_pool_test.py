"""Build the real device pool allocator with the shared kernel host-test setup."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("device-pool-test", "device_pool.h",
                  ("testing/device_pool_test.c", "kernel/memory/ms_device_pool.c"))
