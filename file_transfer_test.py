"""Exercise real file transfer clients and filed forwarding on the host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-file-transfer-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'net', 'contracts/filesystem',
                    'contracts/storage', 'contracts/usbhost', 'contracts/usbhub'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    (output / 'internal').mkdir()
    (output / 'internal/_utils.h').write_text(
        (ROOT / 'librt/libc/include/internal/_utils.h').read_text())
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DMOLLENOS', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-sign-compare', '-ffunction-sections',
             '-fdata-sections', '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-mllvm', '-asan-globals=0', '-g', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include',
               'librt/libusb/include', 'modules/filesystems/common/include')]
    binary = str(output / 'test')
    command = flags + [str(ROOT / 'testing/file_transfer_test.c'), '-o', binary]
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, env=environment)
    storage_binary = str(output / 'storage')
    subprocess.run(flags + [str(ROOT / 'testing/storage_transfer_test.c'),
                            '-o', storage_binary], check=True)
    subprocess.run([storage_binary], check=True, env=environment)
    usb_binary = str(output / 'usb')
    subprocess.run(flags + [str(ROOT / 'testing/usb_transfer_client_test.c'),
                            '-o', usb_binary], check=True)
    subprocess.run([usb_binary], check=True, env=environment)
    view_flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
                  '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-fms-extensions',
                  '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
                  '-Wl,--gc-sections', '-fsanitize=address,undefined',
                  '-fno-sanitize-recover=all', '-g', '-I' + directory]
    view_flags += ['-I' + str(ROOT / path) for path in
                   ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
                    'librt/libddk/include', 'librt/libusb/include', 'librt/libgracht/include')]
    view_binary = str(output / 'view')
    subprocess.run(view_flags + [str(ROOT / 'testing/fileview_transfer_test.c'),
                                str(ROOT / 'librt/libds/list.c'),
                                '-o', view_binary], check=True)
    subprocess.run([view_binary], check=True, env=environment)
    socket_binary = str(output / 'socket')
    subprocess.run(view_flags + ['-mllvm', '-asan-globals=0',
                                str(ROOT / 'testing/socket_transfer_test.c'),
                                '-o', socket_binary], check=True)
    subprocess.run([socket_binary], check=True, env=environment)
    if args.mutation_check:
        source = (ROOT / 'librt/libos/services/file_transfer.c').read_text()
        mutant = output / 'file-transfer-mutant.c'
        mutant.write_text(source.replace('completed > length', 'completed < length'))
        subprocess.run(command + [f'-DFILE_TRANSFER_SOURCE="{mutant}"'], check=True)
        result = subprocess.run([binary], env=environment, capture_output=True)
        if result.returncode == 0:
            raise RuntimeError('Undetected result-bound mutation')
        print('Rejected file result-bound mutation')
