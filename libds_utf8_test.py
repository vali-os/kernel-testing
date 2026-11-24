"""Check UTF-8 decoding in librt/libds (mstr_next and the functions built on it)
on a POSIX host with ASan, UBSan and the leak checker.

Set CC to the host compiler. --mutation-check builds changed copies of the
libds sources and fails if the test does not catch each change.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIBDS = ROOT / 'librt/libds'

# libds sources linked into the test, relative to librt/libds.
SOURCES = (
    'mstring/unicode/mstr_unicode.c',
    'mstring/core/mstr_new.c',
    'mstring/core/mstr_cmp.c',
    'mstring/core/mstr_delete.c',
)

# Generated tables that mstr_unicode.c needs for its case and property
# lookups. They are not under test, and mstr_digits.c has constants that do
# not fit their fields, so they are built without -Werror.
TABLE_SOURCES = (
    'mstring/unicode/mstr_conv.c',
    'mstring/unicode/mstr_digits.c',
    'mstring/unicode/mstr_props.c',
)

# Each mutation must make a check in libds_utf8_test.c fail or make a
# sanitizer stop the test. Paths are relative to librt/libds.
MUTATIONS = (
    ('mstring/unicode/mstr_unicode.c',
     '    if (bytes[0] == 0) {\n        return MSTRING_EOS;\n    }\n',
     '',
     'a call at the end of the string moves past the zero terminator'),
    ('mstring/unicode/mstr_unicode.c',
     '        if (!IS_UTF8(bytes[i])) {',
     '        if (0) {',
     'a cut off sequence is read past the end of the string'),
    ('mstring/unicode/mstr_unicode.c',
     '    for (i = 1; i < length; i++) {',
     '    for (i = 1; i <= length; i++) {',
     'one byte too many is read for each character'),
    ('mstring/unicode/mstr_unicode.c',
     '        minimum   = 0x80;',
     '        minimum   = 0;',
     'overlong 2 byte forms are accepted'),
    ('mstring/unicode/mstr_unicode.c',
     '        minimum   = 0x800;',
     '        minimum   = 0x80;',
     'overlong 3 byte forms are accepted'),
    ('mstring/unicode/mstr_unicode.c',
     '        minimum   = 0x10000;',
     '        minimum   = 0x800;',
     'overlong 4 byte forms are accepted'),
    ('mstring/unicode/mstr_unicode.c',
     ' || character > __UTF8_MAX_CHARACTER ||',
     ' ||',
     'values above U+10FFFF are accepted'),
    ('mstring/unicode/mstr_unicode.c',
     ' ||\n        (character >= __UTF8_SURROGATE_FIRST && character <= __UTF8_SURROGATE_LAST)) {',
     ') {',
     'surrogates are accepted'),
    ('mstring/unicode/mstr_unicode.c',
     '        (*indexp)++;\n        return __UTF8_REPLACEMENT_CHARACTER;\n    }\n\n    *indexp += length;',
     '        *indexp += length;\n        return __UTF8_REPLACEMENT_CHARACTER;\n    }\n\n    *indexp += length;',
     'a rejected sequence skips the bytes after its lead byte'),
    ('mstring/unicode/mstr_unicode.c',
     '            (*indexp)++;\n            return __UTF8_REPLACEMENT_CHARACTER;',
     '            *indexp += i;\n            return __UTF8_REPLACEMENT_CHARACTER;',
     'a cut off sequence skips the bytes after its lead byte'),
    ('include/ds/mstring.h',
     '#define MSTRING_EOS 0xFFFFFFFF',
     '#define MSTRING_EOS 0xFFFD',
     'the end of the string looks like a rejected byte'),
)


def build(source_directory, output):
    binary = output / 'libds_utf8_test'
    compiler = [os.environ.get('CC', 'clang'), '-std=gnu11',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
                '-I', str(source_directory / 'include')]
    tables = []
    for source in TABLE_SOURCES:
        table = output / (Path(source).stem + '.o')
        subprocess.run(compiler + ['-w', '-c', str(source_directory / source),
                       '-o', str(table)], check=True)
        tables.append(str(table))
    command = compiler + ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                          str(ROOT / 'testing/libds_utf8_test.c')]
    command += [str(source_directory / source) for source in SOURCES]
    subprocess.run(command + tables + ['-o', str(binary)], check=True)
    return binary


def run(binary, capture):
    result = subprocess.run([str(binary)], capture_output=capture, timeout=60)
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-libds-utf8-') as directory:
        output = Path(directory)
        if not run(build(LIBDS, output), capture=False):
            raise RuntimeError('libds UTF-8 tests failed')
        if not args.mutation_check:
            return
        for name, before, after, reason in MUTATIONS:
            text = (LIBDS / name).read_text()
            assert text.count(before) == 1, before
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            shutil.copytree(LIBDS, mutant)
            (mutant / name).write_text(text.replace(before, after))
            if run(build(mutant, output), capture=True):
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)


if __name__ == '__main__':
    main()
