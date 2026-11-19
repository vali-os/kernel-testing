#!/usr/bin/env python3
"""Exercise the SMMUv3 backend with real DMA/arena code and a delayed MMIO model."""
import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
SHARED = Path("kernel/arch/aarch64/smmu")
V3 = SHARED / "v3"
TESTS = Path("testing/smmu/v3")
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


def check_register_models(output, flags):
    # Link both register models without either standalone kernel runtime. This
    # catches duplicate symbols, conflicting headers and dependencies on either
    # fixture's allocation or synchronization mocks.
    source = output / "register-models-check.c"
    source.write_text(r'''
#include "testing/smmu/v3/model.h"
#include "testing/smmu/mmu500/model.h"
#include <string.h>

static uintptr_t    g_expectedRegisters;
static unsigned int g_lockChecks;

void* SMMUTestPhysical(uint64_t physicalAddress, size_t length)
{
    (void)physicalAddress;
    (void)length;
    assert(!"register reset/probe must not access fixture memory");
    return NULL;
}

void SMMUTestAssertUnitLocked(uintptr_t registers)
{
    assert(registers == g_expectedRegisters);
    g_lockChecks++;
}

int main(int argc, char** argv)
{
    uintptr_t v3 = (uintptr_t)&g_smmuv3Model;
    uintptr_t mmu500 = (uintptr_t)&g_smmu500Model;

    if (argc == 2) {
        SMMUv3ModelReset();
        SMMU500ModelReset();
        g_smmu500Model.RegisterLength = SMMU500ModelDescription().RegisterLength;
        // Let the shared boundary accept the other unit's lock. Each model
        // must reject the wrong base independently of that lock check.
        g_expectedRegisters = mmu500;
        if (strcmp(argv[1], "v3-write32") == 0) SMMUv3TestWrite32(mmu500, 0x28, 0);
        else if (strcmp(argv[1], "v3-write64") == 0) SMMUv3TestWrite64(mmu500, 0x80, 0);
        else {
            g_expectedRegisters = v3;
            if (strcmp(argv[1], "mmu500-write32") == 0) SMMU500TestWrite32(v3, 0, 0);
            else if (strcmp(argv[1], "mmu500-write64") == 0) {
                SMMU500TestWrite64(v3, SMMU500ModelBank(0) + 0x20, 0);
            }
            else return 2;
        }
        return 0;
    }

    // Reset must clear pending work and caches without touching the other unit.
    memset(&g_smmuv3Model, 0xa5, sizeof(g_smmuv3Model));
    memset(&g_smmu500Model, 0xa5, sizeof(g_smmu500Model));
    SMMUv3ModelReset();
    assert(!g_smmuv3Model.PendingAck && !g_smmuv3Model.Routes[0].Present);
    g_smmuv3Model.Writes = 17;
    SMMU500ModelReset();
    assert(g_smmuv3Model.Writes == 17);
    assert(!g_smmu500Model.PendingSync && !g_smmu500Model.Routes[0].Present);
    assert(g_smmu500Model.PostedCount == 0 && g_smmu500Model.RegisterLength == 0);
    g_smmu500Model.Writes = 23;
    SMMUv3ModelReset();
    assert(g_smmu500Model.Writes == 23 && g_smmuv3Model.Writes == 0);
    assert(SMMUv3ModelDescription().Kind == ArmSMMUv3);
    assert(SMMU500ModelDescription().Kind == ArmSMMU500);
    g_smmu500Model.RegisterLength = SMMU500ModelDescription().RegisterLength;

    // Capability reads remain legal before a unit mutex exists. Mutable reads
    // and barriers must identify the correct unit to the shared lock boundary.
    assert(SMMUv3TestRead32(v3, 0) & 1U);
    assert(SMMU500TestRead32(mmu500, 0x20) & (1U << 29));
    assert(g_lockChecks == 0);
    g_expectedRegisters = v3;
    assert(SMMUv3TestRead32(v3, 0x20) == 0);
    SMMUv3TestBarrier();
    assert(g_lockChecks == 2);
    g_expectedRegisters = mmu500;
    assert(SMMU500TestRead32(mmu500, 0) == 1);
    SMMU500TestBarrier();
    assert(g_lockChecks == 4);
    return 0;
}
''')
    binary = output / "register-models-check"
    sources = [source, ROOT / "testing/smmu/v3/register_model.c",
               ROOT / "testing/smmu/mmu500/register_model.c"]
    subprocess.run(flags + ["-DSMMU500_TEST", "-I" + str(ROOT)] +
                   [str(path) for path in sources] + ["-o", str(binary)], check=True)
    environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}
    subprocess.run([str(binary)], check=True, timeout=30, env=environment)
    for case in ("v3-write32", "v3-write64", "mmu500-write32", "mmu500-write64"):
        result = subprocess.run([str(binary), case], capture_output=True, timeout=30,
                                env=environment)
        if (result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr
                or b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr):
            raise AssertionError(f"Wrong-model register access was not rejected: {case}\n"
                                 + (result.stdout + result.stderr).decode(errors="replace"))
    print("SMMU register models: shared headers and standalone linking passed", flush=True)


