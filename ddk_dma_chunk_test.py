"""Check DMAMapSHMChunk against scripted DMAMapSHMDirect and DMAMapSHM results."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "librt/libddk/dma/chunk.c"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-ddk-dma-chunk-") as directory:
        output = Path(directory)
        binary = output / "chunk"
        # The real chunk.c is linked against fakes of the two public map calls
        # defined in the test, so no syscall layer is needed.
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        test_source = str(ROOT / "testing/ddk_dma_chunk_test.c")
        subprocess.run(flags + [test_source, str(ROOT / SOURCE), "-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)
        if not args.mutation_check:
            return
        mutations = (
            ("map the whole bounce length once without shrinking",
             "if (status != OS_EOOM && status != OS_EBUFFER) {",
             "if (true) {"),
            ("drop straight to one block instead of halving",
             "attemptLength = __RoundDownToBlocks(attemptLength / 2, blockLength);",
             "attemptLength = blockLength;"),
            ("fall back to a bounce buffer on every direct error",
             "if (status != OS_ENOENT && status != OS_ENOTSUPPORTED) {",
             "if (false) {"),
            ("use limits without rounding them to whole blocks",
             "__RoundDownToBlocks(limits->DirectLength, limits->BlockLength)\n    );",
             "limits->DirectLength\n    );"),
        )
        original = (ROOT / SOURCE).read_text()
        for name, before, after in mutations:
            assert original.count(before) == 1, name
            mutant = output / "chunk_mutant.c"
            mutant.write_text(original.replace(before, after))
            subprocess.run(flags + [test_source, str(mutant), "-o", str(binary)], check=True)
            completed = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=30)
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("DMA chunk mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
