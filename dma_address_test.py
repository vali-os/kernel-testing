"""Check direct address mapping lifetime with the shared sanitizer harness."""
from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("dma-address-test", "dma/address.h", (
        "testing/dma_address_test.c",
        "kernel/memory/dma/address.c",
        "kernel/memory/dma/direct.c",
    ))
