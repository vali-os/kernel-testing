"""Move MFS files and directories with FsMove while the disk fails at each write,
with the real MFS record and bucket map code, on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MFS = 'modules/filesystems/mfs/'
SOURCES = [MFS + path for path in ('main.c', 'records.c', 'bucket_map.c', 'utilities.c')]
# The libds sources are not under test. They are built once, without -Werror.
LIBRARY_SOURCES = ['librt/libds/' + path for path in (
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

# Each mutation must make the test fail: (source, ((text, replacement), ...), reason).
MUTATIONS = (
    (MFS + 'main.c',
     (('    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_DELETE);\n'
       '    if (oserr != OS_EOK) {\n'
       '        ERROR("FsMove failed to remove the record of %ms: %u", source->Name, oserr);\n'
       '        undoStatus = MfsUpdateRecord(mfs, target, MFS_ACTION_DELETE);\n',
       '    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_DELETE);\n'
       '    if (oserr != OS_EOK) {\n'
       '        ERROR("FsMove failed to remove the record of %ms: %u", source->Name, oserr);\n'
       '        undoStatus = OS_EOK;\n'),),
     'a failed removal of the source record keeps the new record'),
    (MFS + 'main.c',
     (('    oserr = MfsCreateRecord(\n            mfs,\n            directory,\n',
       '    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_DELETE);\n'
       '    if (oserr != OS_EOK) {\n        goto cleanup;\n    }\n'
       '    oserr = MfsCreateRecord(\n            mfs,\n            directory,\n'),
      ('    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_DELETE);\n'
       '    if (oserr != OS_EOK) {\n'
       '        ERROR("FsMove failed to remove the record of %ms: %u", source->Name, oserr);\n',
       '    if (oserr != OS_EOK) {\n'
       '        ERROR("FsMove failed to remove the record of %ms: %u", source->Name, oserr);\n')),
     'the source record is removed before the new record is written'),
    (MFS + 'main.c',
     (('    oserr = __FindPath(mfs, to, &existing, &searchResult);\n    if (oserr != OS_EOK) {\n',
       '    oserr = __FindPath(mfs, to, &existing, &searchResult);\n'
       '    if (oserr == OS_ENOENT) {\n        oserr = OS_EOK;\n        searchResult = MFS_SEARCH_MISSING;\n    }\n'
       '    if (oserr != OS_EOK) {\n'),),
     'OS_ENOENT from the target lookup is taken for a missing target'),
    (MFS + 'main.c',
     (('    if (mstr_icmp(fromDirectory, toDirectory) == 0) {', '    if (false) {'),),
     'a rename within one directory creates a new record'),
    (MFS + 'main.c',
     (('        if (inside) {\n            oserr = OS_EINVALPARAMS;', '        if (false) {\n            oserr = OS_EINVALPARAMS;'),),
     'a directory can be moved into itself'),
    (MFS + 'main.c',
     (('    if (copy) {\n        return OS_ENOTSUPPORTED;\n    }\n', ''),),
     'a copy is done as a move'),
    (MFS + 'main.c',
     (('    if (searchResult == MFS_SEARCH_MISSING) {\n        oserr = OS_ENOENT;\n        goto cleanup;\n    }\n'
       '    if (!(directory->NativeFlags & MFS_FILERECORD_DIRECTORY)) {\n',
       '    if (!(directory->NativeFlags & MFS_FILERECORD_DIRECTORY)) {\n'),),
     'a missing target directory is not checked'),
    (MFS + 'utilities.c',
     (('        memset(&record->Name[0], 0, sizeof(record->Name));\n        memcpy(&record->Name[0], entryName, nameLength);\n',
       '        memcpy(&record->Name[0], entryName, nameLength);\n'),),
     'a rename to a shorter name keeps the end of the old name'),
    (MFS + 'records.c',
     (('                oserr = OS_EOK;\n                *searchResultOut = MFS_SEARCH_MISSING;\n'
       '                __CleanupEntryIterator(&currentEntry);\n',
       '                oserr = OS_ENOENT;\n                __CleanupEntryIterator(&currentEntry);\n'),),
     'a directory without buckets on the way is a lookup error'),
)


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
with tempfile.TemporaryDirectory(prefix='vali-mfs-move-') as directory:
    output = Path(directory)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-comma',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    # The MFS records are packed structures read straight out of the transfer
    # buffer. Their alignment is outside this test.
    flags += ['-fno-sanitize=alignment']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'modules/filesystems/common/include', 'librt/libos/include',
               'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include')]
    binary = str(output / 'test')

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None):
        sources = [str(ROOT / 'testing/mfs_move_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    def mutant_file(path, text):
        # A copy outside the source tree must find the MFS header.
        text = text.replace('#include "mfs.h"', '#include "' + str(ROOT / MFS / 'mfs.h') + '"')
        mutant = output / ('mutant_' + Path(path).name)
        mutant.write_text(text)
        return str(mutant)

    def expect_rejected(replacements, reason):
        build(replacements=replacements)
        result = subprocess.run([binary], capture_output=True, text=True, timeout=120)
        if result.returncode == 0:
            raise RuntimeError('Undetected regression: ' + reason)
        failure = next((line for line in (result.stdout + result.stderr).splitlines()
                        if 'Assertion' in line or 'Sanitizer' in line), 'exit code %d' % result.returncode)
        print('Rejected mutation: %s (%s)' % (reason, failure.strip()), flush=True)

    build()
    subprocess.run([binary], check=True, timeout=120)
    if args.mutation_check:
        # The FsMove from before this change, with the rest of main.c as it was.
        old = previous_version(MFS + 'main.c', 'fromBaseName')
        expect_rejected({MFS + 'main.c': mutant_file(MFS + 'main.c', old)}, 'the old FsMove')
        for path, edits, reason in MUTATIONS:
            text = (ROOT / path).read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            expect_rejected({path: mutant_file(path, text)}, reason)
