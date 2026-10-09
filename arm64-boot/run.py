#!/usr/bin/env python3
"""Standalone ARM64 bootloader gate; uses a fixture, not the unfinished OS kernel."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--firmware', type=Path, required=True)
p.add_argument('--clang', default='clang')
p.add_argument('--lld-link', default='lld-link')
p.add_argument('--qemu', default='qemu-system-aarch64')
a = p.parse_args()
a.build = a.build.resolve()
a.firmware = a.firmware.resolve()
a.build.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
# Clang's PE driver finds lld-link via PATH.
tooldir = a.build / 'tools'
tooldir.mkdir(exist_ok=True)
linker = shutil.which(a.lld_link)
if not linker:
    raise SystemExit('lld-link not found; supply --lld-link /path/to/lld-link')
link = tooldir / 'lld-link'
if not link.exists():
    link.symlink_to(Path(linker).resolve())
env['PATH'] = str(tooldir) + os.pathsep + env['PATH']

def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, env=env, check=True)

includes = ['-I' + str(ROOT / x) for x in (
    'boot/include', 'boot/uefi/include', 'boot/uefi/include/efi', 'boot/uefi/include/efi/AArch64')]
run('cc', '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror', *includes,
    'testing/arm64-boot/services_test.c', 'boot/uefi/arm64/services.c', '-o', a.build / 'services-test')
run(a.build / 'services-test')
run('cmake', '-S', 'boot/uefi', '-B', a.build / 'uefi', '-DVALI_ARCH=aarch64',
    '-DCMAKE_C_COMPILER=' + a.clang, '-DCMAKE_ASM_COMPILER=' + a.clang)
run('cmake', '--build', a.build / 'uefi', '-j4')
run('cmake', '-S', 'tools/lzss', '-B', a.build / 'lzss')
run('cmake', '--build', a.build / 'lzss', '-j4')
run(a.clang, '--target=aarch64-pc-windows-msvc', '-ffreestanding', '-fno-stack-protector',
    '-mgeneral-regs-only', '-mstrict-align', '-O2', '-Iboot/include', '-c',
    'testing/arm64-boot/entry.c', '-o', a.build / 'entry.obj')
run(link, '/entry:kentry', '/subsystem:native', '/nodefaultlib', '/base:0x48000000',
    '/out:' + str(a.build / 'kernel.mos'), a.build / 'entry.obj')
esp = a.build / 'esp'
(esp / 'EFI/BOOT').mkdir(parents=True, exist_ok=True)
(esp / 'EFI/VALI').mkdir(parents=True, exist_ok=True)
shutil.copy2(a.build / 'uefi/BOOTAA64.EFI', esp / 'EFI/BOOT/BOOTAA64.EFI')
shutil.copy2(a.build / 'kernel.mos', esp / 'EFI/VALI/phoenix.mos')
(esp / 'EFI/VALI/initrd.mos').write_bytes(b'ARM64 fixture initrd')
run(a.build / 'lzss/lzss', 'c', a.build / 'kernel.mos', esp / 'EFI/VALI/kernel.mos')
print('Firmware SHA256:', hashlib.sha256(a.firmware.read_bytes()).hexdigest(), flush=True)
run(a.qemu, '--version')
for virtualization, cpu, marker in [('off', 'cortex-a57', 'ARM64 HANDOFF PASS'),
                                    ('on', 'cortex-a57', 'ARM64 HANDOFF PASS'),
                                    ('off', 'cortex-a53', 'Unsupported ARM64 handoff state')]:
    log = a.build / f'qemu-{cpu}-{virtualization}.log'
    with log.open('wb') as output:
        process = subprocess.Popen([a.qemu, '-machine',
            f'virt-9.2,gic-version=3,virtualization={virtualization},secure=off',
            '-cpu', cpu, '-m', '1024', '-smp', '1', '-bios', str(a.firmware),
            '-drive', f'format=raw,file=fat:{esp},readonly=on', '-nographic', '-no-reboot'],
            stdout=output, stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and process.poll() is None:
                text = log.read_text(errors='replace')
                if marker in text:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f'Boot gate failed; inspect {log}')
            print(f'{cpu}, virtualization={virtualization}: PASS ({log})', flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
