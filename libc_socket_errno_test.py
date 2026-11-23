"""Test the descriptor checks of libc's socket and IPC calls, and socketpair's cleanup."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LIBC = ROOT / 'librt/libc'
PASSED = 'libc sockets: bad descriptors, non-sockets and socketpair failures passed'
INET = ('accept.c', 'bind.c', 'connect.c', 'getpeername.c', 'getsockname.c',
        'getsockopt.c', 'listen.c', 'recv.c', 'send.c', 'setsockopt.c',
        'shutdown.c', 'socketatmark.c')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()


def run(binary):
    """Returns whether the test passed, and the completed process."""
    try:
        result = subprocess.run([binary], capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired as timeout:
        return False, subprocess.CompletedProcess(
            timeout.cmd, -1, stdout='', stderr='line 0: timed out after 30 seconds')
    return result.returncode == 0 and PASSED in result.stdout, result


def reason(result):
    """Returns the first failed check or sanitizer report of a failed run."""
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith('line ') or 'Sanitizer' in line or 'runtime error' in line:
            return line.strip()
    return 'exit code %d' % result.returncode


with tempfile.TemporaryDirectory(prefix='vali-libc-socket-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-Wno-incompatible-library-redeclaration',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libc/stdio/core')]
    sources = ['stdio/core/iod_bitmap.c', 'io/close.c', 'os/ipc.c',
               'inet/socket.c', 'inet/socketpair.c'] + ['inet/' + name for name in INET]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/libc_socket_errno_test.c')]
    command += [str(LIBC / source) for source in sources]
    command += [str(ROOT / 'librt/libds/hashtable.c'),
                str(ROOT / 'librt/libos/error_codes.c'), '-o', binary]
    subprocess.run(command, check=True)
    passed, result = run(binary)
    if not passed:
        print(result.stdout + result.stderr)
        raise RuntimeError('libc socket test failed with code %d' % result.returncode)
    print(result.stdout.strip())

    if args.mutation_check:
        badDescriptor = ('    if ({0} == NULL) {{\n        _set_errno(EBADF);\n'
                         '        return -1;\n    }}\n\n')
        mutations = {}
        # Each socket call without the check for a descriptor that is not open,
        # which is the old code: it reported ENOTSOCK.
        for name in INET:
            variable = 'source' if name == 'accept.c' else 'handle'
            mutations[name + ' reports ENOTSOCK for a bad descriptor'] = (
                'inet/' + name, badDescriptor.format(variable), '')
        mutations['ipsend reports ENOTSOCK for a bad descriptor'] = (
            'os/ipc.c', badDescriptor.format('handle'), '')
        mutations['iprecv reports EINVAL for a bad descriptor'] = (
            'os/ipc.c', '    if (handle == NULL) {\n        _set_errno(EBADF);\n'
                        '        goto exit;\n    }\n',
            '    if (handle == NULL) {\n        _set_errno(EINVAL);\n        goto exit;\n    }\n')
        mutations['ipsend reports EBADF for a descriptor that is not IPC'] = (
            'os/ipc.c', '        _set_errno(ENOTSOCK);\n        return -1;\n',
            '        _set_errno(EBADF);\n        return -1;\n')
        # The old socketpair closed descriptor -1 when the second socket failed.
        mutations['socketpair closes the failed descriptor'] = (
            'inet/socketpair.c', '        __close_pair_socket(iods[0]);\n        iods[0] = -1;\n'
                                 '        return -1;\n',
            '        __close_pair_socket(iods[1]);\n        iods[0] = -1;\n        return -1;\n')
        mutations['socketpair keeps both sockets when pairing fails'] = (
            'inet/socketpair.c', '        __close_pair_socket(iods[1]);\n'
                                 '        __close_pair_socket(iods[0]);\n', '')
        mutations['socketpair loses errno while it closes'] = (
            'inet/socketpair.c', '    _set_errno(error);\n', '    (void)error;\n')
        for name, (path, anchor, replacement) in mutations.items():
            text = (LIBC / path).read_text()
            assert text.count(anchor) == 1, (name, anchor)
            mutant = Path(directory) / ('mutant-' + Path(path).name)
            mutant.write_text(text.replace(anchor, replacement))
            subprocess.run([str(mutant) if part == str(LIBC / path) else part
                            for part in command], check=True)
            passed, result = run(binary)
            if passed:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: %s (%s)' % (name, reason(result)))
