#!/usr/bin/env python3
"""Check SMMU control arenas and stage-2 fields against real kernel allocators."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ARENA = ROOT / "kernel/arch/aarch64/smmu"


def host_run(output, mutation=None):
    files = {name: (ARENA / name).read_text() for name in ("memory.c", "memory.h", "stage2.h")}
    if mutation:
        name, source, before, after = mutation
        assert files[source].count(before) == 1, name
        files[source] = files[source].replace(before, after)
    for name, source in files.items():
        (output / name).write_text(source)
    test = (ROOT / "testing/smmu/memory_test.c").read_text()
    test = test.replace('../../kernel/arch/aarch64/smmu/', '')
    (output / "test.c").write_text(test)
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-fms-extensions",
             "-fasm-blocks", "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline",
             "-DSERVICEABI=", "-DLIBC_KERNEL", "-D__LIBDS_KERNEL__", "-DTEST_ARM64",
             "-U__amd64__", "-D__aarch64__", '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    binary = output / "memory-test"
    sources = [output / "test.c", output / "memory.c", ROOT / "kernel/components/memory.c",
               ROOT / "kernel/utils/memory_stack.c"]
    subprocess.run(flags + [str(path) for path in sources]
                   + ["-Wl,--wrap=SystemMemoryReleaseAllocation", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=mutation is not None,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    if mutation is None:
        result.check_returncode()
    elif (result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr
          or b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr):
        raise AssertionError(f"Mutation did not fail cleanly at an assertion: {mutation[0]}\n"
                             + (result.stdout + result.stderr).decode(errors="replace"))


def compile_target(output):
    flags = [os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang"),
             "--target=aarch64-uml-vali", "-std=gnu11", "-fms-extensions", "-ffreestanding",
             "-nostdlibinc", "-O2", "-Wall", "-Werror", "-march=armv8-a", "-mno-outline-atomics",
             "-D__LIBDS_KERNEL__", "-DLIBC_KERNEL"]
    for directory in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                      "librt/libddk/include", "librt/libds/include"):
        flags += ["-I" + str(ROOT / directory)]
    subprocess.run(flags + ["-c", str(ARENA / "memory.c"), "-o", str(output / "memory.obj")], check=True)
    # Each header must compile by itself under the production target ABI.
    for name in ("memory", "stage2"):
        header = output / "header.c"
        header.write_text(f'#include "{ARENA / (name + ".h")}"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    print("Arm SMMU memory: AArch64 production and standalone header compilation passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmu-memory-") as directory:
        output = Path(directory)
        host_run(output)
        compile_target(output)
        if not args.mutation_check:
            return
        mutations = (
            ("physical final byte", "memory.c", "length - 1 > physicalLimit -", "length - 1 >= physicalLimit -"),
            ("physical base limit", "memory.c", "memory->Allocation.PhysicalBase > physicalLimit ||", "false ||"),
            ("arena zeroing", "memory.c", "memset((void*)memory->Mapping, 0, length);", "// Mutant omits initial zeroing."),
            ("block zeroing", "memory.c", "memset(block.VirtualAddress, 0, length);", "// Mutant exposes old descriptors."),
            ("persistent CPU mapping", "memory.c", "MAPPING_COMMIT | MAPPING_PERSISTENT", "MAPPING_COMMIT"),
            ("pool cache policy", "memory.c", "if (!memory->Allocation.Cached)", "if (memory->Allocation.Cached)"),
            ("physical alignment", "memory.c", "        length,\n        alignment,\n        0,\n        &physical",
             "        length,\n        0,\n        0,\n        &physical"),
            ("exact block length", "memory.c", "memory->BlockPages[first] != count", "false"),
            ("virtual offset", "memory.c", "block->VirtualAddress != (void*)(memory->Mapping + offset)", "false"),
            ("interior page tracking", "memory.c", "memory->BlockPages[first + i] = __INTERIOR_PAGE;", "memory->BlockPages[first + i] = 0;"),
            ("duplicate block rejection", "memory.c", "memset(&memory->BlockPages[first], 0, count * sizeof(*memory->BlockPages));", "// Mutant keeps released block metadata."),
            ("clear returned block", "memory.c", "memset(block, 0, sizeof(*block));", "// Mutant keeps the caller's record."),
            ("live block destruction", "memory.c", "if (memory->BlockCount != 0)", "if (false)"),
            ("closing allocation gate", "memory.c", "memory->State == ArmSMMUMemoryReady ?", "memory->State != ArmSMMUMemoryEmpty ?"),
            ("failed state latch", "memory.c", "memory->State = ArmSMMUMemoryFailed;", "// Mutant leaves allocation enabled."),
            ("preserve unmap failure", "memory.c", "ERROR(\"ArmSMMUMemoryDestroy: CPU unmap failed: %u\", status);\n            return status;", "ERROR(\"ArmSMMUMemoryDestroy: CPU unmap failed: %u\", status);"),
            ("unmap exactly once", "memory.c", "memory->Mapping = 0;", "// Mutant repeats CPU unmap."),
            ("final free-page count", "memory.c", "MemoryStackFreeBlockCount(&memory->FreePages) != memory->PageCount", "false"),
            ("final block table", "memory.c", "memory->BlockPages[i] != 0", "false"),
            ("write-only permission", "stage2.h", "(1ULL << 7)", "(3ULL << 6)"),
            ("execute-never extension", "stage2.h", "(1ULL << 54)", "(3ULL << 53)"),
            ("descriptor address mask", "stage2.h", "0x0000FFFFFFFFF000ULL", "0x000FFFFFFFFFF000ULL"),
        )
        for mutation in mutations:
            host_run(output, mutation)
            print("Rejected arena mutation:", mutation[0], flush=True)


if __name__ == "__main__":
    main()
