#!/usr/bin/env python3
"""Build actual early parser/platform sources and test physical ownership policy."""
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOADER = ROOT / 'boot/rpi/loader'


def fixture(board=4, reservation='static', initrd=True, malformed=False, memory=True,
            compatible=True, reverse_initrd=False, duplicate_chosen=False,
            initrd_start=0x4000000, initrd_end=0x4012345, stray=False,
            dynamic_size=0x100, alignment=None, allocation=None, no_map=False):
    strings = bytearray()
    structure = bytearray()

    def word(value):
        structure.extend(struct.pack('>I', value))

    def data(value):
        structure.extend(value)
        structure.extend(bytes(-len(structure) % 4))

    def begin(name):
        word(1)
        data(name.encode() + b'\0')

    def prop(name, value):
        offset = len(strings)
        strings.extend(name.encode() + b'\0')
        word(3)
        word(len(value))
        word(offset)
        data(value)

    def cells(*values):
        return struct.pack('>' + 'I' * len(values), *values)

    begin('')
    if compatible:
        prop('compatible', f'raspberrypi,{board}-model-b\0brcm,bcm{2711 if board == 4 else 2712}\0'.encode())
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(2))
    # Put reservations before RAM to exercise order-independent exclusions.
    begin('reserved-memory')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(2))
    prop('ranges', b'')
    begin('pool@2000000')
    if reservation == 'dynamic':
        prop('size', cells(0, dynamic_size))
        if alignment is not None:
            prop('alignment', cells(0, alignment))
        if allocation is not None:
            prop('alloc-ranges', cells(0, allocation[0], 0, allocation[1]))
    else:
        prop('reg', cells(0, 0x2000000, 0, 0x100))
    if reservation == 'no-map' or no_map:
        prop('no-map', b'')
    word(2)
    word(2)
    if memory:
        begin('memory@0')
        prop('reg', cells(0, 0, 0, 0x40000000))
        prop('device_type', b'memory\0')
        if malformed:
            prop('status', b'okay')
        word(2)
    begin('chosen')
    if initrd is not None:
        endpoints = [('linux,initrd-start', initrd_start)]
        if initrd:
            endpoints.append(('linux,initrd-end', initrd_end))
        if reverse_initrd:
            endpoints.reverse()
        for name, address in endpoints:
            prop(name, cells(0, address))
    word(2)
    if duplicate_chosen:
        begin('chosen')
        word(2)
    if stray:
        begin('unrelated-device')
        # Same property names under another binding must not affect boot state.
        prop('compatible', b'other,board\0')
        prop('linux,initrd-start', cells(0, 7))
        prop('cpu-release-addr', cells(0, 3))
        word(2)
    begin('cpus')
    begin('cpu@1')
    prop('cpu-release-addr', cells(0, 0x1100088))
    word(2)
    word(2)
    word(2)
    word(9)
    reservations = struct.pack('>QQQQ', 0x300123, 4, 0, 0)
    offset = 40 + len(reservations)
    string_offset = offset + len(structure)
    header = struct.pack('>10I', 0xd00dfeed, string_offset + len(strings), offset,
                         string_offset, 40, 17, 16, 0, len(strings), len(structure))
    return header + reservations + structure + strings


class PlatformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-platform-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.binary = cls.directory / 'test'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                   '-D_GNU_SOURCE', '-g', '-fsanitize=address,undefined', '-no-pie',
                   '-Wl,--wrap=FdtParseStructure',
                   '-Wl,--defsym,__rpi_loader_start=0x80000',
                   '-Wl,--defsym,__rpi_loader_end=0xa0000']
        for include in ['boot/include', 'kernel/include', 'librt/libfdt/include', 
                        'librt/libos/include', 'librt/libc/include',
                        'boot/rpi/loader']:
            command.extend(['-I', str(ROOT / include)])
        command.extend(str(ROOT / source) for source in [
            'testing/rpi-loader/platform_test.c', 'librt/libfdt/parser.c',
            'boot/rpi/loader/devicetree.c', 'boot/rpi/loader/reservations.c',
            'boot/rpi/loader/platform.c'])
        subprocess.run(command + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_blob(self, data, board=4, expected=0):
        path = self.directory / 'input.dtb'
        path.write_bytes(data)
        subprocess.run([str(self.binary), str(path), str(board), str(expected)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_pi4_ownership(self):
        self.run_blob(fixture())

    def test_pi5_ownership(self):
        self.run_blob(fixture(board=5), board=5)

    def test_board_mismatch(self):
        self.run_blob(fixture(), board=5, expected=3)

    def test_dynamic_reservation(self):
        self.run_blob(fixture(reservation='dynamic'))

    def test_dynamic_alignment_and_ranges(self):
        self.run_blob(fixture(reservation='dynamic', dynamic_size=0x18000,
                              alignment=0x20000, allocation=(0x20000000, 0x10000000)))

    def test_dynamic_no_map(self):
        self.run_blob(fixture(reservation='dynamic', no_map=True))

    def test_dynamic_bad_alignment(self):
        self.run_blob(fixture(reservation='dynamic', alignment=3), expected=3)

    def test_dynamic_empty_size(self):
        self.run_blob(fixture(reservation='dynamic', dynamic_size=0), expected=3)

    def test_dynamic_unsatisfiable_range(self):
        self.run_blob(fixture(reservation='dynamic', allocation=(0x70000000, 0x1000)), expected=4)

    def test_no_map_reservation(self):
        self.run_blob(fixture(reservation='no-map'))

    def test_incomplete_initrd(self):
        self.run_blob(fixture(initrd=False), expected=3)

    def test_unterminated_status(self):
        self.run_blob(fixture(malformed=True), expected=3)

    def test_missing_ram(self):
        self.run_blob(fixture(memory=False), expected=4)

    def test_missing_compatible(self):
        self.run_blob(fixture(compatible=False), expected=3)

    def test_initrd_property_order(self):
        self.run_blob(fixture(reverse_initrd=True))

    def test_duplicate_chosen(self):
        self.run_blob(fixture(duplicate_chosen=True), expected=3)

    def test_initrd_overlaps_wrapper(self):
        self.run_blob(fixture(initrd_start=0x80000), expected=3)

    def test_reversed_initrd(self):
        self.run_blob(fixture(initrd_end=0x3000000), expected=3)

    def test_no_initrd(self):
        self.run_blob(fixture(initrd=None))

    def test_property_scope(self):
        self.run_blob(fixture(stray=True))

    def test_nonzero_property_padding(self):
        data = bytearray(fixture())
        # libfdt can leave previous contents after the seven-byte string value.
        offset = data.index(b'memory\0', data.index(b'memory@0\0')) + 7
        data[offset] = 0x6d
        self.run_blob(data)

    def test_unterminated_property_name(self):
        data = bytearray(fixture())
        data[-1] = 0x61
        self.run_blob(data, expected=3)

    def test_bad_block_bounds(self):
        data = bytearray(fixture())
        struct.pack_into('>I', data, 36, 0xffffffff)
        self.run_blob(data, expected=3)

    def test_reservation_terminator_missing(self):
        data = bytearray(fixture())
        data[56:72] = struct.pack('>QQ', 0x4000, 0x100)
        self.run_blob(data, expected=3)

    def test_firmware_trees_require_memory_fixups(self):
        for board, name in [(4, 'bcm2711-rpi-4-b.dtb'), (5, 'bcm2712-rpi-5-b.dtb')]:
            self.run_blob((ROOT / 'boot/rpi/firmware' / name).read_bytes(), board, 3)


if __name__ == '__main__':
    unittest.main()
