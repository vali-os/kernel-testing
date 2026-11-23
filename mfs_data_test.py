"""Check MFS file data with the real MFS code, on a POSIX host with ASan/UBSan:
writes on a new handle, zeroed directory buckets, truncate on an open handle,
writes past the allocated end, old data that must read as zero, and the order
in which a record and its buckets are removed."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
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
# (source, ((text, replacement), ...), reason). EXTRA_EDITS adds edits in
# other sources to the mutation with the same reason.
MUTATIONS = (
    (MFS + 'records.c',
     (('    MFSEntryResetPosition(entry);\n\n    osStatus = MfsUpdateRecord(mfs, entry, MFS_ACTION_CREATE);',
       '    osStatus = MfsUpdateRecord(mfs, entry, MFS_ACTION_CREATE);'),),
     'the handle FsCreate returns has the data bucket 0'),
    (MFS + 'records.c',
     (('    MFSEntryResetPosition(entry);\n\n    osStatus = MfsUpdateRecord(mfs, entry, MFS_ACTION_CREATE);',
       '    osStatus = MfsUpdateRecord(mfs, entry, MFS_ACTION_CREATE);'),),
     'the code before the fix: a new handle has the data bucket 0, and a write only '
     'looks up the bucket of the position when the data bucket is MFS_ENDOFCHAIN'),
    (MFS + 'records.c',
     (('    // A handle opened on the record starts at the start of its data\n    MFSEntryResetPosition(entry);\n',
       ''),),
     'an opened handle keeps the data bucket of the directory it was found in'),
    (MFS + 'records.c',
     (('    oserr = __ZeroBucketChain(mfs, mapRecord->Link);',
       '    oserr = MfsZeroBucket(mfs, mapRecord->Link, mapRecord->Length);'),),
     'a directory expansion zeroes only its first run of buckets'),
    (MFS + 'records.c',
     (('    oserr = __ZeroBucketChain(mfs, record.Link);',
       '    oserr = MfsZeroBucket(mfs, record.Link, record.Length);'),),
     'the first buckets of a directory are zeroed only in their first run'),
    (MFS + 'file_operations.c',
     (('    entry->DataBucketPosition = MFS_ENDOFCHAIN;\n    entry->DataBucketLength   = 0;\n'
       '    entry->BucketByteBoundary = 0;\n    entry->ActionOnClose      = MFS_ACTION_UPDATE;\n',
       '    entry->ActionOnClose      = MFS_ACTION_UPDATE;\n'),),
     'a truncate to 0 leaves the handle on the freed buckets'),
    (MFS + 'file_operations.c',
     (('    if (bytesToWrite != 0) {\n        oserr = __SeekToBucketOfPosition(mfs, entry, position);',
       '    if (entry->DataBucketPosition == MFS_ENDOFCHAIN) {\n        oserr = FsSeekInFile(mfs, entry, position);'),),
     'a write only looks up the bucket of the position when the data bucket is MFS_ENDOFCHAIN'),
    (MFS + 'file_operations.c',
     (('    oserr = __SeekToBucketOfPosition(mfs, entry, position);\n    if (oserr != OS_EOK) {\n'
       '        ERROR("FsReadFromFile',
       '    oserr = OS_EOK;\n    if (oserr != OS_EOK) {\n        ERROR("FsReadFromFile'),),
     'a read trusts the data bucket of the handle'),
    (MFS + 'file_operations.c',
     (('    if (position > entry->ActualSize) {\n        oserr = __ZeroFileRange(',
       '    if (false) {\n        oserr = __ZeroFileRange('),),
     'a write past the end leaves the gap before it as it was on disk'),
    (MFS + 'file_operations.c',
     (('    if (size > entry->ActualSize) {\n        oserr = __ZeroFileRange(',
       '    if (false) {\n        oserr = __ZeroFileRange('),),
     'a truncate that grows the file leaves the new bytes as they were on disk'),
    (MFS + 'file_operations.c',
     (('        memset(transferBuffer + sectorOffset, 0, byteCount);',
       '        memset(transferBuffer, 0, mfs->SectorSize);'),),
     'a sector that is cleared in part is cleared whole'),
    (MFS + 'file_operations.c',
     (('        // Step to the next run of buckets at the end of the current one\n        if (position ==',
       '        if (false && position =='),),
     'the clearing does not step to the next run of buckets'),
    (MFS + 'main.c',
     (('    oserr = MfsUpdateRecord(mfs, mfsEntry, MFS_ACTION_DELETE);\n    if (oserr != OS_EOK) {\n'
       '        ERROR("FsUnlink failed to remove the record of %ms: %u", path, oserr);\n        goto cleanup;\n    }\n',
       '    if (mfsEntry->StartBucket != MFS_ENDOFCHAIN) {\n'
       '        oserr = MfsFreeBuckets(mfs, mfsEntry->StartBucket, mfsEntry->StartLength);\n'
       '        if (oserr != OS_EOK) {\n            goto cleanup;\n        }\n    }\n'
       '    oserr = MfsUpdateRecord(mfs, mfsEntry, MFS_ACTION_DELETE);\n'
       '    if (oserr != OS_EOK) {\n        goto cleanup;\n    }\n'),
      ('        freeStatus = MfsFreeBuckets(mfs, mfsEntry->StartBucket, mfsEntry->StartLength);',
       '        freeStatus = OS_EOK;')),
     'an unlink frees the buckets before it removes the record'),
    (MFS + 'main.c',
     (('                  path, mfsEntry->StartBucket, mfsEntry->StartLength, freeStatus);\n',
       '                  path, mfsEntry->StartBucket, mfsEntry->StartLength, freeStatus);\n'
       '            oserr = freeStatus;\n'),),
     'an unlink that removed the record returns the error of the free'),
    (MFS + 'file_operations.c',
     (('    oserr = MfsUpdateRecord(mfs, entry, MFS_ACTION_UPDATE);\n    if (oserr != OS_EOK) {\n'
       '        ERROR("FsChangeFileSize failed',
       '    oserr = OS_EOK;\n    if (oserr != OS_EOK) {\n        ERROR("FsChangeFileSize failed'),),
     'a truncate to 0 frees the buckets while the record on disk still points at them'),
    (MFS + 'file_operations.c',
     (('        memcpy(entry, &previous, sizeof(MFSEntry_t));\n        return oserr;',
       '        return oserr;'),),
     'a failed truncate to 0 leaves the handle without its buckets'),
)
EXTRA_EDITS = {
    'the code before the fix: a new handle has the data bucket 0, and a write only '
    'looks up the bucket of the position when the data bucket is MFS_ENDOFCHAIN':
    (MFS + 'file_operations.c',
     (('    if (bytesToWrite != 0) {\n        oserr = __SeekToBucketOfPosition(mfs, entry, position);',
       '    if (entry->DataBucketPosition == MFS_ENDOFCHAIN) {\n        oserr = FsSeekInFile(mfs, entry, position);'),)),
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
parser.add_argument('--case', help='run only this case of mfs_data_test.c')
parser.add_argument('--sources', help='a directory with the MFS sources to test instead of ' + MFS)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-mfs-data-') as directory:
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
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = 'detect_leaks=1'
    command = [binary] + ([args.case] if args.case else [])

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def source_path(path):
        if args.sources:
            return str(Path(args.sources) / Path(path).name)
        return str(ROOT / path)

    def build(replacements=None):
        sources = [str(ROOT / 'testing/mfs_data_test.c')]
        sources += [(replacements or {}).get(path, source_path(path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    build()
    result = subprocess.run(command, timeout=120, env=environment)
    if result.returncode != 0:
        sys.exit('mfs_data_test failed with exit code %d' % result.returncode)
    if args.mutation_check:
        for path, edits, reason in MUTATIONS:
            changes = [(path, edits)]
            if reason in EXTRA_EDITS:
                changes.append(EXTRA_EDITS[reason])
            replacements = {}
            for changed_path, changed_edits in changes:
                text = Path(source_path(changed_path)).read_text()
                for before, after in changed_edits:
                    assert text.count(before) == 1, before
                    text = text.replace(before, after)
                # A copy outside the source tree must find the MFS header.
                text = text.replace('#include "mfs.h"', '#include "' + str(ROOT / MFS / 'mfs.h') + '"')
                mutant = output / ('mutant_' + Path(changed_path).name)
                mutant.write_text(text)
                replacements[changed_path] = str(mutant)
            build(replacements=replacements)
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=120, env=environment)
            except subprocess.TimeoutExpired:
                print('Rejected mutation: %s (the test did not finish)' % reason, flush=True)
                continue
            if result.returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            failure = next((line for line in (result.stdout + result.stderr).splitlines()
                            if 'Assertion' in line or 'Sanitizer' in line or 'expected' in line
                            or 'written' in line or 'returned' in line or 'free list' in line
                            or 'listing' in line or 'not zero' in line),
                           'exit code %d' % result.returncode)
            print('Rejected mutation: %s (%s)' % (reason, failure.strip()), flush=True)
