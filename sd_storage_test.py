"""Check SD sector I/O and the SD storage adapter against a fake host, fake DMA and fake IPC."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SD_DIRECTORY = ROOT / "modules/storage/sd"
SOURCES = ("sequence.c", "registers.c", "sectors.c", "storage.c")
HEADERS = ("sectors.h", "storage.h")
# Protocols whose generated headers storage.c includes, directly or through
# the DDK storage helper.
PROTOCOLS = ("process", "storage", "contracts/storage")

# Each mutation must make an assertion in sd_storage_test.c fail. The whole sd
# directory is copied and the named file is changed in the copy.
MUTATIONS = (
    ("send a failed write a second time", "sectors.c",
     "    return SdStepFail(oserr);\n}\n\n/////////////////////////////////////////////////////////////////////////////\n"
     "// Programming check after a write.",
     "    if (io->Direction == SdDataWrite && io->ProgramWaitedUs++ == 0) {\n"
     "        return SdStepGoTo(__StepData, 0);\n    }\n"
     "    return SdStepFail(oserr);\n}\n\n/////////////////////////////////////////////////////////////////////////////\n"
     "// Programming check after a write."),
    ("answer before DMAUnmap", "storage.c",
     "    if (request->Mapping != NULL) {\n        oserr = DMAUnmap(&request->Mapping);",
     "    __Answer(adapter, request);\n    if (request->Mapping != NULL) {\n        oserr = DMAUnmap(&request->Mapping);"),
    ("sector number used on a byte addressed card", "sectors.c",
     "*addressOut = (uint32_t)(firstSector * SD_SECTOR_SIZE);",
     "*addressOut = (uint32_t)firstSector;"),
    ("queue limit off by one", "storage.c",
     "if (queued >= SD_STORAGE_QUEUE_LIMIT) {",
     "if (queued > SD_STORAGE_QUEUE_LIMIT) {"),
    ("one more device pool attempt than the 1 second budget", "storage.c",
     "if (request->WaitedUs >= __POOL_WAIT_BUDGET_US) {",
     "if (request->WaitedUs > __POOL_WAIT_BUDGET_US) {"),
    ("end the transfer with the whole chunk instead of the confirmed bytes", "storage.c",
     "DMAEndTransfer(request->Mapping, (size_t)request->ChunkConfirmed * SD_STORAGE_SECTOR_SIZE);",
     "DMAEndTransfer(request->Mapping, (size_t)request->ChunkSectors * SD_STORAGE_SECTOR_SIZE);"),
    ("explicit CMD12 on a host that requires Auto CMD12", "sectors.c",
     "        return SdStopAutoCmd12;",
     "        return SdStopCmd12;"),
    ("repeat DMAEndTransfer after it succeeded", "storage.c",
     "        request->TransferActive = false;\n",
     ""),
    ("retry a read without the CMD13 check", "sectors.c",
     "return SdStepGoTo(__StepReadStatus, 0);",
     "return SdStepGoTo(__StepData, 0);"),
)


def generate_protocols(output):
    directory = output / "protocols"
    directory.mkdir()
    for protocol in PROTOCOLS:
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols" / (protocol + ".gr")),
                        "--out", str(directory), "--lang-c", "--client", "--server"],
                       check=True, stdout=subprocess.DEVNULL)
    return directory


def host_flags(protocols):
    # The fake StorageAttachTransfer header comes first. The generated
    # protocol headers are system headers, so their own warnings do not count.
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-fms-extensions", "-DVALI", "-DTESTING",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    flags += ["-I" + str(ROOT / "testing/sd_storage_include")]
    flags += ["-I" + str(ROOT / path) for path in
              ("modules/storage", "testing/include", "librt/libos/include", "librt/libddk/include",
               "librt/libgracht/include", "librt/libds/include")]
    flags += ["-isystem", str(protocols), "-idirafter", str(ROOT / "librt/libc/include")]
    return flags


def check_headers(flags, output):
    # Each header must compile on its own, and twice in a row.
    for header in HEADERS:
        probe = output / "header_probe.c"
        include = '#include "' + str(SD_DIRECTORY / header) + '"\n'
        probe.write_text(include + include + "int main(void) { return 0; }\n")
        subprocess.run(flags + ["-fsyntax-only", str(probe)], check=True)
        print("Header compiles alone and twice:", header, flush=True)


def cross_compile(protocols, output):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    if not Path(compiler).exists():
        print("Vali compiler not found, cross-compilation skipped:", compiler, flush=True)
        return
    includes = ["-I" + str(ROOT / path) for path in (
        "modules/storage", "librt/libc/include", "librt/libos/include",
        "librt/libddk/include", "librt/libds/include", "librt/libgracht/include")]
    includes += ["-isystem", str(protocols)]
    for arch in ("i386", "amd64", "aarch64"):
        target_flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                        "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                        "-Wall", "-Wextra", "-Werror"]
        if arch == "aarch64":
            target_flags += ["-march=armv8-a", "-mno-outline-atomics"]
        for source in SOURCES:
            subprocess.run(target_flags + includes + ["-c", str(SD_DIRECTORY / source),
                           "-o", str(output / (source + ".o"))], check=True)
        for header in HEADERS:
            probe = output / "header_probe.c"
            probe.write_text('#include "' + str(SD_DIRECTORY / header) + '"\n')
            subprocess.run(target_flags + includes + ["-fsyntax-only", str(probe)], check=True)
        print("SD storage production compilation:", arch, "passed", flush=True)


def run_test(flags, source_directory, binary, environment):
    test_source = str(ROOT / "testing/sd_storage_test.c")
    sources = [str(source_directory / source) for source in SOURCES]
    subprocess.run(flags + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sd-storage-") as directory:
        output = Path(directory)
        binary = output / "sd_storage"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        protocols = generate_protocols(output)
        flags = host_flags(protocols)

        check_headers(flags, output)
        completed = run_test(flags, SD_DIRECTORY, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SD storage test failed: " + completed.stderr.decode())
        cross_compile(protocols, output)
        if not args.mutation_check:
            return

        # The mutant sources include their headers from their own directory,
        # so the whole directory is copied. The test itself keeps using the
        # original headers, which the mutations never change.
        mutant_directory = output / "mutant"
        for name, file_name, before, after in MUTATIONS:
            if mutant_directory.exists():
                shutil.rmtree(mutant_directory)
            shutil.copytree(SD_DIRECTORY, mutant_directory)
            target = mutant_directory / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            completed = run_test(flags, mutant_directory, binary, environment)
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("SD storage mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
