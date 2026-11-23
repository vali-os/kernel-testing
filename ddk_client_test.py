#!/usr/bin/env python3
"""Run the host tests for the libddk service lookup, device registration and device I/O.

Each test includes its libddk source file and replaces the syscalls and the IPC
calls with stubs. --mutation-check also builds each source with one regression
put back in, and fails when a test does not catch it.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

# The syscalls the tested sources use. The real header implements them with a
# trap into the kernel, which a host test cannot run.
SYSCALLS = """
#include <ddk/io.h>
oserr_t Syscall_HandleLookup(const char*, uuid_t*);
oserr_t Syscall_IoSpaceRegister(DeviceIo_t*);
oserr_t Syscall_IoSpaceAcquire(DeviceIo_t*);
oserr_t Syscall_IoSpaceRelease(DeviceIo_t*);
oserr_t Syscall_IoSpaceDestroy(DeviceIo_t*);
oserr_t Syscall_IoSpaceMap(uuid_t, size_t, size_t, void**);
"""

PROTOCOLS = ("device", "file", "process", "contracts/filesystem", "contracts/usbhost", "contracts/driver")

# test name, tested source, define that selects the source, mutations.
# A mutation is (reason, text to replace, replacement).
TESTS = (
    ("ddk_service_test", "librt/libddk/services/service.c", "DDK_SERVICE_SOURCE", (
        ("timeout that is not a multiple of the poll interval never ends",
         "if (timeout != 0 && waited >= timeout) {", "if (timeout != 0 && waited == timeout) {"),
        ("waited time not counted",
         "            waited += SERVICE_POLL_INTERVAL_MS;\n", ""),
        ("found handle not cached",
         "    service->Handle = handle;\n", ""),
        ("USB service looked up with the device path",
         "{ SERVICE_USB_PATH,     UUID_INVALID }", "{ SERVICE_DEVICE_PATH,  UUID_INVALID }"),
        ("zero timeout does not wait forever",
         "if (timeout != 0 && waited >= timeout) {", "if (waited >= timeout) {"),
    )),
    ("ddk_device_test", "librt/libddk/devices/device.c", "DDK_DEVICE_SOURCE", (
        ("registration result read after a failed send",
         '        ERROR("DeviceRegister: send failed: %i", status);\n        return UUID_INVALID;\n', ""),
        ("registration result read after a failed wait",
         '        ERROR("DeviceRegister: wait failed: %i", status);\n        return UUID_INVALID;\n', ""),
        ("registration id used after a failed decode",
         '        ERROR("DeviceRegister: decode failed: %i", status);\n        return UUID_INVALID;\n', ""),
        ("rejected registration returns an id",
         '        WARNING("DeviceRegister: registration rejected: %u", oserr);\n        return UUID_INVALID;\n',
         '        WARNING("DeviceRegister: registration rejected: %u", oserr);\n'),
        ("removal result read after a failed wait",
         '        WARNING("DeviceUnregister: device %u wait failed: %i", deviceId, status);\n'
         '        return OS_EPROTOCOL;\n', ""),
        ("failed removal decode reported as success",
         '        WARNING("DeviceUnregister: device %u decode failed: %i", deviceId, status);\n'
         '        return OS_EPROTOCOL;\n',
         '        WARNING("DeviceUnregister: device %u decode failed: %i", deviceId, status);\n'
         '        return OS_EOK;\n'),
    )),
    ("ddk_deviceio_test", "librt/libddk/io/deviceio.c", "DDK_DEVICEIO_SOURCE", (
        ("16-bit write widened to 32 bits",
         "*(volatile uint16_t*)address = (uint16_t)(value & 0xFFFF);",
         "*(volatile uint32_t*)address = (uint32_t)(value & 0xFFFF);"),
        ("8-bit read widened to 16 bits",
         "*valueOut = *(volatile uint8_t*)address;", "*valueOut = *(volatile uint16_t*)address;"),
        ("port offset ignored on read",
         "status = __ReadPort(LOWORD(ioSpace->Access.Port.Base) + LOWORD(offset), length, &value);",
         "status = __ReadPort(LOWORD(ioSpace->Access.Port.Base), length, &value);"),
        ("missing I/O space written as success",
         "    if (ioSpace == NULL) {\n        return OS_EINVALPARAMS;\n    }\n\n    switch (ioSpace->Type) {",
         "    if (ioSpace == NULL) {\n        return OS_EOK;\n    }\n\n    switch (ioSpace->Type) {"),
        ("memory I/O registered with a stale mapping address",
         "    ioSpace->Access.Memory.VirtualBase  = 0;\n", ""),
    )),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="vali-ddk-client-") as directory:
        output = Path(directory)
        (output / "internal").mkdir()
        (output / "internal/_syscalls.h").write_text(SYSCALLS)
        for protocol in PROTOCOLS:
            subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                            "--service", str(ROOT / "protocols" / (protocol + ".gr")),
                            "--out", directory, "--lang-c", "--client", "--server"],
                           check=True, stdout=subprocess.DEVNULL)

        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI", "-DNDEBUG",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-D__time64_t=int64_t",
                 "-fms-extensions", "-fasm-blocks", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                 "-Wno-macro-redefined", "-Wno-unknown-warning-option",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include",
                   "librt/libds/include", "librt/libgracht/include", "librt/libusb/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"

        for name, source, define, mutations in TESTS:
            test = str(ROOT / "testing" / (name + ".c"))
            binary = str(output / name)
            subprocess.run(flags + [test, "-o", binary], check=True)
            subprocess.run([binary], check=True, env=environment, timeout=60)
            if not args.mutation_check:
                continue

            original = (ROOT / source).read_text()
            for reason, before, after in mutations:
                assert original.count(before) == 1, reason
                mutant = output / (name + "-mutant.c")
                mutant.write_text(original.replace(before, after))
                subprocess.run(flags + [f'-D{define}="{mutant}"', test, "-o", binary], check=True)
                result = subprocess.run([binary], env=environment, capture_output=True, timeout=60)
                if result.returncode == 0:
                    raise RuntimeError(f"Undetected regression in {source}: {reason}")
                print(f"Rejected mutation: {reason}", flush=True)


if __name__ == "__main__":
    main()
