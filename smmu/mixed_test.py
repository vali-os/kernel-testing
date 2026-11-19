#!/usr/bin/env python3
"""Exercise both native SMMU backends through the real DMA/SHM bound fixture."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from dma_bound_test import ROOT, build_host_fixture


def compile_source(flags, original, target, replacement=None):
    """Keep private includes relative to the original source during mutation."""
    compile_flags = [flag for flag in flags if not flag.startswith('-Wl,')]
    # Vali prints uint64_t with %llx. The host typedef is unsigned long.
    # Both are 64 bits here. Keep format checks on generic DMA/SHM code.
    if original.startswith('kernel/arch/aarch64/smmu/'):
        compile_flags += ['-Wno-format']
    source = ROOT / original
    if replacement is not None:
        compile_flags += ['-I' + str(source.parent)]
        source = replacement
    subprocess.run(compile_flags + ['-c', str(source), '-o', str(target)], check=True)


def run_mutations(output, flags, sources, objects, environment):
    """Only a failed assertion counts. Sanitizer faults and timeouts fail the run."""
    mutations = [
        ('block all reset peers', 'kernel/memory/dma/recovery.c',
         'assert(*count < DMA_MANAGER_MAX_ATTACHMENTS);\n        DMARecoveryBlockLocked(manager, group);',
         'assert(*count < DMA_MANAGER_MAX_ATTACHMENTS);'),
        ('preserve source until removal completes', 'kernel/memory/shm/device/cleanup.c',
         'status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (status != OS_EOK) {',
         'status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (false) {'),
        ('keep connection space referenced', 'kernel/memory/dma/connection.c',
         'status = DMAAddressSpaceAcquire(space);', 'status = OS_EOK;'),
        ('keep manager-loss admission closed', 'kernel/memory/dma/binding.c',
         'if (group->Access.ManagerClosed) {', 'if (false) {'),
        ('v3 wait for final sync', 'kernel/arch/aarch64/smmu/v3/queue.c',
         'unit->CommandsCompleted >= batch->EndPosition', 'unit->CommandsCompleted + 1 >= batch->EndPosition'),
        ('mmu500 wait for hardware idle', 'kernel/arch/aarch64/smmu/mmu500/context.c',
         'if ((active & SMMU500_SYNC_ACTIVE) == 0) {', 'if ((active & 0U) == 0) {'),
        ('v3 reject duplicate SID', 'kernel/arch/aarch64/smmu/v3/stream.c',
         'if (__IsHardwareIDReserved(unit, sid)) {', 'if (false) {'),
        ('mmu500 reject duplicate SID', 'kernel/arch/aarch64/smmu/mmu500/stream.c',
         'if (unit->Slots[slot].Reserved && unit->Slots[slot].SID == hardwareIDs[index]) {', 'if (false) {'),
    ]
    # Both backends share one leaf writer, so its mutations are checked once.
    shared = 'kernel/arch/aarch64/smmu/stage2-table.c'
    mutations += [
        ('read-only permission', shared,
         'if (config->Direction != DMAToDevice) {', 'if (true) {'),
        ('write-only permission', shared,
         'if (config->Direction != DMAFromDevice) {', 'if (true) {'),
        ('bidirectional read', shared,
         'if (config->Direction != DMAFromDevice) {', 'if (config->Direction == DMAToDevice) {'),
        ('bidirectional write', shared,
         'if (config->Direction != DMAToDevice) {', 'if (config->Direction == DMAFromDevice) {'),
    ]
    for backend, prefix in (('v3', 'SMMUv3'), ('mmu500', 'SMMU500')):
        path = f'kernel/arch/aarch64/smmu/{backend}/table.c'
        if backend == 'v3':
            mutations.append(('v3 complete removal invalidation', path,
                              '    status = SMMUv3CommandBatchRun(space->Unit, &mapping->Batch, &deadline);\n'
                              '    if (status != OS_EOK) {\n        return status;\n    }\n\n'
                              '    mapping->Phase = SMMUv3MappingRemoved;',
                              '    mapping->Phase = SMMUv3MappingRemoved;'))
        else:
            mutations.append((backend + ' complete removal invalidation', path,
                              f'mapping->Phase = {prefix}MappingInvalidate;',
                              f'mapping->Phase = {prefix}MappingRemoved; return OS_EOK;'))
        model = 'g_smmuv3Model' if backend == 'v3' else 'g_smmu500Model'
        acknowledgement = 'case 0x100AC:' if backend == 'v3' else 'g_smmu500Model.BankRace = 0;'
        for cache in ('Routes', 'Translations'):
            mutations.append((backend + ' fault acknowledgement preserves ' + cache,
                              f'testing/smmu/{backend}/register_model.c', acknowledgement,
                              acknowledgement + f' memset({model}.{cache}, 0, sizeof({model}.{cache}));'))
    for name, path, before, after in mutations:
        original = (ROOT / path).read_text()
        assert original.count(before) == 1, (name, original.count(before))
        source = output / 'mutant.c'
        source.write_text(original.replace(before, after))
        target = output / 'mutant.o'
        compile_source(flags, path, target, source)
        compiled = objects.copy()
        compiled[sources.index(path)] = str(target)
        binary = output / 'mutant'
        subprocess.run(flags + compiled + ['-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=60)
        child = re.search(rb'^Mixed SMMU child \d+ failed \(wait status (\d+)\)$', result.stderr, re.M)
        child_aborted = (child is not None and os.WIFSIGNALED(int(child[1])) and
                         os.WTERMSIG(int(child[1])) == signal.SIGABRT)
        aborted = result.returncode == -signal.SIGABRT or (result.returncode == 1 and child_aborted)
        sanitizer_fault = b'AddressSanitizer' in result.stderr or b'runtime error:' in result.stderr
        if not aborted or b'Assertion' not in result.stderr or sanitizer_fault:
            raise AssertionError(f'Mutation did not fail at an assertion: {name}: {result.stderr.decode()}')
        print('Mixed SMMU mutation rejected:', name, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-smmu-mixed-') as directory:
        output = Path(directory)
        flags, sources, environment = build_host_fixture(output)
        # The arena includes machine/scheduler records directly. Match the
        # time-type include supplied by the real threading header.
        threading = output / 'threading.h'
        threading.write_text('#include <os/types/time.h>\n' + threading.read_text())
        sources[0] = 'testing/smmu/mixed_test.c'
        sources += ['kernel/memory/shm/shm.c', 'kernel/arch/aarch64/smmu/memory.c',
                    'kernel/arch/aarch64/smmu/stage2-table.c', 'kernel/arch/aarch64/smmu/deadline.c',
                    'kernel/arch/aarch64/smmu/unit-common.c', 'testing/smmu/v3/register_model.c', 'testing/smmu/mmu500/register_model.c']
        for backend in ('v3', 'mmu500'):
            sources += [str(path.relative_to(ROOT)) for path in
                        sorted((ROOT / 'kernel/arch/aarch64/smmu' / backend).glob('*.c'))]
        flags += ['-DDMA_TEST_REAL_DEVICE_BUFFER', '-DSMMUV3_TEST', '-DSMMU500_TEST',
                  '-Wno-sign-compare',
                  '-Wl,--wrap=MemorySpaceMap', '-Wl,--wrap=MemorySpaceUnmap',
                  '-Wl,--wrap=GetCurrentMemorySpace', '-Wl,--wrap=GetDomainMemorySpace',
                  '-Wl,--wrap=SystemMemoryReleaseAllocation']
        binary = output / 'mixed'
        objects = []
        for index, path in enumerate(sources):
            target = output / f'source-{index}.o'
            compile_source(flags, path, target)
            objects.append(str(target))
        subprocess.run(flags + objects + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=60)
        if args.mutation_check:
            run_mutations(output, flags, sources, objects, environment)


if __name__ == '__main__':
    main()
