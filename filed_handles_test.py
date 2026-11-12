"""Exercise filed resource handoff, RPC authority and cleanup with production VFS code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-handles-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'mount', 'device', 'storage', 'usb', 'contracts/filesystem', 'contracts/usbhost'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    (output / 'ioset.h').write_text((ROOT / 'librt/libc/include/ioset.h').read_text())
    (output / 'internal').mkdir()
    (output / 'internal/_tls.h').write_text('''#pragma once
#include <os/types/async.h>
struct test_tls { OSAsyncContext_t* async_context; };
struct test_tls* __tls_current(void);
''')
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DMOLLENOS', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-variable',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include')]
    binary = str(output / 'test')
    sources = [str(ROOT / p) for p in ('testing/filed_handles_test.c', 'librt/libds/hashtable.c')]
    command = flags + sources + ['-o', binary]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run([binary], check=True, env=environment)
    if args.mutation_check:
        for path, before, after, define in (
            ('services/filed/vfs/handles.c', 'options.Flags = GrantTokenFlagsSingleUse;',
             'options.Flags = 0;', 'FILED_HANDLES_SOURCE'),
            ('services/filed/vfs/handles.c', 'options.TargetGlobalHandleID = (uuid_t)caller->principal;',
             'options.TargetGlobalHandleID = 999;', 'FILED_HANDLES_SOURCE'),
            ('services/filed/api.c', 'direction == SYS_TRANSFER_DIRECTION_READ ? __FILE_READ_ACCESS : __FILE_WRITE_ACCESS',
             '0', 'FILED_API_SOURCE'),
            ('services/filed/filesystem.c', 'VFSDestroy(fileSystem->VFS);\n        fileSystem->VFS = NULL;',
             'fileSystem->VFS = NULL;', 'FILED_FILESYSTEM_SOURCE'),
        ):
            text = (ROOT / path).read_text()
            assert before in text
            mutant = output / Path(path).name
            mutant.write_text(text.replace(before, after).replace('#include "private.h"',
                '#include "' + str(ROOT / 'services/filed/vfs/private.h') + '"'))
            subprocess.run(command + [f'-D{define}="{mutant}"'], check=True)
            result = subprocess.run([binary], capture_output=True, env=environment)
            if result.returncode == 0:
                raise RuntimeError('Mutation survived: ' + before)
            print('Rejected mutation: ' + before)
