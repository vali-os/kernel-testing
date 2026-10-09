#!/usr/bin/env python3
"""Boot the built Pi kernel and Phoenix through the native loader under QEMU.

The default fixture supplies an empty /services directory. --ramdisk-root
instead boots a populated system directory and can check service registration.
The gate checks timer IRQ delivery, all four CPU startups, EL0 entry, syscalls,
DT dynamic reservations, no-map exclusion, and Phoenix opening the initramfs.
"""
import argparse
import contextlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(*arguments):
    subprocess.run([str(argument) for argument in arguments], check=True,
                   stdout=subprocess.DEVNULL)


def symbol(mapping, name):
    match = re.search(r'^([0-9a-fA-F]+).*?\s' + re.escape(name) + r'$', mapping, re.M)
    if not match:
        raise ValueError(f'missing kernel map symbol: {name}')
    return int(match[1], 16)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--mkvafs', required=True, type=Path)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    parser.add_argument('--gdb', default='gdb')
    parser.add_argument('--ramdisk-root', type=Path,
                        help='Use a populated system directory instead of the minimal fixture')
    parser.add_argument('--run-seconds', type=int, default=3)
    parser.add_argument('--output-dir', type=Path,
                        help='Retain boot artifacts and debugger output for diagnosis')
    parser.add_argument('--require-service', action='append', default=[],
                        help='Require a named service path to register during the boot')
    args = parser.parse_args()
    build = args.build.resolve()
    mapping = (build / 'bin/kernel.map').read_text()
    phoenix = build / 'bin/phoenix.mos'
    data = phoenix.read_bytes()
    optional = struct.unpack_from('<I', data, 60)[0] + 24
    entry = struct.unpack_from('<Q', data, optional + 24)[0]
    entry += struct.unpack_from('<I', data, optional + 16)[0]

    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        storage = contextlib.nullcontext(str(args.output_dir.resolve()))
    else:
        storage = tempfile.TemporaryDirectory(prefix='vali-kernel-boot-')
    with storage as temporary:
        directory = Path(temporary)
        dtb = directory / 'board.dtb'
        shutil.copyfile(ROOT / 'boot/rpi/firmware/bcm2711-rpi-4-b.dtb', dtb)
        run('fdtput', '-t', 's', dtb, '/chosen', 'stdout-path', 'serial1:115200n8')
        run('fdtput', '-t', 's', dtb, '/soc/serial@7e201000', 'status', 'okay')
        # Keep the firmware's dynamic CMA request enabled. Also add a region
        # that must never acquire a direct mapping, including speculative access.
        run('fdtput', '-c', dtb, '/reserved-memory/boot-test@1000000')
        run('fdtput', '-t', 'x', dtb, '/reserved-memory/boot-test@1000000',
            'reg', '0', '1000000', '1000')
        run('fdtput', dtb, '/reserved-memory/boot-test@1000000', 'no-map')
        if args.ramdisk_root:
            ramdisk_root = args.ramdisk_root.resolve()
        else:
            ramdisk_root = directory / 'root'
            (ramdisk_root / 'services').mkdir(parents=True)
            # VaFS needs a nonempty data stream when finalizing the image.
            (ramdisk_root / 'README').write_text('ARM64 kernel boot fixture\n')
        ramdisk = directory / 'initrd.mos'
        run(args.mkvafs.resolve(), '--arch', 'arm64', '--out', ramdisk, ramdisk_root)
        bundle = directory / 'boot-payload.bin'
        run('python3', ROOT / 'tools/rpi/bundle.py', '--phoenix', phoenix,
            '--ramdisk', ramdisk, '--output', bundle)
        serial = directory / 'serial.log'
        commands = [
            f'target remote | {args.qemu} -M raspi4b -m 2G -smp 4 '
            f'-kernel {build}/boot-assets/kernel8.img -dtb {dtb} -initrd {bundle} '
            f'-display none -serial file:{serial} -monitor none -S -gdb stdio',
            'break RpiLoaderStop',
            'break __JumpToKernel',
            'continue',
            'python assert int(gdb.parse_and_eval("$pc")) == '
            'int(gdb.parse_and_eval("&__JumpToKernel")), "loader rejected the boot artifacts"',
            f'set $base = $x1 - {symbol(mapping, "kentry")}',
            'delete breakpoints',
            f'break *($base+{symbol(mapping, "__TimerInterrupt")})',
            f'break *($base+{symbol(mapping, "DebugPanic")})',
            'continue',
            'python assert int(gdb.parse_and_eval("$pc")) == '
            f'int(gdb.parse_and_eval("$base")) + {symbol(mapping, "__TimerInterrupt")}, '
            '"kernel panicked before delivering a timer IRQ"',
            'delete breakpoints',
            f'break *{entry}',
            f'break *($base+{symbol(mapping, "DebugPanic")})',
            'continue',
            f'python assert int(gdb.parse_and_eval("$pc")) == {entry}, "Phoenix did not start"',
            'python assert int(gdb.parse_and_eval("$cpsr")) & 15 == 0, "Phoenix must enter at EL0"',
            'python',
            'inferior = gdb.selected_inferior()',
            'def read64(address):',
            '    return int.from_bytes(inferior.read_memory(address, 8).tobytes(), "little")',
            f'table = read64(int(gdb.parse_and_eval("$base")) + {symbol(mapping, "g_identityTable")})',
            'for shift in (39, 30, 21, 12):',
            '    descriptor = read64(table + ((0x1000000 >> shift) & 511) * 8)',
            '    if descriptor & 1 == 0:',
            '        break',
            '    assert shift != 12 and descriptor & 3 == 3, "no-map region was mapped"',
            '    table = descriptor & 0x0000fffffffff000',
            'end',
            'delete breakpoints',
            f'break *($base+{symbol(mapping, "DebugPanic")})',
            'python import threading, os, signal; '
            f'threading.Timer({args.run_seconds}, lambda: os.kill(os.getpid(), signal.SIGINT)).start()',
            'continue',
            'python assert int(gdb.parse_and_eval("$pc")) != '
            f'int(gdb.parse_and_eval("$base")) + {symbol(mapping, "DebugPanic")}, '
            '"kernel panicked during userspace execution"',
            'thread apply all info registers pc sp x29 x30',
            'python print("ARM64 KERNEL BOOT PASS")',
            'monitor quit',
        ]
        script = directory / 'boot.gdb'
        script.write_text('\n'.join(commands) + '\n')
        result = subprocess.run([args.gdb, '-q', '-batch',
                                 str(build / 'boot-assets/bin/rpi-loader.elf'), '-x', str(script)],
                                capture_output=True, text=True, timeout=30 + args.run_seconds,
                                env={**os.environ, 'LC_ALL': 'C'})
        output = result.stdout + result.stderr
        (directory / 'debugger.log').write_text(output)
        log = serial.read_text(errors='replace')
        if 'ARM64 KERNEL BOOT PASS' not in output or 'AssertionError' in output:
            raise RuntimeError(output + '\n' + log)
        for core in range(1, 4):
            if f'CpuCoreStart {core} is online' not in log:
                raise RuntimeError(f'core {core} did not start\n{log}')
        if '__ServiceMain()' not in log or 'ProcessBootstrap failed to parse ramdisk' in log:
            raise RuntimeError(f'Phoenix did not initialize its service and ramdisk\n{log}')
        if 'DebugPanic(' in log:
            raise RuntimeError(f'The kernel panicked during boot\n{log}')
        if 'Crashed in module' in log or 'HandleProcessCrashReport(' in log:
            raise RuntimeError(f'A userspace process crashed during boot\n{log}')
        for service in args.require_service:
            if f'path={service})' not in log:
                raise RuntimeError(f'Service {service} did not register\n{log}')
        print(log)
        print('ARM64 KERNEL BOOT PASS: four cores, timer IRQ, EL0, syscalls, initramfs, no-map')


if __name__ == '__main__':
    main()
