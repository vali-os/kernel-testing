"""Check the SDHCI engine setup, reset, bus settings, the standard variant and the PCI slot."""
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
# sdhci_fake_host.inc. controller.c publishes the request and worker
# operations, so the whole engine is linked.
TEST_SOURCES = ("sdhci/controller.c", "sdhci/variants/standard.c", "sdhci/pci.c", "sdhci/adma.c",
                "sdhci/request.c", "sdhci/recovery.c", "sdhci/interrupt.c", "sdhci/worker.c",
                "sdhci/event.c")
# Sources and headers that must also build for every Vali target.
TARGET_SOURCES = TEST_SOURCES
HEADERS = ("sdhci/sdhci.h", "sdhci/variants/standard.h", "sdhci/pci.h")

# Each mutation must make an assertion in sdhci_controller_test.c fail. The
# path is relative to modules/storage, inside a copy of the source tree.
MUTATIONS = (
    ("clock divider one step too fast", "sdhci/controller.c",
     "divisor = ((uint64_t)baseClockHz + requestedHz - 1U) / requestedHz;",
     "divisor = (uint64_t)baseClockHz / requestedHz;"),
    ("bus power written before the voltage select", "sdhci/controller.c",
     "status = SdhciWrite8(controller, SDHCI_POWER_CONTROL, voltage);",
     "status = SdhciWrite8(controller, SDHCI_POWER_CONTROL, voltage | SDHCI_POWER_BUS_ON);"),
    ("power control not restored after a reset", "sdhci/controller.c",
     "if (settings->Power == SdBusPowerOn) {\n        status = __WritePowerOn(controller);",
     "if (false) {\n        status = __WritePowerOn(controller);"),
    ("DeviceStart before both DMA allocations", "sdhci/controller.c",
     "if (controller == NULL || (controller->Stages & SDHCI_STAGE_CONFIGURED) == 0 ||\n"
     "        (controller->Stages & SDHCI_STAGE_STARTED) != 0) {",
     "if (controller == NULL || (controller->Stages & SDHCI_STAGE_STARTED) != 0) {"),
    ("version 2 host with reserved base clock bits accepted", "sdhci/variants/standard.c",
     "if ((raw->Capabilities & __CAPS_BASE_CLOCK_V3_ONLY) != 0) {",
     "if (false) {"),
    ("64-bit ADMA2 bit used before version 3", "sdhci/variants/standard.c",
     "if (specVersion >= SDHCI_SPEC_300 && (raw->Capabilities & SDHCI_CAPS_64BIT_V3) != 0) {",
     "if (specVersion >= SDHCI_SPEC_100 && (raw->Capabilities & SDHCI_CAPS_64BIT_V3) != 0) {"),
    ("host without ADMA2 accepted unless it reports SDMA", "sdhci/controller.c",
     "if (!__DmaEngineIsAdma2(capabilities->DmaEngine)) {",
     "if (capabilities->DmaEngine == SDHCI_DMA_ENGINE_SDMA) {"),
    ("removable PCI slot wired for eight lines", "sdhci/pci.c",
     "if (!removable && (capabilities->BusWidths & SD_HOST_BUS_WIDTH_8) != 0) {",
     "if ((capabilities->BusWidths & SD_HOST_BUS_WIDTH_8) != 0) {"),
    ("slot wiring not applied", "sdhci/controller.c",
     "    capabilities->BusWidths &= wiredWidths;\n", ""),
    ("slot card detect ignored", "sdhci/controller.c",
     "    if ((controller->SlotValues.Flags & SDHCI_SLOT_CARD_DETECT) == 0) {\n        return OS_ENOTSUPPORTED;",
     "    if (false) {\n        return OS_ENOTSUPPORTED;"),
    ("1.8 V bit not set", "sdhci/controller.c",
     "        value = SDHCI_HOST_CONTROL2_1V8_ENABLE;",
     "        value = 0;"),
    ("1.8 V bit not restored after a reset", "sdhci/controller.c",
     "    // reported is kept, and its bit goes back before anything else.\n"
     "    status = __WriteHostControl2(controller);",
     "    // reported is kept, and its bit goes back before anything else.\n"
     "    status = SdhciWrite16(controller, SDHCI_HOST_CONTROL2, 0);"),
    ("1.8 V bit not read back", "sdhci/controller.c",
     "    if ((readBack & SDHCI_HOST_CONTROL2_1V8_ENABLE) == 0) {",
     "    if (false) {"),
    ("1.8 V refusal skipped", "sdhci/controller.c",
     "    if (voltage == SdhciSignalVoltage180 && !__HostSignals180(controller)) {",
     "    if (voltage == SdhciSignalVoltage180 && !__HostSignals180(controller) && false) {"),
    ("1.8 V accepted on a version 2 host", "sdhci/controller.c",
     "    return controller->Capabilities.SpecVersion >= SDHCI_SPEC_300;",
     "    return controller->Capabilities.SpecVersion >= SDHCI_SPEC_100;"),
    ("1.8 V signaling gated on the 1.8 V bus power bit", "sdhci/controller.c",
     "    return controller->Capabilities.SpecVersion >= SDHCI_SPEC_300;",
     "    return controller->Capabilities.SpecVersion >= SDHCI_SPEC_300 &&\n"
     "        (controller->Capabilities.Voltages & SDHCI_CAPS_VOLTAGE_180) != 0;"),
    ("card supply before the signaling voltage", "sdhci/controller.c",
     "    status = __SelectSignalVoltage(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "    if (controller->Bus.SignalVoltage == SdhciSignalVoltage180) {",
     "    (void)controller->Slot->SetCardPower(controller, SdBusPowerOn);\n"
     "    status = __SelectSignalVoltage(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "    if (controller->Bus.SignalVoltage == SdhciSignalVoltage180) {"),
    ("host control 2 written at 3.3 V power on", "sdhci/controller.c",
     "    if (controller->Bus.SignalVoltage == SdhciSignalVoltage180) {\n"
     "        status = __WriteHostControl2(controller);",
     "    if (true) {\n"
     "        status = __WriteHostControl2(controller);"),
    ("variant bus hook skipped by SetBus", "sdhci/controller.c",
     "    status = __ConfigureVariantBus(controller, settings);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "\n"
     "    *clockHzOut = 0;",
     "    *clockHzOut = 0;"),
    ("variant bus hook skipped after a reset", "sdhci/controller.c",
     "    status = __ConfigureVariantBus(controller, settings);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "\n"
     "    if (settings->ClockHz == 0) {",
     "    if (settings->ClockHz == 0) {"),
    ("invalid device kind accepted", "sdhci/controller.c",
     "    if (settings->Device != SdBusDeviceSd && settings->Device != SdBusDeviceMmc) {",
     "    if (false) {"),
    ("variant window size not checked", "sdhci/controller.c",
     "        controller->Registers.Access.Memory.Length < minimumSize) {",
     "        controller->Registers.Access.Memory.Length < SDHCI_REGISTER_WINDOW_SIZE) {"),
    ("config window size not checked", "sdhci/controller.c",
     "        controller->ConfigRegisters.Access.Memory.Length < minimumSize) {",
     "        false) {"),
    ("config window not released", "sdhci/controller.c",
     "    status = DeviceIoRelease(&controller->ConfigRegisters);",
     "    status = OS_EOK;"),
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
        print("SDHCI controller production compilation:", arch, "passed", flush=True)


def run_test(flags, source_root, binary, environment):
    test_source = str(ROOT / "testing/sdhci_controller_test.c")
    sources = [str(source_root / source) for source in TEST_SOURCES]
    subprocess.run(flags + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-controller-") as directory:
        output = Path(directory)
        binary = output / "controller"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, STORAGE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI controller test failed: " + completed.stderr.decode())
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
            print("SDHCI controller mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
