#!/usr/bin/env python3
"""BCM2711 firmware, receiver model and production interrupt-core tests."""
import argparse
import pathlib
import struct
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fixture(mode):
    strings, data = bytearray(), bytearray()

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
    prop('interrupt-parent', cells(1))
    begin('gic')
    prop('compatible', b'arm,gic-400\0')
    prop('phandle', cells(1))
    prop('#interrupt-cells', cells(3))
    prop('interrupt-controller', b'')
    end()
    begin('scb')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(1))
    prop('ranges', cells(0, 0x7c000000, 0, 0xfc000000, 0x03800000))
    if mode == 'disabled-bus':
        prop('status', b'disabled\0')
    for index in range(2 if mode == 'duplicate-host' else 1):
        begin('pcie@' + str(index))
        prop('compatible', b'brcm,bcm2711-pcie\0')
        prop('phandle', cells(2 + index))
        prop('reg', cells(0, 0x7d500001 if mode == 'unaligned' else 0x7d500000,
                          0x100 if mode == 'short-reg' else 0x9310))
        if mode == 'disabled-host':
            prop('status', b'disabled\0')
        if mode != 'missing-parent':
            prop('msi-parent', cells(1 if mode == 'external-parent' else 2 + index) +
                 (cells(0) if mode == 'parent-args' else b''))
        if mode != 'missing-marker':
            prop('msi-controller', cells(0) if mode == 'bad-marker' else b'')
        prop('#msi-cells', b'' if mode == 'bad-cells' else cells(1 if mode == 'msi-cells' else 0))
        names = b'pcie\0msi\0'
        if mode == 'duplicate-name':
            names = b'msi\0msi\0'
        if mode == 'unterminated-name':
            names = b'pcie\0msi'
        if mode == 'reordered':
            names = b'msi\0pcie\0'
        if mode != 'missing-name':
            prop('interrupt-names', names)
        irq = cells(1 if mode == 'ppi' else 0, 988 if mode == 'overflow' else 148,
                    1 if mode == 'edge' else 4)
        normal = cells(0, 147, 4)
        if mode == 'extended':
            prop('interrupts-extended', cells(1) + normal + cells(1) + irq)
        elif mode == 'reordered':
            prop('interrupts', irq + normal)
        else:
            interrupts = normal + irq
            prop('interrupts', interrupts[:-4] if mode == 'short-interrupts' else interrupts)
        end()
    end()
    end()
    data.extend(cells(9))
    offset = 56 + len(data)
    return cells(0xd00dfeed, offset + len(strings), 56, offset, 40, 17, 16, 0,
                 len(strings), len(data)) + bytes(16) + data + strings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=str(pathlib.Path.home() / 'ValiCC/bin/clang.exe'))
    parser.add_argument('--target', default='aarch64-pc-windows-msvc')
    parser.add_argument('--asan', action='store_true')
    args = parser.parse_args()
    output = ROOT / 'build/interrupt-tests/bcm2711-msi'
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'bcm2711-msi-test.exe'
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
    for source in ['testing/arm64-kernel/bcm2711_msi_test.c', 'testing/interrupt_host_windows.c',
                   'kernel/arch/aarch64/rpi/bcm2711-msi-firmware.c', 'kernel/devicetree/tree.c',
                   'kernel/devicetree/resources.c', 'librt/libfdt/parser.c']:
        command.append(str(ROOT / source))
    subprocess.run(command + ['-o', str(binary)], check=True)
    cases = {mode: (0, 1) for mode in ['valid', 'reordered', 'extended']}
    for mode in ['disabled-bus', 'disabled-host', 'missing-parent', 'external-parent', 'msi-cells']:
        cases[mode] = (0, 0)
    for mode in ['duplicate-host', 'unaligned', 'short-reg', 'parent-args', 'missing-marker',
                 'bad-marker', 'bad-cells', 'duplicate-name', 'unterminated-name',
                 'missing-name', 'ppi', 'overflow', 'edge', 'short-interrupts']:
        cases[mode] = (4, 0)
    for mode, (status, domains) in cases.items():
        path = output / (mode + '.dtb')
        path.write_bytes(fixture(mode))
        subprocess.run([str(binary), str(path), str(status), str(domains)], check=True)
    for name, domains in [('bcm2711-rpi-4-b.dtb', 1), ('bcm2712-rpi-5-b.dtb', 0)]:
        subprocess.run([str(binary), str(ROOT / 'boot/rpi/firmware' / name), '0', str(domains)], check=True)
    print(f'BCM2711 MSI: {len(cases)} synthetic cases and both bundled Pi trees passed')


if __name__ == '__main__':
    main()
