from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("dma-syscall-test", "shm_device.h", ("testing/dma_syscall_test.c",))
