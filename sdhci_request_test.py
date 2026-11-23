"""Check the SDHCI request engine, interrupt handling, polling and worker."""
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
TEST_SOURCES = ("sdhci/controller.c", "sdhci/variants/standard.c", "sdhci/pci.c", "sdhci/adma.c",
                "sdhci/request.c", "sdhci/recovery.c", "sdhci/interrupt.c", "sdhci/worker.c",
                "sdhci/event.c")
# Sources and headers that must also build for every Vali target.
TARGET_SOURCES = TEST_SOURCES + ("sdhci/timer.c",)
HEADERS = ("sdhci/request.h", "sdhci/interrupt.h", "sdhci/worker.h", "sdhci/sdhci.h", "sdhci/timer.h",
           "sdhci/event.h")

# Each mutation must make an assertion in sdhci_request_test.c fail. The path
# is relative to modules/storage, inside a copy of the source tree.
MUTATIONS = (
    ("command register written before the argument", "sdhci/request.c",
     "    status = SdhciWrite32(controller, SDHCI_ARGUMENT, state->Command.Argument);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "\n"
     "    // Transfer mode and command in one 32-bit write, which BCM2711 needs.\n"
     "    // The command half is the upper half, so the write that starts the\n"
     "    // command is also the last one.\n"
     "    word = state->Command.TransferMode | ((uint32_t)state->Command.CommandBits << SDHCI_COMMAND_WORD_SHIFT);\n"
     "    return SdhciWrite32(controller, SDHCI_TRANSFER_MODE_COMMAND, word);",
     "    word = state->Command.TransferMode | ((uint32_t)state->Command.CommandBits << SDHCI_COMMAND_WORD_SHIFT);\n"
     "    status = SdhciWrite32(controller, SDHCI_TRANSFER_MODE_COMMAND, word);\n"
     "    if (status != OS_EOK) {\n"
     "        return status;\n"
     "    }\n"
     "    return SdhciWrite32(controller, SDHCI_ARGUMENT, state->Command.Argument);"),
    ("success reported when an error bit is in the same snapshot", "sdhci/request.c",
     "    if ((seen & SDHCI_INT_HANDLED_ERRORS) != 0) {\n"
     "        __OnCommandFailed(controller, seen & SDHCI_INT_HANDLED_ERRORS);\n"
     "        return;\n"
     "    }\n"
     "    if ((seen & SDHCI_INT_TRANSFER_COMPLETE) != 0) {\n"
     "        __OnTransferComplete(controller);\n"
     "    }",
     "    if ((seen & SDHCI_INT_TRANSFER_COMPLETE) != 0) {\n"
     "        __OnTransferComplete(controller);\n"
     "        return;\n"
     "    }\n"
     "    if ((seen & SDHCI_INT_HANDLED_ERRORS) != 0) {\n"
     "        __OnCommandFailed(controller, seen & SDHCI_INT_HANDLED_ERRORS);\n"
     "    }"),
    ("HardwareStopped set without the line reset completing", "sdhci/request.c",
     "    status = SdhciRecoveryResetLines(controller);\n"
     "    SdhciRecoveryReport(controller, &diagnostics, status == OS_EOK);\n"
     "    if (status != OS_EOK) {",
     "    status = SdhciRecoveryResetLines(controller);\n"
     "    SdhciRecoveryReport(controller, &diagnostics, status == OS_EOK);\n"
     "    result->HardwareStopped = true;\n"
     "    if (status != OS_EOK) {"),
    ("Complete called from inside Submit", "sdhci/request.c",
     "    __Accept(controller, request);\n"
     "    return OS_EOK;",
     "    __Accept(controller, request);\n"
     "    request->Complete(request);\n"
     "    return OS_EOK;"),
    ("fast handler acknowledges with a read-modify-write of all bits", "sdhci/interrupt.c",
     "    if (interruptTable->WriteIoSpace(registers, SDHCI_INTERRUPT_STATUS, status, sizeof(uint32_t)) != OS_EOK) {",
     "    if (interruptTable->WriteIoSpace(registers, SDHCI_INTERRUPT_STATUS,\n"
     "            interruptTable->ReadIoSpace(registers, SDHCI_INTERRUPT_STATUS, sizeof(uint32_t)),\n"
     "            sizeof(uint32_t)) != OS_EOK) {"),
    ("fence does not wait for running fast handlers", "sdhci/interrupt.c",
     "        if (atomic_load(&controller->Interrupt.Shared.Running) == 0) {",
     "        if (true) {"),
    ("polled wait never slows down after 2 ms", "sdhci/request.c",
     "    return SDHCI_POLL_SLOW_INTERVAL_US;\n}",
     "    return SDHCI_POLL_FAST_INTERVAL_US;\n}"),
    ("engine CMD12 waits for data inhibit", "sdhci/request.c",
     "    if (!command->IsStop && (command->HasData || SdResponseHasBusy(command->ResponseType))) {",
     "    if (command->HasData || SdResponseHasBusy(command->ResponseType) || command->IsStop) {"),
    ("equal due times run out of arrival order", "sdhci/worker.c",
     "while (*link != NULL && (*link)->HostDueUs <= work->HostDueUs) {",
     "while (*link != NULL && (*link)->HostDueUs < work->HostDueUs) {"),
    ("signals armed while a write delay applies", "sdhci/request.c",
     "    if (!state->Polled) {\n"
     "        status = SdhciInterruptArm(controller);",
     "    if (true) {\n"
     "        status = SdhciInterruptArm(controller);"),
    ("raised event counter counted as a failed signal", "sdhci/interrupt.c",
     "    if (signalStatus != OS_EOK && signalStatus != OS_EINCOMPLETE) {",
     "    if (signalStatus != OS_EOK) {"),
    ("worker join without a place for the exit code", "sdhci/worker.c",
     "    if (usched_job_join(worker->Job, &exitCode) != 0) {",
     "    (void)exitCode;\n"
     "    if (usched_job_join(worker->Job, NULL) != 0) {"),
    ("fence drops a saved card change", "sdhci/interrupt.c",
     "    (void)SdhciInterruptTake(controller);\n    return OS_EOK;",
     "    atomic_store(&controller->Interrupt.Deferred, 0);\n"
     "    atomic_store(&controller->Interrupt.Shared.Pending, 0);\n    return OS_EOK;"),
    ("card change reported without a debounce", "sdhci/event.c",
     "    if (nowUs - cardDetect->StableSinceUs >= SDHCI_CARD_DEBOUNCE_US) {",
     "    if (true) {"),
    ("event delivered at once from inside the caller", "sdhci/event.c",
     "    events->Count++;\n}",
     "    events->Count++;\n    SdhciEventDeliver(controller);\n}"),
    ("both card detect directions enabled at once", "sdhci/event.c",
     "    watchBit = present ? SDHCI_INT_CARD_REMOVAL : SDHCI_INT_CARD_INSERTION;",
     "    watchBit = SDHCI_INT_CARD_DETECT;"),
    ("removal followed by insertion merged into nothing", "sdhci/event.c",
     "    if (cardDetect->Present && cardDetect->RemovalSeen) {\n        __Report(controller, false);",
     "    if (false) {\n        __Report(controller, false);"),
    ("card change during a full reset not looked for", "sdhci/controller.c",
     "    SdhciCardDetectAfterReset(controller);\n    return OS_EOK;",
     "    return OS_EOK;"),
    ("removal does not end the accepted request", "sdhci/request.c",
     "    if (!SdhciCardDetectAllowsRequests(controller)) {\n        __OnCardRemoved(controller);",
     "    if (false) {\n        __OnCardRemoved(controller);"),
    ("CMD12 started for a removed card", "sdhci/request.c",
     " &&\n        (state->Active->Result.TransportErrors & SD_TRANSPORT_CARD_REMOVED) == 0;",
     ";"),
    ("removal settle not postponed while a request is accepted", "sdhci/event.c",
     "    if (controller->Request.Active != NULL && !SdhciCardDetectAllowsRequests(controller)) {",
     "    if (false) {"),
    ("card bits read only after the status enable write", "sdhci/interrupt.c",
     "    latched = SdhciRead32(controller, SDHCI_INTERRUPT_STATUS) & SDHCI_INT_CARD_DETECT;\n"
     "    *latchedOut = latched;\n"
     "    status = SdhciWrite32(controller, SDHCI_INTERRUPT_ENABLE",
     "    latched = 0;\n"
     "    status = SdhciWrite32(controller, SDHCI_INTERRUPT_ENABLE"),
    ("a command without an answer logged as an error", "sdhci/recovery.c",
     "    if (__IsPlainNoAnswer(&request->Result, stopProven)) {",
     "    if (__IsPlainNoAnswer(&request->Result, stopProven) && false) {"),
    ("no answer traced without the stop proof", "sdhci/recovery.c",
     "    return stopProven && result->Status == OS_ETIMEOUT &&",
     "    return (stopProven || true) && result->Status == OS_ETIMEOUT &&"),
    ("any timeout traced as a command without an answer", "sdhci/recovery.c",
     "           result->TransportErrors == SD_TRANSPORT_COMMAND_TIMEOUT;\n}",
     "           (result->TransportErrors & SD_TRANSPORT_COMMAND_TIMEOUT) != 0;\n}"),
    ("deadline counts without waiting for running fast handlers", "sdhci/request.c",
     "    status = SdhciInterruptWaitForHandlers(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        ERROR(\"SDHCI %u: waiting for the fast interrupt handler",
     "    status = OS_EOK;\n"
     "    if (status != OS_EOK) {\n"
     "        ERROR(\"SDHCI %u: waiting for the fast interrupt handler"),
    ("deadline counts without taking the saved bits after the wait", "sdhci/request.c",
     "        handlerBits = SdhciInterruptTake(controller);",
     "        handlerBits = 0;"),
    ("saved bits taken before the wait for running fast handlers", "sdhci/request.c",
     "    status = SdhciInterruptWaitForHandlers(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        ERROR(\"SDHCI %u: waiting for the fast interrupt handler at the deadline of CMD%u failed: %u\",\n"
     "            controller->Device->Id, opcode, status);\n"
     "    } else {\n"
     "        handlerBits = SdhciInterruptTake(controller);\n"
     "    }",
     "    handlerBits = SdhciInterruptTake(controller);\n"
     "    status = SdhciInterruptWaitForHandlers(controller);\n"
     "    if (status != OS_EOK) {\n"
     "        ERROR(\"SDHCI %u: waiting for the fast interrupt handler at the deadline of CMD%u failed: %u\",\n"
     "            controller->Device->Id, opcode, status);\n"
     "        handlerBits = 0;\n"
     "    }"),
    ("late handler bits logged as a lost interrupt", "sdhci/request.c",
     "    if (lostBits != 0) {",
     "    if ((lostBits | handlerBits) != 0) {"),
    ("bit seen by the register read and by a handler logged as a lost interrupt", "sdhci/request.c",
     "    lostBits = registerBits & ~handlerBits;",
     "    lostBits = registerBits;"),
    ("failed event sent for every failed reset", "sdhci/event.c",
     "    if (controller->Failed) {\n        return;\n    }\n    controller->Failed = true;",
     "    controller->Failed = true;"),
    ("failed busy wait in the wait for fast handlers not logged", "sdhci/interrupt.c",
     "        if (status != OS_EOK) {\n"
     "            ERROR(\"SDHCI %u: the busy wait for a fast interrupt handler to return failed: %u\",\n"
     "                controller->Device->Id, status);\n"
     "            return status;\n"
     "        }",
     "        if (status != OS_EOK) {\n"
     "            return status;\n"
     "        }"),
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
        print("SDHCI request production compilation:", arch, "passed", flush=True)


def run_test(flags, source_root, binary, environment):
    test_source = str(ROOT / "testing/sdhci_request_test.c")
    sources = [str(source_root / source) for source in TEST_SOURCES]
    subprocess.run(flags + [test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-sdhci-request-") as directory:
        output = Path(directory)
        binary = output / "request"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        flags = host_flags()

        check_headers(output)
        completed = run_test(flags, STORAGE, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("SDHCI request test failed: " + completed.stderr.decode())
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
            print("SDHCI request mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
