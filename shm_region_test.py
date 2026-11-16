"""Exercise canonical CPU regions, kernel entry points and libos wrappers."""
from shm_device_test import ROOT, run_host_test

if __name__ == "__main__":
    run_host_test("shm-region-test", "os/shm_region.h",
                  ("testing/shm_region_test.c", "kernel/memory/shm/device/context.c",
                   "kernel/memory/shm/access.c", "kernel/memory/shm/device/device.c",
                   "testing/shm_access_futex.c", "kernel/components/memory.c",
                   "kernel/utils/memory_stack.c"),
                  extra_flags=("-idirafter", str(ROOT / "librt/libc/include")))
