"""Check the SDHCI platform device glue: SD configuration, supplies, clock and the BCM2711 board values."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "modules/storage"
# Sources linked into the test, relative to modules/storage. timer.c is
# replaced by the fake clock in sdhci_fake_host.inc.
TEST_SOURCES = ("sdhci/platform.c", "sdhci/integrations/bcm2711.c", "sdhci/integrations/standard.c",
                "sdhci/controller.c", "sdhci/adma.c", "sdhci/request.c", "sdhci/recovery.c",
                "sdhci/interrupt.c", "sdhci/worker.c")
# Sources and headers that must also build for every Vali target.
TARGET_SOURCES = ("sdhci/platform.c",)
HEADERS = ("sdhci/platform.h",)

PLATFORM = "sdhci/platform.c"
BCM2711 = "sdhci/integrations/bcm2711.c"
CONTROLLER = "sdhci/controller.c"
INTERRUPT = "sdhci/interrupt.c"

# Each mutation must make an assertion in sdhci_platform_test.c fail. The
# path is relative to modules/storage, inside a copy of the source tree.
MUTATIONS = (
    ("SDIO host not left alone", PLATFORM,
     "    if (platform->Configuration.Role != DeviceSdRoleMemory) {",
     "    if (false) {"),
    ("vqmmc not taken over", PLATFORM,
     "    status = __ManageSupply(platform, DEVICE_SD_SUPPLY_VQMMC, SDHCI_PLATFORM_SUPPLY_VQMMC);",
     "    status = OS_EOK;"),
    ("card power switched before the signaling voltage", PLATFORM,
     "    if (__HasSupply(platform, DEVICE_SD_SUPPLY_VQMMC)) {\n"
     "        status = DeviceSupplySetVoltage(",
     "    if (__HasSupply(platform, DEVICE_SD_SUPPLY_VMMC)) {\n"
     "        (void)DeviceSupplySetState(platform->Device->Id, SDHCI_PLATFORM_SUPPLY_VMMC, 1);\n"
     "    }\n"
     "    if (__HasSupply(platform, DEVICE_SD_SUPPLY_VQMMC)) {\n"
     "        status = DeviceSupplySetVoltage("),
    ("always-on vmmc fails power off", PLATFORM,
     "    if (status == OS_ENOTSUPPORTED) {",
     "    if (false) {"),
    ("host register position ignored", PLATFORM,
     "    uint32_t         index = platform->Configuration.HostRegister;",
     "    uint32_t         index = 0;"),
    ("removable flag left to the slot type", BCM2711,
     "    if (context->Removable) {\n"
     "        capabilitiesOut->HostFlags |= SD_HOST_FLAG_REMOVABLE;\n"
     "    } else {\n"
     "        capabilitiesOut->HostFlags &= ~SD_HOST_FLAG_REMOVABLE;\n"
     "    }",
     ""),
    ("board clock limit ignored", BCM2711,
     "        capabilitiesOut->MaximumClockHz = context->MaximumClockHz;",
     "        (void)0;"),
    ("base clock mismatch not reported", BCM2711,
     "    if (capabilityClockHz != 0 && clockHz != 0 && capabilityClockHz != clockHz) {",
     "    if (false) {"),
    ("input clock read again for the base clock", BCM2711,
     "    if (context->InputClockRead) {\n"
     "        clockHz = context->InputClockHz;\n"
     "    } else if",
     "    if (false) {\n"
     "    } else if"),
    ("card supply left on at teardown", CONTROLLER,
     "    __SwitchCardSupplyOff(controller);\n",
     ""),
    ("platform interrupt source not selected", INTERRUPT,
     "    if (!controller->Interrupt.Object.IsPci) {\n"
     "        status = DeviceInterruptSelectSource(",
     "    if (false) {\n"
     "        status = DeviceInterruptSelectSource("),
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
        print("SDHCI platform production compilation:", arch, "passed", flush=True)


def run_test(flags, source_root, binary, environment):
    test_source = str(ROOT / "testing/sdhci_platform_test.c")
    sources = [str(source_root / source) for source in TEST_SOURCES]
    # The shared fakes carry helpers for a PCI host (__DefaultHost, __Prepare)
    # that this test does not use.
    subprocess.run(flags + ["-Wno-unused-function", test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-platform-") as directory:
        output = Path(directory)
        binary = output / "platform"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, STORAGE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI platform test failed: " + completed.stderr.decode())
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
            print("SDHCI platform mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
