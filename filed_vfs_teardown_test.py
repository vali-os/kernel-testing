"""Unregister a disk in filed while a request still works on its filesystem,
with the real VFS code and blocking locks, on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILED = 'services/filed/'
# Production sources compiled as their own translation units. filesystem.c and
# storage/utils.c are included by the test itself.
SOURCES = [FILED + path for path in (
    'vfs/vfs.c', 'vfs/utils.c', 'vfs/bind.c', 'vfs/mount.c', 'vfs/memfs.c',
    'vfs/handlers/mkdir.c', 'vfs/handlers/open.c', 'vfs/handlers/close.c',
    'vfs/handlers/unlink.c', 'vfs/handlers/handle_stat.c')]
# The libds sources are not under test. They are built once, without -Werror.
LIBRARY_SOURCES = ['librt/libds/' + path for path in (
    'hashtable.c', 'guid.c',
    'mstring/common/mstr_builder.c', 'mstring/core/merr.c', 'mstring/core/mstr_at.c',
    'mstring/core/mstr_clone.c', 'mstring/core/mstr_cmp.c', 'mstring/core/mstr_delete.c',
    'mstring/core/mstr_find.c', 'mstring/core/mstr_hash.c', 'mstring/core/mstr_join.c',
    'mstring/core/mstr_new.c', 'mstring/core/mstr_replace.c', 'mstring/core/mstr_split.c',
    'mstring/core/mstr_substr.c', 'mstring/unicode/mstr_conv.c',
    'mstring/unicode/mstr_digits.c', 'mstring/unicode/mstr_props.c',
    'mstring/unicode/mstr_unicode.c', 'mstring/path/mstr_path_basename.c',
    'mstring/path/mstr_path_change_extension.c', 'mstring/path/mstr_path_dirname.c',
    'mstring/path/mstr_path_join.c', 'mstring/path/mstr_path_new_u8.c',
    'mstring/path/mstr_path_tokens.c')]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-vfs-teardown-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'contracts/filesystem',
                    'contracts/usbhost', 'contracts/storage'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-unused-but-set-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-implicit-fallthrough', '-Wno-type-limits',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + directory]
    # memfs packs directory entries back to back (__ReadDirectoryEntry), so the
    # second entry is not 8-byte aligned. That is outside this test.
    flags += ['-fno-sanitize=alignment']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include',
               'librt/libgracht/include')]
    binary = str(output / 'test')

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None, defines=()):
        sources = [str(ROOT / 'testing/filed_vfs_teardown_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + list(defines) + sources + library + ['-o', binary], check=True)

    # Two string leaks in the VFS are known and outside this test: the root
    # node name from __StatRootNode, and the name VFSNodeCreateChild clones
    # before VFSNodeChildNew clones it again. The root scope stays allocated
    # until the test ends. Any other leak fails the test.
    suppressions = output / 'lsan.supp'
    suppressions.write_text('leak:__StatRootNode\nleak:VFSNodeCreateChild\n'
                            'leak:__CreateRootScope\n')
    environment = dict(os.environ)
    environment['LSAN_OPTIONS'] = 'suppressions=' + str(suppressions)

    build()
    subprocess.run([binary], check=True, env=environment, timeout=120)
    if args.mutation_check:
        for path, edits, reason in (
            (FILED + 'vfs/vfs.c',
             (('        while (vfs->Operations != 0) {', '        while (false) {'),),
             'teardown does not wait for operations in flight'),
            (FILED + 'vfs/vfs.c',
             (('    if (vfs->State != VFS_STATE_ACTIVE) {\n        oserr = OS_ENOENT;',
               '    if (false) {\n        oserr = OS_ENOENT;'),),
             'a closing VFS can still be entered'),
            (FILED + 'vfs/vfs.c',
             (('    __RemoveBindsToNode(node);\n', ''),),
             'a bind into the destroyed VFS is left in place'),
            (FILED + 'vfs/utils.c',
             (('                real = fs->Root;\n                oserr = __LockNode(real);',
               '                real = fs->Root;\n                usched_rwlock_r_lock(&real->Lock);\n'
               '                oserr = OS_EOK;'),),
             'a lookup crosses into a mounted VFS without entering it'),
        ):
            text = (ROOT / path).read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            # A copy outside the source tree must find the private VFS headers.
            text = text.replace('#include "private.h"',
                                '#include "' + str(ROOT / FILED / 'vfs/private.h') + '"')
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            build(replacements={path: str(mutant)})
            try:
                result = subprocess.run([binary], capture_output=True, env=environment,
                                        timeout=60)
                survived = result.returncode == 0
            except subprocess.TimeoutExpired:
                survived = False
            if survived:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
