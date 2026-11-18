"""Check the SDHCI ADMA2 descriptor builder and the SD host and register headers."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "modules/storage/sdhci/adma.c"
HEADERS = ("modules/storage/sd/host.h", "modules/storage/sdhci/registers.h",
           "modules/storage/sdhci/adma.h")

# Each mutation must make an assertion in sdhci_adma_test.c fail. The file
# named first is copied next to the other one and changed there.
MUTATIONS = (
    ("allow 65,536-byte entries with the zero length encoding", "adma.h",
     "#define SDHCI_ADMA_ENTRY_MAXIMUM_LENGTH 65532U",
     "#define SDHCI_ADMA_ENTRY_MAXIMUM_LENGTH 65536U"),
    ("put the end attribute on the first entry", "adma.c",
     "if ((uint64_t)index + 1U == entryCount) {",
     "if (index == 0) {"),
    ("drop the 4 GiB check of the 32-bit format", "adma.c",
     "if (format == SdhciAdmaFormat32 && lastAddress > __ADMA_32BIT_LAST_ADDRESS) {",
     "if (format == SdhciAdmaFormat32 && lastAddress > UINT64_MAX) {"),
    ("size the table with the 32-bit entry size", "adma.c",
     "capacity = size / entrySize;",
     "capacity = size / SDHCI_ADMA_ENTRY_SIZE_32;"),
    ("accept lengths that are not a multiple of four", "adma.c",
     "(segmentLength % SDHCI_ADMA_DATA_ALIGNMENT) != 0) {",
     "false) {"),
)


def host_flags():
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
             "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    flags += ["-I" + str(ROOT / path) for path in
              ("modules/storage", "testing/include", "librt/libos/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    return flags


def check_headers(output):
    # Each header must compile on its own, and twice in a row, to prove that it
    # includes what it needs and that its include guard works.
    flags = host_flags()
    for header in HEADERS:
        probe = output / "header_probe.c"
        include = '#include "' + str(ROOT / header) + '"\n'
        probe.write_text(include + include + "int main(void) { return 0; }\n")
        subprocess.run(flags + ["-fsyntax-only", str(probe)], check=True)
        print("Header compiles alone and twice:", header, flush=True)


def cross_compile(output):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    if not Path(compiler).exists():
        print("Vali compiler not found, cross-compilation skipped:", compiler, flush=True)
        return
    includes = ["-I" + str(ROOT / path) for path in (
        "modules/storage", "librt/libc/include", "librt/libos/include",
        "librt/libddk/include", "librt/libds/include")]
    for arch in ("i386", "amd64", "aarch64"):
        target_flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                        "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                        "-Wall", "-Wextra", "-Werror"]
        if arch == "aarch64":
            target_flags += ["-march=armv8-a", "-mno-outline-atomics"]
        subprocess.run(target_flags + includes + ["-c", str(ROOT / SOURCE),
                       "-o", str(output / "adma.o")], check=True)
        for header in HEADERS:
            probe = output / "header_probe.c"
            probe.write_text('#include "' + str(ROOT / header) + '"\n')
            subprocess.run(target_flags + includes + ["-fsyntax-only", str(probe)], check=True)
        print("SDHCI ADMA production compilation:", arch, "passed", flush=True)


def run_test(flags, source, binary, environment):
    test_source = str(ROOT / "testing/sdhci_adma_test.c")
    subprocess.run(flags + [test_source, str(source), "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-adma-") as directory:
        output = Path(directory)
        binary = output / "adma"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, ROOT / SOURCE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI ADMA test failed: " + completed.stderr.decode())
        cross_compile(output)
        if not args.mutation_check:
            return

        # The mutant adma.c includes "adma.h" from its own directory, so both
        # files are copied. The test itself keeps using the original header,
        # which holds the expected values.
        mutant_directory = output / "mutant"
        mutant_directory.mkdir()
        for name, file_name, before, after in MUTATIONS:
            for copy in ("adma.c", "adma.h"):
                shutil.copy(ROOT / "modules/storage/sdhci" / copy, mutant_directory / copy)
            target = mutant_directory / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            completed = run_test(flags, mutant_directory / "adma.c", binary, environment)
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("SDHCI ADMA mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
