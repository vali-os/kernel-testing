"""Test the standard streams of a process without inherited stdio on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIBC = ROOT / 'librt/libc'
PASSED = 'null stdio: write, read, seek, close and exit() without a fault passed'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()


def run(binary, scenario):
    """Returns True when the scenario ran to the end of the exit handlers."""
    try:
        result = subprocess.run([binary] + scenario, capture_output=True, text=True,
                                timeout=30)
    except subprocess.TimeoutExpired as timeout:
        # A scenario that never ends fails, for example an exit handler that
        # tries the same descriptor for ever.
        return False, subprocess.CompletedProcess(
            timeout.cmd, -1, stdout='', stderr='line 0: timed out after 30 seconds')
    return result.returncode == 0 and PASSED in result.stdout, result


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


def reason(result):
    """Returns the first failed check or sanitizer report of a failed run."""
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith('line ') or 'Sanitizer' in line or 'runtime error' in line:
            return line.strip()
    return 'exit code %d' % result.returncode


with tempfile.TemporaryDirectory(prefix='vali-null-stdio-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-Wno-incompatible-library-redeclaration',
             # streamout.c passes &va_list to format_float, which only works on
             # targets where va_list is not an array. The test formats no floats.
             '-Wno-incompatible-pointer-types',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libc/stdio/core')]
    sources = ['stdio/core/iod_bitmap.c', 'stdio/core/file_stream.c',
               'io/close.c', 'io/write.c', 'io/read.c', 'io/dup.c', 'io/chsize.c',
               'io/isatty.c', 'io/marktty.c', 'io/ioctl.c', 'stdio/fseek.c',
               'stdio/fclose.c', 'stdio/fflush.c', 'stdio/fputc.c', 'stdio/fputs.c',
               'stdio/fwrite.c', 'stdio/fgetc.c', 'stdio/feof.c', 'stdio/ferror.c',
               'stdio/ungetwc.c', 'stdio/ungetc.c', 'stdio/clearerr.c',
               'stdlib/wide/wctomb.c',
               'stdio/stream/vfprintf.c', 'stdio/stream/streamout.c']
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/libc_null_stdio_test.c')]
    command += [str(LIBC / source) for source in sources]
    command += [str(ROOT / 'librt/libds/hashtable.c'),
                str(ROOT / 'librt/libos/error_codes.c'), '-lm', '-o', binary]
    subprocess.run(command, check=True)
    # Every stream operation followed by exit(), and exit() alone. Calls on a
    # descriptor that is not open. Then exit() with more descriptors open: 13
    # or more made the descriptor table shrink while the exit handler still
    # enumerated it.
    scenarios = {'streams and exit': [], 'exit only': ['exit'],
                 'unknown descriptor': ['bad']}
    for count in (0, 12, 13, 30, 60):
        scenarios['exit with %d more' % count] = ['exit', str(count)]
        scenarios['close counts with %d more' % count] = ['count', str(count)]
        scenarios['exit with %d more, no memory' % count] = ['exit-nomem', str(count)]
    # A descriptor whose close fails stays open, and is tried once per pass.
    for count in (0, 30):
        scenarios['exit with %d more and one stuck' % count] = ['exit-stuck', str(count)]
        scenarios['exit with %d more and one stuck, no memory' % count] = [
            'exit-nomem-stuck', str(count)]
    for scenario, arguments in scenarios.items():
        passed, result = run(binary, arguments)
        if not passed:
            print(result.stdout + result.stderr)
            raise RuntimeError('%s: failed with code %d' % (scenario, result.returncode))
        print('%s: %s' % (scenario, result.stdout.strip()))

    if args.mutation_check:
        source = (LIBC / 'stdio/core/core.c').read_text()
        table = 'case NULL_SIGNATURE: return &g_stdioNullOps;'
        written = '    *bytesWritten = length;\n'
        endOfFile = '    *bytesRead = 0;\n    return OS_EOK;\n'
        seek = '    .seek = __null_seek,\n'
        mutations = {
            # The old table: NULL_SIGNATURE descriptors had no operations.
            'NULL_SIGNATURE without operations': source.replace(
                table, 'case NULL_SIGNATURE: return NULL;'),
            'null write reports a short write': source.replace(
                written, '    *bytesWritten = 0;\n'),
            'null write fails': source.replace(
                written, written + '    return OS_EDEVFAULT;\n'),
            'null read fails': source.replace(
                endOfFile, '    *bytesRead = 0;\n    return OS_EDEVFAULT;\n'),
            'null seek missing': source.replace(seek, ''),
        }
        # The exit handler from before the fix, which closed the descriptors
        # from inside hashtable_enumerate.
        oldCleanup = previous_version('librt/libc/stdio/core/core.c',
                                      '    entry->handle = NULL;\n')
        withoutList = '        return __close_io_descriptors_without_list(excludeFlags);\n'
        after = ' || entry->id <= context->after'
        exclude = '    return !(excludeFlags && (handle->XTFlags & excludeFlags));\n'
        mutations['no memory, nothing is closed'] = source.replace(
            withoutList, '        return 0;\n')
        mutations['no memory, a failed close is tried again'] = source.replace(after, '')
        mutations['priority descriptors close in the first pass'] = source.replace(
            exclude, '    return 1;\n')
        # Each of these must fail in the one scenario named with it.
        exitOnly = {
            'NULL_SIGNATURE without operations, exit only': (
                mutations['NULL_SIGNATURE without operations'], 'exit only'),
            'the old exit cleanup, 13 more descriptors': (oldCleanup, 'exit with 13 more'),
            'the old exit cleanup, 60 more descriptors': (oldCleanup, 'exit with 60 more'),
        }
        for anchor in (table, written, endOfFile, seek, withoutList, after, exclude):
            assert source.count(anchor) == 1, anchor
        mutant = Path(directory) / 'core-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + [f'-DSTDIO_CORE_SOURCE="{mutant}"'], check=True)
            for scenario, arguments in scenarios.items():
                passed, result = run(binary, arguments)
                if not passed:
                    break
            if passed:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: %s, %s (%s)' % (name, scenario, reason(result)))

        # Mutations of the other sources: each puts back a missing descriptor
        # check. They are built in place of the real file.
        others = {}
        writeCheck = ('    if (!handle) {\n        errno = EBADFD;\n        return -1;\n    }\n\n'
                      '    if (!handle->Ops->write) {')
        isattyCheck = '        errno = EBADF;\n        return 0;\n'
        ungetwcCheck = ('    if (!handle) {\n        // The stream\'s descriptor is not open. '
                        'fgetwc reports the same.\n        funlockfile(file);\n'
                        '        _set_errno(EBADFD);\n        return WEOF;\n    }\n\n')
        for path, anchor, replacement, name in (
                ('io/write.c', writeCheck, '    if (!handle->Ops->write) {',
                 'write() without the descriptor check'),
                ('io/isatty.c', isattyCheck, '        return EBADF;\n',
                 'isatty() returns EBADF for a descriptor that is not open'),
                ('stdio/ungetwc.c', ungetwcCheck, '',
                 'ungetwc() without the descriptor check')):
            text = (LIBC / path).read_text()
            assert text.count(anchor) == 1, anchor
            others[name] = (path, text.replace(anchor, replacement))
        for name, (path, body) in others.items():
            mutantSource = Path(directory) / ('mutant-' + Path(path).name)
            mutantSource.write_text(body)
            subprocess.run([str(mutantSource) if part == str(LIBC / path) else part
                            for part in command], check=True)
            for scenario, arguments in scenarios.items():
                passed, result = run(binary, arguments)
                if not passed:
                    break
            if passed:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: %s, %s (%s)' % (name, scenario, reason(result)))

        # The served crash at exit: exit() alone must fail on the old table.
        # The old exit cleanup must fail once more descriptors are open.
        for name, (body, scenario) in exitOnly.items():
            mutant.write_text(body)
            subprocess.run(command + [f'-DSTDIO_CORE_SOURCE="{mutant}"'], check=True)
            passed, result = run(binary, scenarios[scenario])
            if passed:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: %s (%s)' % (name, reason(result)))
