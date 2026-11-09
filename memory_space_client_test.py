"""Exercise the DDK memory-space result boundary with a failing syscall."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-memory-space-client-") as directory:
        output = Path(directory)
        (output / "internal").mkdir()
        (output / "internal/_syscalls.h").write_text("""
#include <ddk/memory.h>
#include <os/types/syscall.h>
oserr_t Syscall_CreateMemorySpace(unsigned int, MemorySpaceCreateResult_t*);
oserr_t Syscall_GetMemorySpaceForThread(uuid_t, uuid_t*);
oserr_t Syscall_CreateMemorySpaceMapping(uuid_t, struct MemoryMappingParameters*, void**);
""")
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-fms-extensions", "-fasm-blocks", "-Wall", "-Wextra", "-Werror",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include")]
        binary = output / "test"
        sources = ("testing/memory_space_client_test.c", "librt/libddk/mappings.c")
        subprocess.run(flags + [str(ROOT / path) for path in sources] +
                       ["-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=10)


if __name__ == "__main__":
    main()
