from shm_device_test import run_host_test

if __name__ == "__main__":
    run_host_test("arm64-timer-test", "../arch/aarch64/common/utils.h",
                  ("testing/arm64_timer_test.c",))
