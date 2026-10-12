"""Exercise real boot-memory code without constructing hardware page tables."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("memory-boot-test", "machine.h",
                  ("testing/memory_boot_test.c", "kernel/utils/memory_stack.c"),
                  extra_flags=("-std=gnu11", "-DTESTING", "-DSERVICEAPI=", "-DSERVICEABI=",
                               "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                               "-Wno-unused-parameter"))
