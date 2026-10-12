"""Link real SHM buffer/device code to the real system memory component."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("shm-memory-ownership-test", "shm.h",
                  ("testing/shm_memory_ownership_test.c", "kernel/memory/ms_shm.c",
                   "kernel/memory/ms_shm_device.c", "kernel/components/memory.c",
                   "kernel/utils/memory_stack.c"),
                  extra_flags=("-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                               "-Wno-unused-parameter", "-Wno-sign-compare"))
