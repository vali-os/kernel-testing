"""Check the MMC and eMMC setup: register decoding and the setup sequence against a fake host and device."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SD_DIRECTORY = ROOT / "modules/storage/sd"
SOURCES = ("sequence.c", "registers.c", "card.c", "mmc_registers.c", "mmc.c")
HEADERS = ("mmc_commands.h", "mmc_registers.h", "mmc.h")

# Each mutation must make an assertion in sd_mmc_test.c fail. The whole sd
# directory is copied and the named file is changed in the copy.
MUTATIONS = (
    ("CMD1 without the sector access mode", "mmc_commands.h",
     "return (voltageWindow & MMC_OCR_VOLTAGE_MASK) | MMC_OCR_ACCESS_MODE_SECTOR;",
     "return (voltageWindow & MMC_OCR_VOLTAGE_MASK);"),
    ("silent first CMD1 is not an empty slot", "mmc.c",
     "if (__NoAnswer(result) && !card->Setup.OperatingConditionAnswered) {",
     "if (__NoAnswer(result) && false) {"),
    ("CMD1 budget ignored", "mmc.c",
     "if (card->Setup.OperatingConditionWaitedUs >= __OCR_BUDGET_US) {",
     "if (card->Setup.OperatingConditionWaitedUs >= 2U * __OCR_BUDGET_US) {"),
    ("specification version below 4 accepted", "mmc.c",
     "if (card->MmcCardData.SpecVersion < MMC_CSD_SPEC_VERSION_4) {",
     "if (false) {"),
    ("EXT_CSD structure not checked", "mmc_registers.c",
     "if (extCsd.Structure > MMC_EXT_CSD_STRUCTURE_MAXIMUM) {",
     "if (false) {"),
    ("SEC_COUNT read with the highest byte first", "mmc_registers.c",
     "return (uint32_t)data[index] | ((uint32_t)data[index + 1U] << 8) |\n"
     "           ((uint32_t)data[index + 2U] << 16) | ((uint32_t)data[index + 3U] << 24);",
     "return ((uint32_t)data[index] << 24) | ((uint32_t)data[index + 1U] << 16) |\n"
     "           ((uint32_t)data[index + 2U] << 8) | (uint32_t)data[index + 3U];"),
    ("sector addressed capacity from the CSD", "mmc.c",
     "    card->SectorCount = extCsd->SectorCount;",
     "    card->SectorCount = card->MmcCardData.SectorCount;"),
    ("CSD capacity without C_SIZE + 1", "mmc_registers.c",
     "(uint64_t)csd->DeviceSize + 1U,",
     "(uint64_t)csd->DeviceSize,"),
    ("year not corrected for the revision", "mmc.c",
     "    MmcCidYearForRevision(&card->MmcIdentity, card->MmcExtendedData.Revision);\n",
     ""),
    ("eight lines without the host wiring", "mmc.c",
     "if ((widths & SD_HOST_BUS_WIDTH_8) != 0 && limit == SdCardWidthLimitNone) {",
     "if (limit == SdCardWidthLimitNone) {"),
    ("four line limit ignored", "mmc.c",
     "if ((widths & SD_HOST_BUS_WIDTH_8) != 0 && limit == SdCardWidthLimitNone) {",
     "if ((widths & SD_HOST_BUS_WIDTH_8) != 0 && limit != SdCardWidthLimitOneLine) {"),
    ("BUS_WIDTH value of four lines for eight", "mmc.c",
     "width == SdBusWidth8 ? MMC_BUS_WIDTH_8 : MMC_BUS_WIDTH_4,",
     "MMC_BUS_WIDTH_4,"),
    ("no EXT_CSD comparison after the width switch", "mmc.c",
     "if (!MmcExtCsdSameProperties(card->Setup.MmcFirstExtCsd, __ControlData(card))) {",
     "if (false) {"),
    ("SEC_COUNT not compared", "mmc_registers.c",
     "    212, // SEC_COUNT, lowest byte\n",
     ""),
    ("SWITCH_ERROR not in the card error mask", "mmc_commands.h",
     "                           MMC_R1_SWITCH_ERROR)",
     "                           0)"),
    ("SD bit 3 in the MMC card error mask", "mmc_commands.h",
     "MMC_R1_CC_ERROR | MMC_R1_ERROR | MMC_R1_CID_CSD_OVERWRITE |",
     "MMC_R1_CC_ERROR | MMC_R1_ERROR | MMC_R1_CID_CSD_OVERWRITE | 0x8U |"),
    ("state after a switch not checked", "mmc.c",
     "    if (!__ReadyForData(result->Response[0])) {\n        return SdStepFail(OS_EPROTOCOL);\n    }\n"
     "    return SdStepNext();\n}\n\n// Whether PARTITION_ACCESS",
     "    return SdStepNext();\n}\n\n// Whether PARTITION_ACCESS"),
    ("52 MHz for a device with HS_26 only", "mmc.c",
     "if (card->Timing == SdBusTimingHighSpeed && (card->MmcExtendedData.CardType & MMC_CARD_TYPE_HS_52) != 0) {",
     "if (card->Timing == SdBusTimingHighSpeed) {"),
    ("high speed without the host flag", "mmc.c",
     "(card->Host->Limits.Flags & SD_HOST_FLAG_HIGH_SPEED) != 0 &&",
     "true &&"),
    ("high speed without DEVICE_TYPE", "mmc.c",
     "return (card->MmcExtendedData.CardType & (MMC_CARD_TYPE_HS_26 | MMC_CARD_TYPE_HS_52)) != 0 &&",
     "return true &&"),
    ("timing limit ignored", "mmc.c",
     "card->Setup.Options.TimingLimit == SdCardTimingLimitNone;",
     "true;"),
    ("default timing at 52 MHz", "mmc.c",
     "#define __DEFAULT_SPEED_CLOCK_HZ SD_CARD_MMC_DEFAULT_SPEED_CLOCK_HZ",
     "#define __DEFAULT_SPEED_CLOCK_HZ SD_CARD_MMC_HIGH_SPEED_CLOCK_HZ"),
    ("data clock limit of the options ignored", "mmc.c",
     "    if (optionLimit != 0 && optionLimit < clockLimit) {",
     "    if (false) {"),
    ("partition access left as found", "mmc.c",
     "return (card->MmcExtendedData.PartitionConfig & MMC_PARTITION_ACCESS_MASK) != 0;",
     "return (card->MmcExtendedData.PartitionConfig & 0U) != 0;"),
    ("partition switch clears the boot bits", "mmc.c",
     "value = (uint8_t)(card->MmcExtendedData.PartitionConfig & ~MMC_PARTITION_ACCESS_MASK);",
     "value = 0;"),
    ("PARTITION_SWITCH_TIME ignored", "mmc.c",
     "__SwitchTimeoutUs(card, card->MmcExtendedData.PartitionSwitchTimeMs, __PARTITION_SWITCH_MINIMUM_MS)",
     "__SwitchTimeoutUs(card, 0, __PARTITION_SWITCH_MINIMUM_MS)"),
    ("write cache left on", "mmc.c",
     "return (card->MmcExtendedData.CacheControl & MMC_CACHE_CTRL_ON) != 0;",
     "return (card->MmcExtendedData.CacheControl & 0U) != 0;"),
    ("GENERIC_CMD6_TIME ignored", "mmc.c",
     "uint32_t timeoutMs = card->MmcExtendedData.GenericSwitchTimeMs;",
     "uint32_t timeoutMs = card->MmcExtendedData.GenericSwitchTimeMs & 0U;"),
    ("no lower bound on the switch time", "mmc.c",
     "    if (timeoutMs < __SWITCH_MINIMUM_MS) {",
     "    if (false) {"),
    ("width failure reported as a plain failure", "mmc.c",
     "return SdCardSetupBusWidthFailed;",
     "return SdCardSetupFailed;"),
    ("speed failure reported as a plain failure", "mmc.c",
     "return SdCardSetupHighSpeedFailed;",
     "return SdCardSetupFailed;"),
    ("host chooses relative address 2", "mmc.c",
     "#define __RELATIVE_ADDRESS 0x0001U",
     "#define __RELATIVE_ADDRESS 0x0002U"),
    ("CMD16 sent to a sector addressed device", "mmc.c",
     "    if (card->Addressing == SdCardAddressingSectors) {\n        return SdStartSkip();\n    }\n"
     "    __PrepareCommand(request, MMC_CMD_SET_BLOCKLEN",
     "    if (card->Addressing == SdCardAddressingSectors && false) {\n        return SdStartSkip();\n    }\n"
     "    __PrepareCommand(request, MMC_CMD_SET_BLOCKLEN"),
    ("card kind not set", "mmc.c",
     "    card->Kind = SdCardKindMmc;\n",
     ""),
    ("setup ignores where the sequence failed", "mmc.c",
     "card->Setup.Sequence.Failure == SdSequenceFailureHost",
     "false"),
    ("power off in front of the MMC CMD0", "mmc.c",
     "    oserr = __SetBus(card, clockLimit, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn, &clockHz);",
     "    (void)__SetBus(card, 0, SdBusWidth1, SdBusTimingDefault, SdBusPowerOff, &clockHz);\n"
     "    oserr = __SetBus(card, clockLimit, SdBusWidth1, SdBusTimingDefault, SdBusPowerOn, &clockHz);"),
    ("power-up wait after the bus setup", "mmc.c",
     "    card->Timing = SdBusTimingDefault;\n    return SdStartDone();",
     "    card->Timing = SdBusTimingDefault;\n    return SdStartDelay(36185U);"),
    ("bus setup clock not taken from the host", "mmc.c",
     "    card->ClockHz = clockHz;\n    card->Width = SdBusWidth1;",
     "    card->ClockHz = clockLimit + (clockHz & 0U);\n    card->Width = SdBusWidth1;"),
    ("bus settings name the wrong device kind", "mmc.c",
     "        .Device = SdBusDeviceMmc",
     "        .Device = SdBusDeviceSd"),
)


def host_flags():
    # The production sources log through OSSysLogWrite and read the control
    # buffer through DMABufferData and DMABufferSegments, which the test
    # defines.
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING", "-D__TRACE",
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
        include = '#include "' + str(SD_DIRECTORY / header) + '"\n'
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
        for source in SOURCES:
            subprocess.run(target_flags + includes + ["-c", str(SD_DIRECTORY / source),
                           "-o", str(output / (source + ".o"))], check=True)
        for header in HEADERS:
            probe = output / "header_probe.c"
            probe.write_text('#include "' + str(SD_DIRECTORY / header) + '"\n')
            subprocess.run(target_flags + includes + ["-fsyntax-only", str(probe)], check=True)
        print("MMC production compilation:", arch, "passed", flush=True)


def run_test(flags, source_directory, binary, environment):
    test_source = str(ROOT / "testing/sd_mmc_test.c")
    sources = [str(source_directory / source) for source in SOURCES]
    # The test includes the headers from the directory it runs against, so a
    # mutated header reaches the test as it reaches the sources.
    include = ["-I" + str(source_directory.parent)]
    subprocess.run(flags[:1] + include + flags[1:] + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sd-mmc-") as directory:
        output = Path(directory)
        binary = output / "sd_mmc"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, SD_DIRECTORY, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("MMC test failed: " + completed.stderr.decode())
        cross_compile(output)
        if not args.mutation_check:
            return

        # The mutant directory is named sd inside its own parent, so the
        # test's "sd/..." includes resolve to the mutant headers.
        mutant_directory = output / "mutant" / "sd"
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
            print("MMC mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
