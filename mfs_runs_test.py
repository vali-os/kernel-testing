"""Check MFS bucket runs with the real MFS code, on a POSIX host with ASan/UBSan:
the map lookups of a request on a long file, joined runs in the file code and
in the allocator, failed writes during an allocation or a join, and handles
whose copy of a run length is shorter than the bucket map."""
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
# bucket_map.c holds MFSBucketMapGetLengthAndLink. Every other MFS source calls
# TestCountedMapLookup instead, which counts the lookup and passes it on.
COUNTED_SOURCES = [path for path in SOURCES if not path.endswith('bucket_map.c')]
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

# Each mutation breaks one rule and must make the test fail:
# (source, ((text, replacement), ...), reason).
MUTATIONS = (
    (MFS + 'file_operations.c',
     (('    if (entry->DataBucketPosition != MFS_ENDOFCHAIN && absolutePosition >= entry->BucketByteBoundary) {',
       '    if (false) {'),),
     'a seek always walks from the first bucket of the file'),
    (MFS + 'utilities.c',
     (('    bucket_t    bucket = entry->DataBucketPosition;\n    MapRecord_t run;',
       '    bucket_t    bucket = MFS_ENDOFCHAIN;\n    MapRecord_t run;'),),
     'the search for the last run walks from the first bucket of the file'),
    (MFS + 'file_operations.c',
     (('//#define __TRACE\n', '#define __TRACE\n'),),
     'file_operations.c logs trace lines'),
    (MFS + 'main.c',
     (('//#define __TRACE\n', '#define __TRACE\n'),),
     'main.c logs trace lines'),
    (MFS + 'utilities.c',
     (('    if (lastBucket + lastRun.Length != newRun->Link) {',
       '    if (true) {'),),
     'a neighbouring extension is linked as a run of its own'),
    (MFS + 'utilities.c',
     (('    if (entry->StartBucket == lastBucket) {\n        entry->StartLength = joinedLength;\n    }\n', ''),),
     'a join of the first run leaves StartLength, and the record, at the old length'),
    (MFS + 'utilities.c',
     (('    if (entry->DataBucketPosition == lastBucket) {\n        entry->DataBucketLength = joinedLength;\n    }\n',
       ''),),
     'a join leaves the handle in the last run with the old length'),
    (MFS + 'bucket_map.c',
     (('        if (runBucket != MFS_ENDOFCHAIN && runBucket + runLength == freeBucket) {',
       '        if (false) {'),),
     'the allocator hands out neighbouring free runs as runs of their own'),
    (MFS + 'bucket_map.c',
     (('    if (current.Link == link && current.Length == length) {\n        return OS_EOK;\n    }\n', ''),),
     'the allocator writes map entries that already hold their values'),
    (MFS + 'bucket_map.c',
     (('    oserr = __FreeListHoldsBuckets(mfs, bucketCount);', '    oserr = OS_EOK;'),),
     'the allocator does not check the free list before it writes'),
    # The rest of a split free run is written after the master record points
    # at it. A failed write then leaves the free list head on a map entry
    # that was never written.
    (MFS + 'bucket_map.c',
     (('    MapRecord_t freeRun;\n    oserr_t     oserr;\n\n    if (bucketCount == 0) {',
       '    MapRecord_t freeRun;\n    oserr_t     oserr;\n    MapRecord_t splitRun = { 0, 0 };\n'
       '    uint32_t    splitTaken = 0;\n\n    if (bucketCount == 0) {'),
      ('            nextFreeBucket = freeBucket + bucketsTaken;\n            oserr = MFSBucketMapSetLinkAndLength(',
       '            nextFreeBucket = freeBucket + bucketsTaken;\n            splitRun = freeRun;\n'
       '            splitTaken = bucketsTaken;\n        }\n        if (false) {\n'
       '            oserr = MFSBucketMapSetLinkAndLength('),
      ('        mfs->MasterRecord.FreeBucket = previousFreeBucket;\n    }\n    return oserr;\n}',
       '        mfs->MasterRecord.FreeBucket = previousFreeBucket;\n    }\n'
       '    if (oserr == OS_EOK && splitRun.Length != 0) {\n'
       '        oserr = MFSBucketMapSetLinkAndLength(mfs, freeBucket, splitRun.Link,\n'
       '                                             splitRun.Length - splitTaken, true);\n    }\n'
       '    return oserr;\n}')),
     'the rest of a split free run is written after the master record'),
    (MFS + 'utilities.c',
     (('    if (currentRun.Length > entry->DataBucketLength) {', '    if (false) {'),),
     'a handle at the old end of a run that grew steps past the bytes it grew by'),
    (MFS + 'file_operations.c',
     (('        runEnd = runStart + ((uint64_t)run.Length * bucketSizeBytes);',
       '        runEnd = runStart + ((uint64_t)(bucket == entry->DataBucketPosition ?\n'
       '                 entry->DataBucketLength : run.Length) * bucketSizeBytes);'),),
     'a seek takes the length of the data bucket from the handle instead of the map'),
    (MFS + 'utilities.c',
     (('    if (bucket == entry->StartBucket) {\n        entry->StartLength = length;\n    }\n', ''),),
     'a handle that finds its first run longer keeps the old StartLength'),
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
parser.add_argument('--case', help='run only this case of mfs_runs_test.c')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-mfs-runs-') as directory:
    output = Path(directory)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-comma',
             '-ffunction-sections', '-fdata-sections', '-g',
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

    def build(replacements=None):
        objects = [str(output / 'mfs_runs_test.o')]
        subprocess.run(flags + ['-c', str(ROOT / 'testing/mfs_runs_test.c'), '-o', objects[0]], check=True)
        for index, path in enumerate(SOURCES):
            source = (replacements or {}).get(path, str(ROOT / path))
            rename = ['-DMFSBucketMapGetLengthAndLink=TestCountedMapLookup'] if path in COUNTED_SOURCES else []
            objects.append(str(output / f'mfs{index}.o'))
            subprocess.run(flags + rename + ['-c', source, '-o', objects[-1]], check=True)
        subprocess.run(flags + ['-Wl,--gc-sections'] + objects + library + ['-o', binary], check=True)

    build()
    result = subprocess.run(command, timeout=300, env=environment)
    if result.returncode != 0:
        sys.exit('mfs_runs_test failed with exit code %d' % result.returncode)
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
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=300, env=environment)
            except subprocess.TimeoutExpired:
                print('Rejected mutation: %s (the test did not finish)' % reason, flush=True)
                continue
            if result.returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            failure = next((line for line in (result.stdout + result.stderr).splitlines()
                            if 'Assertion' in line or 'Sanitizer' in line or 'expected' in line
                            or 'lookups' in line or 'trace' in line or 'free list' in line
                            or 'returned' in line or 'did not' in line),
                           'exit code %d' % result.returncode)
            print('Rejected mutation: %s (%s)' % (reason, failure.strip()), flush=True)
        print('mfs_runs_test: %d mutations rejected' % len(MUTATIONS))
