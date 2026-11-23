"""Move and copy files in filed (VFSNodeMove) with the real VFS, the real driver
interface and two mounted disks, on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILED = 'services/filed/'
MOVE = FILED + 'vfs/handlers/move.c'
# Production sources compiled as their own translation units. filesystem.c and
# storage/utils.c are included by the test itself.
SOURCES = [FILED + path for path in (
    'vfs/vfs.c', 'vfs/utils.c', 'vfs/bind.c', 'vfs/mount.c', 'vfs/memfs.c',
    'vfs/handlers/mkdir.c', 'vfs/handlers/open.c', 'vfs/handlers/close.c',
    'vfs/handlers/unlink.c', 'vfs/handlers/handle_stat.c', 'vfs/handlers/stat.c',
    'vfs/handlers/move.c', 'interfaces/driver_ops.c')]
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

def previous_version(path, marker):
    """Returns the newest committed version of path that still contains marker."""
    commits = subprocess.run(['git', '-C', str(ROOT), 'log', '--format=%H', '--', path],
                             capture_output=True, text=True, check=True).stdout.split()
    for commit in commits:
        text = subprocess.run(['git', '-C', str(ROOT), 'show', commit + ':' + path],
                              capture_output=True, text=True, check=True).stdout
        if marker in text:
            return text
    raise RuntimeError('no version of %s contains %r' % (path, marker))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-vfs-move-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'contracts/filesystem',
                    'contracts/usbhost', 'contracts/storage'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    # driver_ops.c takes GetGrachtClient from the libc internal header. Only
    # that header is used, as the rest of the libc headers would replace the
    # host's.
    (output / 'libc-internal' / 'internal').mkdir(parents=True)
    (output / 'libc-internal' / 'internal' / '_utils.h').write_text(
        (ROOT / 'librt/libc/include/internal/_utils.h').read_text())
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-unused-but-set-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-implicit-fallthrough', '-Wno-type-limits',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + directory]
    # memfs packs directory entries back to back (__ReadDirectoryEntry), and
    # so does the driver double, so the second entry is not 8-byte aligned.
    flags += ['-fno-sanitize=alignment']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include',
               'librt/libgracht/include')]
    flags += ['-I' + str(output / 'libc-internal')]
    binary = str(output / 'test')

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None):
        sources = [str(ROOT / 'testing/filed_vfs_move_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    # Known string leaks outside this test: the root node name from
    # __StatRootNode, the name VFSNodeCreateChild clones before VFSNodeChildNew
    # clones it again, and the label from_fsstat makes for __GetLabel. The root
    # scope stays allocated until the test ends. Any other leak fails the test.
    suppressions = output / 'lsan.supp'
    suppressions.write_text('leak:__StatRootNode\nleak:VFSNodeCreateChild\n'
                            'leak:__CreateRootScope\nleak:from_fsstat\n')
    environment = dict(os.environ)
    environment['LSAN_OPTIONS'] = 'suppressions=' + str(suppressions)

    build()
    subprocess.run([binary], check=True, env=environment, timeout=120)
    if args.mutation_check:
        old = previous_version(MOVE, 'while (i) {')
        mutations = [
            # The last committed handler with the old loop over the parents
            # of the source, which never advanced.
            (None, old, 'the old move handler'),
            ((('    hashtable_remove(\n            &directory->Children,\n'
               '            &(struct __VFSChild) { .Key = node->Name }\n    );\n'
               '    VFSNodeDestroy(node);\n', '    usched_rwlock_w_unlock(&node->Lock);\n'),),
             None, 'the source node is kept after a move'),
            ((('    oserr = __AddNode(directory, request);\n    __UnlockDirectory(directory);',
               '    oserr = OS_EOK;\n    __UnlockDirectory(directory);'),),
             None, 'the target node is not added'),
            ((('    oserr = __AddNode(directory, request);\n    __UnlockDirectory(directory);',
               '    oserr = __AddNode(directory, request);'),),
             None, 'the target directory stays locked'),
            ((('    VFSLeave(source->VFS);\n', ''),),
             None, 'the source VFS stays entered'),
            ((('    if (handles != 0 || node->Mounts.element_count != 0) {',
               '    if (handles == (size_t)-1) {'),),
             None, 'an open file is moved'),
            ((('    unlinkStatus = ops->Unlink(vfs->Interface, vfs->Data, request->TargetPath);',
               '    unlinkStatus = OS_EOK;'),),
             None, 'an incomplete copy is left behind'),
            ((('    oserr = VFSNodeUnlink(vfs, cfrom);', '    oserr = OS_EOK;'),),
             None, 'the source is kept after a move across filesystems'),
            ((('    if (oserr != OS_EOK || copy) {', '    if (copy) {'),),
             None, 'the source is removed although the copy failed'),
        ]
        for edits, text, reason in mutations:
            if text is None:
                text = (ROOT / MOVE).read_text()
                for before, after in edits:
                    assert text.count(before) == 1, before
                    text = text.replace(before, after)
            # A copy outside the source tree must find the private VFS headers.
            text = text.replace('#include "../private.h"',
                                '#include "' + str(ROOT / FILED / 'vfs/private.h') + '"')
            mutant = output / 'mutant_move.c'
            mutant.write_text(text)
            build(replacements={MOVE: str(mutant)})
            try:
                result = subprocess.run([binary], capture_output=True, env=environment,
                                        timeout=30)
                survived = result.returncode == 0
                how = 'failed with code %d' % result.returncode
            except subprocess.TimeoutExpired:
                survived = False
                how = 'timed out'
            if survived:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: %s (%s)' % (reason, how), flush=True)
