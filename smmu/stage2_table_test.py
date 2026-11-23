#!/usr/bin/env python3
"""Check the shared SMMU stage-2 table code against the real control arena."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SMMU = Path("kernel/arch/aarch64/smmu")
TABLE = SMMU / "stage2-table.c"


def prepare(output):
    # Mutations run on a source copy. The copy keeps the repository layout, so
    # the relative includes of the sources resolve as they do in a normal build.
    paths = [SMMU / name for name in ("memory.c", "memory.h", "stage2.h", "stage2-table.c",
                                      "stage2-table.h", "description.h")]
    paths += [Path("kernel/memory/dma") / name for name in ("backend.h", "iova.h")]
    paths += [Path("testing/smmu/stage2_table_test.c")]
    for source in paths:
        target = output / source
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / source, target)


def host_run(output, mutation=None):
    prepare(output)
    if mutation:
        name, path, before, after = mutation
        target = output / path
        source = target.read_text()
        assert source.count(before) == 1, (name, source.count(before))
        target.write_text(source.replace(before, after))
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-fms-extensions",
             "-fasm-blocks", "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline",
             "-DSERVICEABI=", "-DLIBC_KERNEL", "-D__LIBDS_KERNEL__", "-DTEST_ARM64",
             "-U__amd64__", "-D__aarch64__", '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    sources = [output / "testing/smmu/stage2_table_test.c", output / SMMU / "stage2-table.c",
               output / SMMU / "memory.c", ROOT / "kernel/components/memory.c",
               ROOT / "kernel/utils/memory_stack.c", ROOT / "librt/libds/list.c"]
    binary = output / "stage2-table-test"
    subprocess.run(flags + [str(path) for path in sources] +
                   ["-Wl,--wrap=SystemMemoryReleaseAllocation", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=60, capture_output=mutation is not None,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    if mutation is None:
        result.check_returncode()
    elif (result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr
          or b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr):
        raise AssertionError(f"Mutation did not fail cleanly at an assertion: {mutation[0]}\n"
                             + (result.stdout + result.stderr).decode(errors="replace"))


def compile_target(output):
    flags = [os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang"),
             "--target=aarch64-uml-vali", "-std=gnu11", "-fms-extensions", "-ffreestanding",
             "-nostdlibinc", "-O2", "-Wall", "-Werror", "-march=armv8-a", "-mno-outline-atomics",
             "-D__LIBDS_KERNEL__", "-DLIBC_KERNEL"]
    for directory in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                      "librt/libddk/include", "librt/libds/include"):
        flags += ["-I" + str(ROOT / directory)]
    subprocess.run(flags + ["-c", str(ROOT / TABLE), "-o", str(output / "stage2-table.obj")], check=True)
    # The header must compile by itself under the production target ABI.
    header = output / "header.c"
    header.write_text('#include "' + str(ROOT / SMMU / "stage2-table.h") + '"\n')
    subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    print("SMMU stage-2 tables: AArch64 production source and standalone header passed", flush=True)


def run_mutations(output):
    mutations = (
        ("device range limit", TABLE,
         "config->DeviceAddress > unit->AddressLimit ||", "false ||"),
        ("device range last byte", TABLE,
         "config->Length - 1 > unit->AddressLimit - config->DeviceAddress", "false"),
        ("physical output limit", TABLE,
         "address > unit->PhysicalLimit", "address > unit->AddressLimit"),
        ("physical output last byte", TABLE,
         "length - 1 > unit->PhysicalLimit - address", "false"),
        ("extents cover the range", TABLE,
         "if (total != config->Length) {", "if (false) {"),
        ("direction check", TABLE,
         "config->Direction != DMABidirectional) {", "false) {"),
        ("extent alignment", TABLE,
         "address % ARM_SMMU_S2_PAGE_SIZE != 0 ||", "false ||"),
        ("worst case page count", TABLE,
         "(config->DeviceAddress >> shift) + 1;", "(config->DeviceAddress >> shift);"),
        ("arena bound", TABLE,
         "if (count > ARM_SMMU_MEMORY_MAX_SIZE / ARM_SMMU_S2_PAGE_SIZE) {", "if (false) {"),
        ("mapping counter overflow", TABLE,
         "if (table->MappingCount == UINT32_MAX) {", "if (false) {"),
        ("return partial reservation", TABLE,
         "cleanupStatus = __FreePages(table, &mapping->SparePages);", "cleanupStatus = OS_EOK;"),
        ("count reservation", TABLE,
         "table->MappingCount++;", "table->MappingCount += 0;"),
        ("count release", TABLE,
         "table->MappingCount--;", "table->MappingCount -= 0;"),
        ("release without reservation", TABLE,
         "if (table->MappingCount == 0) {", "if (false) {"),
        ("fail the unit", TABLE,
         "table->Unit->Fail(table->Unit->Context, status);", "(void)status;"),
        ("failed root allocation latches", TABLE,
         "if (unit->Memory->State == ArmSMMUMemoryFailed) {\n            ERROR(\"ArmSMMUStage2TableCreate",
         "if (false) {\n            ERROR(\"ArmSMMUStage2TableCreate"),
        ("failed page allocation latches", TABLE,
         "if (unit->Memory->State == ArmSMMUMemoryFailed) {\n                ERROR(\"__ReservePages",
         "if (false) {\n                ERROR(\"__ReservePages"),
        ("barrier before publishing a table", TABLE,
         "unit->Barrier();", "(void)unit;"),
        ("table entry type", TABLE,
         "if ((entry & ARM_SMMU_S2_TYPE_MASK) != ARM_SMMU_S2_TABLE) {", "if (false) {"),
        ("existing leaf", TABLE,
         "if (*leaf != ARM_SMMU_S2_INVALID) {", "if (false) {"),
        ("poll before every written page", TABLE,
         "status = poll(pollContext);\n            if (status != OS_EOK) {\n                return status;\n            }\n\n            deviceAddress = config->DeviceAddress + mapping->InstalledBytes;\n            status = __FindLeaf(table, mapping, deviceAddress, true, &leaf);",
         "deviceAddress = config->DeviceAddress + mapping->InstalledBytes;\n            status = __FindLeaf(table, mapping, deviceAddress, true, &leaf);"),
        # The blank line after the poll check holds trailing spaces in stage2-table.c.
        ("poll before every cleared page", TABLE,
         "status = poll(pollContext);\n        if (status != OS_EOK) {\n            return status;\n        }\n        \n        deviceAddress = mapping->Config->DeviceAddress",
         "deviceAddress = mapping->Config->DeviceAddress"),
        ("read-only permission", TABLE,
         "if (config->Direction != DMAToDevice) {", "if (true) {"),
        ("write-only permission", TABLE,
         "if (config->Direction != DMAFromDevice) {", "if (true) {"),
        ("execute never", TABLE,
         "ARM_SMMU_S2_ACCESS_FLAG | ARM_SMMU_S2_EXECUTE_NEVER |", "ARM_SMMU_S2_ACCESS_FLAG |"),
        ("record installed prefix", TABLE,
         "mapping->InstalledBytes += ARM_SMMU_S2_PAGE_SIZE;", "mapping->InstalledBytes += 2 * ARM_SMMU_S2_PAGE_SIZE;"),
        ("clear removed leaf", TABLE,
         "*leaf = ARM_SMMU_S2_INVALID;", "(void)leaf;"),
        ("clear from the end of the prefix", TABLE,
         "mapping->InstalledBytes - ARM_SMMU_S2_PAGE_SIZE;", "mapping->InstalledBytes;"),
        ("shrink installed prefix", TABLE,
         "mapping->InstalledBytes -= ARM_SMMU_S2_PAGE_SIZE;", "mapping->InstalledBytes = 0;"),
        ("leaf output address", TABLE,
         "*leaf = (config->Extents[index].Address + extentOffset) | attributes;",
         "*leaf = config->Extents[index].Address | attributes;"),
    )
    for mutation in mutations:
        host_run(output, mutation)
        print("Mutation rejected:", mutation[0], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmu-stage2-") as directory:
        output = Path(directory)
        host_run(output)
        compile_target(output)
        if args.mutation_check:
            run_mutations(output)


if __name__ == "__main__":
    main()
