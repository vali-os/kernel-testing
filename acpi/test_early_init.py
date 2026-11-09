#!/usr/bin/env python3
"""Test the shared ACPICA startup with mock, x86 and ARM architecture hooks."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='vali-acpi-hook-') as temporary:
    flags = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-fms-extensions',
             '-fasm-blocks', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=static inline',
             '-DSERVICEABI=', '-DLIBC_KERNEL', '-D__LIBDS_KERNEL__',
             '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
             '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-but-set-variable', '-Wno-unused-but-set-global', '-Wno-unused-function', '-Wno-unused-variable',
             '-Wno-unused-parameter', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-g']
    for directory in ['testing/include', 'kernel/include', 'librt/libos/include',
                      'librt/libacpi/source/include']:
        flags += ['-I' + str(ROOT / directory)]
    for directory in ['librt/libds/include', 'librt/libddk/include', 'boot/include']:
        flags += ['-isystem', str(ROOT / directory)]
    for name, hook in [('shared', None), ('x86', 'kernel/arch/x86/components/acpi.c'),
                       ('arm64', 'kernel/arch/aarch64/acpi/platform.c')]:
        binary = Path(temporary) / name
        command = flags + ['-DTEST_' + name.upper() + '_HOOK',
                           str(ROOT / 'testing/acpi/early_init_test.c'),
                           str(ROOT / 'kernel/acpi/early_init.c')]
        if hook:
            command.append(str(ROOT / hook))
        if name == 'arm64':
            command += ['-U__amd64__', '-D__aarch64__', '-I' + str(ROOT / 'librt/libfdt/include')]
            command += [str(ROOT / ('kernel/arch/aarch64/acpi/' + source)) for source in ['tables.c', 'madt.c', 'gtdt.c', 'fadt.c']]
        subprocess.run(command + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        print(name + ': PASS', flush=True)
