#!/usr/bin/env python3
"""Exercise the SMMU500 backend with real DMA/arena code and a delayed MMIO model."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SHARED = Path("kernel/arch/aarch64/smmu")
BACKEND = SHARED / "mmu500"
TESTS = Path("testing/smmu/mmu500")
MUTEX = """#ifndef __VALI_MUTEX_H__
#define __VALI_MUTEX_H__
#include <pthread.h>
typedef struct { pthread_mutex_t Native; } Mutex_t;
#define MUTEX_FLAG_PLAIN 0
void MutexConstruct(Mutex_t*, unsigned int);
void MutexDestruct(Mutex_t*);
void MutexLock(Mutex_t*);
void MutexUnlock(Mutex_t*);
#endif
"""


def prepare(output):
    # Mutations run on a source copy. Relative private includes retain their
    # normal layout; no build ever patches the working tree to inject a fault.
    paths = list((ROOT / BACKEND).glob("*.[ch]")) + list((ROOT / TESTS).glob("*.[ch]"))
    paths += [ROOT / ("kernel/arch/aarch64/smmu/" + name) for name in
              ("memory.c", "memory.h", "stage2.h", "stage2-table.c", "stage2-table.h",
               "description.h", "deadline.c", "deadline.h", "mmio.h", "unit-common.c",
               "unit-common.h")]
    paths += [ROOT / ("kernel/memory/dma/" + name) for name in
              ("backend.h", "private.h", "iova.h", "unit.c")]
    paths += [ROOT / "kernel/arch/aarch64/common/device.h",
              ROOT / "testing/smmu/model_support.h"]
    for source in paths:
        target = output / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    (output / "mutex.h").write_text(MUTEX)


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
             "-DSMMU500_TEST", "-D_POSIX_C_SOURCE=200809L", "-include", str(output / "mutex.h"),
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    sources = list((output / BACKEND).glob("*.c")) + list((output / TESTS).glob("*.c"))
    sources += [output / ("kernel/arch/aarch64/smmu/" + name) for name in ("memory.c", "stage2-table.c", "deadline.c", "unit-common.c")]
    sources += [output / "kernel/memory/dma/unit.c"]
    sources += [ROOT / path for path in ("kernel/components/memory.c", "kernel/utils/memory_stack.c",
        "kernel/memory/dma/address.c", "kernel/memory/dma/direct.c", "kernel/memory/dma/translated.c",
        "kernel/memory/dma/iova.c", "librt/libds/rbtree.c", "librt/libds/list.c")]
    binary = output / "backend-test"
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
    shared = [ROOT / SHARED / name for name in ("stage2-table.c", "deadline.c", "unit-common.c")]
    for source in list((ROOT / BACKEND).glob("*.c")) + shared:
        subprocess.run(flags + ["-c", str(source), "-o", str(output / (source.stem + ".obj"))], check=True)
    for name in ("smmu500", "private", "registers"):
        header = output / "header.c"
        header.write_text('#include "' + str(ROOT / BACKEND / (name + ".h")) + '"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    for name in ("deadline", "mmio", "unit-common"):
        header = output / "header.c"
        header.write_text('#include "' + str(ROOT / SHARED / (name + ".h")) + '"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    print("SMMU500: AArch64 production sources and standalone headers passed", flush=True)


def run_mutations(output):
    mutations = (
        ('require stage2', BACKEND / 'unit.c',
         '    TRACE("__Probe:', '    idr0 |= SMMU500_IDR0_STAGE2;\n    TRACE("__Probe:'),
        ('require coherent firmware', BACKEND / 'unit.c',
         'description->TableCoherent &&', 'true &&'),
        ('reject extended SIDs', BACKEND / 'unit.c',
         '(idr0 & SMMU500_IDR0_EXTENDED) != 0', 'false'),
        ('geometry from IDR1', BACKEND / 'unit.c',
         'unit->ContextBase = 1U << (unit->PageShift + pageIndex + 1U);',
         'unit->ContextBase = unit->Common.Description.RegisterLength / 2;'),
        ('zero dedicated banks supported', BACKEND / 'unit.c',
         'unit->Stage2Count > unit->ContextCount', 'unit->Stage2Count == 0 || unit->Stage2Count > unit->ContextCount'),
        ('separate output limit', BACKEND / 'unit.c',
         'unit->Stage2.PhysicalLimit = (1ULL << addressBits[output]) - 1ULL;',
         'unit->Stage2.PhysicalLimit = unit->Stage2.AddressLimit;'),
        ('probe-only software destruction', BACKEND / 'unit.c',
         'unit->StartProgress != SMMU500NoWrites && unit->CloseProgress != SMMU500CloseMemory',
         'unit->CloseProgress != SMMU500CloseMemory'),
        ('failed start Poll cannot report ready', SHARED / 'unit-common.c',
         'if (status == OS_EOK && state == ArmSMMUUnitStartFailed) {', 'if (false) {'),
        ('deadline remains bounded', SHARED / 'deadline.c',
         '>= deadline->Budget', '>= deadline->Budget * 2'),
        ('wait for hardware idle', BACKEND / 'context.c',
         'if ((active & SMMU500_SYNC_ACTIVE) == 0) {', 'if ((active & 0U) == 0) {'),
        ('save ticket once', BACKEND / 'context.c',
         'sync->Ticket = ++unit->SyncsSubmitted;', 'unit->SyncsSubmitted++;'),
        ('old ticket independent of new work', BACKEND / 'context.c',
         'if (sync->Ticket != 0 && sync->Ticket <= unit->SyncsCompleted) {', 'if (false) {'),
        ('counter overflow pins unit', BACKEND / 'context.c',
         'if (unit->SyncsSubmitted == UINT64_MAX) {', 'if (false) {'),
        ('completion fault resampled', BACKEND / 'context.c',
         'if (unit->StartProgress == SMMU500NoWrites) {', 'if (true) {'),
        ('table walks follow CPU caching', BACKEND / 'context.c',
         'if (unit->Common.Memory.Allocation.Cached) {', 'if (true) {'),
        ('TCR reserved high bit clear', BACKEND / 'context.c',
         'tcr = (unit->OutputEncoding << 16) |', 'tcr = (1U << 31) | (unit->OutputEncoding << 16) |'),
        ('nonzero VMID derived from bank', BACKEND / 'context.c',
         'space->Bank + 1U, SMMU500_CBAR_FIELDS', 'space->Bank, SMMU500_CBAR_FIELDS'),
        ('disable the context bank', BACKEND / 'context.c',
         'bankOffset + SMMU500_CB_SCTLR, 0, SMMU500_SCTLR_FIELDS',
         'bankOffset + SMMU500_CB_SCTLR, 1, SMMU500_SCTLR_FIELDS'),
        ('global faults fail new work', BACKEND / 'fault.c',
         '    DMAIOMMUUnitFail(unit->Common.Backend, OS_EDEVFAULT);\n    if ((fault', '    if ((fault'),
        ('write-one-to-clear preserves racing faults', BACKEND / 'fault.c',
         'fault & SMMU500_GFSR_W1C);', 'SMMU500_GFSR_W1C);'),
        ('read-only permission', SHARED / 'stage2-table.c',
         'if (config->Direction != DMAToDevice) {', 'if (true) {'),
        ('write-only permission', SHARED / 'stage2-table.c',
         'if (config->Direction != DMAFromDevice) {', 'if (true) {'),
        ('execute never', SHARED / 'stage2-table.c',
         'ARM_SMMU_S2_ACCESS_FLAG | ARM_SMMU_S2_EXECUTE_NEVER |', 'ARM_SMMU_S2_ACCESS_FLAG |'),
        ('record installed prefix', SHARED / 'stage2-table.c',
         'mapping->InstalledBytes += ARM_SMMU_S2_PAGE_SIZE;', 'mapping->InstalledBytes += 2 * ARM_SMMU_S2_PAGE_SIZE;'),
        ('clear removed leaf', SHARED / 'stage2-table.c',
         '*leaf = ARM_SMMU_S2_INVALID;', '(void)leaf;'),
        ('complete removal invalidation', BACKEND / 'table.c',
         'mapping->Phase = SMMU500MappingInvalidate;', 'mapping->Phase = SMMU500MappingRemoved; return OS_EOK;'),
        ('preserve clear phase on timeout', BACKEND / 'table.c',
         'status = __ClearMapping(space, mapping, &deadline);\n        if (status != OS_EOK) {',
         'status = __ClearMapping(space, mapping, &deadline);\n        if (false) {'),
        ('validate physical output range', SHARED / 'stage2-table.c',
         'address > unit->PhysicalLimit', 'address > unit->AddressLimit'),
        ('reserve complete SID set', BACKEND / 'stream.c',
         'if (unit->Slots[slot].Reserved && unit->Slots[slot].SID == hardwareIDs[index]) {', 'if (false) {'),
        ('reject duplicate SIDs', BACKEND / 'stream.c',
         'if (hardwareIDs[previous] == hardwareIDs[index]) {', 'if (false) {'),
        ('make exact stream match', BACKEND / 'stream.c',
         'SMMU500_SMR_VALID | sid', 'sid'),
        ('fault routes on disconnect', BACKEND / 'stream.c',
         'SMMU500_S2CR_FAULT, SMMU500_S2CR_FAULT_FIELDS', '0, SMMU500_S2CR_FAULT_FIELDS'),
        ('resume route index after timeout', BACKEND / 'stream.c',
         'if (connection->Phase < SMMU500ConnectionFaultRoutes) {',
         'if (connection->Phase <= SMMU500ConnectionFaultRoutes) {'),
        ('check fault route write acceptance', BACKEND / 'fault.c',
         'if ((observed & mask) != (value & mask)) {', 'if (false) {'),
    )
    for mutation in mutations:
        host_run(output, mutation)
        print("Mutation rejected:", mutation[0], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmu500-") as directory:
        output = Path(directory)
        host_run(output)
        compile_target(output)
        if args.mutation_check:
            run_mutations(output)


if __name__ == "__main__":
    main()
