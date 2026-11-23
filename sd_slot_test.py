"""Check the SD slot (card detection, setup, disk, recovery and removal) against a fake host, DMA and IPC."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SD_DIRECTORY = ROOT / "modules/storage/sd"
SOURCES = ("sequence.c", "registers.c", "card.c", "sectors.c", "storage.c", "slot.c")
HEADERS = ("card.h", "storage.h", "slot.h")
# Protocols whose generated headers storage.c includes, directly or through
# the DDK storage helper.
PROTOCOLS = ("process", "storage", "contracts/storage")

# Each mutation must make an assertion in sd_slot_test.c fail. The whole sd
# directory is copied and the named file is changed in the copy.
MUTATIONS = (
    ("old storage device ID reused for a new card", "slot.c",
     "parameters->DeviceId = slot->Callbacks->AllocateStorageId(slot->Context);",
     "parameters->DeviceId = slot->StorageId != 0 ? slot->StorageId : "
     "slot->Callbacks->AllocateStorageId(slot->Context);"),
    ("write sent again after reinitialisation", "storage.c",
     "return request->Direction == SdDataRead && !request->RecoveryRetried;",
     "return !request->RecoveryRetried;"),
    ("new disk registered before the old one is unregistered", "slot.c",
     "    if (slot->Disk != NULL && !__DiskUnregistered(slot)) {\n        return;\n    }\n",
     ""),
    ("idle check while a request runs", "slot.c",
     "    if (SDStoragePause(slot->Disk) != OS_EOK) {\n        return;\n    }\n",
     "    (void)SDStoragePause(slot->Disk);\n"),
    ("third recovery does not fail the disk", "slot.c",
     "if (slot->RecoveryCount >= __RECOVERY_LIMIT) {",
     "if (slot->RecoveryCount > __RECOVERY_LIMIT) {"),
    ("second recovery keeps the data clock", "slot.c",
     "    clockHz /= 2U;\n",
     ""),
    ("eleven fast probes instead of ten", "slot.c",
     "if (slot->ProbeCount < __FAST_PROBE_COUNT) {\n                return __FAST_PROBE_INTERVAL_US;",
     "if (slot->ProbeCount <= __FAST_PROBE_COUNT) {\n                return __FAST_PROBE_INTERVAL_US;"),
    ("stopped reported twice", "slot.c",
     "    slot->Callbacks->Stopped(slot, status, slot->Context);\n",
     "    slot->Callbacks->Stopped(slot, status, slot->Context);\n"
     "    slot->Callbacks->Stopped(slot, status, slot->Context);\n"),
    ("same card gets a new media generation", "slot.c",
     "    slot->Card.MediaGeneration = slot->DiskGeneration;\n",
     ""),
    ("setup that failed in the host counted as a card failure", "slot.c",
     "        case SdCardSetupHostFailed:\n            // The runner has logged the step and the host's error. Another\n"
     "            // probe would fail the same way, so this is a host failure.\n"
     "            __Leave(slot, __LeaveSetupHostFailed);\n",
     "        case SdCardSetupHostFailed:\n            __OnSetupFailed(slot, result, status);\n"),
    ("second reset after setup failed in the host again", "slot.c",
     "if (slot->LeaveReason == __LeaveSetupHostFailed && slot->SetupHostReset == __SetupHostResetUsed) {",
     "if (false) {"),
    ("a ready card does not bring the reset back", "slot.c",
     "    slot->SetupHostReset = __SetupHostResetAvailable;\n",
     ""),
    ("width failure does not limit the next setup", "slot.c",
     "        slot->WidthLimit = SdCardWidthLimitOneLine;\n",
     ""),
    ("speed failure does not limit the next setup", "slot.c",
     "                slot->LogId);\n        slot->TimingLimit = SdCardTimingLimitDefaultSpeed;\n",
     "                slot->LogId);\n"),
    ("probe ignores the width limit", "slot.c",
     "        .WidthLimit = slot->WidthLimit,",
     "        .WidthLimit = SdCardWidthLimitNone,"),
    ("limits kept for the next card", "slot.c",
     "    slot->WidthLimit = SdCardWidthLimitNone;\n    slot->TimingLimit = SdCardTimingLimitNone;\n",
     ""),
    ("second recovery from high speed halves the clock", "slot.c",
     "    if (slot->DataTiming == SdBusTimingHighSpeed) {\n"
     "        slot->TimingLimit = SdCardTimingLimitDefaultSpeed;\n        return;\n    }\n",
     ""),
    ("timing of a ready card not remembered", "slot.c",
     "    slot->DataTiming = slot->Card.Timing;\n    slot->RecoveryCount = 0;",
     "    slot->RecoveryCount = 0;"),
    ("module not told when setup failed in the host", "slot.c",
     "if (reason == __LeaveHostFailed || reason == __LeaveSetupHostFailed) {",
     "if (reason == __LeaveHostFailed) {"),
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


def target_flags(compiler, arch):
    flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
             "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
             "-Wall", "-Wextra", "-Werror"]
    if arch == "aarch64":
        flags += ["-march=armv8-a", "-mno-outline-atomics"]
    return flags


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
        flags = target_flags(compiler, arch)
        for source in SOURCES:
            subprocess.run(flags + includes + ["-c", str(SD_DIRECTORY / source),
                           "-o", str(output / (source + ".o"))], check=True)
        for header in HEADERS:
            probe = output / "header_probe.c"
            probe.write_text('#include "' + str(SD_DIRECTORY / header) + '"\n')
            subprocess.run(flags + includes + ["-fsyntax-only", str(probe)], check=True)
        print("SD slot production compilation:", arch, "passed", flush=True)


def run_test(flags, source_directory, binary, environment):
    test_source = str(ROOT / "testing/sd_slot_test.c")
    sources = [str(source_directory / source) for source in SOURCES]
    subprocess.run(flags + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=300)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sd-slot-") as directory:
        output = Path(directory)
        binary = output / "sd_slot"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        protocols = generate_protocols(output)
        flags = host_flags(protocols)

        check_headers(flags, output)
        completed = run_test(flags, SD_DIRECTORY, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SD slot test failed: " + completed.stderr.decode())
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
            print("SD slot mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
