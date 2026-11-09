"""Exercise restricted SHM against real grants and VM code with a simulated MMU."""
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-shm-restricted-") as directory:
        output = Path(directory)
        for name, source in SHIMS.items():
            if name == "memoryspace.h":
                continue
            if name == "threading.h":
                source = "#pragma once\n" + source
                source += "\n#include <os/types/time.h>\ntypedef struct Thread Thread_t;\n"
            if name == "threading.h":
                source += "Thread_t* ThreadCurrentForCore(int);\nMemorySpace_t* ThreadMemorySpace(Thread_t*);\nuuid_t ThreadMemorySpaceHandle(Thread_t*);\n"
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fms-extensions", "-fasm-blocks", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare", "-g", '-DPRIxIN="zx"', "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        binary = str(output / "shm-restricted")
        sources = ("testing/shm_restricted_test.c", "librt/libds/hashtable.c",
                   "librt/libds/queue.c", "librt/libds/streambuffer.c", "librt/libds/list.c")
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment, timeout=10)


if __name__ == "__main__":
    main()
