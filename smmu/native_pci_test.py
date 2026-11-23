#!/usr/bin/env python3
"""Check the native SMMU test-device adapter against mocked PCI and MMIO boundaries.

Builds testing/smmu/native_pci.c with testing/smmu/native_pci_test.c under
ASan/UBSan, runs it, then compiles the native coordinator files and adapter
for AArch64 with the production compiler. With --mutation-check, each mutation
below changes one rule in a temporary copy of native_pci.c. A mutation only
counts as rejected when the test stops at a failed assertion. Sanitizer
reports, other signals and timeouts fail the run.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "testing/smmu"
ADAPTER = TESTS / "native_pci.c"
# The coordinator, split by topic. native.c holds the case table and is the
# only file that reads the case ID.
COORDINATOR = ("native.c", "native_setup.c", "native_requests.c", "native_cases.c", "native_cleanup.c")

MUTATIONS = (
    # Configuration access widths and order.
    ("16-bit command write", "__ConfigWrite(function, __PCI_COMMAND, 16, command);",
     "__ConfigWrite(function, __PCI_COMMAND, 32, command);"),
    ("command readback", "if ((readback & (clearMask | setMask)) != setMask) {", "if (false) {"),
    ("stop decode before BAR probing",
     "        function = __FunctionFromDevice(&devices[i]);\n        status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, 0);",
     "        function = __FunctionFromDevice(&devices[i]);\n        status = OS_EOK;"),
    ("enable decode after assignment",
     "status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, __PCI_COMMAND_MEMORY);",
     "status = __UpdateCommand(&function, __PCI_COMMAND_DECODE, __PCI_COMMAND_MEMORY | __PCI_COMMAND_BUS_MASTER);"),
    # Topology.
    ("single ACPI root", "    if (status != OS_ENOENT) {\n        ERROR(\"__ReadRoot: the fixture",
     "    if (false) {\n        ERROR(\"__ReadRoot: the fixture"),
    ("missing expected device", "        if (expected) {", "        if (false) {"),
    ("endpoint header type", "headerType != __PCI_HEADER_ENDPOINT", "false"),
    ("other functions of a slot", "        if (present) {", "        if (false) {"),
    ("record identities before writes", "    status = __CheckTopology(rootInfo);\n    if (status != OS_EOK) {",
     "    status = __CheckTopology(rootInfo);\n    if (false) {"),
    ("zeroed records", "!__IsZeroed(&devices[0]) ||", ""),
    # BAR layout and arithmetic.
    ("BAR size", "size != ARM_SMMU_TEST_DEVICE_BAR_SIZE", "size == 0"),
    ("BAR type bits", "(mask & __PCI_BAR_FLAGS_MASK) != 0 ||", ""),
    ("absent BARs", "        if (mask != 0) {", "        if (false) {"),
    ("BAR alignment", "~((uint64_t)ARM_SMMU_TEST_DEVICE_BAR_SIZE - 1U)", "~0ULL"),
    ("window room for both BARs", "last - first < needed - 1U", "last - first < ARM_SMMU_TEST_DEVICE_BAR_SIZE - 1U"),
    ("32-bit BAR limit", "        if (last > __BAR32_LIMIT) {", "        if (false) {"),
    ("window above 4 GiB", "window->BusBase > __BAR32_LIMIT) {", "false) {"),
    ("prefetchable window", "(window->Attributes & PCI_WINDOW_PREFETCHABLE) ||", ""),
    ("unambiguous window", "    if (matches != 1) {", "    if (matches == 0) {"),
    ("translated CPU address", "registers = Arm64MapDevice(physical,", "registers = Arm64MapDevice(busAddress,"),
    ("BAR readback", "    if (readback != (uint32_t)busAddress) {", "    if (false) {"),
    ("failed mapping", "    if (registers == 0) {", "    if (false) {"),
    # Register protocol.
    ("reset values", "        status = __ExpectRegister(device, resetValues[i].Offset, resetValues[i].Value);\n        if (status != OS_EOK) {",
     "        status = __ExpectRegister(device, resetValues[i].Offset, resetValues[i].Value);\n        if (false) {"),
    ("NonSecure attributes", "{ __ITD_ATTRIBUTES,    __ITD_ATTR_NONSECURE }", "{ __ITD_ATTRIBUTES,    __ITD_ATTR_RESET }"),
    ("physical high word", "{ __ITD_PHYSICAL_HIGH, (uint32_t)(expectedPhysicalAddress >> 32) },",
     "{ __ITD_PHYSICAL_HIGH, 0 },"),
    ("register readback", "    __DeviceWrite32(device->Registers + offset, value);\n    return __ExpectRegister(device, offset, value);",
     "    __DeviceWrite32(device->Registers + offset, value);\n    return OS_EOK;"),
    ("enable bus mastering before arming",
     "status = __UpdateCommand(&function, __PCI_COMMAND_IO, __PCI_COMMAND_MEMORY | __PCI_COMMAND_BUS_MASTER);",
     "status = OS_EOK;"),
    ("BUSY after arming", "status = __ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_BUSY);\n    if (status != OS_EOK) {",
     "status = __ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_BUSY);\n    if (false) {"),
    ("single trigger read", "    (void)__DeviceRead32(device->Registers + __ITD_TRIGGER);\n",
     "    (void)__DeviceRead32(device->Registers + __ITD_TRIGGER);\n    (void)__DeviceRead32(device->Registers + __ITD_TRIGGER);\n"),
    ("IDLE after disarming", "    return __ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_IDLE);",
     "    (void)__ExpectRegister(device, __ITD_RESULT, ARM_SMMU_TEST_RESULT_IDLE);\n    return OS_EOK;"),
    ("clear bus mastering after the request",
     "status = __UpdateCommand(&function, __PCI_COMMAND_IO | __PCI_COMMAND_BUS_MASTER, __PCI_COMMAND_MEMORY);",
     "status = OS_EOK;"),
    ("result unchanged on failure", "    result = __DeviceRead32(device->Registers + __ITD_RESULT);\n",
     "    result = __DeviceRead32(device->Registers + __ITD_RESULT);\n    *resultOut = result;\n"),
    ("maximum length", "length > ARM_SMMU_TEST_DEVICE_MAX_LENGTH ||", ""),
    ("length alignment", "(length & 3U) ||", ""),
    ("Ready state required", "device->State != ArmSMMUTestDeviceReady ||", ""),
    # Disable.
    ("disarm before disable", "    if (device->State == ArmSMMUTestDeviceReady) {\n        status = __Disarm(device);",
     "    if (false) {\n        status = __Disarm(device);"),
    ("disable keeps record on failure",
     "              device->RequesterId, status);\n        return status;\n    }\n    device->State = ArmSMMUTestDeviceIdentified;",
     "              device->RequesterId, status);\n        device->State = ArmSMMUTestDeviceIdentified;\n        return status;\n    }\n    device->State = ArmSMMUTestDeviceIdentified;"),
    ("disable returns to Identified", "    device->State = ArmSMMUTestDeviceIdentified;\n    return OS_EOK;\n}\n",
     "    return OS_EOK;\n}\n"),
    # Endpoint report.
    ("endpoint report reads the device again", "        status = __ReportDevice(&devices[i]);",
     "        status = OS_EOK;"),
)


def host_flags():
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-fms-extensions",
             "-fasm-blocks", "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline",
             "-DSERVICEABI=", "-DLIBC_KERNEL", "-D__LIBDS_KERNEL__", "-DTEST_ARM64",
             "-U__amd64__", "-D__aarch64__", '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
             "-DARM_SMMU_NATIVE_PCI_HOST_TEST",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             # Vali prints uint64_t with %llx. The host typedef is unsigned long.
             "-Wno-format",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    for directory in ("testing/smmu", "testing/include", "kernel/include", "librt/libos/include",
                      "librt/libacpi/source/include", "librt/libfdt/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    return flags


def host_run(output, mutation=None):
    source = ADAPTER
    if mutation is not None:
        name, before, after = mutation
        original = ADAPTER.read_text()
        assert original.count(before) == 1, (name, original.count(before))
        source = output / "native_pci_mutant.c"
        source.write_text(original.replace(before, after))
    binary = output / "native-pci-test"
    # The mutant is compiled from the temporary directory. -Itesting/smmu
    # keeps its relative kernel includes working. A mutation may leave a
    # variable unused, which is not the rule under test.
    flags = host_flags()
    if mutation is not None:
        flags += ["-Wno-unused-variable", "-Wno-unused-but-set-variable"]
    subprocess.run(flags + [str(TESTS / "native_pci_test.c"), str(source), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=mutation is not None,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    if mutation is None:
        result.check_returncode()
    elif (result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr
          or b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr):
        raise AssertionError(f"Mutation did not fail cleanly at an assertion: {mutation[0]}\n"
                             + (result.stdout + result.stderr).decode(errors="replace"))


def compile_target(output):
    """Compile the adapter and coordinator files as the selftest kernel does."""
    flags = [os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang"),
             "--target=aarch64-uml-vali", "-std=gnu11", "-fms-extensions", "-ffreestanding",
             "-nostdlibinc", "-O2", "-Wall", "-Werror", "-march=armv8-a", "-mno-outline-atomics",
             "-mgeneral-regs-only", "-D__LIBDS_KERNEL__", "-DLIBC_KERNEL", "-D__OSCONFIG_ACPI_SUPPORT",
             "-DVALI_ARM64_SMMU_SELFTEST_CASE_ID=0"]
    for directory in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                      "librt/libddk/include", "librt/libds/include", "librt/libacpi/source/include",
                      "librt/libfdt/include"):
        flags += ["-I" + str(ROOT / directory)]
    for name in ("native_pci.c",) + COORDINATOR:
        subprocess.run(flags + ["-c", str(TESTS / name), "-o", str(output / (name + ".obj"))], check=True)
    for name in ("native.h", "native_private.h", "native_coordinator.h"):
        header = output / "header.c"
        header.write_text(f'#include "{TESTS / name}"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    # Every case ID from 0 (positive) to 6 (after-disconnect) must compile,
    # and any other ID must stop the build of the coordinator.
    for case_id in range(1, 7):
        subprocess.run([flag.replace("CASE_ID=0", f"CASE_ID={case_id}") for flag in flags] +
                       ["-fsyntax-only", str(TESTS / "native.c")], check=True)
    rejected = subprocess.run([flag.replace("CASE_ID=0", "CASE_ID=7") for flag in flags] +
                              ["-fsyntax-only", str(TESTS / "native.c")], capture_output=True)
    assert rejected.returncode != 0 and b"one of the seven native SMMUv3 cases" in rejected.stderr
    print("Native SMMU test: AArch64 production and standalone header compilation passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmu-native-pci-") as directory:
        output = Path(directory)
        host_run(output)
        compile_target(output)
        if not args.mutation_check:
            return
        for mutation in MUTATIONS:
            host_run(output, mutation)
            print("Rejected adapter mutation:", mutation[0], flush=True)
        print(f"Native SMMU test adapter: all {len(MUTATIONS)} mutations rejected", flush=True)


if __name__ == "__main__":
    main()
