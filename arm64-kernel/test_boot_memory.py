#!/usr/bin/env python3
"""Run the actual early ARM64 memory policy with ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='vali-boot-memory-') as temporary:
    binary = Path(temporary) / 'test'
    command = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fms-extensions',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-no-pie']
    for include in ['boot/include', 'librt/libos/include', 'librt/libc/include',
                    'kernel/arch/aarch64/common']:
        command += ['-I', str(root / include)]
    command += [str(root / 'testing/arm64-kernel/boot_memory_test.c'),
                str(root / 'kernel/arch/aarch64/common/boot-memory.c'), '-o', str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
