"""Check persistent use lifetime and the counter overflow boundary with sanitizers."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("shm-persistent-test", "shm/device.h",
                  ("testing/shm_device_persistent_test.c", "kernel/memory/shm/device_context.c", "kernel/memory/shm/access.c", "testing/shm_access_futex.c", "kernel/components/memory.c",
                   "kernel/utils/memory_stack.c"))
