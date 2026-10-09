#!/usr/bin/env python3
"""Exercise the native PE loader with malformed files and real ARM64 linker output."""
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def fixture():
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 64)
    struct.pack_into('<IHH', data, 64, 0x4550, 0xaa64, 3)
    struct.pack_into('<HH', data, 84, 240, 2)
    struct.pack_into('<H', data, 88, 0x20b)
    struct.pack_into('<I', data, 104, 0x1000)
    struct.pack_into('<QII', data, 112, 0x48000000, 4096, 512)
    struct.pack_into('<II', data, 144, 0x5000, 512)
    struct.pack_into('<I', data, 196, 16)
    struct.pack_into('<II', data, 240, 0x4000, 12)
    for i, (name, size, rva, raw, flags) in enumerate([
        (b'.text', 4, 0x1000, 512, 0x60000020),
        (b'.data', 0x1800, 0x2000, 1024, 0xc0000040),
        (b'.reloc', 12, 0x4000, 1536, 0x42000040),
    ]):
        offset = 328 + i * 40
        data[offset:offset + len(name)] = name
        struct.pack_into('<IIII', data, offset + 8, size, rva, 512, raw)
        struct.pack_into('<I', data, offset + 36, flags)
    struct.pack_into('<I', data, 512, 0xd503205f)  # WFE
    struct.pack_into('<Q', data, 1024, 0x48001000)
    struct.pack_into('<IIHH', data, 1536, 0x2000, 12, 0xa000, 0)
    return data


class PeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-rpi-pe-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.binary = cls.directory / 'test'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                   '-g', '-fsanitize=address,undefined', '-no-pie']
        for include in ['boot/include', 'kernel/include', 'librt/libfdt/include', 
                        'librt/libos/include', 'librt/libc/include',
                        'boot/rpi/loader']:
            command.extend(['-I', str(ROOT / include)])
        command.extend(str(ROOT / source) for source in [
            'testing/rpi-loader/pe_test.c', 'librt/libfdt/parser.c',
            'boot/rpi/loader/devicetree.c',
            'boot/rpi/loader/pe.c'])
        subprocess.run(command + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_image(self, data=None, expected=0, mode='fallback'):
        path = self.directory / 'input.exe'
        path.write_bytes(fixture() if data is None else data)
        subprocess.run([str(self.binary), str(path), str(expected), mode], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_relocation_zero_fill_and_reservation(self):
        self.run_image()

    def test_preferred_address(self):
        self.run_image(mode='preferred')
        self.run_image(mode='lower')

    def test_fixed_image(self):
        data = fixture()
        struct.pack_into('<II', data, 240, 0, 0)
        struct.pack_into('<H', data, 86, 3)
        self.run_image(data, mode='preferred')
        self.run_image(data, expected=4)

    def test_placement_ownership(self):
        for mode in ['reserved', 'small', 'source', 'full']:
            with self.subTest(mode=mode):
                self.run_image(expected=4, mode=mode)
        self.run_image(mode='fragmented')

    def test_truncated_headers_and_raw_data(self):
        data = fixture()
        for length in [0, 1, 63, 64, 87, 100, 327, 447, 511, 1024, 2047]:
            with self.subTest(length=length):
                self.run_image(data[:length], expected=1)

    def test_invalid_layout(self):
        for offset, fmt, value in [
            (0, 'H', 0), (60, 'I', 0xfffffff0), (64, 'I', 0), (70, 'H', 0),
            (104, 'I', 0x2000), (104, 'I', 0x1002), (104, 'I', 0x1800),
            (112, 'Q', 0xfffffffffffff000), (144, 'I', 0xfffff000),
            (144, 'I', 0x4000), (148, 'I', 0), (148, 'I', 0x10000),
            (340, 'I', 0), (344, 'I', 0x10000), (348, 'I', 0xfffffe00),
            (380, 'I', 0x1000), (240, 'I', 0x4fff), (244, 'I', 0),
            (1536, 'I', 0xfffff000), (1540, 'I', 0), (1540, 'I', 10),
            (1540, 'I', 16),
        ]:
            with self.subTest(offset=offset, value=value):
                data = fixture()
                struct.pack_into('<' + fmt, data, offset, value)
                self.run_image(data, expected=1)

    def test_unsupported_profile(self):
        for offset, fmt, value in [
            (68, 'H', 0x8664), (84, 'H', 112), (86, 'H', 0x2002),
            (88, 'H', 0x10b), (120, 'I', 8192), (124, 'I', 4096),
            (196, 'I', 17), (1544, 'H', 0x3000),
        ]:
            with self.subTest(offset=offset, value=value):
                data = fixture()
                struct.pack_into('<' + fmt, data, offset, value)
                self.run_image(data, expected=2)
        for index in [1, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15]:
            with self.subTest(directory=index):
                data = fixture()
                struct.pack_into('<II', data, 200 + index * 8, 0x2000, 8)
                self.run_image(data, expected=2)

    def test_relocation_metadata_must_be_file_backed(self):
        data = fixture()
        struct.pack_into('<II', data, 240, 0x2200, 12)
        self.run_image(data, expected=1)

    def test_relocation_target_cannot_be_header_or_gap(self):
        for page, fixup in [(0, 0xa008), (0x1000, 0xa008), (0x3000, 0xaffc)]:
            data = fixture()
            struct.pack_into('<I', data, 1536, page)
            struct.pack_into('<H', data, 1544, fixup)
            self.run_image(data, expected=1)

    def test_real_linked_arm64_image(self):
        linker = shutil.which('lld-link') or '/usr/local/valicc/bin/lld-link'
        source = self.directory / 'kernel.c'
        obj = self.directory / 'kernel.obj'
        image = self.directory / 'kernel.exe'
        source.write_text('''_Noreturn void kentry(void);
void (*volatile reference)(void) = kentry;
volatile unsigned char zero[8192];
_Noreturn void kentry(void) { for (;;) { __asm__("wfe"); } }
''')
        subprocess.run(['clang', '--target=aarch64-pc-windows-msvc', '-ffreestanding',
                        '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([linker, '/entry:kentry', '/subsystem:native', '/nodefaultlib',
                        '/base:0x48000000', '/opt:noref', str(obj), '/out:' + str(image)], check=True)
        self.run_image(image.read_bytes(), mode='linked')


if __name__ == '__main__':
    unittest.main()
