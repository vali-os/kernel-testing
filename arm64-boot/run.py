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
p.add_argument('--edk2-source', type=Path, help='Writable EDK2 source tree; otherwise fetched by CMake')
p.add_argument('--qemu', default='qemu-system-aarch64')
p.add_argument('--ld', default='ld.lld', help='ELF linker for the CPU transition probe')
p.add_argument('--gdb', default='gdb')
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
def tool_link(name, source):
    if not source:
        raise SystemExit(f'{name} not found')
    target = tooldir / name
    if target.is_symlink():
        target.unlink()
    target.symlink_to(Path(source).resolve())
    return target

link = tool_link('lld-link', linker)
compiler = tool_link('clang', shutil.which(a.clang))
for name in ['llvm-lib', 'llvm-objcopy']:
    tool_link(name, shutil.which(name))
env['PATH'] = str(tooldir) + os.pathsep + env['PATH']

def run(*args):
    subprocess.run([str(x) for x in args], cwd=ROOT, env=env, check=True)

run('python3', 'testing/arm64-boot/test_layout.py')
configure = ['cmake', '-S', 'boot/uefi', '-B', a.build / 'uefi',
             '-DVALI_ARCH=aarch64', '-DCMAKE_C_COMPILER=' + str(compiler)]
if a.edk2_source:
    configure.append('-DFETCHCONTENT_SOURCE_DIR_EDK2=' + str(a.edk2_source.resolve()))
run(*configure)
edk2 = a.edk2_source.resolve() if a.edk2_source else a.build / 'uefi/_deps/edk2-src'
includes = ['-Iboot/include', '-Iboot/uefi/include',
            '-I' + str(edk2 / 'MdePkg/Include'), '-I' + str(edk2 / 'MdePkg/Include/X64')]
run(a.clang, '-std=c11', '-fshort-wchar', '-Wall', '-Wextra', '-Werror', *includes,
    'testing/arm64-boot/services_test.c', 'boot/uefi/arm64/library.c', '-o', a.build / 'services-test')
run(a.build / 'services-test')
run(a.clang, '-std=c11', '-D__aarch64__', '-DNO_MSABI_VA_FUNCS', '-fshort-wchar', '-Wall', '-Wextra', '-Werror',
    '-Wno-address-of-packed-member', '-fsanitize=address,undefined', *includes,
    'testing/arm64-boot/console_video_test.c', 'boot/uefi/console.c', 'boot/uefi/video.c',
    '-o', a.build / 'console-video-test')
# Pool balance is asserted by the mock; LSan requires ptrace unavailable in sandboxes.
subprocess.run([str(a.build / 'console-video-test')], check=True,
               env={**env, 'ASAN_OPTIONS': 'detect_leaks=0'})
run(a.clang, '--version')
run(link, '--version')
run('git', '-C', edk2, 'rev-parse', 'HEAD')
run('cmake', '--build', a.build / 'uefi', '-j4')
run('python3', 'testing/arm64-boot/test_transition.py', '--build', a.build / 'transition',
    '--edk2-source', edk2, '--clang', a.clang, '--ld', a.ld, '--qemu', a.qemu, '--gdb', a.gdb)
run('cmake', '-S', 'tools/lzss', '-B', a.build / 'lzss')
run('cmake', '--build', a.build / 'lzss', '-j4')
run(a.clang, '--target=aarch64-pc-windows-msvc', '-ffreestanding', '-fno-stack-protector',
    '-mgeneral-regs-only', '-mstrict-align', '-O2', '-Iboot/include', '-c',
    'testing/arm64-boot/entry.c', '-o', a.build / 'entry.obj')
run(a.clang, '--target=aarch64-pc-windows-msvc', '-c', 'testing/arm64-boot/entry.S',
    '-o', a.build / 'entry-asm.obj')
run(link, '/entry:kentry', '/subsystem:native', '/nodefaultlib', '/base:0x48000000',
    '/out:' + str(a.build / 'kernel.mos'), a.build / 'entry.obj', a.build / 'entry-asm.obj')
