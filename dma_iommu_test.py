"""Run translated DMA against a host IOMMU model and compile production targets."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from shm_device_test import ROOT, run_host_test


# All translation units use the same checked host mutex layout. The C fixture
# implements these functions and rejects nested native locks. The simulator
# uses its own pthread mutex below the native unit lock.
MUTEX = """#ifndef __VALI_MUTEX_H__
#define __VALI_MUTEX_H__
#include <pthread.h>
typedef struct { pthread_mutex_t Native; } Mutex_t;
#define MUTEX_FLAG_PLAIN 0
void MutexConstruct(Mutex_t*, unsigned int);
void MutexDestruct(Mutex_t*);
void MutexLock(Mutex_t*);
void MutexUnlock(Mutex_t*);
#endif
"""

SOURCES = ("testing/dma_iommu_test.c", "testing/dma_iommu_sim.c",
           "kernel/memory/dma/address.c", "kernel/memory/dma/direct.c",
           "kernel/memory/dma/unit.c", "kernel/memory/dma/translated.c",
           "kernel/memory/dma/iova.c", "librt/libds/rbtree.c")


def compile_targets(output):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    includes = ["-I" + str(ROOT / path) for path in (
        "kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
        "librt/libddk/include", "librt/libds/include")]
    header = output / "backend_header.c"
    header.write_text('#include "' + str(ROOT / "kernel/memory/dma/private.h") + '"\n'
                      '_Static_assert(sizeof(struct DMAAddressSpace) % '
                      '_Alignof(struct SHMDeviceRange) == 0, "direct range tail alignment");\n')
    for arch in ("i386", "amd64", "aarch64"):
        flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                 "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2", "-Wall", "-Werror",
                 "-D__LIBDS_KERNEL__", "-D__OSCONFIG_ENABLE_MULTIPROCESSORS"]
        if arch == "aarch64":
            flags += ["-march=armv8-a", "-mno-outline-atomics"]
        subprocess.run(flags + includes + ["-fsyntax-only", str(header)], check=True)
        for name in ("address", "direct", "unit", "translated", "iova"):
            subprocess.run(flags + includes + ["-c", str(ROOT / ("kernel/memory/dma/" + name + ".c")),
                           "-o", str(output / (arch + "-" + name + ".obj"))], check=True)
        print("DMA IOMMU production compilation passed:", arch, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-iommu-") as directory:
        output = Path(directory)
        mutex = output / "mutex.h"
        mutex.write_text(MUTEX)
        flags = ("-D_POSIX_C_SOURCE=200809L", "-D__LIBDS_KERNEL__", "-include", str(mutex),
                 "-Wl,--wrap=DMAIOMMUUnitCreate", "-Wl,--wrap=DMAIOVAAllocationRelease")
        header = str(ROOT / "testing/dma_iommu_sim.h")
        run_host_test("dma-iommu-test", header, SOURCES, flags)
        compile_targets(output)
        if args.mutation_check:
            run_mutations(output, header, flags)


def run_mutations(output, header, flags):
    """Each mutation must compile and be rejected at an assertion."""
    mutations = (
        ("release without backend removal", "kernel/memory/dma/translated.c",
         "mapping->State != DMAAddressMappingPrepared && mapping->State != DMAAddressMappingRemoved", "false"),
        ("forget removal completion", "kernel/memory/dma/translated.c",
         "mapping->State = DMAAddressMappingRemoved;", "// Mutant forgets completed removal."),
        ("forget installation attempt", "kernel/memory/dma/translated.c",
         "mapping->State = DMAAddressMappingInstallStarted;", "// Mutant permits another install."),
        ("repeat a failed installation", "kernel/memory/dma/address.c",
         "mapping->State != DMAAddressMappingPrepared", "mapping->State == DMAAddressMappingInstalled"),
        ("ignore failure after installation", "kernel/memory/dma/translated.c",
         """    status = DMAAddressSpaceCheck(space);
    if (status != OS_EOK) {
        MutexUnlock(&unit->Lock);
        return status;
    }
    mapping->State = DMAAddressMappingInstalled;""",
         """    status = OS_EOK;
    mapping->State = DMAAddressMappingInstalled;"""),
        ("ignore failure after removal", "kernel/memory/dma/translated.c",
         """        status = DMAAddressSpaceCheck(space);
        if (status != OS_EOK) {
            MutexUnlock(&unit->Lock);
            return status;
        }
        // Record completion""",
         """        status = OS_EOK;
        // Record completion"""),
        ("ignore shared unit failure", "kernel/memory/dma/address.c",
         "atomic_load(&space->Translated.Unit->Failure) != OS_EOK", "false"),
        ("discard a release pointer on error", "kernel/memory/dma/address.c",
         """        status = DMATranslatedMappingRelease(released);
        if (status != OS_EOK) {
            return status;
        }""",
         """        status = DMATranslatedMappingRelease(released);
        if (status != OS_EOK) {
            *mapping = NULL;
            return status;
        }"""),
        ("drop the mapping space reference", "kernel/memory/dma/address.c",
         "status = DMAAddressSpaceAcquire(space);",
         "status = DMAAddressSpaceAcquire(space); if (status == OS_EOK) { atomic_fetch_sub(&space->References, 1); }"),
        ("accept partial granules", "kernel/memory/dma/translated.c",
         "length == 0 || (physical & mask) != 0 || (length & mask) != 0 ||",
         "length == 0 || (mask & 0) != 0 ||"),
        ("ignore device limit", "kernel/memory/dma/translated.c",
         "request.Limit = addressLimit;", "(void)addressLimit; request.Limit = UINT64_MAX;"),
        ("grant both device permissions", "kernel/memory/dma/translated.c",
         "translated->Config.Direction = direction;", "translated->Config.Direction = DMABidirectional;"),
        ("keep caller extents instead of the copy", "kernel/memory/dma/translated.c",
         "translated->Config.Extents = translated->Extents;", "translated->Config.Extents = extents;"),
        ("leave cached translations after completion", "testing/dma_iommu_sim.c",
         "leaf->Cache[i].Present = false;", "// Mutant keeps stale translation."),
        ("discard cache before invalidation completion", "testing/dma_iommu_sim.c",
         "leaf->Table[index % __SIM_LEAF_ENTRIES].Present = false;",
         """leaf->Table[index % __SIM_LEAF_ENTRIES].Present = false;
        if (leaf->Cache[index % __SIM_LEAF_ENTRIES].Present) {
            leaf->Cache[index % __SIM_LEAF_ENTRIES].Present = false;
            sim->Stats.CachedTranslations--;
        }"""),
        ("ignore device permissions", "testing/dma_iommu_sim.c",
         "leaf->Cache[slot].Direction != DMABidirectional && leaf->Cache[slot].Direction != access",
         "((void)access, false)"),
        ("resubmit pending command tokens", "testing/dma_iommu_sim.c",
         "command->State != __SimCommandFresh", "false"),
        ("restart the removal deadline", "testing/dma_iommu_sim.c",
         "status = __Submit(sim, &mapping->Removal);",
         "deadline = sim->Now + timeoutNs; status = __Submit(sim, &mapping->Removal);"),
        ("publish output on failed preparation", "kernel/memory/dma/address.c",
         """    if (status != OS_EOK) {
        DMAAddressSpaceRelease(&space);
        return status;
    }""",
         """    if (status != OS_EOK) {
        *mappingOut = (struct DMAAddressMapping*)space;
        DMAAddressSpaceRelease(&space);
        return status;
    }"""),
        ("drop backend record after failed unpublished cleanup", "kernel/memory/dma/translated.c",
         "unit->Ops.DestroyMapping(space->Translated.BackendSpace, translated->BackendMapping);",
         "// Mutant leaks backend reference to config storage."),
    )
    for name, path, before, after in mutations:
        original = (ROOT / path).read_text()
        expected = 2 if name == "drop backend record after failed unpublished cleanup" else 1
        assert original.count(before) == expected, name
        mutant = output / Path(path).name
        mutant.write_text(original.replace(before, after, 1))
        sources = tuple(str(mutant) if source == path else source for source in SOURCES)
        extra = flags + ("-I" + str((ROOT / path).parent),)
        run_host_test("dma-iommu-mutant", header, sources, extra, mutation=name)
        print("Rejected DMA IOMMU mutation:", name, flush=True)



if __name__ == "__main__":
    main()
