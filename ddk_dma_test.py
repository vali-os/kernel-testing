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
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread",
                 "-I" + str(output)]
        flags += ["-I" + str(ROOT / p) for p in
                  ("testing/include", "librt/libos/include", "librt/libddk/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        for header in ("ddk/dma.h", "os/types/dma.h", "os/types/dma_config.h", "os/types/syscall.h"):
            source = output / "header.c"
            source.write_text('#include <' + header + '>\n')
            subprocess.run(flags + ["-fsyntax-only", str(source)], check=True)
        subprocess.run(flags + ["-fsyntax-only", str(ROOT / "testing/dma_api_compile.c")], check=True)
        # Buffers are continuously shared; the compiler must reject accidental
        # use of the streaming handoff API before it reaches the kernel.
        source.write_text('#include <ddk/dma.h>\n'
                          'oserr_t misuse(struct DMABuffer* buffer) {\n'
                          '    return DMABeginTransfer(buffer, 0, 1);\n}\n')
        misuse = subprocess.run(flags + ["-fsyntax-only", str(source)], capture_output=True)
        if misuse.returncode == 0:
            raise AssertionError("DMA buffer unexpectedly accepted as a streaming mapping")
        binary = output / "ddk-dma-test"
        subprocess.run(flags + [str(ROOT / "testing/ddk_dma_test.c"),
                                str(ROOT / "librt/libos/syscalls.c"), "-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment)


if __name__ == "__main__":
    main()
