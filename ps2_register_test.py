"""Test the PS/2 driver's device registration order on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-ps2-register-') as directory:
    # main.c includes the driver and input servers, and libddk's converters
    # include the client headers of every system and contract protocol.
    contracts = sorted((ROOT / 'protocols').glob('*.gr'))
    contracts += sorted((ROOT / 'protocols/contracts').glob('*.gr'))
    for contract in contracts:
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(contract), '--out', directory, '--lang-c', '--client'],
                       check=True, stdout=subprocess.DEVNULL)
    for contract in ('driver.gr', 'input.gr'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols/contracts' / contract),
                        '--out', directory, '--lang-c', '--server'],
                       check=True, stdout=subprocess.DEVNULL)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g', '-pthread', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libgracht/include', 'librt/libusb/include',
               'modules/input/ps2')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/ps2_register_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        source = (ROOT / 'modules/input/ps2/main.c').read_text()
        lock = '    usched_mtx_lock(&g_registrationLock);\n'
        unlock = '    usched_mtx_unlock(&g_registrationLock);\n'
        assert source.count(lock) == 1
        assert source.count(unlock) == 1
        mutations = {
            # The old code matched a port device before its id was stored.
            'registration without the lock': source.replace(lock, '').replace(unlock, ''),
        }
        mutant = Path(directory) / 'main-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + [f'-DPS2_MAIN_SOURCE="{mutant}"'],
                           check=True)
            try:
                result = subprocess.run([binary], capture_output=True, timeout=30)
            except subprocess.TimeoutExpired:
                result = None
            if result is not None and result.returncode == 0:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: ' + name)
