"""Check the BCM2711 SDHCI controller variant: 32-bit access, shadows, write delay and capabilities."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "modules/storage"
# Sources linked into the test, relative to modules/storage. The time
# helpers of ddk/time.h and usched are replaced by the fake clock in
# sdhci_fake_host.inc.
TEST_SOURCES = ("sdhci/variants/bcm2711.c", "sdhci/variants/standard.c", "sdhci/pci.c", "sdhci/controller.c",
                "sdhci/adma.c", "sdhci/request.c", "sdhci/recovery.c", "sdhci/interrupt.c",
                "sdhci/worker.c", "sdhci/event.c")
# Sources and headers that must also build for every Vali target.
TARGET_SOURCES = ("sdhci/variants/bcm2711.c",)
HEADERS = ("sdhci/variants/bcm2711.h",)

BCM2711 = "sdhci/variants/bcm2711.c"

# Each mutation must make an assertion in sdhci_bcm2711_test.c fail. The path
# is relative to modules/storage, inside a copy of the source tree.
MUTATIONS = (
    ("command written before the block word is flushed", BCM2711,
     "    status = __FlushBlock(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "\n"
     "    if (context->Transfer.Pending) {\n"
     "        word = context->Transfer.Value;\n"
     "    } else {\n"
     "        word = __ReadWord(controller, SDHCI_TRANSFER_MODE_COMMAND);\n"
     "    }\n"
     "    context->Transfer.Pending = false;\n"
     "    return __WriteWord(controller, SDHCI_TRANSFER_MODE_COMMAND, __Merge(word, SDHCI_COMMAND, __HALF_MASK, value));",
     "    if (context->Transfer.Pending) {\n"
     "        word = context->Transfer.Value;\n"
     "    } else {\n"
     "        word = __ReadWord(controller, SDHCI_TRANSFER_MODE_COMMAND);\n"
     "    }\n"
     "    context->Transfer.Pending = false;\n"
     "    status = __WriteWord(controller, SDHCI_TRANSFER_MODE_COMMAND, __Merge(word, SDHCI_COMMAND, __HALF_MASK, value));\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "    return __FlushBlock(controller);"),
    ("no delay after the block flush", BCM2711,
     "    return __WriteWord(controller, SDHCI_BLOCK_SIZE_COUNT, context->Block.Value);",
     "    return DeviceIoWrite(&controller->Registers, SDHCI_BLOCK_SIZE_COUNT, context->Block.Value, sizeof(uint32_t));"),
    ("an 8-bit read let through", BCM2711,
     "    return (uint8_t)((__ReadWordOfField(controller, offset) >> __FieldShift(offset)) & __BYTE_MASK);",
     "    return (uint8_t)DeviceIoRead(&controller->Registers, offset, sizeof(uint8_t));"),
    ("minimum clock left at the divider's value", BCM2711,
     "        capabilitiesOut->MinimumClockHz = SDHCI_BCM2711_MINIMUM_CLOCK_HZ;",
     "        (void)0;"),
    ("delay rounded down", BCM2711,
     "+ cardClockHz - 1U) / cardClockHz;",
     ") / cardClockHz;"),
    ("line reset keeps the shadows", BCM2711,
     "    if (offset == SDHCI_SOFTWARE_RESET && (value & __RESET_BITS) != 0) {",
     "    if (offset == SDHCI_SOFTWARE_RESET && (value & SDHCI_RESET_ALL) != 0) {"),
    ("direct block word write keeps the shadow", BCM2711,
     "    if (offset == SDHCI_BLOCK_SIZE_COUNT) {\n"
     "        context->Block.Pending = false;\n"
     "    } else if",
     "    if (false) {\n"
     "    } else if"),
    ("reset bit written back with the clock word", BCM2711,
     "        word &= ~__RESET_BYTE_IN_WORD;",
     "        (void)0;"),
    ("interrupt status read-modify-written", BCM2711,
     "    if (wordOffset == SDHCI_INTERRUPT_STATUS) {\n"
     "        return 0;\n"
     "    }",
     ""),
    ("no write delay reported to the engine", BCM2711,
     "    (void)controller;\n"
     "    return SdhciBcm2711WriteDelayUs(cardClockHz);",
     "    (void)controller;\n"
     "    (void)cardClockHz;\n"
     "    return 0;"),
    ("Auto CMD12 not required", BCM2711,
     "    capabilitiesOut->HostFlags |= SD_HOST_FLAG_AUTO_CMD12_REQUIRED;",
     ""),
    ("minimum card clock not raised", BCM2711,
     "        capabilitiesOut->MinimumClockHz = SDHCI_BCM2711_MINIMUM_CLOCK_HZ;",
     "        (void)0;"),
    ("wired width ignored", "sdhci/controller.c",
     "    capabilities->BusWidths &= wiredWidths;\n",
     ""),
    ("board removable flag ignored", "sdhci/controller.c",
     "    if (values->Removable) {",
     "    if (true) {"),
    ("input clock used over the capability base clock", "sdhci/controller.c",
     "    if (capabilityClockHz != 0) {\n        // The capability value stays in use.",
     "    if (false) {\n        // The capability value stays in use."),
)


def host_flags():
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=",
             "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    flags += ["-I" + str(ROOT / path) for path in
              ("modules/storage", "testing/include", "librt/libos/include", "librt/libddk/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    return flags


def check_headers(output):
    # Each header must compile on its own, and twice in a row, to prove that it
    # includes what it needs and that its include guard works.
    flags = host_flags()
    for header in HEADERS:
        probe = output / "header_probe.c"
        include = '#include "' + str(STORAGE / header) + '"\n'
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
        for source in TARGET_SOURCES:
            subprocess.run(target_flags + includes + ["-c", str(STORAGE / source),
                           "-o", str(output / "target.o")], check=True)
        for header in HEADERS:
            probe = output / "header_probe.c"
            probe.write_text('#include "' + str(STORAGE / header) + '"\n')
            subprocess.run(target_flags + includes + ["-fsyntax-only", str(probe)], check=True)
        print("SDHCI BCM2711 production compilation:", arch, "passed", flush=True)


def run_test(flags, source_root, binary, environment):
    test_source = str(ROOT / "testing/sdhci_bcm2711_test.c")
    sources = [str(source_root / source) for source in TEST_SOURCES]
    # The shared fakes carry helpers for a PCI host (__DefaultHost, __Prepare)
    # that this test replaces with its own board setup.
    subprocess.run(flags + ["-Wno-unused-function", test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-bcm2711-") as directory:
        output = Path(directory)
        binary = output / "bcm2711"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, STORAGE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI BCM2711 test failed: " + completed.stderr.decode())
        cross_compile(output)
        if not args.mutation_check:
            return

        # The mutant sources include their headers by relative path, so the
        # sd and sdhci directories are copied together. The test itself keeps
        # using the original headers.
        for name, file_name, before, after in MUTATIONS:
            mutant_root = output / "mutant"
            if mutant_root.exists():
                shutil.rmtree(mutant_root)
            for directory_name in ("sd", "sdhci"):
                shutil.copytree(STORAGE / directory_name, mutant_root / directory_name)
            target = mutant_root / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            completed = run_test(flags, mutant_root, binary, environment)
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("SDHCI BCM2711 mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
