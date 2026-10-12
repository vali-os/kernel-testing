"""Check the real DMA wrappers and syscall gate with deterministic transport failures."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    # Use the real syscall macros and runtime forwarding. Only the final machine
    # trap is replaced; the test checks each syscall number and argument layout.
    with tempfile.TemporaryDirectory(prefix="vali-ddk-dma-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING", "-DSERVICEAPI=extern", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 "-I" + str(output)]
        flags += ["-I" + str(ROOT / p) for p in
                  ("testing/include", "librt/libos/include", "librt/libddk/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        for header in ("ddk/dma.h", "os/types/dma.h", "os/types/syscall.h"):
            source = output / "header.c"
            source.write_text('#include <' + header + '>\n')
            subprocess.run(flags + ["-fsyntax-only", str(source)], check=True)
        binary = output / "ddk-dma-test"
        subprocess.run(flags + [str(ROOT / "testing/ddk_dma_test.c"),
                                str(ROOT / "librt/libos/syscalls.c"), "-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment)


if __name__ == "__main__":
    main()
