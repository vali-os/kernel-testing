#!/usr/bin/env python3
"""Exercise ACPI PCI roots, ECAM, routing and deviced swizzling with ASan/UBSan."""
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
         '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-but-set-variable',
         '-Wno-unused-but-set-global', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-g']
for directory in ['testing/include', 'kernel/include', 'librt/libos/include',
                  'librt/libacpi/source/include', 'librt/libfdt/include', 'services/deviced', 'services/deviced/include', 'librt/libgracht/include']:
    flags += ['-I' + str(ROOT / directory)]
for directory in ['librt/libds/include', 'librt/libddk/include', 'boot/include']:
    flags += ['-isystem', str(ROOT / directory)]
cases = [('pci-links', ['testing/acpi/pci_links_test.c', 'librt/libds/list.c']), ('pci-swizzle', ['testing/acpi/pci_swizzle_test.c']), ('pci-config', ['testing/acpi/pci_config_test.c']), ('pci-roots', ['testing/acpi/pci_roots_test.c']), ('pci-windows', ['testing/acpi/pci_windows_test.c']), ('pci-prt', ['testing/acpi/pci_prt_test.c']), ('pci-prt-arm64', ['testing/acpi/pci_prt_test.c'])]
with tempfile.TemporaryDirectory(prefix='vali-acpi-pci-') as temporary:
    for name, sources in cases:
        binary = Path(temporary) / name
        extra = ['-U__amd64__', '-D__aarch64__'] if name.endswith('-arm64') else []
        subprocess.run(flags + extra + [str(ROOT / p) for p in sources] + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
