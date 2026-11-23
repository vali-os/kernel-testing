"""Check the MFS mount lock: run a second file system operation while the first
one waits for the disk, with the real MFS code, on a POSIX host with ASan/UBSan.
Also checks that every Fs* function takes and releases the lock on each path."""
import argparse
import os
from pathlib import Path
import re
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

# Every Fs* function that filed calls on a mounted filesystem.
ENTRY_POINTS = ('FsOpen', 'FsCreate', 'FsClose', 'FsStat', 'FsUnlink', 'FsLink', 'FsReadLink',
                'FsMove', 'FsRead', 'FsWrite', 'FsSeek', 'FsTruncate')
LOCK = '    usched_mtx_lock(&mfs->Lock);\n'
UNLOCK = '    usched_mtx_unlock(&mfs->Lock);\n'


def function_body(text, name):
    """Returns the start and end offsets of the definition of name in main.c."""
    match = re.search(r'\noserr_t\n%s\(\n' % name, text)
    assert match, name
    end = text.index('\n}\n', match.start()) + 3
    return match.start(), end


def without_lock(text, name):
    """The wrapper of name calls its body without the lock."""
    start, end = function_body(text, name)
    body = text[start:end]
    assert body.count(LOCK) == 1 and body.count(UNLOCK) == 1, name
    return text[:start] + body.replace(LOCK, '').replace(UNLOCK, '') + text[end:]


def early_error_return(text, name):
    """The wrapper of name returns an error before it releases the lock."""
    start, end = function_body(text, name)
    body = text[start:end]
    assert body.count(UNLOCK) == 1, name
    body = body.replace(UNLOCK, '    if (oserr != OS_EOK) {\n        return oserr;\n    }\n' + UNLOCK)
    return text[:start] + body + text[end:]


def replaced(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before, after)


# Each mutation must make the test fail: (source, function(text), reason).
MUTATIONS = [(MFS + 'main.c', (lambda text, name=name: without_lock(text, name)),
              '%s runs without the lock' % name) for name in ENTRY_POINTS]
MUTATIONS += [(MFS + 'main.c', (lambda text, name=name: early_error_return(text, name)),
               '%s keeps the lock after an error' % name) for name in ('FsOpen', 'FsClose', 'FsTruncate')]
MUTATIONS += [
    (MFS + 'main.c',
     lambda text: replaced(text,
                           '    if (usched_mtx_trylock(&fileSystem->Lock) != 0) {\n',
                           '    if (false) {\n'),
     'FsDestroy frees a mount whose lock is held'),
    (MFS + 'main.c',
     lambda text: replaced(text,
                           '    source->Name = targetName;\n    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_RENAME);\n',
                           '    source->Name = targetName;\n    usched_mtx_lock(&mfs->Lock);\n'
                           '    oserr = MfsUpdateRecord(mfs, source, MFS_ACTION_RENAME);\n'
                           '    usched_mtx_unlock(&mfs->Lock);\n'),
     'an inner helper of FsMove takes the lock again'),
]

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-mfs-lock-') as directory:
    output = Path(directory)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-comma',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g', '-pthread',
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

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None):
        sources = [str(ROOT / 'testing/mfs_lock_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + sources + library + ['-o', binary], check=True)

    build()
    subprocess.run([binary], check=True, timeout=120)
    if args.mutation_check:
        for path, mutate, reason in MUTATIONS:
            text = mutate((ROOT / path).read_text())
            # A copy outside the source tree must find the MFS header.
            text = text.replace('#include "mfs.h"', '#include "' + str(ROOT / MFS / 'mfs.h') + '"')
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            build(replacements={path: str(mutant)})
            try:
                result = subprocess.run([binary], capture_output=True, text=True, timeout=60)
            except subprocess.TimeoutExpired:
                print('Rejected mutation: %s (the test did not finish)' % reason, flush=True)
                continue
            if result.returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            failure = next((line for line in (result.stdout + result.stderr).splitlines()
                            if 'Assertion' in line or 'Sanitizer' in line or 'returned' in line),
                           'exit code %d' % result.returncode)
            print('Rejected mutation: %s (%s)' % (reason, failure.strip()), flush=True)
