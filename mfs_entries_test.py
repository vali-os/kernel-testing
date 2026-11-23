"""Check MFS directory expansion, unlink, seek in an empty file and the memory of
a mount, with the real MFS code, on a POSIX host with ASan/UBSan and
LeakSanitizer."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MFS = 'modules/filesystems/mfs/'
SOURCES = [MFS + path for path in ('main.c', 'records.c', 'bucket_map.c', 'utilities.c',
                                   'file_operations.c', 'directory_operations.c')]
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

# Each mutation puts one of the fixed bugs back and must make the test fail:
# (source, ((text, replacement), ...), reason).
MUTATIONS = (
    (MFS + 'records.c',
     (('            mapRecord->Link,\n            0,\n            false\n    );',
       '            mapRecord->Link,\n            mapRecord->Length,\n            true\n    );'),),
     'a directory expansion sets the length of the last bucket to the length of the new buckets'),
    (MFS + 'main.c',
     (('    if (mfsEntry->StartBucket != MFS_ENDOFCHAIN) {\n        freeStatus = MfsFreeBuckets(',
       '    if (true) {\n        freeStatus = MfsFreeBuckets('),),
     'an unlink frees the buckets of a file that has none'),
    (MFS + 'main.c',
     (('    if (mfsEntry->StartBucket != MFS_ENDOFCHAIN) {\n        freeStatus = MfsFreeBuckets(',
       '    if (false) {\n        freeStatus = MfsFreeBuckets('),),
     'an unlink keeps the buckets of a file'),
    (MFS + 'file_operations.c',
     (('    if (entry->StartBucket == MFS_ENDOFCHAIN) {\n        entry->DataBucketPosition = MFS_ENDOFCHAIN;\n',
       '    if (false) {\n        entry->DataBucketPosition = MFS_ENDOFCHAIN;\n'),),
     'a seek in a file without buckets walks the bucket map'),
    (MFS + 'file_operations.c',
     (('        oserr = __SeekToBucketOfPosition(mfs, entry, position);\n'
       '        if (oserr != OS_EOK) {\n'
       '            ERROR("FsWriteToFile: failed to find the bucket of position %u: %u", LODWORD(position), oserr);\n'
       '            return oserr;\n'
       '        }\n',
       '        entry->DataBucketPosition = entry->StartBucket;\n'
       '        entry->DataBucketLength   = entry->StartLength;\n'
       '        entry->BucketByteBoundary = 0;\n'),),
     'the first write after a seek past 0 in an empty file starts in the first bucket'),
    (MFS + 'main.c',
     (('    mstr_delete(mfs->Label);\n    mstr_delete(mfs->RootEntry.Name);\n',
       '    mstr_delete(mfs->RootEntry.Name);\n'),),
     'the label of a mount is not freed'),
    (MFS + 'main.c',
     (('    mstr_delete(mfs->Label);\n    mstr_delete(mfs->RootEntry.Name);\n',
       '    mstr_delete(mfs->Label);\n'),),
     'the name of the root of a mount is not freed'),
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-mfs-entries-') as directory:
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
    # file_operations.c traces sizes with the Vali libc format macros, which
    # the host libc does not have.
    flags += ['-DPRIuIN="zu"', '-DPRIxIN="zx"']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'modules/filesystems/common/include', 'librt/libos/include',
               'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include')]
    binary = str(output / 'test')
    # The mount memory case relies on LeakSanitizer, which reports a leak at
    # exit and then fails the run.
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = 'detect_leaks=1'

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None):
        sources = [str(ROOT / 'testing/mfs_entries_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    build()
    subprocess.run([binary], check=True, timeout=120, env=environment)
    if args.mutation_check:
        for path, edits, reason in MUTATIONS:
            text = (ROOT / path).read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            # A copy outside the source tree must find the MFS header.
            text = text.replace('#include "mfs.h"', '#include "' + str(ROOT / MFS / 'mfs.h') + '"')
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            build(replacements={path: str(mutant)})
            result = subprocess.run([binary], capture_output=True, text=True, timeout=120, env=environment)
            if result.returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            failure = next((line for line in (result.stdout + result.stderr).splitlines()
                            if 'Assertion' in line or 'Sanitizer' in line or 'missing' in line
                            or 'expected' in line or 'returned' in line),
                           'exit code %d' % result.returncode)
            print('Rejected mutation: %s (%s)' % (reason, failure.strip()), flush=True)
