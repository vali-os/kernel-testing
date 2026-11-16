"""Build the real SHM device context with the existing host-test header setup."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_host_test(name, header, sources, extra_flags=(), *, mutation=None):
    """Build kernel sources with sanitizers and run them, without a boot.

    Shared with other kernel memory host tests so they all use the same flags.
    A named mutation must compile and fail at an assertion. Build errors,
    timeouts and other runtime failures remain test errors.
    """
    with tempfile.TemporaryDirectory(prefix="vali-" + name + "-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING", "-fms-extensions", "-fasm-blocks",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-g", "-pthread"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        # SHM's private header and the DDK barriers pull these in. Their own
        # warnings are not under test.
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        # A separate translation unit catches accidental reliance on include order.
        header_source = output / "header.c"
        header_source.write_text("#include <" + header + ">\n")
        subprocess.run(flags + ["-fsyntax-only", str(header_source)], check=True)
        flags += ["-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
        flags += list(extra_flags)
        if any(path in sources for path in ("kernel/memory/shm/shm.c", "kernel/memory/shm/access.c", "kernel/memory/shm/device/device.c")):
            sources = (*sources, "kernel/memory/shm/view.c",
                       "kernel/memory/space/mapping.c")
        if "kernel/memory/shm/device/context.c" in sources:
            sources = (*sources, "kernel/memory/dma/address.c", "kernel/memory/dma/direct.c",
                       "kernel/memory/shm/device/layout.c",
                       "kernel/memory/shm/device/cleanup.c", "kernel/memory/shm/device/attachment.c")
        if "kernel/memory/dma/direct.c" in sources:
            # Both modes are linked even when the fixture creates only direct
            # spaces. Keep the common dispatcher and allocator code real.
            if "kernel/memory/dma/iova.c" not in sources:
                sources = (*sources, "kernel/memory/dma/unit.c", "kernel/memory/dma/translated.c",
                           "kernel/memory/dma/iova.c", "librt/libds/rbtree.c")
                sources = tuple(dict.fromkeys(sources))
            flags += ["-D__LIBDS_KERNEL__"]
        binary = str(output / name)
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
        # Match other host tests: explicit live-allocation counts cover leaks
        # even in sandboxes where LeakSanitizer cannot inspect the process.
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        result = subprocess.run([binary], env=environment, timeout=30,
                                capture_output=mutation is not None)
        if mutation is None:
            result.check_returncode()
        elif result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr:
            raise AssertionError(
                f"Mutation did not fail at an assertion: {mutation}\n"
                f"Exit status: {result.returncode}\n"
                + (result.stdout + result.stderr).decode(errors="replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    sources = ("testing/shm_device_test.c", "kernel/memory/shm/device/context.c",
               "kernel/memory/shm/access.c", "testing/shm_access_futex.c",
               "kernel/memory/shm/device/device.c", "kernel/components/memory.c",
               "kernel/utils/memory_stack.c")
    run_host_test("shm-device-test", "shm/device.h", sources)
    if not args.mutation_check:
        return

    original = (ROOT / "kernel/memory/shm/access.c").read_text()
    mutations = (
        ("admit an initially cancelled reservation",
         "if (access->Cancelled) {\n        MutexUnlock(&buffer->Mutex);",
         "if (false) {\n        MutexUnlock(&buffer->Mutex);"),
        ("ignore cancellation after a wait",
         "if (access->Cancelled) {\n            status = OS_ECANCELLED;",
         "if (false) {\n            status = OS_ECANCELLED;"),
        ("remove quarantine when a waiter returns",
         "} else if (access->Phase != SHMAccessQuarantined) {", "} else {"),
        ("quarantine only the submitted prefix",
         "    __AccessSetBounds(access, 0, access->Length);", "    // Mutant leaves old bounds."),
        ("extend cancelled access",
         "if (access->Cancelled) {\n        status = OS_ECANCELLED;",
         "if (false) {\n        status = OS_ECANCELLED;"),
        ("remove quarantine through ordinary end",
         "access->Phase != SHMAccessIdle && access->Phase != SHMAccessQuarantined",
         "access->Phase != SHMAccessIdle"),
    )
    for name, old, new in mutations:
        assert original.count(old) == 1, name
        with tempfile.TemporaryDirectory(prefix="vali-shm-mutation-") as directory:
            source = Path(directory) / "access.c"
            # The copied production file still includes its module-private header.
            source.write_text(original.replace(old, new).replace(
                '#include "private.h"',
                '#include "' + str(ROOT / "kernel/memory/shm/private.h") + '"'
            ).replace(
                '#include "../utils.h"',
                '#include "' + str(ROOT / "kernel/memory/utils.h") + '"'
            ))
            mutated = tuple(str(source) if path == "kernel/memory/shm/access.c" else path
                            for path in sources)
            run_host_test("shm-device-mutant", "shm/device.h", mutated, mutation=name)
            print("Rejected mutation:", name, flush=True)


if __name__ == "__main__":
    main()
