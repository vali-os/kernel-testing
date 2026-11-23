"""Create an MFS directory while the disk is removed at each write, with the
real MFS record and bucket map code, on a POSIX host with ASan/UBSan."""
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

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-mfs-create-') as directory:
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
        sources = [str(ROOT / 'testing/mfs_create_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    build()
    subprocess.run([binary], check=True, timeout=120)
    if args.mutation_check:
        zero = ('    oserr = __ZeroBucketChain(mfs, record.Link);\n'
                '    if (oserr != OS_EOK) {\n'
                '        ERROR("__InitiateDirectory failed to zero the buckets from %u: %u", record.Link, oserr);\n'
                '        return oserr;\n'
                '    }\n')
        for path, edits, reason in (
            (MFS + 'records.c',
             (('            ERROR("__FindEntryOrFreeInDirectory failed to set up directory %ms: %u", directory->Name, oserr);\n'
               '            return oserr;',
               '            *searchResultOut = MFS_SEARCH_MISSING;\n'
               '            return OS_EOK;'),),
             'a failed directory setup is taken for a missing name'),
            (MFS + 'records.c',
             ((zero, ''),
              ('    *bucketOut = record.Link;\n    return OS_EOK;',
               zero + '    *bucketOut = record.Link;\n    return OS_EOK;')),
             'directory record updated before its bucket is zeroed'),
            (MFS + 'records.c',
             (('        entry->StartBucket = previous.StartBucket;\n', ''),),
             'directory entry keeps the bucket its record update failed to store'),
            (MFS + 'bucket_map.c',
             (('        mfs->BucketMap[(bucket * 2)] = previousLink;\n', ''),),
             'bucket map in memory keeps a change its write failed to store'),
            (MFS + 'bucket_map.c',
             (('        ERROR("MFSBucketMapAllocate failed to store the new free bucket %u: %u", freeBucket, oserr);\n'
               '        mfs->MasterRecord.FreeBucket = previousFreeBucket;\n',
               '        ERROR("MFSBucketMapAllocate failed to store the new free bucket %u: %u", freeBucket, oserr);\n'),),
             'free list head in memory moves although the master record write failed'),
            (MFS + 'bucket_map.c',
             (('        ERROR("Failed to write (secondary) master-record to disk: %u", oserr);\n'
               '        return oserr;',
               '        ERROR("Failed to write (secondary) master-record to disk: %u", oserr);\n'
               '        return OS_EUNKNOWN;'),),
             'master record write error replaced by OS_EUNKNOWN'),
        ):
            text = (ROOT / path).read_text()
            for before, after in edits:
                assert text.count(before) == 1, before
                text = text.replace(before, after)
            # A copy outside the source tree must find the MFS header.
            text = text.replace('#include "mfs.h"', '#include "' + str(ROOT / MFS / 'mfs.h') + '"')
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            build(replacements={path: str(mutant)})
            if subprocess.run([binary], capture_output=True, timeout=120).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
