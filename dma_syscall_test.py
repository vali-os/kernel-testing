"""Exercise DMA and CPU kernel handles in the shared syscall path."""
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-dma-syscall-") as directory:
        output = Path(directory)
        # Keep the real security context layout and IRQ lock interfaces.
        for name in ("ds/mstring.h", "threading.h", "ddk/io.h"):
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(SHIMS[name])
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-fms-extensions", "-fasm-blocks",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections", "-g",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        objects = []
        for index, source in enumerate(("testing/dma_handle_runtime.c", "librt/libds/hashtable.c",
                                        "librt/libds/queue.c", "testing/dma_syscall_test.c")):
            obj = output / (str(index) + ".o")
            subprocess.run(flags + ["-c", str(ROOT / source), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = str(output / "dma-syscalls")
        subprocess.run(flags + objects + ["-Wl,--gc-sections", "-o", binary], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment)


if __name__ == "__main__":
    main()
