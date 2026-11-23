"""Fail filed's requests to filesystem drivers at each step, with the real driver
interface (interfaces/driver_ops.c) and the VFS code that sends the requests,
on a POSIX host with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FILED = 'services/filed/'
DRIVER = FILED + 'interfaces/driver_ops.c'
FILESYSTEM = FILED + 'filesystem.c'
# Production sources compiled as their own translation units. filesystem.c and
# storage/utils.c are included by the test itself.
SOURCES = [FILED + path for path in (
    'vfs/vfs.c', 'vfs/utils.c', 'vfs/bind.c', 'vfs/mount.c', 'vfs/memfs.c',
    'vfs/handlers/mkdir.c', 'vfs/handlers/open.c', 'vfs/handlers/close.c',
    'vfs/handlers/unlink.c', 'vfs/handlers/handle_stat.c', 'vfs/handlers/stat.c',
    'interfaces/driver_ops.c')]
# The libds sources are not under test. They are built once, without -Werror.
LIBRARY_SOURCES = ['librt/libds/' + path for path in (
    'hashtable.c', 'guid.c',
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


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-filed-driver-ops-') as directory:
    output = Path(directory)
    for service in ('process', 'file', 'device', 'storage', 'usb', 'contracts/filesystem',
                    'contracts/usbhost', 'contracts/storage'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    # driver_ops.c takes GetGrachtClient from the libc internal header. Only
    # that header is used, as the rest of the libc headers would replace the
    # host's.
    (output / 'libc-internal' / 'internal').mkdir(parents=True)
    (output / 'libc-internal' / 'internal' / '_utils.h').write_text(
        (ROOT / 'librt/libc/include/internal/_utils.h').read_text())
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
             '-Wno-unused-parameter', '-Wno-unused-but-set-parameter', '-Wno-sign-compare',
             '-Wno-unknown-warning-option', '-Wno-missing-field-initializers',
             '-Wno-unused-variable', '-Wno-implicit-fallthrough', '-Wno-type-limits',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-g', '-pthread',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + directory]
    # memfs packs directory entries back to back (__ReadDirectoryEntry), and
    # so does the driver double, so the second entry is not 8-byte aligned.
    flags += ['-fno-sanitize=alignment']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/filed/include', 'librt/libos/include',
               'librt/libusb/include', 'librt/libddk/include', 'librt/libds/include',
               'librt/libgracht/include')]
    flags += ['-I' + str(output / 'libc-internal')]
    binary = str(output / 'test')

    library = []
    for index, path in enumerate(LIBRARY_SOURCES):
        library.append(str(output / f'library{index}.o'))
        subprocess.run([flag for flag in flags if flag != '-Werror'] +
                       ['-w', '-c', str(ROOT / path), '-o', library[-1]], check=True)

    def build(replacements=None, defines=()):
        sources = [str(ROOT / 'testing/filed_driver_ops_test.c')]
        sources += [(replacements or {}).get(path, str(ROOT / path)) for path in SOURCES]
        subprocess.run(flags + list(defines) + sources + library + ['-o', binary], check=True)

    # Known string leaks outside this test: the root node name from
    # __StatRootNode, the name VFSNodeCreateChild clones before VFSNodeChildNew
    # clones it again, and the label from_fsstat makes for __GetLabel. The root
    # scope stays allocated until the test ends. Any other leak fails the test.
    suppressions = output / 'lsan.supp'
    suppressions.write_text('leak:__StatRootNode\nleak:VFSNodeCreateChild\n'
                            'leak:__CreateRootScope\nleak:from_fsstat\n')
    environment = dict(os.environ)
    environment['LSAN_OPTIONS'] = 'suppressions=' + str(suppressions)

    build()
    subprocess.run([binary], check=True, env=environment, timeout=120)

    # Part three: the same requests through the real gracht client, with an
    # error answer from the driver (testing/filed_driver_gracht_test.c). The
    # client runtime is compiled from libgracht, which normally generates
    # config.h with CMake.
    (output / 'gracht-config').mkdir()
    (output / 'gracht-config' / 'config.h').write_text(
        '#ifndef __GRACHT_CONFIG_H__\n#define __GRACHT_CONFIG_H__\n'
        '#define HAVE_PTHREAD\n#endif\n')
    GRACHT = 'librt/libgracht/runtime/'
    GRACHT_HEADER = 'librt/libos/include/internal/_gracht.h'
    grachtBinary = str(output / 'gracht-test')
    grachtFlags = flags + ['-I' + str(output / 'gracht-config')]

    # The rest of the client runtime is not under test. It needs alloca, which
    # _POSIX_C_SOURCE hides, so it is built once with the default features.
    runtime = []
    for path in ('client_config.c', 'buffer_pool.c', 'stream_pool_registry.c', 'shared.c',
                 'hashtable.c', 'control.c', 'crc.c', 'stack.c'):
        runtime.append(str(output / ('gracht-' + path.replace('.c', '.o'))))
        subprocess.run([flag for flag in grachtFlags
                        if flag not in ('-D_POSIX_C_SOURCE=200809L', '-Werror')] +
                       ['-w', '-D_DEFAULT_SOURCE', '-c', str(ROOT / GRACHT / path),
                        '-o', runtime[-1]], check=True)

    def build_gracht(client=None, header=None, defines=()):
        """Builds part three, with other copies of client.c or _gracht.h."""
        command = list(grachtFlags) + list(defines)
        if header is not None:
            # The copy of internal/_gracht.h is found before the real one.
            (output / 'gracht-header' / 'internal').mkdir(parents=True, exist_ok=True)
            (output / 'gracht-header' / 'internal' / '_gracht.h').write_text(header)
            command.insert(1, '-I' + str(output / 'gracht-header'))
        if client is not None:
            mutant = output / 'gracht-client.c'
            mutant.write_text(client)
            command.append(f'-DGRACHT_CLIENT_SOURCE="{mutant}"')
        command += [str(ROOT / 'testing/filed_driver_gracht_test.c'), str(ROOT / DRIVER),
                    str(output / 'ctt_filesystem_service_client.c')]
        subprocess.run(command + library + runtime + ['-o', grachtBinary], check=True)
        if header is not None:
            (output / 'gracht-header' / 'internal' / '_gracht.h').unlink()

    def run_gracht():
        """Returns how part three ended: None when it passed."""
        try:
            result = subprocess.run([grachtBinary], capture_output=True, text=True,
                                    env=environment, timeout=60)
        except subprocess.TimeoutExpired:
            return 'timed out'
        if result.returncode != 0:
            return 'failed with code %d: %s' % (
                result.returncode, (result.stderr.strip().splitlines() or [''])[-1])
        print(result.stdout.strip(), flush=True)
        return None

    def previous_gracht(path, marker):
        """Returns the newest committed libgracht file without marker."""
        repository = str(ROOT / 'librt/libgracht')
        commits = subprocess.run(['git', '-C', repository, 'log', '--format=%H', '--', path],
                                 capture_output=True, text=True, check=True).stdout.split()
        for commit in commits:
            text = subprocess.run(['git', '-C', repository, 'show', commit + ':' + path],
                                  capture_output=True, text=True, check=True).stdout
            if marker not in text:
                return text
        raise RuntimeError('every version of %s contains %r' % (path, marker))

    build_gracht()
    failure = run_gracht()
    if failure is not None:
        raise RuntimeError('requests through gracht: ' + failure)

    # The client from before the fix refuses the abandon after a wait. Each
    # refusal must be logged, and each request stays in the client.
    oldClient = previous_gracht('runtime/client.c', '__await_unlink')
    build_gracht(client=oldClient, defines=['-DGRACHT_ABANDON_REFUSED'])
    failure = run_gracht()
    if failure is not None:
        raise RuntimeError('requests through the old gracht client: ' + failure)
    if args.mutation_check:
        old = previous_version(DRIVER, '    gracht_client_await(GetGrachtClient(), &msg.base, GRACHT_AWAIT_ASYNC);\n'
                                       '    ctt_filesystem_close_result(')
        close_wait = ('    oserr = __AwaitAnswer(interface, "__DriverClose", &msg);\n'
                      '    if (oserr != OS_EOK) {\n        return oserr;\n    }\n')
        mutations = [
            # The last committed wrappers that did not check the wait and the
            # reading of the answer.
            (DRIVER, None, old, 'the old driver wrappers'),
            (DRIVER, (('    oserr = __GrachtAwaitCompleted(GetGrachtClient(), &msg->base, GRACHT_AWAIT_ASYNC);',
                       '    oserr = gracht_client_await(GetGrachtClient(), &msg->base, GRACHT_AWAIT_ASYNC) ?\n'
                       '            OS_EUNKNOWN : OS_EOK;'),),
             None, 'an error answer is read as an answer'),
            (DRIVER, ((close_wait, '    (void)__AwaitAnswer(interface, "__DriverClose", &msg);\n'),),
             None, 'the close reads its answer after a failed wait'),
            (DRIVER, (('    if (status != 0) {\n        return __RequestNotSent(interface, "__DriverOpen");\n    }\n',
                       '    (void)status;\n'),),
             None, 'the open waits for a request that was not sent'),
            (DRIVER, (('        return __AnswerUnreadable(interface, "__DriverStat");',
                       '        oserr = OS_EOK;'),),
             None, 'the stat uses an answer it could not read'),
            (DRIVER, (('    if (oserr == OS_EOK) {\n        *absolutePositionOut = position;\n    }\n',
                       '    *absolutePositionOut = position;\n'),),
             None, 'the seek sets its position on a driver error'),
            (FILESYSTEM, (('        if (interface->Operations.Destroy) {\n'
                           '            destroyStatus = interface->Operations.Destroy(interface, fileSystem->Data, 0);',
                           '        if (false) {\n'
                           '            destroyStatus = interface->Operations.Destroy(interface, fileSystem->Data, 0);'),),
             None, 'a filesystem the VFS could not be made for is kept in the driver'),
            (FILESYSTEM, (('            WARNING("__DestroyDriver failed to destroy filesystem %u: %u", fileSystem->ID, oserr);\n',
                           ''),),
             None, 'a failed destroy is not logged'),
        ]
        # Part three. The old client leaves the requests behind, and a failed
        # abandon must be logged.
        header = (ROOT / GRACHT_HEADER).read_text()
        logged = ('        if (gracht_client_abandon(client, context) != 0) {\n'
                  '            ERROR("__GrachtAwaitCompleted: failed to abandon request %u: errno %i",\n'
                  '                  context->message_id, errno);\n'
                  '        }\n')
        assert header.count(logged) == 1
        unlink = '    __await_unlink(client, contexts, awaiter);\n'
        clientText = (ROOT / GRACHT / 'client.c').read_text()
        assert clientText.count(unlink) == 1
        grachtMutations = [
            ({'client': oldClient}, 'the gracht client from before the fix'),
            ({'client': clientText.replace(unlink, '')},
             'the await does not end the link to its awaiter'),
            ({'client': oldClient, 'header': header.replace(
                logged, '        gracht_client_abandon(client, context);\n'),
              'defines': ['-DGRACHT_ABANDON_REFUSED']},
             'a failed abandon is not logged'),
        ]
        for arguments, reason in grachtMutations:
            build_gracht(**arguments)
            failure = run_gracht()
            if failure is None:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: %s (%s)' % (reason, failure), flush=True)

        for path, edits, text, reason in mutations:
            if text is None:
                text = (ROOT / path).read_text()
                for before, after in edits:
                    assert text.count(before) == 1, before
                    text = text.replace(before, after)
            mutant = output / ('mutant_' + Path(path).name)
            mutant.write_text(text)
            if path == FILESYSTEM:
                build(defines=[f'-DFILED_FILESYSTEM_SOURCE="{mutant}"'])
            else:
                build(replacements={path: str(mutant)})
            try:
                result = subprocess.run([binary], capture_output=True, env=environment,
                                        timeout=60)
                survived = result.returncode == 0
                how = 'failed with code %d' % result.returncode
            except subprocess.TimeoutExpired:
                survived = False
                how = 'timed out'
            if survived:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: %s (%s)' % (reason, how), flush=True)
