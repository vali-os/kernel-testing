"""Test the PS/2 mouse fast handler and event handler on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-ps2-mouse-') as directory:
    subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                    '--service', str(ROOT / 'protocols/contracts/input.gr'),
                    '--out', directory, '--lang-c', '--server'], check=True)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libgracht/include',
               'modules/input/ps2/devices')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/ps2_mouse_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        source = (ROOT / 'modules/input/ps2/devices/mouse.c').read_text()
        signal = '((index + 1) % breakAtBytes) == 0'
        drain = '(port->ResponseWriteIndex - index) >= bytesRequired'
        mutations = {
            # The old code signalled on the first byte of each packet.
            'signal on the first byte': source.replace(signal, '!(index % breakAtBytes)'),
            # The old check wrapped below 0 with less than a packet stored.
            'drain check wraps': source.replace(
                drain, 'index <= (port->ResponseWriteIndex - bytesRequired)'),
        }
        assert source.count(signal) == 1
        assert source.count(drain) == 1
        mutant = Path(directory) / 'mouse-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + [f'-DPS2_MOUSE_SOURCE="{mutant}"'], check=True)
            try:
                result = subprocess.run([binary], capture_output=True, timeout=30)
            except subprocess.TimeoutExpired:
                result = None
            if result is not None and result.returncode == 0:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: ' + name)
