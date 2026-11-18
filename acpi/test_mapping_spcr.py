#!/usr/bin/env python3
"""Exercise ACPICA mapping policies and SPCR validation with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
flags = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-fms-extensions',
         '-fasm-blocks', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=static inline',
         '-DSERVICEABI=', '-DLIBC_KERNEL', '-D__LIBDS_KERNEL__',
         '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
         '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
         '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
         '-Wno-unused-but-set-global', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-g']
for directory in ['testing/include', 'kernel/include', 'librt/libos/include',
                  'librt/libacpi/source/include', 'librt/libfdt/include']:
    flags += ['-I' + str(ROOT / directory)]
for directory in ['librt/libds/include', 'librt/libddk/include', 'boot/include']:
    flags += ['-isystem', str(ROOT / directory)]
arm = ['-DTEST_ARM64', '-U__amd64__', '-D__aarch64__']
cases = [
    ('gicv3', arm, ['testing/gic/gicv3_test.c']),
    ('platform', arm, ['testing/acpi/platform_test.c', 'kernel/arch/aarch64/acpi/platform.c',
       'kernel/arch/aarch64/acpi/tables.c', 'kernel/arch/aarch64/acpi/madt.c',
       'kernel/arch/aarch64/acpi/gtdt.c', 'kernel/arch/aarch64/acpi/fadt.c']),
    ('device', arm, ['testing/acpi/device_test.c', 'kernel/arch/aarch64/common/device.c']),
    ('output', arm, ['testing/acpi/output_test.c']),
    ('root-arm64', arm, ['testing/acpi/root_test.c', 'librt/libacpi/source/os_specific/service_layers/osvaliktbl.c']),
    ('root-x86', [], ['testing/acpi/root_test.c', 'librt/libacpi/source/os_specific/service_layers/osvaliktbl.c']),
    ('spcr', ['-U__amd64__', '-D__aarch64__'], ['testing/acpi/spcr_test.c', 'kernel/arch/aarch64/acpi/spcr.c', 'kernel/arch/aarch64/acpi/tables.c']),
    ('mapping-arm64', ['-DTEST_ARM64', '-U__amd64__', '-D__aarch64__'], ['testing/acpi/mapping_test.c',
       'librt/libacpi/source/os_specific/service_layers/osvalikmem.c',
       'kernel/arch/aarch64/acpi/memory.c']),
    ('mapping-x86', [], ['testing/acpi/mapping_test.c',
       'librt/libacpi/source/os_specific/service_layers/osvalikmem.c',
       'kernel/arch/x86/components/acpi.c']),
]
with tempfile.TemporaryDirectory(prefix='vali-acpi-') as temporary:
    for name, extra, sources in cases:
        binary = Path(temporary) / name
        subprocess.run(flags + extra + [str(ROOT / p) for p in sources] + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    if args.mutation_check:
        source = ROOT / 'kernel/arch/aarch64/acpi/madt.c'
        original = source.read_text()
        mutations = [
            ('ITS duplicate identity', 'entry->TranslationId == other->Identifier', 'false'),
            ('ITS frame overlap', '__Overlap(entry->BaseAddress, GICV3_ITS_SIZE, other->Base, other->Length)', 'false'),
            ('ITS redistributor overlap', '__Overlap(its->Base, its->Length, base, length)', 'false'),
            ('ITS copied identifier', '= entry->TranslationId;', '= 0;'),
            ('ITS address width', '!__ValidRange(entry->BaseAddress, GICV3_ITS_SIZE, GICV3_FRAME_SIZE)', 'false'),
        ]
        sources = next(sources for name, _, sources in cases if name == 'platform')
        for name, old, new in mutations:
            assert original.count(old) == 1, name
            changed = original.replace(old, new)
            changed = re.sub(r'#include "([^"]+)"',
                             lambda match: '#include "' + str(source.parent / match[1]) + '"', changed)
            mutant = Path(temporary) / 'mutant.c'
            mutant.write_text(changed)
            binary = Path(temporary) / 'platform-mutant'
            inputs = [str(mutant) if ROOT / p == source else str(ROOT / p) for p in sources]
            subprocess.run(flags + arm + inputs + ['-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, timeout=30,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            assert result.returncode == -signal.SIGABRT and b'Assertion' in result.stderr, name
            print('Rejected MADT mutation:', name, flush=True)
