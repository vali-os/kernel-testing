"""Build the real SHM device context with the existing host-test header setup."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_host_test(name, header, sources):
    """Build kernel sources with sanitizers and run them, without a boot.

    Shared with other kernel memory host tests so they all use the same flags.
    """
    with tempfile.TemporaryDirectory(prefix="vali-" + name + "-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI",
                 "-DKERNELAPI=", "-DKERNELABI=", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-g", "-pthread"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        # SHM's private header and the DDK barriers pull these in; their own
        # warnings are not under test.
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        # A separate translation unit catches accidental reliance on include order.
        header_source = output / "header.c"
        header_source.write_text("#include <" + header + ">\n")
        subprocess.run(flags + ["-fsyntax-only", str(header_source)], check=True)
        binary = str(output / name)
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
        # Match other host tests: explicit live-allocation counts cover leaks
        # even in sandboxes where LeakSanitizer cannot inspect the process.
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment)


def main():
    run_host_test("shm-device-test", "shm_device.h",
                  ("testing/shm_device_test.c", "kernel/memory/ms_shm_device.c"))


if __name__ == "__main__":
    main()
