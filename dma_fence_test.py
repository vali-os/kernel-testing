"""Exercise the real completion fence with a pthread-backed futex model."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-dma-fence-") as directory:
        binary = Path(directory) / "fence"
        (Path(directory) / "internal").mkdir()
        (Path(directory) / "internal/_utils.h").write_text("/* No libc internals needed by sync syscalls. */\n")
        (Path(directory) / "event.h").write_text((ROOT / "librt/libc/include/event.h").read_text())
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=extern", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread", "-I" + directory, "-fms-extensions", "-fasm-blocks"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include", "kernel/include")]
        flags += ["-isystem" + str(ROOT / "librt/libds/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        subprocess.run(flags + [str(ROOT / path) for path in ("testing/dma_fence_test.c", "librt/libos/futex.c", "librt/libos/syscalls.c", "kernel/api/sync.c")] + ["-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
