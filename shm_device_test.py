"""Build the real SHM device context with the existing host-test header setup."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    """Use temporary output and sanitizers to check memory safety without a boot."""
    with tempfile.TemporaryDirectory(prefix="vali-shm-device-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI",
                 "-DKERNELAPI=", "-DKERNELABI=", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-g", "-pthread"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        # A separate translation unit catches accidental reliance on include order.
        header = output / "header.c"
        header.write_text("#include <shm_device.h>\n")
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
        binary = str(output / "shm-device-test")
        sources = [str(ROOT / path) for path in
                   ("testing/shm_device_test.c", "kernel/memory/ms_shm_device.c")]
        subprocess.run(flags + sources + ["-o", binary], check=True)
        # Match other host tests: explicit live-allocation counts cover leaks
        # even in sandboxes where LeakSanitizer cannot inspect the process.
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment)


if __name__ == "__main__":
    main()
