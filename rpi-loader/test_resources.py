#!/usr/bin/env python3
"""Validate bundle framing, Phoenix staging, and exact ramdisk handoff."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

from test_pe import fixture

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/rpi'))
from bundle import bundle


class ResourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-rpi-resources-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.binary = cls.directory / 'resources'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra',
                   '-Werror', '-fsanitize=address,undefined', '-no-pie']
        for include in ['boot/include', 'boot/rpi/loader', 'kernel/include', 'librt/libfdt/include',
                        'kernel/devicetree', 'librt/libos/include', 'librt/libc/include']:
            command.extend(['-I', str(ROOT / include)])
        for source in ['testing/rpi-loader/resources_test.c', 'boot/rpi/loader/resources.c',
                       'boot/rpi/loader/pe.c', 'boot/rpi/loader/devicetree.c',
                       'librt/libfdt/parser.c']:
            command.append(str(ROOT / source))
        subprocess.run(command + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_bundle(self, data, expected):
        path = self.directory / 'bundle.bin'
        path.write_bytes(data)
        subprocess.run([str(self.binary), str(path), str(expected)], check=True,
                       env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_staging_preserves_virtual_pointers_and_ramdisk(self):
        self.run_bundle(bundle(fixture(), b'Vali ramdisk!'), 0)

    def test_corrupt_header(self):
        for offset, value in [(0, 0), (8, 2), (16, 64), (24, 0), (32, 0),
                              (32, 2**64 - 1), (40, 64), (40, 4097), (48, 0),
                              (48, 2**64 - 1), (56, 1)]:
            with self.subTest(offset=offset, value=value):
                data = bytearray(bundle(fixture(), b'Vali ramdisk!'))
                struct.pack_into('<Q', data, offset, value)
                self.run_bundle(data, 1)

    def test_truncated_firmware_load(self):
        data = bundle(fixture(), b'Vali ramdisk!')
        for length in [1, 63, 64, 1024, len(data) - 1]:
            self.run_bundle(data[:length], 1)

    def test_invalid_pe(self):
        data = bytearray(bundle(fixture(), b'Vali ramdisk!'))
        data[64] = 0
        self.run_bundle(data, 1)


if __name__ == '__main__':
    unittest.main()
