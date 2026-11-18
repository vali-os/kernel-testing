"""Check the MSD disk registration order on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-msd-register-') as directory:
    output = Path(directory)
    for service in ('device', 'file', 'process', 'storage', 'contracts/driver', 'contracts/storage',
                    'contracts/filesystem', 'contracts/usbhost'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DMOLLENOS', '-DVALI', '-DTESTING',
               '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=', '-DNDEBUG',
               '-D__time64_t=int64_t', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g']
    # modules/storage/msd comes first so a mutated main.c in the temporary
    # directory still finds msd.h.
    for include in ('modules/storage/msd', 'kernel/include', 'boot/include', 'librt/libc/include',
                    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
                    'librt/libgracht/include', 'librt/libusb/include'):
        command += ['-I', str(ROOT / include)]
    command += ['-I', directory]
    binary = str(output / 'test')
    command += [str(ROOT / 'testing/msd_register_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True)
    if args.mutation_check:
        source = ROOT / 'modules/storage/msd/main.c'
        for before, after, reason in (
            ('    list_append(&g_devices, &msdDevice->Header);\n    MSDDeviceRegisterStorage(msdDevice);\n',
             '    MSDDeviceRegisterStorage(msdDevice);\n    list_append(&g_devices, &msdDevice->Header);\n',
             'disk listed after it is registered with filed'),
            ('    if (usbDevice == NULL) {\n', '    if (0) {\n',
             'unreadable device description not rejected'),
        ):
            text = source.read_text()
            assert text.count(before) == 1, before
            mutant = output / 'main.c'
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-DMSD_MAIN_SOURCE="{mutant}"'], check=True)
            if subprocess.run([binary], capture_output=True).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
