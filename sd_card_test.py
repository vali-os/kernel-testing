"""Check the SD card layer: register decoding, the step sequence runner and card setup."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SD_DIRECTORY = ROOT / "modules/storage/sd"
SOURCES = ("sequence.c", "registers.c", "card.c")
HEADERS = ("commands.h", "registers.h", "sequence.h", "card.h")

# Each mutation must make an assertion in sd_card_test.c fail. The whole sd
# directory is copied and the named file is changed in the copy.
MUTATIONS = (
    ("repeat ACMD41 without CMD55 in front", "card.c",
     "return SdStepGoTo(__StepAppCommand, __OCR_RETRY_DELAY_US);",
     "return SdStepGoTo(__StepOperatingCondition, __OCR_RETRY_DELAY_US);"),
    ("set HCS after a CMD8 timeout", "card.c",
     "highCapacity = card->Setup.InterfaceConditionAnswered;",
     "highCapacity = true;"),
    ("compute version 2 capacity as C_SIZE * 1024", "registers.c",
     "(uint64_t)csd->DeviceSize + 1U,\n        __CSD2_SECTORS_PER_UNIT",
     "(uint64_t)csd->DeviceSize,\n        __CSD2_SECTORS_PER_UNIT"),
    ("call the done callback twice", "sequence.c",
     "    done(context, status);\n",
     "    done(context, status);\n    done(context, status);\n"),
    ("failed command logged without the command and its result", "sequence.c",
     "    if (verdict.Kind == SdStepVerdictFail) {",
     "    if (verdict.Kind == SdStepVerdictFail && false) {"),
    ("treat every CMD8 timeout as a legacy card", "card.c",
     "result->TransportErrors == SD_TRANSPORT_COMMAND_TIMEOUT;",
     "(result->TransportErrors & SD_TRANSPORT_COMMAND_TIMEOUT) != 0;"),
    ("failed host step reported as a failed card command", "sequence.c",
     "__Fail(sequence, SdSequenceFailureHost, start.Status);",
     "__Fail(sequence, SdSequenceFailureCommand, start.Status);"),
    ("command refused by the host reported as a failed card command", "sequence.c",
     "__Fail(sequence, SdSequenceFailureHost, oserr);\n    }\n}\n\n// Runs step index after",
     "__Fail(sequence, SdSequenceFailureCommand, oserr);\n    }\n}\n\n// Runs step index after"),
    ("setup ignores where the sequence failed", "card.c",
     "card->Setup.Sequence.Failure == SdSequenceFailureHost",
     "false"),
    ("ACMD6 asks for one data line", "card.c",
     "SD_ACMD_SET_BUS_WIDTH, SD_BUS_WIDTH_ARGUMENT_4,",
     "SD_ACMD_SET_BUS_WIDTH, SD_BUS_WIDTH_ARGUMENT_1,"),
    ("host switched to four lines before the card", "card.c",
     "{ \"ACMD6 SET_BUS_WIDTH\",      __BusWidthStart,           __BusWidthFinish },\n"
     "    [__StepHostBusWidth]       = { \"host bus width\",           __HostBusWidthStart,       NULL },",
     "{ \"ACMD6 SET_BUS_WIDTH\",      __HostBusWidthStart,       NULL },\n"
     "    [__StepHostBusWidth]       = { \"host bus width\",           __BusWidthStart,           __BusWidthFinish },"),
    ("four lines without checking the host wiring", "card.c",
     "(card->Host->Limits.BusWidths & SD_HOST_BUS_WIDTH_4) != 0 &&",
     "true &&"),
    ("four lines without checking the SCR", "card.c",
     "(card->Configuration.BusWidths & SD_SCR_BUS_WIDTH_4) != 0 &&",
     "true &&"),
    ("width limit ignored", "card.c",
     "card->Setup.Options.WidthLimit != SdCardWidthLimitOneLine;",
     "true;"),
    ("SD status width not checked", "card.c",
     "if (status.BusWidth != SD_STATUS_BUS_WIDTH_4) {",
     "if (false) {"),
    ("refused ACMD6 fails setup", "card.c",
     "return __KeepOneLine(card, \"ACMD6\", result);",
     "return ((void)card, SdStepFail(oserr));"),
    ("refused width still counted against the width", "card.c",
     "    card->Setup.Part = SdCardSetupPartBasic;\n    return SdStepGoTo(__StepSwitchCheck, 0);",
     "    (void)card;\n    return SdStepGoTo(__StepSwitchCheck, 0);"),
    ("high speed without the host flag", "card.c",
     "(card->Host->Limits.Flags & SD_HOST_FLAG_HIGH_SPEED) != 0 &&",
     "true &&"),
    ("high speed without checking the SCR version", "card.c",
     "card->Configuration.SpecVersion >= SD_SCR_SPEC_VERSION_1_10 &&",
     "(card->Configuration.SpecVersion | 1U) != 0 &&"),
    ("high speed without command class 10", "card.c",
     "(card->CardData.CommandClasses & SD_CSD_CLASS_SWITCH) != 0 &&",
     "true &&"),
    ("timing limit ignored", "card.c",
     "card->Setup.Options.TimingLimit == SdCardTimingLimitNone;",
     "true;"),
    ("switch mode instead of check mode", "card.c",
     "SDSwitchGroup1Argument(SD_SWITCH_MODE_CHECK, SD_SWITCH_ACCESS_HIGH_SPEED)",
     "SDSwitchGroup1Argument(SD_SWITCH_MODE_SWITCH, SD_SWITCH_ACCESS_HIGH_SPEED)"),
    ("check mode support bits not read", "card.c",
     "if ((status.Group1Support & (1U << SD_SWITCH_ACCESS_HIGH_SPEED)) == 0 ||",
     "if (false ||"),
    ("switch error value fails setup", "card.c",
     "if (status.Group1Selection == SD_SWITCH_FUNCTION_ERROR) {",
     "if (false) {"),
    ("host timing before the card switched", "card.c",
     "{ \"CMD6 SWITCH_FUNC switch\",  __SwitchStart,             __SwitchFinish },\n"
     "    [__StepSwitchWait]         = { \"switch wait\",              __SwitchWaitStart,         NULL },\n"
     "    [__StepHostTiming]         = { \"host timing\",              __HostTimingStart,         NULL },",
     "{ \"CMD6 SWITCH_FUNC switch\",  __HostTimingStart,         NULL },\n"
     "    [__StepSwitchWait]         = { \"switch wait\",              __SwitchWaitStart,         NULL },\n"
     "    [__StepHostTiming]         = { \"host timing\",              __SwitchStart,             __SwitchFinish },"),
    ("no wait after the switch", "card.c",
     "return SdStartDelay(__ClockCyclesUs(__SWITCH_CLOCKS, card->ClockHz));",
     "return SdStartSkip();"),
    ("high speed keeps the default speed clock", "card.c",
     "        clockLimit = __HIGH_SPEED_CLOCK_HZ;",
     "        clockLimit = __DEFAULT_SPEED_CLOCK_HZ;"),
    ("default speed after high speed counted as plain failure", "card.c",
     "card->Setup.Part = card->Width == SdBusWidth4 ? SdCardSetupPartBusWidth : SdCardSetupPartBasic;",
     "card->Setup.Part = SdCardSetupPartBasic;"),
    ("width failure reported as a plain failure", "card.c",
     "return SdCardSetupBusWidthFailed;",
     "return SdCardSetupFailed;"),
    ("speed failure reported as a plain failure", "card.c",
     "return SdCardSetupHighSpeedFailed;",
     "return SdCardSetupFailed;"),
    ("SD status width read from the wrong bits", "registers.c",
     "#define __SD_STATUS_BUS_WIDTH_START    510U",
     "#define __SD_STATUS_BUS_WIDTH_START    508U"),
    ("switch status selection read from group 2", "registers.c",
     "#define __SWITCH_GROUP1_SELECTED_START 376U",
     "#define __SWITCH_GROUP1_SELECTED_START 380U"),
    ("switch status accepts any version", "registers.c",
     "    if (status.StructureVersion != SD_SWITCH_STATUS_VERSION_0 &&\n"
     "        status.StructureVersion != SD_SWITCH_STATUS_VERSION_1) {",
     "    if (false) {"),
    ("CMD6 argument changes group 2", "commands.h",
     "return mode | (SD_SWITCH_KEEP_ALL & ~SD_SWITCH_GROUP_1_MASK) | (function & SD_SWITCH_GROUP_1_MASK);",
     "return mode | (0x00FFFF0FU & ~SD_SWITCH_GROUP_1_MASK) | (function & SD_SWITCH_GROUP_1_MASK);"),
    ("bus settings name the wrong device kind", "card.c",
     "        .Device = SdBusDeviceSd",
     "        .Device = SdBusDeviceMmc"),
)


def host_flags():
    # The production sources log through OSSysLogWrite and read the control
    # buffer through DMABufferData and DMABufferSegments, which the test
    # defines. __TRACE turns on the TRACE line that setup logs when it starts.
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
        print("SD card production compilation:", arch, "passed", flush=True)


def run_test(flags, source_directory, binary, environment):
    test_source = str(ROOT / "testing/sd_card_test.c")
    sources = [str(source_directory / source) for source in SOURCES]
    subprocess.run(flags + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sd-card-") as directory:
        output = Path(directory)
        binary = output / "sd_card"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, SD_DIRECTORY, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SD card test failed: " + completed.stderr.decode())
        cross_compile(output)
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
            print("SD card mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
