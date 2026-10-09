#!/usr/bin/env python3
"""Validate the native contract's descriptors and ownership without ARM execution."""
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-rpi-contract-')
        cls.binary = pathlib.Path(cls.temp.name) / 'test'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                   '-g', '-fsanitize=address,undefined', '-no-pie',
                   '-Wl,--defsym,__rpi_loader_start=0x80000',
                   '-Wl,--defsym,__rpi_loader_end=0xa0000',
                   '-Wl,--defsym,__rpi_boot_stack_bottom=0x88000',
                   '-Wl,--defsym,__rpi_boot_stack_top=0x98000']
        for include in ['boot/include', 'librt/libos/include', 'librt/libc/include',
                        'boot/rpi/loader']:
            command.extend(['-I', str(ROOT / include)])
        command.extend(str(ROOT / source) for source in [
            'testing/rpi-loader/contract_test.c',
            'boot/rpi/loader/contract.c'])
        subprocess.run(command + ['-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_cases(self, cases):
        for scenario in cases:
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binary), str(scenario)], check=True,
                               env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

    def test_bringup_contract_with_and_without_external_payload(self):
        self.run_cases([0, 24, 25])

    def test_memory_map_invariants(self):
        self.run_cases([1, 2, 3, 4, 13, 14, 15, 26])

    def test_reserved_storage_and_lifetimes(self):
        self.run_cases([5, 6, 11, 12, 16, 18, 22, 27])

    def test_descriptor_ranges_and_overlap(self):
        self.run_cases([7, 8, 9, 10, 17, 19, 23, 28])

    def test_external_resources_require_their_own_loader(self):
        self.run_cases([20, 21])


if __name__ == '__main__':
    unittest.main()
