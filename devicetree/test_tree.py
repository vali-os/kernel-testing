#!/usr/bin/env python3
"""Host validation of retained DTBs, resource translation, and allocation failure."""
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fixture(duplicate=False, extended=False):
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
    prop('interrupt-parent', cells(1))
    begin('aliases')
    prop('serial0', b'/soc/serial@1000\0')
    end()
    begin('soc')
    prop('#address-cells', cells(1))
    prop('#size-cells', cells(1))
    prop('ranges', cells(0, 0x10, 0x7d000000, 0x10000))
    begin('serial@1000')
    prop('compatible', b'brcm,bcm2712-uart\0arm,pl011\0')
    prop('reg', cells(0x1000, 0x1000))
    prop('clocks', cells(2, 7))
    if extended:
        prop('interrupts-extended', cells(1, 0, 91, 4))
    else:
        prop('interrupts', cells(0, 91, 4))
    end()
    end()
    begin('gic')
    prop('phandle', cells(1))
    prop('#interrupt-cells', cells(3))
    prop('interrupt-controller', b'')
    end()
    begin('clock')
    prop('phandle', cells(1 if duplicate else 2))
    prop('#clock-cells', cells(1))
    end()
    begin('disabled')
    prop('status', b'disabled\0')
    begin('device@0')
    end()
    end()
    end()
    data.extend(cells(9))
    strings_offset = 56 + len(data)
    header = cells(0xd00dfeed, strings_offset + len(strings), 56, strings_offset,
                   40, 17, 16, 0, len(strings), len(data))
    return header + bytes(16) + data + strings


class TreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-dtb-tree-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.binary = cls.directory / 'tree'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra',
                   '-Werror', '-DKERNELAPI=', '-DKERNELABI=', '-fsanitize=address,undefined']
        for include in ['kernel/include', 'librt/libfdt/include', 'librt/libos/include', 'librt/libc/include']:
            command.extend(['-I', str(ROOT / include)])
        for source in ['testing/devicetree/tree_test.c', 'librt/libfdt/parser.c',
                       'kernel/devicetree/tree.c', 'kernel/devicetree/resources.c']:
            command.append(str(ROOT / source))
        subprocess.run(command + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_tree(self, path, status=0):
        subprocess.run([str(self.binary), str(path), str(status)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_forward_references_aliases_and_high_address_translation(self):
        for extended in [False, True]:
            path = self.directory / 'fixture.dtb'
            path.write_bytes(fixture(extended=extended))
            self.run_tree(path)

    def test_duplicate_phandle_rejected(self):
        path = self.directory / 'duplicate.dtb'
        path.write_bytes(fixture(duplicate=True))
        # OS_EINVALPARAMS is 4 in Vali's public status ABI.
        self.run_tree(path, 4)

    def test_real_pi_trees(self):
        for name in ['bcm2711-rpi-4-b.dtb', 'bcm2712-rpi-5-b.dtb']:
            self.run_tree(ROOT / 'boot/rpi/firmware' / name)


if __name__ == '__main__':
    unittest.main()
