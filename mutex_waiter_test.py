"""Exercise kernel mutex wakeups, including repeated losses to a competing thread."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
             "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
             "-fms-extensions", "-fasm-blocks", "-ffunction-sections", "-fdata-sections",
             "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare", "-g"]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "kernel/include", "librt/libos/include")]
    flags += ["-isystem" + str(ROOT / path) for path in
              ("librt/libds/include", "librt/libddk/include", "boot/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    with tempfile.TemporaryDirectory(prefix="vali-mutex-waiter-") as directory:
        output = Path(directory)
        for name in ("mutex_owner_test", "mutex_waiter_test"):
            binary = output / name
            subprocess.run(flags + [str(ROOT / "testing" / (name + ".c")),
                                    "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env=environment, timeout=10)
        if args.mutation_check:
            source = (ROOT / "kernel/sync/mutex.c").read_text()
            before = "        __SetFlags(mutex, MUTEX_FLAG_PENDING);"
            assert source.count(before) == 1
            source = source.replace(before, "", 1)
            source = source.replace("    for (;;) {", "    __SetFlags(mutex, MUTEX_FLAG_PENDING);\n    for (;;) {", 1)
            mutant = output / "mutex-before-loop.c"
            mutant.write_text(source)
            subprocess.run(flags + [f'-DMUTEX_SOURCE="{mutant.as_posix()}"',
                                    str(ROOT / "testing/mutex_waiter_test.c"),
                                    "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env=environment, timeout=10)
            if result.returncode == 0 or "g_wakeCount == g_blockCount" not in result.stderr:
                raise RuntimeError("Lost-wakeup mutation was not detected: " + result.stderr)
            print("Mutex waiter: pending flag before retry loop mutation rejected")


if __name__ == "__main__":
    main()