def prepare(output):
    # Mutations run on a source copy. Relative private includes retain their
    # normal layout; no build ever patches the working tree to inject a fault.
    paths = list((ROOT / V3).glob("*.[ch]")) + list((ROOT / TESTS).glob("*.[ch]"))
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
             "-DSMMUV3_TEST", "-D_POSIX_C_SOURCE=200809L", "-include", str(output / "mutex.h"),
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include"):
        flags += ["-I" + str(ROOT / directory)]
    for directory in ("librt/libds/include", "librt/libddk/include", "boot/include"):
        flags += ["-isystem", str(ROOT / directory)]
    sources = list((output / V3).glob("*.c")) + list((output / TESTS).glob("*.c"))
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
        check_register_models(output, flags)
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
    for source in list((ROOT / V3).glob("*.c")) + shared:
        subprocess.run(flags + ["-c", str(source), "-o", str(output / (source.stem + ".obj"))], check=True)
    for name in ("smmuv3", "private", "registers"):
        header = output / "header.c"
        header.write_text('#include "' + str(ROOT / V3 / (name + ".h")) + '"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    for name in ("deadline", "mmio", "unit-common"):
        header = output / "header.c"
        header.write_text('#include "' + str(ROOT / SHARED / (name + ".h")) + '"\n')
        subprocess.run(flags + ["-fsyntax-only", str(header)], check=True)
    print("SMMUv3: AArch64 production sources and standalone headers passed", flush=True)


def run_mutations(output):
    mutations = (
        ('require stage2', V3 / 'unit.c',
         '    TRACE("__ProbeHardware: unit', '    id0 |= SMMUV3_IDR0_STAGE2;\n    TRACE("__ProbeHardware: unit'),
        ('require coherence', V3 / 'unit.c',
         '    TRACE("__ProbeHardware: unit', '    id0 |= SMMUV3_IDR0_COHERENT;\n    TRACE("__ProbeHardware: unit'),
        ('reject reserved command geometry', V3 / 'unit.c',
         'unit->CommandBits > 19U', 'false'),
        ('reject reserved event geometry', V3 / 'unit.c',
         'unit->EventBits > 19U', 'false'),
        ('respect absent ATS', V3 / 'unit.c',
         'if (unit->ATSSupported) {', 'if (true) {'),
        ('set global abort', V3 / 'unit.c',
         'gbpa | SMMUV3_GBPA_ABORT | SMMUV3_GBPA_UPDATE', 'gbpa | SMMUV3_GBPA_UPDATE'),
        ('check register completion', V3 / 'unit.c',
         'if ((__SMMUv3Read32(unit->Common.Registers, offset) & mask) == expected) {', 'if (true) {'),
        ('preserve arena on cleanup failure', SHARED / 'unit-common.c',
         'if (common->CompletionUnreliable || common->Memory.State == ArmSMMUMemoryFailed) {', 'if (false) {'),
        ('software-only probe destruction', V3 / 'unit.c',
         'unit->StartProgress != SMMUv3NoWrites && unit->CloseProgress != SMMUv3CloseHardwareStopped', 'unit->CloseProgress != SMMUv3CloseHardwareStopped'),
        ('failed startup poll is busy', SHARED / 'unit-common.c',
         'if (status == OS_EOK && state == ArmSMMUUnitStartFailed) {', 'if (false) {'),
        ('one caller starts the unit', SHARED / 'unit-common.c',
         'if (!atomic_compare_exchange_strong(&common->State, &expected, ArmSMMUUnitStarting)) {', 'if (false) {'),
        ('live spaces block destruction', SHARED / 'unit-common.c',
         'if (common->Spaces.count != 0 ||', 'if (false ||'),
        ('failure latches unreliable completion', SHARED / 'unit-common.c',
         '    common->CompletionUnreliable = true;\n    DMAIOMMUUnitFail(common->Backend, status);', '    DMAIOMMUUnitFail(common->Backend, status);'),
        ('wait for final sync', V3 / 'queue.c',
         'unit->CommandsCompleted >= batch->EndPosition', 'unit->CommandsCompleted + 1 >= batch->EndPosition'),
        ('save submitted batch', V3 / 'queue.c',
         'batch->State = SMMUv3BatchSubmitted;', 'batch->State = SMMUv3BatchPrepared;'),
        ('preserve command error reliability', V3 / 'queue.c',
         'advance > unit->CommandsSubmitted - unit->CommandsCompleted', 'false'),
        ('ignore stale error bits', V3 / 'queue.c',
         'if (unit->CommandsCompleted > unit->CommandsSubmitted ||', 'if ((consumer & SMMUV3_QUEUE_ERROR_MASK) != 0 || unit->CommandsCompleted > unit->CommandsSubmitted ||'),
        ('bound deadline', SHARED / 'deadline.c',
         '>= deadline->Budget', '>= deadline->Budget * 2'),
        ('fail on global error', V3 / 'fault.c',
         'if (errors != 0) {', 'if (false) {'),
        ('device fault keeps the unit healthy', V3 / 'fault.c',
         '    if (reason != NULL) {\n        __FailForEvent', '    if (true) {\n        __FailForEvent'),
        ('device faults are not logged', V3 / 'fault.c',
         '    __FillDeviceFault(entry, words, fault);\n    return true;',
         '    ERROR("device fault");\n    __FillDeviceFault(entry, words, fault);\n    return true;'),
        ('STE fetch fails the unit', V3 / 'fault.c',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },\n'
         '    { SMMUV3_EVENT_F_STE_FETCH,        DMAIOMMUFaultOther,        0 },'),
        ('access flag fault fails the unit', V3 / 'fault.c',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },\n'
         '    { SMMUV3_EVENT_F_ACCESS,           DMAIOMMUFaultOther,        0 },'),
        ('page request hint fails the unit', V3 / 'fault.c',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },',
         '    { SMMUV3_EVENT_C_BAD_STREAMID,     DMAIOMMUFaultNoRoute,      0 },\n'
         '    { SMMUV3_EVENT_E_PAGE_REQUEST,     DMAIOMMUFaultOther,        0 },'),
        ('translation fault kind', V3 / 'fault.c',
         '{ SMMUV3_EVENT_F_TRANSLATION,      DMAIOMMUFaultNoMapping,',
         '{ SMMUV3_EVENT_F_TRANSLATION,      DMAIOMMUFaultOther,'),
        ('stalled transaction fails the unit', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_STALL) != 0 && (words[1] & SMMUV3_EVENT_STALL) != 0) {', 'if (false) {'),
        ('stall bit only in events that define it', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_STALL) != 0 && (words[1] & SMMUV3_EVENT_STALL) != 0) {',
         'if ((words[1] & SMMUV3_EVENT_STALL) != 0) {'),
        ('stage 2 address size fails the unit', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_STAGE1_ONLY) != 0 && (words[1] & SMMUV3_EVENT_STAGE2) != 0) {', 'if (false) {'),
        ('stage 1 only rule limited to address size', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_STAGE1_ONLY) != 0 && (words[1] & SMMUV3_EVENT_STAGE2) != 0) {',
         'if ((words[1] & SMMUV3_EVENT_STAGE2) != 0) {'),
        ('direction from RnW', V3 / 'fault.c',
         '(words[1] & SMMUV3_EVENT_READ) != 0 ?', '(words[1] & SMMUV3_EVENT_READ) == 0 ?'),
        ('address only where defined', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_ADDRESS) != 0) {', 'if (true) {'),
        ('direction only where defined', V3 / 'fault.c',
         'if ((entry->Fields & __EVENT_DIRECTION) != 0) {', 'if (true) {'),
        ('hardware ID from word 0', V3 / 'fault.c',
         'fault->HardwareID = (uint32_t)(words[0] >> SMMUV3_EVENT_SID_SHIFT);',
         'fault->HardwareID = (uint32_t)(words[0] >> 33);'),
        ('report queue overflow', V3 / 'fault.c',
         '        __AcknowledgeEventOverflow(unit, producer, &faults[0]);\n        count = 1;',
         '        __AcknowledgeEventOverflow(unit, producer, &faults[0]);\n        count = 0;'),
        ('acknowledge queue overflow', V3 / 'fault.c',
         '    unit->EventConsumer = (unit->EventConsumer & ~SMMUV3_EVENT_OVERFLOW) | (producer & SMMUV3_EVENT_OVERFLOW);\n',
         ''),
        ('overflow keeps the unit healthy', V3 / 'fault.c',
         '    fault->Kind = DMAIOMMUFaultReportsLost;',
         '    ArmSMMUUnitFail(&unit->Common, OS_EDEVFAULT);\n    fault->Kind = DMAIOMMUFaultReportsLost;'),
        ('overflow record takes a slot', V3 / 'fault.c',
         'if (events > capacity - count) {', 'if (events > capacity) {'),
        ('capacity bounds reads', V3 / 'fault.c',
         'if (events > capacity - count) {', 'if (false) {'),
        ('records survive a unit failure', V3 / 'fault.c',
         '    *countOut = count;\n    return DMAIOMMUUnitCheck',
         '    *countOut = DMAIOMMUUnitCheck(unit->Common.Backend) == OS_EOK ? count : 0;\n    return DMAIOMMUUnitCheck'),
        ('broken queue pointers fail the unit', V3 / 'fault.c',
         'if (available > queueSize) {', 'if (false) {'),
        ('clear countOut on invalid arguments', V3 / 'unit.c',
         '    if (countOut != NULL) {\n        *countOut = 0;\n    }\n', ''),
        ('reject zero capacity', V3 / 'unit.c',
         'countOut == NULL || capacity == 0) {', 'countOut == NULL) {'),
        ('return poll record count', V3 / 'unit.c',
         '    *countOut = request.Count;\n', ''),
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
        ('complete removal invalidation', V3 / 'table.c',
         '    status = SMMUv3CommandBatchRun(space->Unit, &mapping->Batch, &deadline);\n'
         '    if (status != OS_EOK) {\n        return status;\n    }\n\n'
         '    mapping->Phase = SMMUv3MappingRemoved;',
         '    mapping->Phase = SMMUv3MappingRemoved;'),
        ('VMID zero remains unavailable', V3 / 'table.c',
         'space->VMID = vmid;', 'space->VMID = 0;'),
        ('reserve complete SID set', V3 / 'stream.c',
         'if (__IsHardwareIDReserved(unit, sid)) {', 'if (false) {'),
        ('report stream table allocation failure', V3 / 'stream.c',
         'ERROR("__AllocateStreamTable: allocating', 'if (false) ERROR("__AllocateStreamTable: allocating'),
        ('validate duplicate SIDs', V3 / 'stream.c',
         'if (hardwareIDs[previous] == sid) {', 'if (false) {'),
        ('shared L1 invalidation', V3 / 'stream.c',
         '__InvalidateEntry(unit, &connection->Batch, sid, false, deadline);', '__InvalidateEntry(unit, &connection->Batch, sid, true, deadline);'),
        ('complete STE publication', V3 / 'stream.c',
         'entry[0] = SMMUV3_STE_STAGE2;', 'entry[0] = SMMUV3_STE_ABORT;'),
        ('abort on disconnect', V3 / 'stream.c',
         'entry[0] = SMMUV3_STE_ABORT;', 'entry[0] = SMMUV3_STE_STAGE2;'),
        ('preserve disconnect retry batch', V3 / 'stream.c',
         'if (connection->Batch.Count == 0) {', 'if (true) {'),
        ('level-one root geometry', V3 / 'unit.c',
         'unit->Stage2.RootLevel = unit->AddressBits > 39U ? 0U : 1U;', 'unit->Stage2.RootLevel = 0;'),
        ('cap output to 48 bits', V3 / 'unit.c',
         'unit->AddressEncoding = outputAddressEncoding > 5U ? 5U : outputAddressEncoding;',
         'unit->AddressEncoding = outputAddressEncoding;'),
        ('observe generic unit failure', Path('kernel/memory/dma/unit.c'),
         'return atomic_load(&unit->Failure) == OS_EOK ? OS_EOK : OS_ECANCELLED;', 'return OS_EOK;'),
    )
    for mutation in mutations:
        host_run(output, mutation)
        print("Mutation rejected:", mutation[0], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-smmuv3-") as directory:
        output = Path(directory)
        host_run(output)
        compile_target(output)
        if args.mutation_check:
            run_mutations(output)


if __name__ == "__main__":
    main()
