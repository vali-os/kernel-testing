"""Check cached software pools and legacy DMA pools with the real scheduler."""
from shm_device_test import ROOT, run_host_test


if __name__ == "__main__":
    run_host_test(
        "usb-scheduler-memory-test",
        str(ROOT / "modules/serial/usb/common/scheduler.h"),
        ("testing/usb_scheduler_memory_test.c",),
        ("-I" + str(ROOT / "librt/libusb/include"), "-Wno-sign-compare"),
    )
