"""Check the PL011 serial port driver: line settings, FIFOs, fast handler, port setup and teardown, serial contract."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "modules/serial/uart/pl011"
# Driver sources linked into the test. main.c only forwards to port.c.
TEST_SOURCES = ("buffer.c", "hardware.c", "interrupt.c", "port.c", "service.c")
# Sources that must also build for every Vali target.
TARGET_SOURCES = ("buffer.c", "hardware.c", "interrupt.c", "port.c", "service.c", "main.c")
HEADERS = ("buffer.h", "hardware.h", "interrupt.h", "pl011.h", "registers.h")
CONTRACTS = ("serial", "driver")

# Each mutation must make an assertion in pl011_test.c fail.
MUTATIONS = (
    ("divisor not rounded", "hardware.c",
     "    divisor = (clockRate * 4U + baudRate / 2U) / baudRate;",
     "    divisor = (clockRate * 4U) / baudRate;"),
    ("largest divisor with a fraction accepted", "hardware.c",
     "    if ((divisor >> PL011_FBRD_BITS) == PL011_IBRD_MAX && (divisor & PL011_FBRD_MASK) != 0) {",
     "    if (false) {"),
    ("even parity bit missing", "hardware.c",
     "            control |= PL011_LCR_H_PEN | PL011_LCR_H_EPS;",
     "            control |= PL011_LCR_H_PEN;"),
    ("flow control accepted", "hardware.c",
     "    if (line->FlowControl == PL011_FLOW_CONTROL_RTS_CTS) {\n        return OS_ENOTSUPPORTED;",
     "    if (false) {\n        return OS_ENOTSUPPORTED;"),
    ("final LCR_H write left out", "hardware.c",
     "    return __Write(hardware, PL011_LCR_H, control);\n}",
     "    (void)control;\n    return OS_EOK;\n}"),
    ("port not turned off before start", "hardware.c",
     "    hardware->Mask = 0;\n    status = Pl011HardwareStop(hardware);",
     "    hardware->Mask = 0;\n    status = OS_EOK;"),
    ("stop gives up after a failed write", "hardware.c",
     "    status = __Write(hardware, PL011_IMSC, 0);\n    if (status != OS_EOK) {\n        first = status;\n    }",
     "    status = __Write(hardware, PL011_IMSC, 0);\n    if (status != OS_EOK) {\n        return status;\n    }"),
    ("break passed on as a byte", "hardware.c",
     "        if (value & PL011_DR_BE) {\n            counters->Breaks++;\n            continue;",
     "        if (value & PL011_DR_BE) {\n            counters->Breaks++;"),
    ("overrun not counted", "hardware.c",
     "            counters->Overruns++;",
     "            (void)counters;"),
    ("transmit cause left on with an empty queue", "hardware.c",
     "        hardware->Mask &= ~PL011_INT_TX;",
     "        (void)0;"),
    ("line change without waiting for the transmit FIFO", "hardware.c",
     "    status = __WaitTransmitDone(hardware, receive);",
     "    status = OS_EOK;"),
    ("receive FIFO not emptied while waiting", "hardware.c",
     "        (void)Pl011HardwareReceive(hardware, receive);\n        flags = __Read(hardware, PL011_FR);",
     "        (void)receive;\n        flags = __Read(hardware, PL011_FR);"),
    ("old line not restored after a failed write", "hardware.c",
     "        __RestoreLine(hardware);\n        return status;",
     "        (void)__RestoreLine;\n        return status;"),
    ("fast handler leaves the causes on", "interrupt.c",
     "    if (interruptTable->WriteIoSpace(registers, PL011_IMSC, 0, PL011_REGISTER_SIZE) != OS_EOK) {",
     "    if (false) {"),
    ("causes not passed to the module", "interrupt.c",
     "    atomic_fetch_or(&shared->Causes, causes);",
     "    (void)shared;"),
    ("interrupt left in place at teardown", "port.c",
     "        status = Pl011InterruptUnregister(&port->Interrupt, port->DeviceId);",
     "        status = OS_EOK;"),
    ("no stop after a failed prepare", "port.c",
     "        port->State = PL011_PORT_PREPARE_FAILED;",
     "        port->State = PL011_PORT_NEW;"),
    ("mapped port not turned off", "port.c",
     "    if (port->State == PL011_PORT_READY || port->State == PL011_PORT_MAPPED) {",
     "    if (port->State == PL011_PORT_READY) {"),
    ("causes not turned back on", "port.c",
     "    status = Pl011HardwareUnmask(&port->Hardware);",
     "    status = OS_EOK;"),
    ("receive notice not made due by an emptying read", "service.c",
     "            port->ReceiveNotice = PL011_RECEIVE_NOTICE_DUE;",
     "            (void)0;"),
    ("partial write not remembered", "service.c",
     "                port->TransmitNotice = PL011_TRANSMIT_NOTICE_WANTED;",
     "                (void)0;"),
    ("notice marked sent with nobody left", "service.c",
     "    if (port->SubscriberCount == 0) {\n        return;\n    }\n    if (receiveDue) {",
     "    if (receiveDue) {"),
    ("new subscriber not told about waiting bytes", "service.c",
     "            __NotifyNewSubscriber(port);\n",
     "            (void)__NotifyNewSubscriber;\n"),
    ("unreachable subscriber kept", "service.c",
     "            __RemoveSubscriber(port, i);\n            continue;",
     "            i++;\n            continue;"),
    ("write limit not checked", "service.c",
     "        if (data_count > CTT_SERIAL_LIMIT_TRANSFER_BYTES) {",
     "        if (false) {"),
    # Removing the subscriber limit itself overflows the list, which the
    # sanitizer reports before any assertion, so the repeat check stands in.
    ("repeated subscriber accepted", "service.c",
     "                oserr = OS_EEXISTS;",
     "                oserr = OS_EOK;"),
    ("full queue accepts a byte", "buffer.c",
     "    if (buffer->Count == PL011_BUFFER_SIZE) {",
     "    if (buffer->Count > PL011_BUFFER_SIZE) {"),
)


def generate_contracts(output):
    # The bindings are generated the same way the build does, into the
    # temporary directory. Only the headers are used: the test fakes the
    # answers and events that the generated sources would send. The client
    # bindings are generated too, so check_client_header can compile them.
    generated = output / "generated"
    generated.mkdir()
    for contract in CONTRACTS:
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/contracts" / (contract + ".gr")),
                        "--out", str(generated), "--lang-c", "--client", "--server"],
                       check=True, capture_output=True)
    subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                    "--service", str(ROOT / "protocols/device.gr"),
                    "--out", str(generated), "--lang-c", "--client"],
                   check=True, capture_output=True)
    return generated


def host_flags(output, generated):
    # The host may have a libevent event.h. A shim points the driver at
    # Vali's own event.h instead.
    shims = output / "shims"
    shims.mkdir(exist_ok=True)
    (shims / "event.h").write_text('#include "' + str(ROOT / "librt/libc/include/event.h") + '"\n')
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI", "-DTESTING",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=",
             # The event descriptor calls are faked. Renaming them keeps the
             # host's own read, close and ioctl, which the sanitizers use.
             "-Dread=Pl011TestRead", "-Dclose=Pl011TestClose", "-Dioctl=Pl011TestIoctl",
             "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    # The generated bindings are not held to the driver's warning rules.
    flags += ["-I" + str(shims), "-isystem", str(generated)]
    flags += ["-I" + str(ROOT / path) for path in
              ("librt/libos/include", "librt/libddk/include", "librt/libds/include", "librt/libgracht/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    return flags


def check_headers(flags, output, driver):
    # Each header must compile on its own, and twice in a row, to prove that it
    # includes what it needs and that its include guard works.
    for header in HEADERS:
        probe = output / "header_probe.c"
        include = '#include "' + str(driver / header) + '"\n'
        probe.write_text(include + include + "int main(void) { return 0; }\n")
        subprocess.run(flags + ["-fsyntax-only", str(probe)], check=True)
        print("Header compiles alone and twice:", header, flush=True)


def check_client_header(flags, output):
    # Clients of the port (syslogd) use the client bindings. gracht adds
    # ctt_serial_subscribe and ctt_serial_unsubscribe to every client header,
    # so a contract call with one of those names does not compile.
    probe = output / "client_probe.c"
    probe.write_text("#include <ctt_serial_service_client.h>\nint main(void) { return 0; }\n")
    subprocess.run(flags + ["-fsyntax-only", str(probe)], check=True)
    print("Serial client bindings compile", flush=True)


def cross_compile(output, generated):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    if not Path(compiler).exists():
        print("Vali compiler not found, cross-compilation skipped:", compiler, flush=True)
        return
    includes = ["-isystem", str(generated)]
    includes += ["-I" + str(ROOT / path) for path in (
        "librt/libc/include", "librt/libos/include", "librt/libddk/include",
        "librt/libds/include", "librt/libgracht/include")]
    for arch in ("i386", "amd64", "aarch64"):
        target_flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                        "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-Wno-address-of-packed-member"]
        if arch == "aarch64":
            target_flags += ["-march=armv8-a", "-mno-outline-atomics"]
        for source in TARGET_SOURCES:
            subprocess.run(target_flags + includes + ["-c", str(DRIVER / source),
                           "-o", str(output / "target.o")], check=True)
        print("PL011 production compilation:", arch, "passed", flush=True)


def run_test(flags, driver, binary, environment):
    test_source = str(ROOT / "testing/pl011_test.c")
    sources = [str(driver / source) for source in TEST_SOURCES]
    sources.append(str(ROOT / "librt/libds/list.c"))
    subprocess.run(flags + ["-I" + str(driver), test_source] + sources + ["-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, env=environment, timeout=120)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-pl011-") as directory:
        output = Path(directory)
        binary = output / "pl011"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        generated = generate_contracts(output)
        flags = host_flags(output, generated)

        check_headers(flags, output, DRIVER)
        check_client_header(flags, output)
        completed = run_test(flags, DRIVER, binary, environment)
        print(completed.stdout.decode(), end="", flush=True)
        if completed.returncode != 0:
            raise AssertionError("PL011 test failed: " + completed.stderr.decode())
        cross_compile(output, generated)
        if not args.mutation_check:
            return

        for name, file_name, before, after in MUTATIONS:
            mutant = output / "mutant"
            if mutant.exists():
                shutil.rmtree(mutant)
            shutil.copytree(DRIVER, mutant)
            target = mutant / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            # A mutant may leave a function or parameter unused. That is
            # not what the check is about, so warnings do not stop it.
            completed = run_test(flags + ["-Wno-error"], mutant, binary, environment)
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that a regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("PL011 mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
