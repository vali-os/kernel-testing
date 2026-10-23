#!/usr/bin/env python3
"""BCM2712 firmware, MMIO initialization and real interrupt-core host tests."""
import argparse
import pathlib
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fixture(mode):
    strings = bytearray()
    data = bytearray()

    def cells(*values):
        return struct.pack('>' + 'I' * len(values), *values)

    def begin(name):
        data.extend(cells(1) + name.encode() + b'\0')
        data.extend(bytes(-len(data) % 4))

    def end():
        data.extend(cells(2))

    def prop(name, value):
        data.extend(cells(3, len(value), len(strings)) + value)
        data.extend(bytes(-len(data) % 4))
        strings.extend(name.encode() + b'\0')

    begin('')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(2))
    begin('gic')
    prop('compatible', b'arm,gic-400\0')
    prop('phandle', cells(1))
    prop('#interrupt-cells', cells(3))
    prop('interrupt-controller', b'')
    end()
    begin('axi')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(2))
    prop('ranges', cells(0, 0, 0x10, 0, 1, 0))
    if mode == 'disabled-parent':
        prop('status', b'disabled\0')
    for i in range(2):
        begin(f'msi@{0x130000 + i * 0x1000:x}')
        prop('compatible', b'brcm,bcm2712-mip\0')
        prop('phandle', cells(2 + i))
        if mode != 'missing-marker':
            prop('msi-controller', cells(0) if mode == 'bad-marker' else b'')
        prop('#msi-cells', cells(1 if mode == 'msi-cells' else 0))
        if mode == 'disabled-mip':
            prop('status', b'disabled\0')
        register = 0x130000 if mode == 'alias-registers' else 0x130000 + i * 0x1000
        registers = cells(0, register, 0, 0x20 if mode == 'short-reg' else 0xc0)
        doorbell = 0xffffe000 + i * 0x1000
        registers += cells(0xff, doorbell + (1 if mode == 'unaligned-message' else 0), 0, 0x1000)
        if mode == 'extra-reg':
            registers += cells(0, 0, 0, 4)
        prop('reg', registers)
        first = 64 if mode == 'overlap-spi' else 64 + i * 64
        if mode == 'gic-overflow':
            first = 980
        count = 64 if mode == 'offset-overflow' else 8
        if mode == 'zero-count':
            count = 0
        ranges = cells(1, 1 if mode == 'ppi' else 0, first, 4 if mode == 'level' else 1, count)
        prop('msi-ranges', ranges[:-4] if mode == 'short-ranges' else ranges)
        prop('brcm,msi-offset', b'' if mode == 'bad-offset' else cells(8))
        end()
    for i in range(3 if mode == 'shared-mip' else 2):
        bank = i % 2
        begin(f'pcie@{0x200000 + i * 0x10000:x}')
        prop('compatible', b'brcm,bcm2712-pcie\0')
        prop('#address-cells', cells(3))
        prop('#size-cells', cells(2))
        if mode == 'disabled-host':
            prop('status', b'disabled\0')
        if mode != 'missing-parent':
            parent = cells(99 if mode == 'missing-provider' else 2 + bank)
            prop('msi-parent', parent + cells(0) if mode == 'parent-args' else parent)
        destination = 0x130000 + bank * 0x1000
        if mode == 'wrong-doorbell':
            destination += 4
        ranges = cells(0x03000000, 0xff, 0xffffe000 + bank * 0x1000,
                       0, destination, 0, 0x1000)
        if mode == 'duplicate-window':
            ranges *= 2
        if mode != 'missing-window':
            prop('dma-ranges', ranges[:-4] if mode == 'short-window' else ranges)
        end()
    end()
    end()
    data.extend(cells(9))
    strings_offset = 56 + len(data)
    header = cells(0xd00dfeed, strings_offset + len(strings), 56, strings_offset,
                   40, 17, 16, 0, len(strings), len(data))
    return header + bytes(16) + data + strings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(pathlib.Path.home() / 'ValiCC/bin/clang.exe'))
    parser.add_argument('--target', default='aarch64-pc-windows-msvc')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/interrupt-tests/mip'
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'mip-test.exe'
    command = [args.compiler, '--target=' + args.target, '-fuse-ld=lld', '-std=c11',
               '-fms-extensions', '-DMOLLENOS', '-DINTERRUPT_WINDOWS_HOST_TEST',
               '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
               '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
               '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions']
    if args.asan:
        command += ['-fsanitize=address', '-g']
    for include in ['kernel/include', 'boot/include', 'librt/libc/include', 'librt/libos/include',
                    'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include',
                    'librt/libacpi/source/include', 'librt/libfdt/include']:
        command += ['-I', str(ROOT / include)]
    for source in ['testing/arm64-kernel/mip_test.c', 'testing/interrupt_host_windows.c',
                   'kernel/arch/aarch64/rpi/mip-firmware.c',
                   'kernel/devicetree/tree.c', 'kernel/devicetree/resources.c', 'librt/libfdt/parser.c']:
        command.append(str(ROOT / source))
    subprocess.run(command + ['-o', str(binary)], check=True)
    cases = {'valid': (0, 2), 'shared-mip': (0, 3)}
    for mode in ['disabled-parent', 'disabled-mip', 'disabled-host', 'missing-parent', 'msi-cells']:
        cases[mode] = (0, 0)
    for mode in ['missing-marker', 'bad-marker', 'alias-registers', 'short-reg', 'unaligned-message',
                 'extra-reg', 'overlap-spi', 'gic-overflow', 'offset-overflow', 'zero-count',
                 'ppi', 'level', 'short-ranges', 'bad-offset', 'missing-provider', 'parent-args',
                 'wrong-doorbell', 'duplicate-window', 'missing-window', 'short-window']:
        cases[mode] = (4, 0)
    for mode, (status, domains) in cases.items():
        path = output / (mode + '.dtb')
        path.write_bytes(fixture(mode))
        subprocess.run([str(binary), str(path), str(status), str(domains)], check=True)
    for name, domains in [('bcm2711-rpi-4-b.dtb', 0), ('bcm2712-rpi-5-b.dtb', 1)]:
        subprocess.run([str(binary), str(ROOT / 'boot/rpi/firmware' / name), '0', str(domains)], check=True)
    subprocess.run([str(binary), str(ROOT / 'boot/rpi/firmware/bcm2712-rpi-5-b.dtb'),
                    '0', '2', '--external'], check=True)
    print(f'MIP: {len(cases)} synthetic cases, both bundled Pi trees and Pi 5 external PCIe passed')


if __name__ == '__main__':
    main()
