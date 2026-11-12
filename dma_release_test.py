"""Inject failed address release into real SHM code through the host linker."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("dma-release-test", "shm/device.h", (
        "testing/dma_release_test.c",
        "kernel/memory/shm/device_context.c",
        "kernel/memory/shm/access.c",
        "testing/shm_access_futex.c",
        "kernel/components/memory.c",
        "kernel/utils/memory_stack.c",
    ), ("-Wl,--wrap=DMAAddressMappingRelease",))
