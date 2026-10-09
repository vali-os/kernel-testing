#!/usr/bin/env python3
"""Validate ARM floating-point instructions and exact remainder arithmetic."""
import ctypes
import math
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / 'librt/libm/aarch64'


class MathTests(unittest.TestCase):
    def test_arm_instructions(self):
        with tempfile.TemporaryDirectory(prefix='vali-arm-math-') as temporary:
            executable = Path(temporary) / 'math'
            compiler = os.environ.get('ARM64_CLANG', '/usr/local/valicc/bin/clang')
            command = [compiler, '--target=aarch64-linux-gnu', '-fuse-ld=lld',
                       '-nostdlib', '-static', '-fno-builtin', '-fno-stack-protector',
                       '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       '-Wl,-e,_start', '-O2', '-fms-extensions']
            for include in ['librt/libm/include', 'librt/libc/include', 'librt/libos/include']:
                command.extend(['-I', str(ROOT / include)])
            command.append(str(ROOT / 'testing/arm64-runtime/math_test.c'))
            for source in ['math.c', 'fenv.c', 'control.c', 'rounding.c', 'remainder.c']:
                command.append(str(ARCH / source))
            command.append(str(ROOT / 'librt/libm/fabs.c'))
            subprocess.run(command + ['-o', str(executable)], check=True)
            subprocess.run(['qemu-aarch64', str(executable)], check=True, timeout=10)

    def test_exact_remainders(self):
        with tempfile.TemporaryDirectory(prefix='vali-remainder-') as temporary:
            library = Path(temporary) / 'remainder.so'
            subprocess.run(['clang', '-shared', '-fPIC', '-O2', '-fno-builtin',
                            '-D_In_=', '-D_Out_=', '-Dremquo=vali_remquo',
                            str(ARCH / 'remainder.c'), '-lm', '-o', str(library)], check=True)
            function = ctypes.CDLL(str(library)).vali_remquo
            function.argtypes = [ctypes.c_double, ctypes.c_double, ctypes.POINTER(ctypes.c_int)]
            function.restype = ctypes.c_double
            generator = random.Random(0x2711)
            cases = [(7.0, 2.0), (5.0, 2.0), (1.0, 2.0),
                     (0x1, float.fromhex('0x1p-1074')), (-0.0, 1.0),
                     (float.fromhex('0x1p-1074'), float.fromhex('0x1p-1073'))]
            for _ in range(10000):
                values = [struct.unpack('d', struct.pack('Q', generator.getrandbits(64)))[0]
                          for _ in range(2)]
                if all(math.isfinite(value) for value in values) and values[1] != 0:
                    cases.append(values)
            for left, right in cases:
                quotient = ctypes.c_int()
                actual = function(left, right, ctypes.byref(quotient))
                expected = math.remainder(left, right)
                self.assertEqual(struct.pack('d', actual), struct.pack('d', expected),
                                 (left, right, actual, expected))
                # Python integer ratios provide an independent exact quotient,
                # including exponent gaps too large for floating-point division.
                ln, ld = left.as_integer_ratio() if isinstance(left, float) else (left, 1)
                rn, rd = right.as_integer_ratio()
                numerator = abs(ln * rd)
                denominator = abs(ld * rn)
                integral, residual = divmod(numerator, denominator)
                if residual * 2 > denominator or (residual * 2 == denominator and integral & 1):
                    integral += 1
                integral &= 127
                if (left < 0) != (right < 0):
                    integral = -integral
                self.assertEqual(quotient.value, integral, (left, right))


if __name__ == '__main__':
    unittest.main()
