"""Check the BCM2712 SDHCI controller variant: config block, presence, pins, command queueing and capabilities."""
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
TEST_SOURCES = ("sdhci/variants/bcm2712.c", "sdhci/variants/standard.c", "sdhci/pci.c", "sdhci/controller.c",
                "sdhci/adma.c", "sdhci/request.c", "sdhci/recovery.c", "sdhci/interrupt.c",
                "sdhci/worker.c", "sdhci/event.c")
# Sources and headers that must also build for every Vali target.
TARGET_SOURCES = ("sdhci/variants/bcm2712.c",)
HEADERS = ("sdhci/variants/bcm2712.h",)

BCM2712 = "sdhci/variants/bcm2712.c"

# Each mutation must make an assertion in sdhci_bcm2712_test.c fail. The path
# is relative to modules/storage, inside a copy of the source tree.
MUTATIONS = (
    ("presence not forced for broken-cd", BCM2712,
     "    if (values->Removable == 0 || (values->Flags & SDHCI_SLOT_BROKEN_CARD_DETECT) != 0) {",
     "    if (values->Removable == 0) {"),
    ("presence not forced for a soldered device", BCM2712,
     "    if (values->Removable == 0 || (values->Flags & SDHCI_SLOT_BROKEN_CARD_DETECT) != 0) {",
     "    if ((values->Flags & SDHCI_SLOT_BROKEN_CARD_DETECT) != 0) {"),
    ("test level left as firmware set it", BCM2712,
     "    value &= ~(SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_EN | SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_LEV);",
     "    value &= ~SDHCI_BCM2712_CFG_CTRL_SDCD_TEST_EN;"),
    ("detect pin not selected for a host detect line", BCM2712,
     "    if ((values->Flags & SDHCI_SLOT_CARD_DETECT) != 0) {\n        return __PresenceFromPin;",
     "    if (false) {\n        return __PresenceFromPin;"),
    ("register written for a GPIO detect line", BCM2712,
     "    if (rule == __PresenceUnchanged) {\n        return OS_EOK;",
     "    if (false) {\n        return OS_EOK;"),
    ("presence not restored", BCM2712,
     "    status = __ApplyPresence(controller);",
     "    status = controller->Stages != 0 ? OS_EOK : __ApplyPresence(controller);"),
    ("queueing capability without its unit", BCM2712,
     "        (SDHCI_BCM2712_CQ_UNIT_1MHZ << SDHCI_BCM2712_CQ_UNIT_SHIFT) | frequencyMhz",
     "        frequencyMhz"),
    ("queueing capability not capped", BCM2712,
     "    if (frequencyMhz > SDHCI_BCM2712_CQ_FREQUENCY_MASK) {",
     "    if (false) {"),
    ("queueing capability zero below 1 MHz", BCM2712,
     "    if (frequencyMhz == 0) {\n        frequencyMhz = 1;",
     "    if (false) {\n        frequencyMhz = 1;"),
    ("command queueing engine left on", BCM2712,
     "    if ((configuration & SDHCI_BCM2712_CQHCI_ENABLE) == 0) {\n        return OS_EOK;",
     "    if (true) {\n        return OS_EOK;"),
    ("command queueing engine that stays on accepted", BCM2712,
     "    if ((configuration & SDHCI_BCM2712_CQHCI_ENABLE) != 0) {",
     "    if (false) {"),
    ("MMC pins for SD high speed", BCM2712,
     "    if (settings->Device == SdBusDeviceMmc && settings->Timing == SdBusTimingHighSpeed) {",
     "    if (settings->Timing == SdBusTimingHighSpeed) {"),
    ("MMC pins at default timing", BCM2712,
     "    if (settings->Device == SdBusDeviceMmc && settings->Timing == SdBusTimingHighSpeed) {",
     "    if (settings->Device == SdBusDeviceMmc) {"),
    ("old pin selection kept", BCM2712,
     "    value &= ~SDHCI_BCM2712_CFG_PIN_SEL_MASK;\n",
     ""),
    ("64-bit table format kept", BCM2712,
     "        capabilitiesOut->DmaEngine = SDHCI_DMA_ENGINE_ADMA2;",
     "        (void)0;"),
    ("host window size not raised", BCM2712,
     "    .RegisterWindowSize = SDHCI_BCM2712_REGISTER_WINDOW_SIZE,",
     "    .RegisterWindowSize = SDHCI_REGISTER_WINDOW_SIZE,"),
    ("config window not asked for", BCM2712,
     "    .Flags = SDHCI_VARIANT_CONFIG_REGISTERS,",
     "    .Flags = 0,"),
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
        print("SDHCI BCM2712 production compilation:", arch, "passed", flush=True)


def run_test(flags, source_root, binary, environment):
    test_source = str(ROOT / "testing/sdhci_bcm2712_test.c")
    sources = [str(source_root / source) for source in TEST_SOURCES]
    # The shared fakes carry helpers for a PCI host (__DefaultHost, __Prepare)
    # that this test replaces with its own board setup.
    subprocess.run(flags + ["-Wno-unused-function", test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-bcm2712-") as directory:
        output = Path(directory)
        binary = output / "bcm2712"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, STORAGE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI BCM2712 test failed: " + completed.stderr.decode())
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
            print("SDHCI BCM2712 mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
