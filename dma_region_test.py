"""Exercise prepared device-region sessions against real SHM and DMA recovery."""
import argparse
import signal
import subprocess
import tempfile
from pathlib import Path
from dma_bound_test import ROOT, build_host_fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-region-") as directory:
        output = Path(directory)
        flags, sources, environment = build_host_fixture(output)
        (output / "spinlock.h").unlink()
        mutex = output / "mutex.h"
        mutex.write_text(mutex.read_text().replace("static inline void MutexLock", "void TestRegionMutex(void);\nstatic inline void MutexLock").replace(
            "{ assert(pthread_mutex_lock", "{ TestRegionMutex(); assert(pthread_mutex_lock"))
        sources[0] = "testing/dma_region_test.c"
        sources += ["kernel/sync/spinlock.c"]
        paths = [str(ROOT / path) for path in sources]
        binary = output / "regions"

        def run(inputs, mutation=None):
            subprocess.run(flags + inputs + ["-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], env=environment, capture_output=True, timeout=45)
            if mutation is None:
                print(result.stdout.decode(), end="")
                if result.returncode:
                    print(result.stderr.decode())
                result.check_returncode()
            else:
                assert result.returncode == -signal.SIGABRT and b"Assertion" in result.stderr, (mutation, result.stderr)
                print("Device-region mutation rejected:", mutation)

        run(paths)
        if args.mutation_check:
            mutations = (
                ("lose interrupted close object", "kernel/api/dma_region.c",
                 "return SHMDeviceRegionSessionDestroy(record, false);",
                 "SHMRecordDetachObject(record);\n    return SHMDeviceRegionSessionDestroy(record, false);"),
                ("use mapping direction for reads", "kernel/memory/shm/device/region_use.c",
                 "SHMDeviceRegionsCache(mapping, session->Region, direction, false);",
                 "SHMDeviceRegionsCache(mapping, session->Region, mapping->Direction, false);"),
                ("lose failed begin generation", "kernel/memory/shm/device/region_use.c",
                 "token->Access.Generation = token->Generation;", "token->Access.Generation = 0;"),
                ("complete stale token", "kernel/memory/shm/device/region_use.c",
                 "token->Generation == generation", "token->Generation != 0"),
                ("unmap with sessions", "kernel/memory/shm/device/cleanup.c",
                 "current->Regions->SessionCount != 0", "current->Regions->SessionCount == 0"),
                ("skip post-stop cache", "kernel/memory/shm/device/region_mapping.c",
                 "SHMDeviceRegionsCache(mapping, region, mapping->Direction, true);", "(void)mapping;"),
            )
            for name, path, before, after in mutations:
                original = (ROOT / path).read_text()
                assert before in original, name
                mutant = output / "mutant.c"
                mutant.write_text(original.replace(before, after))
                if path == "kernel/api/dma_region.c":
                    flags += ['-DDMA_REGION_API_SOURCE="' + str(mutant) + '"']
                    run(paths, name)
                    flags.pop()
                    continue
                compile_flags = ["-I" + str((ROOT / path).parent)]
                flags += compile_flags
                run([str(mutant) if item == str(ROOT / path) else item for item in paths], name)


if __name__ == "__main__":
    main()
