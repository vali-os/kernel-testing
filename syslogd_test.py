"""Check the syslog service in services/syslogd on a POSIX host with ASan,
UBSan and the leak checker: the text grid, drawing on a framebuffer in
memory, the console, and the reader that walks the records returned by
OSKernelLogRead.

Set CC to the host compiler. --mutation-check builds changed copies of the
sources and fails if the test does not catch each change. When the Vali
compiler exists (VALI_CLANG, default /usr/local/valicc/bin/clang), the
sources are also compiled for i386, AMD64 and AArch64.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SYSLOGD = ROOT / 'services/syslogd'

# Sources linked into each of the two test binaries. The reader binary
# replaces the console and OSKernelLogRead with stubs.
CONSOLE_SOURCES = ('console/text.c', 'console/fb.c', 'console/font.c', 'console/console.c')
READER_SOURCES = ('reader.c',)

# The text grid decodes UTF-8 with mstr_next from libds. These libds sources,
# relative to librt/libds, are linked into the console binary. The generated
# tables are only needed by mstr_unicode.c and are built without -Werror, the
# same way testing/libds_utf8_test.py builds them. mstr_next itself is tested
# there.
LIBDS = ROOT / 'librt/libds'
LIBDS_SOURCE = 'mstring/unicode/mstr_unicode.c'
LIBDS_TABLE_SOURCES = (
    'mstring/unicode/mstr_conv.c',
    'mstring/unicode/mstr_digits.c',
    'mstring/unicode/mstr_props.c',
)

# Each mutation must make a check in syslogd_test.c fail or make a sanitizer
# stop the test.
MUTATIONS = (
    ('console/text.c',
     '    if (grid->CursorColumn == grid->Columns) {\n        __NewLine(grid);\n    }\n',
     '',
     'text past the right edge is written outside the row'),
    ('console/text.c',
     '    grid->FirstRow = (grid->FirstRow + 1) % grid->Rows;\n',
     '',
     'scrolling does not move the rows up'),
    ('console/text.c',
     '    __ClearRow(grid, grid->Rows - 1);\n    TextGridMarkAllDirty(grid);',
     '    __ClearRow(grid, grid->Rows - 1);',
     'scrolling does not redraw the moved rows'),
    ('console/text.c',
     '    __MarkDirty(grid, grid->CursorRow);\n',
     '    if (false) {\n        __MarkDirty(grid, grid->CursorRow);\n    }\n',
     'a written row is not redrawn'),
    ('console/fb.c',
     '        if (g_fontIndices[middle] < character) {',
     '        if (g_fontIndices[middle] > character) {',
     'the glyph search looks in the wrong half'),
    ('console/fb.c',
     '    return ((value * component->Mask) / 0xFF) << component->Position;',
     '    return value << component->Position;',
     'a color component is not scaled to its mask'),
    ('console/fb.c',
     '    if (video->Type != OSBOOTVIDEO_FRAMEBUFFER || video->DepthBits != 32 ||',
     '    if (video->Type != OSBOOTVIDEO_FRAMEBUFFER ||',
     'modes that are not 32 bit are accepted'),
    ('console/fb.c',
     '    if (top < framebuffer->Height) {',
     '    if (false) {',
     'clearing leaves the rows below the last text row'),
    ('console/fb.c',
     '        __CopyScratch(framebuffer, FRAMEBUFFER_MARGIN + row * g_fontHeight);',
     '        __CopyScratch(framebuffer, row * g_fontHeight);',
     'text is drawn over the top margin'),
    ('console/console.c',
     '    status = FramebufferCheckMode(&video);\n    if (status != OS_EOK) {\n        return status;\n    }\n',
     '',
     'a framebuffer is mapped in a mode that cannot be drawn on'),
    ('console/console.c',
     '    if (record->Level == OSSYSLOGLEVEL_TRACE ||',
     '    if (false ||',
     'trace messages are shown'),
    ('console/console.c',
     '    if (state == __CONSOLE_STATE_INHIBITED) {\n        return;\n    }',
     '',
     'an inhibited console still draws'),
    ('console/console.c',
     '        FramebufferClear(&g_console.Framebuffer);\n        TextGridMarkAllDirty(&g_console.Grid);',
     '        TextGridMarkAllDirty(&g_console.Grid);',
     'an uninhibit keeps what others drew on the screen'),
    ('reader.c',
     '    reader->NextSequence = result.NextSequence;\n',
     '',
     'the reader reads the same messages again'),
    ('reader.c',
     '    if (result.Dropped != 0) {\n        ConsoleShowDropped(result.Dropped);\n    }',
     '',
     'dropped messages are not reported'),
    ('reader.c',
     '        record->Size < sizeof(OSKernelLogRecord_t) + record->Length ||',
     '',
     'a record shorter than its text is accepted'),
    ('reader.c',
     "        text[record->Length] = '\\0';",
     '',
     'the padding after a message is shown as text'),
    ('reader.c',
     '        if (recordCount != 0) {',
     '        if (false) {',
     'the reader sleeps while a backlog is waiting'),
    ('reader.c',
     '    LogReaderDestroy(reader);\n    free(reader);\n}',
     '}',
     'the job leaks its reader'),
)


def host_flags():
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-DTESTING',
               '-DSERVICEAPI=static inline', '-DSERVICEABI=',
               '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g']
    for include in ('testing/include', 'librt/libos/include', 'librt/libddk/include',
                    'librt/libds/include'):
        command += ['-I', str(ROOT / include)]
    command += ['-idirafter', str(ROOT / 'librt/libc/include')]
    return command


def build_libds(output):
    compiler = [os.environ.get('CC', 'clang'), '-std=gnu11',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
                '-I', str(LIBDS / 'include')]
    objects = []
    for source in (LIBDS_SOURCE,) + LIBDS_TABLE_SOURCES:
        warnings = ['-Wall', '-Wextra', '-Werror'] if source == LIBDS_SOURCE else ['-w']
        target = output / (Path(source).stem + '.o')
        subprocess.run(compiler + warnings + ['-c', str(LIBDS / source), '-o', str(target)],
                       check=True)
        objects.append(str(target))
    return objects


def build(source_directory, output, libds):
    flags = host_flags()
    flags += ['-I', str(source_directory), '-I', str(source_directory / 'console')]
    test_source = str(ROOT / 'testing/syslogd_test.c')
    console = output / 'syslogd_console'
    reader = output / 'syslogd_reader'
    subprocess.run(flags + [test_source] +
                   [str(source_directory / source) for source in CONSOLE_SOURCES] +
                   libds + ['-o', str(console)], check=True)
    subprocess.run(flags + ['-DSYSLOGD_TEST_READER', test_source] +
                   [str(source_directory / source) for source in READER_SOURCES] +
                   ['-o', str(reader)], check=True)
    return console, reader


def run(binaries, capture):
    for binary in binaries:
        result = subprocess.run([str(binary)], capture_output=capture, timeout=60)
        if result.returncode != 0:
            return False
    return True


def cross_compile(output):
    compiler = os.environ.get('VALI_CLANG', '/usr/local/valicc/bin/clang')
    if not Path(compiler).exists():
        print('Vali compiler not found, cross-compilation skipped:', compiler, flush=True)
        return
    includes = []
    for include in ('librt/libc/include', 'librt/libos/include', 'librt/libddk/include',
                    'librt/libds/include', 'services/syslogd', 'services/syslogd/console'):
        includes += ['-I', str(ROOT / include)]
    for arch in ('i386', 'amd64', 'aarch64'):
        flags = [compiler, '--target=' + arch + '-uml-vali', '-std=gnu11', '-fms-extensions',
                 '-ffreestanding', '-nostdlibinc', '-O2', '-Wall', '-Wextra', '-Werror']
        if arch == 'aarch64':
            flags += ['-march=armv8-a', '-mno-outline-atomics']
        for source in CONSOLE_SOURCES + READER_SOURCES:
            subprocess.run(flags + includes + ['-c', str(SYSLOGD / source),
                           '-o', str(output / (Path(source).stem + '.o'))], check=True)
        print('syslogd production compilation:', arch, 'passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-syslogd-') as directory:
        output = Path(directory)
        cross_compile(output)
        libds = build_libds(output)
        if not run(build(SYSLOGD, output, libds), capture=False):
            raise RuntimeError('syslogd tests failed')
        if not args.mutation_check:
            return
        for name, before, after, reason in MUTATIONS:
            text = (SYSLOGD / name).read_text()
            assert text.count(before) == 1, before
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            shutil.copytree(SYSLOGD, mutant)
            (mutant / name).write_text(text.replace(before, after))
            if run(build(mutant, output, libds), capture=True):
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)


if __name__ == '__main__':
    main()
