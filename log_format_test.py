"""Test the z, t and j length modifiers of the log formatters on a POSIX host.

Two printf engines format Vali log lines:
  - librt/libc/stdio/stream/streamout.c. libc builds it for userspace
    (vsnprintf, libddk SystemDebug) and libk builds it with LIBC_KERNEL for
    the kernel. Kernels built without ACPI (RPi) use it for every log line.
  - librt/libacpi/source/components/utilities/utprint.c. Kernels built with
    ACPI (generic AMD64 and AArch64) link this vsnprintf ahead of libk's, so it
    formats every kernel TRACE, WARNING and ERROR line on those builds.

Each engine is built for the host width, and for 32 bits when the host
compiler can link a 32-bit binary. The streamout engine is built in both its
kernel and libc modes.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STREAMOUT = ROOT / 'librt/libc/stdio/stream/streamout.c'
UTPRINT = ROOT / 'librt/libacpi/source/components/utilities/utprint.c'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()


def streamout_command(defines, width):
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-D__OSLIB_C_IMPLEMENTATION',
             '-fms-extensions', '-Wno-macro-redefined', '-Wno-ignored-attributes',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fno-sanitize-recover=all', '-g', width]
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    return flags + defines + [str(ROOT / 'testing/streamout_test.c'), '-lm']


def utprint_command(defines, width):
    flags = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-fms-extensions',
             '-fasm-blocks', '-DKERNELAPI=', '-DKERNELABI=',
             '-DSERVICEAPI=static inline', '-DSERVICEABI=', '-DLIBC_KERNEL',
             '-D__LIBDS_KERNEL__', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
             # ACPICA computes its unbounded end pointer from NULL.
             '-Wno-null-pointer-subtraction',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fno-sanitize-recover=all', '-g', width]
    for directory in ('testing/include', 'kernel/include', 'librt/libos/include',
                      'librt/libacpi/source/include'):
        flags.append('-I' + str(ROOT / directory))
    for directory in ('librt/libds/include', 'librt/libddk/include'):
        flags += ['-isystem', str(ROOT / directory)]
    return flags + defines + [str(ROOT / 'testing/acpi/utprint_test.c')]


# Name, command builder, extra defines, macro that selects the source file.
ENGINES = [
    ('streamout-kernel', streamout_command, ['-DLIBC_KERNEL'], 'STREAMOUT_SOURCE'),
    # On an x86-64 host va_list is an array, so the float helper's va_list
    # pointer does not type check. No case formats a float.
    ('streamout-libc', streamout_command, ['-Wno-incompatible-pointer-types'],
     'STREAMOUT_SOURCE'),
    ('acpica', utprint_command, [], 'UTPRINT_SOURCE'),
]


def build(directory, name, builder, defines, width, source_macro=None, source=None):
    command = builder(defines, width)
    # The sanitizer runtimes are only installed for the host width.
    if width == '-m64':
        command.append('-fsanitize=address,undefined')
    if source is not None:
        command.append(f'-D{source_macro}="{source}"')
    binary = str(Path(directory) / name)
    result = subprocess.run(command + ['-o', binary], capture_output=True, text=True)
    if result.returncode != 0:
        return None, result.stderr
    return binary, ''


def remove_block(path, start, end):
    """Returns the source of path with the lines from start up to end removed."""
    source = path.read_text()
    assert source.count(start) == 1, start
    first = source.index(start)
    last = source.index(end, first)
    return source[:first] + source[last:]


# Each mutation removes the z, t and j handling, which restores the original
# fault: the conversion is not consumed and the later arguments shift.
MUTATIONS = {
    'streamout-kernel': (STREAMOUT, "            else if (chr == _T('z')) {",
                         '            else break;'),
    'streamout-libc': (STREAMOUT, "            else if (chr == _T('z')) {",
                       '            else break;'),
    'acpica': (UTPRINT, "        else if (*Format == 'z' || *Format == 't' || *Format == 'j')",
               '        switch (*Format)'),
}

with tempfile.TemporaryDirectory(prefix='vali-log-format-') as directory:
    built = []
    for width in ('-m64', '-m32'):
        for engine, builder, defines, macro in ENGINES:
            name = f'{engine}{width[2:]}'
            binary, errors = build(directory, name, builder, defines, width)
            if binary is None:
                if width == '-m64':
                    raise RuntimeError(f'{name} build failed:\n{errors}')
                print(f'Skipped {name}: the host cannot build it at 32 bits')
                continue
            subprocess.run([binary], check=True, timeout=30,
                           env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            built.append((name, engine, builder, defines, width, macro))

    if args.mutation_check:
        for name, engine, builder, defines, width, macro in built:
            path, start, end = MUTATIONS[engine]
            mutant = Path(directory) / f'{name}-mutant.c'
            mutant.write_text(remove_block(path, start, end))
            binary, errors = build(directory, name + '-mutant', builder, defines, width,
                                   macro, mutant)
            if binary is None:
                raise RuntimeError(f'{name} mutant build failed:\n{errors}')
            result = subprocess.run([binary], capture_output=True, timeout=30,
                                    env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
            if result.returncode == 0:
                raise RuntimeError(f'Undetected mutation in {name}: z, t and j removed')
            print(f'Rejected mutation in {name}: z, t and j removed')
