"""Exercise SHM creation validation and anonymous mmap using target headers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-shm-client-") as directory:
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI", "-DTESTING",
                 "-DMOLLENOS", "-D__time64_t=int64_t", "-DNDEBUG", "-fms-extensions",
                 "-Wno-macro-redefined", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-fsanitize=address,undefined", "-g"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("librt/libc/include", "librt/libos/include", "librt/libds/include",
                   "librt/libddk/include")]
        cases = {
            "shm_release_test": ["librt/libos/syscalls.c"],
            "mmap_shm_test": ["librt/libds/list.c"],
        }
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        for name, support in cases.items():
            binary = str(Path(directory) / name)
            sources = ["testing/" + name + ".c"] + support
            subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
            subprocess.run([binary], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
