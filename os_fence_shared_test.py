"""Run libos fences across processes and distinct mappings with Linux futexes."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-os-fence-") as directory:
        binary = Path(directory) / "fence"
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
                 "-Derrno_t=int", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-g", "-pthread"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        subprocess.run(flags + [str(ROOT / path) for path in
                               ("testing/os_fence_shared_test.c", "librt/libos/fence.c", "librt/libos/fence_storage.c")]
                       + ["-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=15)


if __name__ == "__main__":
    main()