# Use EDK2's real parser and relocator in native placement/malformed-image tests.
run(a.clang, '-std=c11', '-fshort-wchar', '-DMDEPKG_NDEBUG', '-DNO_MSABI_VA_FUNCS',
    '-fsanitize=undefined', *includes,
    'testing/arm64-boot/image_memory_test.c', 'boot/uefi/arm64/image.c',
    'boot/uefi/arm64/firmware.c', edk2 / 'MdePkg/Library/BasePeCoffLib/BasePeCoff.c',
    edk2 / 'MdePkg/Library/BasePeCoffLib/PeCoffLoaderEx.c', '-o', a.build / 'image-memory-test')
run(a.build / 'image-memory-test', a.build / 'kernel.mos')
esp = a.build / 'esp'
(esp / 'EFI/BOOT').mkdir(parents=True, exist_ok=True)
(esp / 'EFI/VALI').mkdir(parents=True, exist_ok=True)
shutil.copy2(a.build / 'uefi/BOOTAA64.EFI', esp / 'EFI/BOOT/BOOTAA64.EFI')
shutil.copy2(a.build / 'kernel.mos', esp / 'EFI/VALI/phoenix.mos')
(esp / 'EFI/VALI/initrd.mos').write_bytes(b'ARM64 fixture initrd')
run(a.build / 'lzss/lzss', 'c', a.build / 'kernel.mos', esp / 'EFI/VALI/kernel.mos')
print('Firmware SHA256:', hashlib.sha256(a.firmware.read_bytes()).hexdigest(), flush=True)
run(a.qemu, '--version')
cases = [
    ('preferred-el1', 'off', 'cortex-a57', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
    ('preferred-el2', 'on', 'cortex-a57', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
    ('relocated-el1', 'off', 'cortex-a57', '0x100000', '1024', 'ARM64 HANDOFF PASS'),
    ('relocated-el2', 'on', 'cortex-a57', '0x100000', '1024', 'ARM64 HANDOFF PASS'),
    ('high-el1', 'off', 'cortex-a57', '0x100000000', '8192', 'ARM64 HIGH HANDOFF PASS'),
    ('high-el2', 'on', 'cortex-a57', '0x100000000', '8192', 'ARM64 HIGH HANDOFF PASS'),
    ('a53-el1', 'off', 'cortex-a53', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
    ('a53-el2', 'on', 'cortex-a53', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
    ('max-el1', 'off', 'max', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
    ('max-el2', 'on', 'max', '0x48000000', '1024', 'ARM64 HANDOFF PASS'),
]
for name, virtualization, cpu, base, ram, marker in cases:
    # An unavailable preferred address forces the actual firmware allocator's
    # fallback path; high cases place the same fixture above 4 GiB.
    run(link, '/entry:kentry', '/subsystem:native', '/nodefaultlib', '/base:' + base,
        '/out:' + str(a.build / 'kernel.mos'), a.build / 'entry.obj', a.build / 'entry-asm.obj')
    run(a.build / 'lzss/lzss', 'c', a.build / 'kernel.mos', esp / 'EFI/VALI/kernel.mos')
    log = a.build / f'qemu-{name}.log'
    with log.open('wb') as output:
        process = subprocess.Popen([a.qemu, '-machine',
            f'virt-9.2,gic-version=3,virtualization={virtualization},secure=off',
            '-cpu', cpu, '-m', ram, '-smp', '1', '-bios', str(a.firmware),
            '-drive', f'format=raw,file=fat:{esp},readonly=on', '-nographic', '-no-reboot'],
            stdout=output, stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline and process.poll() is None:
                text = log.read_text(errors='replace')
                if 'ARM64 HANDOFF FAIL' in text:
                    raise RuntimeError(f'Handoff assertion failed; inspect {log}')
                if marker in text:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f'Boot gate failed; inspect {log}')
            print(f'{name}: PASS ({log})', flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
