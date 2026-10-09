#!/usr/bin/env python3
"""Exercise the packager against the actual freestanding trailer decoder."""

import ctypes
import importlib.util
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "boot/rpi/loader"
spec = importlib.util.spec_from_file_location("rpi_pack", ROOT / "tools/rpi/pack.py")
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class View(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in ("offset", "length", "total")]


def fixture():
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 64)
    data[64:68] = b"PE\0\0"
    struct.pack_into("<HH", data, 68, 0xAA64, 1)
    struct.pack_into("<HH", data, 84, 112, 2)
    struct.pack_into("<H", data, 88, 0x20B)
    struct.pack_into("<II", data, 216, 4, 512)
    data[512:516] = b"PE!!"
    return bytes(data)


class PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="vali-rpi-payload-")
        library = pathlib.Path(cls.temp.name) / "payload.so"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared",
                        "-fPIC", "-I", str(ROOT / "boot/include"),
                        "-I", str(ROOT / "librt/libos/include"),
                        "-I", str(ROOT / "librt/libc/include"),
                        str(SOURCE / "header.c"), "-o", str(library)], check=True)
        cls.library = ctypes.CDLL(str(library))
        cls.decode = cls.library.RpiParseHeader
        cls.decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint64,
                               ctypes.c_uint64, ctypes.POINTER(View)]
        cls.decode.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.loader = bytes(4096 - 64) + packager.HEADER.pack(
            packager.MAGIC, 1, 64, 4096, 4096, 0, 0, 0, 0)
        self.image = packager.pack(self.loader, fixture())

    def result(self, header, bound=None):
        view = View()
        buffer = ctypes.create_string_buffer(header)
        code = self.decode(buffer, len(header), 4096,
                           len(self.image) if bound is None else bound, ctypes.byref(view))
        return code, view

    def test_round_trip_preserves_pe(self):
        code, view = self.result(self.image[4032:4096])
        self.assertEqual(code, 0)
        self.assertEqual(self.image[view.offset:view.offset + view.length], fixture())
        self.assertEqual(view.total, len(self.image))

    def test_every_header_field_is_checked(self):
        for offset in (0, 8, 12, 16, 24, 32, 40, 48, 56):
            with self.subTest(offset=offset):
                header = bytearray(self.image[4032:4096])
                header[offset] ^= 1
                self.assertNotEqual(self.result(bytes(header))[0], 0)

    def test_truncation_and_integer_overflow(self):
        header = self.image[4032:4096]
        self.assertNotEqual(self.result(header, len(self.image) - 1)[0], 0)
        self.assertNotEqual(self.result(header[:-1])[0], 0)
        invalid = bytearray(header)
        struct.pack_into("<QQ", invalid, 32, 0xFFFFFFFFFFFFFFFF, 4095)
        self.assertNotEqual(self.result(bytes(invalid))[0], 0)

    def test_unpackaged_loader_is_not_bootable(self):
        self.assertNotEqual(self.result(self.loader[-64:])[0], 0)

    def test_bad_pe_inputs(self):
        for offset, value in ((0, 0), (68, 0), (88, 0)):
            bad = bytearray(fixture())
            bad[offset] = value
            with self.assertRaises(ValueError):
                packager.pack(self.loader, bytes(bad))
        bad = bytearray(fixture())
        struct.pack_into("<I", bad, 220, len(bad) - 1)
        with self.assertRaises(ValueError):
            packager.pack(self.loader, bytes(bad))

    def test_already_patched_loader_rejected(self):
        with self.assertRaises(ValueError):
            packager.pack(self.image[:4096], fixture())


if __name__ == "__main__":
    unittest.main()
