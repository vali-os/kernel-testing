"""Check the Raspberry Pi firmware driver: mailbox transport, property layer, GPIO and clock services,
setup and teardown, against a fake mailbox and firmware on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "modules/platform/rpi-firmware"
# Sources linked into the test. main.c is included by the test file. The
# time helpers of ddk/time.h and usched are replaced by the fake clock.
TEST_SOURCES = ("clock.c", "controller.c", "gpio.c", "log.c", "mailbox.c", "property.c")
# Every module source must also build for each Vali target.
TARGET_SOURCES = TEST_SOURCES + ("main.c",)
HEADERS = ("firmware.h", "log.h", "mailbox.h", "property.h")
CONTRACTS = ("driver", "gpio", "clock")

# Each mutation must make an assertion in rpi_firmware_test.c fail. The path
# is relative to the module directory, inside a copy of it.
MUTATIONS = (
    ("slot of a timed-out call used again before its answer", "mailbox.c",
     "        mailbox->WaitingSlots |= 1ULL << slot;\n",
     ""),
    ("write without room in MAIL1", "mailbox.c",
     "        if (!(status & RPI_MAILBOX_STATUS_FULL)) {",
     "        if (true) {"),
    ("stale answers not drained at setup", "mailbox.c",
     "    // A boot loader may have left answers behind. No slot waits yet, so\n"
     "    // every word read here is dropped.\n"
     "    RpiMailboxDrain(mailbox);\n",
     ""),
    ("deadline never passes", "mailbox.c",
     "    return DDKGetTimeUs(OSTimeSource_MONOTONIC) - startUs >= RPI_MAILBOX_CALL_DEADLINE_US;",
     "    return DDKGetTimeUs(OSTimeSource_MONOTONIC) - startUs >= 4 * RPI_MAILBOX_CALL_DEADLINE_US;"),
    ("no sleep after the busy phase", "mailbox.c",
     "    if (usched_job_sleep_us(RPI_MAILBOX_SLEEP_INTERVAL_US) != 0) {",
     "    if (DDKStallUs(RPI_MAILBOX_SLEEP_INTERVAL_US) != OS_EOK) {"),
    ("oversized answer accepted", "property.c",
     "    if (length > request->ValueWords * sizeof(uint32_t)) {",
     "    if (false) {"),
    ("short answer refused", "property.c",
     "    request->AnswerLength = length;\n",
     "    request->AnswerLength = length;\n"
     "    if (length < request->AnswerWords * sizeof(uint32_t)) {\n"
     "        *failureOut = (RpiFailure_t) { RpiFailureShortAnswer, length };\n"
     "        return OS_EPROTOCOL;\n"
     "    }\n"),
    ("disable_turbo word not cleared", "clock.c",
     "    request->Value[__VALUE_DISABLE_TURBO] = 0;\n",
     "    request->Value[__VALUE_DISABLE_TURBO] = clock;\n"),
    ("old clock state still refused", "clock.c",
     "    if (oserr == OS_EOK && ((request.Value[__VALUE_STATE] & __STATE_ON) != 0) != enabled &&\n"
     "        RpiLogFirstTime(&firmware->Notes, request.Tag, RpiFailureClockUnchanged)) {",
     "    if (oserr == OS_EOK && ((request.Value[__VALUE_STATE] & __STATE_ON) != 0) != enabled) {\n"
     "        oserr = OS_EDEVFAULT;"),
    ("tag code bit 31 not checked", "property.c",
     "    if (!request->Answered && !request->AcceptUnanswered) {",
     "    if (false) {"),
    ("tag code bit 31 not answered with OS_EPROTOCOL", "property.c",
     "        *failureOut = (RpiFailure_t) { RpiFailureNotAnswered, tagCode };\n        return OS_EPROTOCOL;",
     "        *failureOut = (RpiFailure_t) { RpiFailureNotAnswered, tagCode };\n        return OS_ENOTSUPPORTED;"),
    ("GPIO answers need tag code bit 31", "gpio.c",
     "    request->AcceptUnanswered = true;",
     "    request->AcceptUnanswered = false;"),
    ("length checked without tag code bit 31", "property.c",
     "    if (!request->Answered) {\n        request->AnswerLength = 0;\n    } else {",
     "    if (false) {\n    } else {"),
    ("missing bit 31 noted on every answer", "property.c",
     "        if (RpiLogFirstTime(notes, request->Tag, RpiFailureNotAnswered)) {",
     "        if (true) {"),
    ("missing bit 31 noted at trace level", "property.c",
     "            WARNING(\"RPI-FW: tag 0x%08x answered with tag code",
     "            NOTICE(\"RPI-FW: tag 0x%08x answered with tag code"),
    ("length note for an answer without bit 31", "property.c",
     "        }\n        return OS_EOK;\n    }\n\n    // Linux checks no answer lengths.",
     "        }\n    }\n\n    // Linux checks no answer lengths."),
    ("slot not cleared before a request", "property.c",
     "    for (uint32_t i = 0; i < __SLOT_WORDS; i++) {\n"
     "        words[i] = 0;\n"
     "    }\n",
     ""),
    ("gpio field not checked", "gpio.c",
     "    if (oserr == OS_EOK && request->Value[__VALUE_GPIO] != 0) {",
     "    if (false) {"),
    ("polarity not kept", "gpio.c",
     "    request.Value[__VALUE_POLARITY] = polarity;",
     "    request.Value[__VALUE_POLARITY] = 0 * polarity;"),
    ("line 8 sent to the firmware", "gpio.c",
     "    if (line >= RPI_GPIO_LINE_COUNT) {\n        return OS_EINVALPARAMS;\n    }\n\n    request.Tag = RPI_TAG_GET_GPIO_STATE;",
     "    request.Tag = RPI_TAG_GET_GPIO_STATE;"),
    ("missing clock not reported", "clock.c",
     "(request->Value[__VALUE_STATE] & __STATE_MISSING)",
     "false"),
    ("page freed while the firmware owes an answer", "controller.c",
     "        oserr = RpiMailboxHasWaitingSlots(&firmware->Mailbox) ? OS_EBUSY : OS_EOK;",
     "        oserr = OS_EOK;"),
    ("cleanup-only DMA context not closed", "controller.c",
     "    if (firmware->Dma != NULL) {\n        oserr = DMAClose(&firmware->Dma);",
     "    if (firmware->Dma != NULL && firmware->Buffer != NULL) {\n        oserr = DMAClose(&firmware->Dma);"),
    ("device served only after the enable returned", "main.c",
     "    __SetServing(device, true);\n    oserr = __EnableControllers(device);\n    if (oserr != OS_EOK) {\n        return oserr;\n    }\n",
     "    oserr = __EnableControllers(device);\n    __SetServing(device, true);\n    if (oserr != OS_EOK) {\n        return oserr;\n    }\n"),
    ("busy Disable ignored", "main.c",
     "    if (oserr != OS_EOK && oserr != OS_ENOENT) {\n        if (RpiLogShouldWrite(&device->DisableLimit",
     "    if (false) {\n        if (RpiLogShouldWrite(&device->DisableLimit"),
    ("one kind missing fails the device", "main.c",
     "    if (oserr == OS_ENOENT) {\n        NOTICE(",
     "    if (false) {\n        NOTICE("),
    ("repeated failures not held back", "log.c",
     "        now - limit->LastUs < RPI_LOG_REPEAT_INTERVAL_US) {",
     "        false) {"),
)


def generate_protocols(output):
    for contract in CONTRACTS:
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/contracts" / (contract + ".gr")),
                        "--out", str(output), "--lang-c", "--server"], check=True, stdout=subprocess.DEVNULL)


def host_flags(output, module_root):
    # The generated protocol headers carry helpers a source may not use, and
    # loops that compare signed and unsigned counts.
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI", "-DTESTING",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-fms-extensions",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-sign-compare",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread",
             "-I" + str(output), "-I" + str(module_root)]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "librt/libos/include", "librt/libddk/include",
               "librt/libds/include", "librt/libgracht/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    return flags


def check_headers(output):
    # Each header must compile on its own, and twice in a row, to prove that it
    # includes what it needs and that its include guard works.
    flags = host_flags(output, MODULE)
    for header in HEADERS:
        probe = output / "header_probe.c"
        include = '#include "' + str(MODULE / header) + '"\n'
        probe.write_text(include + include + "int main(void) { return 0; }\n")
        subprocess.run(flags + ["-fsyntax-only", str(probe)], check=True)
        print("Header compiles alone and twice:", header, flush=True)


def cross_compile(output):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    if not Path(compiler).exists():
        print("Vali compiler not found, cross-compilation skipped:", compiler, flush=True)
        return
    includes = ["-I" + str(output), "-I" + str(MODULE)]
    includes += ["-I" + str(ROOT / path) for path in (
        "librt/libc/include", "librt/libos/include", "librt/libddk/include",
        "librt/libds/include", "librt/libgracht/include")]
    for arch in ("i386", "amd64", "aarch64"):
        target_flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                        "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-sign-compare"]
        if arch == "aarch64":
            target_flags += ["-march=armv8-a", "-mno-outline-atomics"]
        for source in TARGET_SOURCES:
            subprocess.run(target_flags + includes + ["-c", str(MODULE / source),
                           "-o", str(output / "target.o")], check=True)
        print("rpi-firmware production compilation:", arch, "passed", flush=True)


def run_test(output, module_root, binary, environment):
    flags = host_flags(output, module_root)
    flags.append('-DRPI_FIRMWARE_MAIN_SOURCE="' + str(module_root / "main.c") + '"')
    sources = [str(module_root / source) for source in TEST_SOURCES]
    sources += [str(ROOT / "testing/rpi_firmware_test.c"), str(ROOT / "librt/libds/list.c")]
    subprocess.run(flags + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-rpi-firmware-") as directory:
        output = Path(directory)
        binary = output / "rpi_firmware"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"

        generate_protocols(output)
        check_headers(output)
        completed = run_test(output, MODULE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("rpi-firmware test failed: " + completed.stderr.decode())
        cross_compile(output)
        if not args.mutation_check:
            return

        for name, file_name, before, after in MUTATIONS:
            mutant_root = output / "mutant"
            if mutant_root.exists():
                shutil.rmtree(mutant_root)
            shutil.copytree(MODULE, mutant_root)
            target = mutant_root / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            try:
                completed = run_test(output, mutant_root, binary, environment)
            except subprocess.TimeoutExpired:
                raise AssertionError(f"Mutation hung instead of failing an assertion: {name}")
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("rpi-firmware mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
