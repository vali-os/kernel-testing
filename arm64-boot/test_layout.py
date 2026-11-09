#!/usr/bin/env python3
"""Check the C wire layout on each target and the NASM BIOS mirror."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='vali-vboot-layout-') as temporary:
    build = Path(temporary)
    source = build / 'layout.c'
    source.write_text('#include <vboot/vboot.h>\n#include "layout.h"\n')
    for target in ['aarch64-pc-windows-msvc', 'x86_64-pc-windows-msvc', 'i686-pc-windows-msvc']:
        subprocess.run(['clang', '--target=' + target, '-fsyntax-only',
                        '-I' + str(ROOT / 'boot/include'),
                        '-I' + str(ROOT / 'testing/arm64-boot'), str(source)], check=True)
    bios = (ROOT / 'boot/bios/stage2/systems/common16.inc').read_text()
    structures = re.findall(r'^struc VBoot(?:MemoryEntry)?\n.*?^endstruc', bios, re.M | re.S)
    version = re.search(r'^%define VBOOT_VERSION (\S+)', bios, re.M)[1]
    fields = ['VBoot_size', 'VBootMemoryEntry_size', 'VBoot.ConfigTable',
              'VBoot.MemoryCount', 'VBoot.VideoFramebuffer', 'VBoot.RamdiskBase',
              'VBoot.KernelBase', 'VBoot.StackBase', 'VBoot.PhoenixBase',
              'VBoot.DeviceTreeBase']
    assembly = build / 'layout.asm'
    constants = re.findall(r'^%define VBOOT_MEMORY_TYPE_(?:RESERVED|FIRMWARE|ACPI|NVS|AVAILABLE|RECLAIM|DEVICEPOOL)\s+\S+', bios, re.M)
    values = ['VBOOT_MEMORY_TYPE_' + name for name in
              ['RESERVED', 'FIRMWARE', 'ACPI', 'NVS', 'AVAILABLE', 'RECLAIM', 'DEVICEPOOL']]
    assembly.write_text('\n'.join(constants + structures) + '\ndd ' + version + ', ' +
                        ', '.join(fields + values) + '\n')
    output = build / 'layout.bin'
    subprocess.run(['nasm', '-f', 'bin', str(assembly), '-o', str(output)], check=True)
    assert struct.unpack('<18I', output.read_bytes()) == (
        0x10002, 176, 36, 20, 28, 44, 100, 112, 132, 144, 164, 0, 1, 2, 3, 4, 5, 6)
print('VBoot 1.2 C/BIOS layout PASS')
