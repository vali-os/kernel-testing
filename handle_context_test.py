"""Run memory-space group creation and cleanup against the production handle table."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-handle-context-") as directory:
        output = Path(directory)
        for name, source in SHIMS.items():
            if name in ("memoryspace.h", "heap.h"):
                continue
            if name == "threading.h":
                source = "#pragma once\n" + source
                source += "\n#include <os/types/time.h>\ntypedef struct Thread Thread_t;\n"
                source += "Thread_t* ThreadCurrentForCore(int);\nMemorySpace_t* ThreadMemorySpace(Thread_t*);\nuuid_t ThreadMemorySpaceHandle(Thread_t*);\n"
            if name == "debug.h":
                source += "\n#include <stdlib.h>\n#define FATAL(...) abort()\n"
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        for name in ("event.h", "ioset.h"):
            (output / name).write_text((ROOT / "librt/libc/include" / name).read_text())
        # Memory-space cleanup uses a typed callback through the handle API.
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fms-extensions", "-fasm-blocks", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                 "-g", "-fno-sanitize=function", '-DPRIxIN="zx"', "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        binary = str(output / "handle-context")
        sources = ("testing/handle_context_test.c", "kernel/memory/space/context.c",
                   "kernel/memory/space/create.c", "kernel/memory/space/struct.c",
                   "librt/libds/hashtable.c", "librt/libds/queue.c")
        # Keep the fixture's page-table destruction stub; unused hardware page
        # table operations are discarded by the linker.
        paging_source = ROOT / "kernel/arch/x86/x64/memory/vmem_api.c"
        paging_object = output / "paging.o"
        paging_flags = [flag for flag in flags if flag != "-Wl,--gc-sections"]
        paging_flags += ["-DMmuDestroyVirtualSpace=TestMmuDestroyVirtualSpace"]
        subprocess.run(paging_flags + ["-c", str(paging_source), "-o", str(paging_object)], check=True)
        command = flags + [str(ROOT / path) for path in sources] + [str(paging_object), "-o", binary]
        subprocess.run(command, check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment, timeout=10)
        if args.mutation_check:
            source = paging_source.read_text()
            before = "parentSpace = LookupGlobalHandle("
            assert source.count(before) == 1
            mutant = output / "paging-mutant.c"
            mutant.write_text(source.replace(before, "parentSpace = LookupHandle("))
            subprocess.run(paging_flags + ["-include", "handles/handle.h", "-c", str(mutant),
                                          "-o", str(paging_object)], check=True)
            subprocess.run(command, check=True)
            result = subprocess.run([binary], capture_output=True, text=True,
                                    env=environment, timeout=10)
            if result.returncode == 0 or "null pointer of type 'MemorySpace_t'" not in result.stderr:
                raise RuntimeError("Grant-checked parent lookup mutation was not detected: " + result.stderr)
            print("Inherited page tables: grant-checked parent lookup mutation rejected")


if __name__ == "__main__":
    main()
