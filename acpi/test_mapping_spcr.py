#!/usr/bin/env python3
"""Exercise ACPICA mapping policies and SPCR validation with ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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
