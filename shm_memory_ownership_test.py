"""Link real SHM buffer/device code to the real system memory component."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("shm-memory-ownership-test", "shm/shm.h",
                  ("testing/shm_memory_ownership_test.c", "kernel/memory/shm/shm.c",
                   "kernel/memory/shm/device.c", "kernel/memory/shm/device_context.c", "kernel/memory/shm/access.c", "testing/shm_access_futex.c", "kernel/components/memory.c",
                   "kernel/utils/memory_stack.c"),
                  extra_flags=("-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                               "-Wno-unused-parameter", "-Wno-sign-compare"))
