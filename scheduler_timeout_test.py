"""Run deterministic production-scheduler wake/timeout interleavings with host Clang.

Set CC to the compiler. On Windows pass --target=x86_64-pc-windows-msvc.
--mutation-check verifies that rejecting pending remote wakeups fails the test.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target")
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-fms-extensions",
             "-DMOLLENOS", "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=",
             "-DKERNELABI=", "-DSERVICEAPI=", "-DSERVICEABI=", "-D__time64_t=int64_t",
             "-DNDEBUG", "-Wno-macro-redefined", "-fuse-ld=lld"]
    if args.target:
        flags += [f"--target={args.target}"]
    for include in ("kernel/include", "boot/include", "librt/libc/include",
                    "librt/libos/include", "librt/libddk/include", "librt/libds/include",
                    "librt/libgracht/include", "librt/libacpi/source/include"):
        flags += ["-I", str(ROOT / include)]
    if os.name == "nt":
        flags += ["-llegacy_stdio_definitions"]

    with tempfile.TemporaryDirectory(prefix="vali-scheduler-timeout-",
                                     ignore_cleanup_errors=os.name == "nt") as directory:
        out = Path(directory)
        binary = out / ("test.exe" if os.name == "nt" else "test")
        command = flags + [str(ROOT / "testing/scheduler_timeout_test.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        if args.mutation_check:
            source = (ROOT / "kernel/components/scheduler.c").read_text()
            condition = "if (state == STATE_QUEUEING) {"
            assert source.count(condition) == 1
            removal = "            __RemoveFromQueue(&scheduler->SleepQueue, object);"
            assert source.count(removal) == 1
            mutations = {
                "pending-wakeup": source.replace(condition, "if (state != STATE_QUEUEING) {"),
                "sleep-unlink": source.replace(removal, "            (void)scheduler;"),
            }
            for name, changed in mutations.items():
                mutant = out / f"{name}.c"
                mutant.write_text(changed)
                mutant_binary = out / (f"{name}.exe" if os.name == "nt" else name)
                command = flags + [f'-DSCHEDULER_SOURCE="{mutant.as_posix()}"',
                                   str(ROOT / "testing/scheduler_timeout_test.c"),
                                   "-o", str(mutant_binary)]
                subprocess.run(command, check=True)
                result = subprocess.run([str(mutant_binary)])
                if result.returncode == 0:
                    raise RuntimeError(f"{name} regression escaped detection")
                print(f"{name} mutation rejected")


if __name__ == "__main__":
    main()
