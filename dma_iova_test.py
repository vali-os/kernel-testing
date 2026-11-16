"""Validate the DMA IOVA allocator, failure paths and production target builds."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from shm_device_test import ROOT, run_host_test


def compile_targets(output):
    """Use production headers so the host mutex cannot hide a target build error."""
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    includes = ["-I" + str(ROOT / path) for path in (
        "kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
        "librt/libddk/include", "librt/libds/include")]
    header = output / "iova_header.c"
    header.write_text('#include "' + str(ROOT / "kernel/memory/dma/iova.h") + '"\n')
    for arch in ("i386", "amd64", "aarch64"):
        flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                 "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                 "-Wall", "-Werror", "-D__LIBDS_KERNEL__", "-D__OSCONFIG_ENABLE_MULTIPROCESSORS"]
        if arch == "aarch64":
            flags += ["-march=armv8-a", "-mno-outline-atomics"]
        subprocess.run(flags + includes + ["-fsyntax-only", str(header)], check=True)
        subprocess.run(flags + includes + ["-c", str(ROOT / "kernel/memory/dma/iova.c"),
                       "-o", str(output / (arch + "-iova.obj"))], check=True)
        print("DMA IOVA production compilation passed:", arch, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    sources = ("testing/dma_iova_test.c", "librt/libds/rbtree.c")
    header = str(ROOT / "kernel/memory/dma/iova.h")
    flags = ("-D__LIBDS_KERNEL__", "-D_POSIX_C_SOURCE=200809L",
             "-Wl,--wrap=rb_tree_append", "-Wl,--wrap=rb_tree_remove")
    run_host_test("dma-iova-test", header, sources, flags)
    with tempfile.TemporaryDirectory(prefix="vali-dma-iova-") as directory:
        output = Path(directory)
        compile_targets(output)
        if not args.mutation_check:
            return
        original = (ROOT / "kernel/memory/dma/iova.c").read_text()
        mutations = (
            ("exclude the inclusive limit", "request->Length - 1 > last - candidate",
             "request->Length > last - candidate"),
            ("truncate allocation addresses", "rangeOut->First = candidate;",
             "rangeOut->First = (uint32_t)candidate;"),
            ("truncate tree key comparison", "uint64_t first = *(const uint64_t*)left;",
             "uint64_t first = (uint32_t)*(const uint64_t*)left;"),
            ("ignore reserved predecessor overlap", "interval->Range.Last >= range->First", "false"),
            ("ignore reserved successor overlap", "interval->Range.First <= range->Last", "false"),
            ("skip address alignment", "uint64_t padding = (alignment - (address & mask)) & mask;",
             "uint64_t padding = mask & 0;"),
            ("skip boundary adjustment", "request->Length - 1 > remaining", "false"),
            ("skip occupied interval insertion", "result = rb_tree_append(&allocator->Intervals, &interval->Node);",
             "(void)allocator; result = 0;"),
            ("skip interval removal", "node = rb_tree_remove(&allocator->Intervals, &released->Interval.Range.First);",
             "node = &released->Interval.Node;"),
            ("destroy with live allocations", "if (released->ActiveCount != 0) {", "if (false) {"),
            ("allocate during release", "released = *allocation;",
             "released = *allocation; void* extra = kmalloc(1); "
             "if (extra == NULL) { return OS_EOOM; } kfree(extra);"),
            ("omit allocator locking", "MutexLock(&allocator->Lock);", "// Mutant omits allocator lock."),
            ("hide insertion failure", "if (result != 0) {", "if (false) {"),
            ("accept partial granule lengths", "(request->Length & (allocator->Granularity - 1)) != 0", "false"),
        )
        for name, before, after in mutations:
            assert before in original, name
            mutant = output / "iova_mutant.c"
            mutant.write_text(original.replace(before, after))
            extra = flags + ('-DDMA_IOVA_SOURCE="' + str(mutant) + '"',
                             "-I" + str(ROOT / "kernel/memory/dma"))
            run_host_test("dma-iova-mutant", header, sources, extra, mutation=name)
            print("Rejected DMA IOVA mutation:", name, flush=True)


if __name__ == "__main__":
    main()
