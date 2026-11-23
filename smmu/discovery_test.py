#!/usr/bin/env python3
"""Validate production IORT discovery with sanitizers and target compilation."""
import argparse
import os
import re
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = ("testing/smmu/discovery_test.c", "kernel/arch/aarch64/smmu/discovery.c",
           "kernel/arch/aarch64/acpi/iort.c", "kernel/arch/aarch64/acpi/tables.c",
           "kernel/arch/aarch64/acpi/iort-catalogue.c", "kernel/arch/aarch64/acpi/iort-dma.c",
           "kernel/arch/aarch64/acpi/iort-msi.c", "kernel/arch/aarch64/smmu/iort.c")


def host_run(output, replacement=None, mutation=None):
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-fms-extensions",
             "-fasm-blocks", "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline",
             "-DSERVICEABI=", "-DLIBC_KERNEL", "-D__LIBDS_KERNEL__", "-DTEST_ARM64",
             "-U__amd64__", "-D__aarch64__", '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include",
                      "librt/libacpi/source/include", "librt/libfdt/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    sources = [str(replacement[1]) if replacement and source == replacement[0]
               else str(ROOT / source) for source in SOURCES]
    binary = output / "discovery-test"
    subprocess.run(flags + sources + ["-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], timeout=30, capture_output=mutation is not None,
                            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    if mutation is None:
        result.check_returncode()
    elif result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr:
        raise AssertionError(f"Mutation did not fail at an assertion: {mutation}\n"
                             + (result.stdout + result.stderr).decode(errors="replace"))


def compile_targets(output):
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    flags = [compiler, "--target=aarch64-uml-vali", "-std=gnu11", "-fms-extensions",
             "-ffreestanding", "-nostdlibinc", "-O2", "-Wall", "-Werror", "-march=armv8-a",
             "-mno-outline-atomics", "-D__LIBDS_KERNEL__", "-DLIBC_KERNEL"]
    for directory in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                      "librt/libddk/include", "librt/libds/include", "librt/libacpi/source/include",
                      "librt/libfdt/include"):
        flags += ["-I" + str(ROOT / directory)]
    for index, source in enumerate(SOURCES[1:]):
        subprocess.run(flags + ["-c", str(ROOT / source), "-o", str(output / f"source-{index}.obj")], check=True)
    print("Arm SMMU discovery: AArch64 production compilation passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmu-discovery-") as directory:
        output = Path(directory)
        host_run(output)
        compile_targets(output)
        if not args.mutation_check:
            return
        mutations = (
            ("ITS group membership", "kernel/arch/aarch64/acpi/iort-msi.c",
             "if (!member)", "if (member && false)"),
            ("ITS MADT identity", "kernel/arch/aarch64/acpi/iort-msi.c",
             "gic->ItsControllers[i].Identifier == identifier", "true"),
            ("MSI control exclusion", "kernel/arch/aarch64/acpi/iort-msi.c",
             "if (mapping->IsOwnInterruptMapping)", "if (false)"),
            ("MSI range end", "kernel/arch/aarch64/acpi/iort-msi.c",
             "input > mapping->InputLast", "input >= mapping->InputLast"),
            ("MSI output translation", "kernel/arch/aarch64/acpi/iort-msi.c",
             "input - mapping->InputFirst", "input"),
            ("MSI segment identity", "kernel/arch/aarch64/acpi/iort-msi.c",
             "node->Segment != segment", "false"),
            ("MSI ambiguity", "kernel/arch/aarch64/acpi/iort-msi.c",
             "if (*mappingOut != NULL)", "if (false)"),
            ("MSI single mapping", "kernel/arch/aarch64/acpi/iort-msi.c",
             "mapping->MapsAllInputsToOne ? 0 :", "false ? 0 :"),
            ("MSI independent DMA policy", "kernel/arch/aarch64/acpi/iort-msi.c",
             "deviceId = __TranslateId(mapping, requesterId);",
             "if (ArmIortGetNode(catalogue, mapping->TargetIndex)->DmaStatus != OS_EOK) return OS_ENOTSUPPORTED; deviceId = __TranslateId(mapping, requesterId);"),
            ("IORT node identity", "kernel/arch/aarch64/acpi/iort.c",
             "node->Identifier = parsed->Header->Identifier;", "node->Identifier = i;"),
            ("MSI ITS identifiers", "kernel/arch/aarch64/acpi/iort.c",
             "memcpy(identifiers, bytes + node->FixedLength, its->ItsCount * sizeof(uint32_t));",
             "memset(identifiers, 0, its->ItsCount * sizeof(uint32_t)); (void)bytes;"),
            ("direct CCA", "kernel/arch/aarch64/acpi/iort-dma.c",
             "root->CachePolicy != SHMDeviceCacheCoherent ||", "false ||"),
            ("direct CPM", "kernel/arch/aarch64/acpi/iort-dma.c",
             "!root->HardwareCoherent ||", "false ||"),
            ("direct DACS", "kernel/arch/aarch64/acpi/iort-dma.c",
             "!root->UsesDeviceMemoryAttributes)", "false)"),
            ("direct requester coverage", "kernel/arch/aarch64/acpi/iort-dma.c",
             "covered != UINT16_MAX + 1U", "covered == 0"),
            ("direct requester bounds", "kernel/arch/aarch64/acpi/iort.c",
             "(uint64_t)entry->InputBase + entry->IdCount > UINT16_MAX", "false"),
            ("direct duplicate root", "kernel/arch/aarch64/acpi/iort-dma.c",
             "if (root != NULL) {", "if (false) {"),
            ("direct source eligibility", "kernel/arch/aarch64/acpi/iort-dma.c",
             "root->DmaStatus != OS_EOK ||", "false ||"),
            ("direct address limit", "kernel/arch/aarch64/acpi/iort-dma.c",
             "*addressLimitOut = root->AddressLimit;", "*addressLimitOut = UINT64_MAX;"),
            ("RMR descriptor budget", "kernel/arch/aarch64/acpi/iort.c",
             "rmr->RmrCount > ARM_IORT_MAX_RANGES - parser->RmrDescriptorCount", "false"),
            ("SID inclusive upper bound", "kernel/arch/aarch64/smmu/discovery.c",
             "input > range->InputLast", "input >= range->InputLast"),
            ("RMR unit identity", "kernel/arch/aarch64/smmu/discovery.c",
             "ids->UnitIndex == unitIndex &&", "true &&"),
            ("RMR rejection", "kernel/arch/aarch64/smmu/discovery.c",
             "return true;", "return false;"),
            ("single mapping arithmetic", "kernel/arch/aarch64/smmu/discovery.c",
             "if (!range->MapsToOneId)", "if (true)"),
            ("SMMUv3 interrupt trigger", "kernel/arch/aarch64/acpi/iort.c",
             "        smmu->EventGsiv,\n        1,\n", "        smmu->EventGsiv,\n        0,\n"),
            ("node reference boundary", "kernel/arch/aarch64/acpi/iort.c",
             "parser->Nodes[i].Offset == offset", "parser->Nodes[i].Offset <= offset && offset < parser->Nodes[i].Offset + parser->Nodes[i].Header->Length"),
            ("RID overlap", "kernel/arch/aarch64/acpi/iort.c",
             "__Overlaps(left->InputFirst, left->InputLast, right->InputFirst, right->InputLast)",
             "false && __Overlaps(left->InputFirst, left->InputLast, right->InputFirst, right->InputLast)"),
            ("cycle detection", "kernel/arch/aarch64/acpi/iort.c",
             "if (remaining == before) {\n            return OS_EINVALPARAMS;", "if (remaining == before) {\n            return OS_EOK;"),
        )
        for name, source, old, new in mutations:
            original = (ROOT / source).read_text()
            assert original.count(old) == 1, name
            # Keep source-relative includes valid without editing the worktree.
            changed = original.replace(old, new)
            parent = (ROOT / source).parent
            changed = re.sub(r'#include "([^"]+)"',
                             lambda match: '#include "' + str(parent / match[1]) + '"', changed)
            mutant = output / "mutant.c"
            mutant.write_text(changed)
            host_run(output, (source, mutant), name)
            print("Rejected discovery mutation:", name, flush=True)


if __name__ == "__main__":
    main()
