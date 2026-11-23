"""Test how processd ends a process: terminate, exit watcher, joins and crash reports."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PASSED = 'processd exit: crash report after terminate, joins, signals, lookups and the exit watcher passed'
PROCESS = 'services/processd/process.c'
REQUESTS = 'services/processd/requests.c'

# Each mutation must make the test fail. (source, text, replacement)
MUTATIONS = {
    # The old behaviour: the record left the table at the terminate.
    'terminate removes the process': (
        PROCESS, '    PmReleaseProcess(process);\n\n    __StartExitWatcher();\n    return OS_EOK;\n',
        '    PmReleaseProcess(process);\n    usched_mtx_lock(&g_processesLock);\n'
        '    hashtable_remove(&g_processes, &(struct process_entry) { .process_id = processHandle });\n'
        '    usched_mtx_unlock(&g_processesLock);\n    PmReleaseProcess(process);\n    return OS_EOK;\n'),
    'second terminate changes the exit code': (
        PROCESS, '    if (process->state != ProcessState_RUNNING) {\n        // A second report',
        '    if (false) {\n        // A second report'),
    'joiners are not woken at terminate': (
        PROCESS, '    usched_cnd_notify_all(&process->condition);\n    usched_mtx_unlock(&process->mutex);\n'
                 '    PmReleaseProcess(process);\n\n    __StartExitWatcher();',
        '    usched_mtx_unlock(&process->mutex);\n    PmReleaseProcess(process);\n\n    __StartExitWatcher();'),
    'watcher ignores a thread that is gone': (
        PROCESS, '    if (__PrimaryThreadGone(process)) {', '    if (false) {'),
    'watcher has no deadline': (
        PROCESS, '    expired = __DeadlinePassed(&sweep->Now, &process->exit_deadline);',
        '    expired = false;'),
    'a failed thread lookup keeps the process': (
        PROCESS, '    return true;\n}\n\n// hashtable_enumerate', '    return oserr == OS_ENOENT;\n}\n\n// hashtable_enumerate'),
    'watcher stops while processes remain': (
        PROCESS, '    if (sweep.Remaining == 0) {', '    if (true) {'),
    'processes past one batch are not counted': (
        PROCESS, '    if (sweep->Count == __EXIT_BATCH_SIZE) {\n        sweep->Remaining++;\n',
        '    if (sweep->Count == __EXIT_BATCH_SIZE) {\n'),
    'a second watcher is queued per terminate': (
        PROCESS, '    if (g_exitWatcher == ExitWatcherState_QUEUED) {', '    if (false) {'),
    'a failed queue marks the watcher queued': (
        PROCESS, '        ERROR("PmTerminateProcess failed to start the exit watcher");\n        return;\n',
        '        ERROR("PmTerminateProcess failed to start the exit watcher");\n'
        '        usched_mtx_lock(&g_processesLock);\n        g_exitWatcher = ExitWatcherState_QUEUED;\n'
        '        usched_mtx_unlock(&g_processesLock);\n        return;\n'),
    'crash report refuses a terminating process': (
        REQUESTS, '    oserr = PmReferenceProcessByHandle(processId, &process);\n    if (oserr != OS_EOK) {\n'
                  '        ERROR("sys_process_report_crash_invocation no process',
        '    oserr = PmReferenceProcessByHandle(processId, &process);\n'
        '    if (oserr == OS_EOK && process->state != ProcessState_RUNNING) {\n'
        '        PmReleaseProcess(process);\n        oserr = OS_ENOENT;\n    }\n    if (oserr != OS_EOK) {\n'
        '        ERROR("sys_process_report_crash_invocation no process'),
    'crash report keeps its reference': (
        REQUESTS, '    PmReleaseProcess(process);\n    if (oserr != OS_EOK) {\n        ERROR("sys_process_report_crash_invocation failed',
        '    if (oserr != OS_EOK) {\n        ERROR("sys_process_report_crash_invocation failed'),
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()


def run(binary, environment):
    """Returns whether the test passed, and the completed process."""
    try:
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env=environment, timeout=30)
    except subprocess.TimeoutExpired:
        return False, subprocess.CompletedProcess([str(binary)], -1, '', 'timed out')
    return result.returncode == 0 and PASSED in result.stdout, result


def reason(result):
    """Returns the first failed assertion or sanitizer report of a failed run."""
    for line in (result.stdout + result.stderr).splitlines():
        if 'Assertion' in line or 'Sanitizer' in line or 'runtime error' in line:
            return line.strip()
    return 'exit code %d' % result.returncode


with tempfile.TemporaryDirectory(prefix='vali-processd-exit-') as directory:
    output = Path(directory)
    subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                    '--service', str(ROOT / 'protocols/process.gr'), '--out', directory,
                    '--lang-c', '--client', '--server'], check=True)
    # The same stand-ins for libc's internal headers as process_startup_test.
    (output / 'internal').mkdir()
    (output / 'internal/_io.h').write_text('''
#pragma once
#include <stdio.h>
#include <unistd.h>
#include <os/types/process.h>
struct InheritanceOptions { unsigned int Flags; int StdOutHandle; int StdInHandle; int StdErrHandle; };
typedef struct StdioDescriptor stdio_handle_t;
#define __IO_PRIORITY 0x8000
stdio_handle_t* stdio_handle_get(int);
void stdio_handle_set_flags(stdio_handle_t*, unsigned int);
struct inheritance_export { uuid_t* controls; uint32_t count; uint32_t length; };
struct sys_process_inheritance_input;
oserr_t CRTWriteInheritanceBlock(const struct InheritanceOptions*, void*, uint32_t, struct inheritance_export*);
void CRTReleaseInheritanceBlock(struct inheritance_export*);
oserr_t CRTReadInheritanceBlock(const void*, uint32_t);
''')
    (output / 'internal/_syscalls.h').write_text('''
#include <os/types/thread.h>
#include <os/types/grants.h>
oserr_t Syscall_HandleImport(const GrantToken_t*, uuid_t, uuid_t*);
oserr_t Syscall_ThreadCreate(uintptr_t, void*, OSThreadParameters_t*, uuid_t*);
oserr_t Syscall_ThreadDetach(uuid_t);
oserr_t Syscall_ThreadSignal(uuid_t, int);
oserr_t Syscall_GetMemorySpaceForThread(uuid_t, uuid_t*);
''')
    (output / 'internal/_utils.h').write_text(
        (ROOT / 'librt/libc/include/internal/_utils.h').read_text())
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DMOLLENOS', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-D_In_Opt_=', '-D_Out_Opt_=', '-DTIME_MONOTONIC=TIME_UTC',
             '-fms-extensions', '-fasm-blocks', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
             '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
             '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
             # The debugger prints addresses with the Vali inttypes.h macro.
             '-DPRIxIN="lx"', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/processd/include', 'librt/libos/include',
               'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include')]
    sources = ['testing/processd_exit_test.c', 'services/processd/startup.c',
               'services/processd/debugger/debugger.c', 'librt/libds/hashtable.c']
    binary = output / 'test'
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    command = flags + [str(ROOT / source) for source in sources] + ['-o', str(binary)]
    subprocess.run(command, check=True)
    passed, result = run(binary, environment)
    if not passed:
        print(result.stdout + result.stderr)
        raise RuntimeError('processd exit test failed: ' + reason(result))
    print(result.stdout.strip())

    if args.mutation_check:
        for name, (path, before, after) in MUTATIONS.items():
            text = (ROOT / path).read_text()
            assert text.count(before) == 1, name
            mutant = output / ('mutant-' + Path(path).name)
            mutant.write_text(text.replace(before, after))
            if path == PROCESS:
                # process.c is included through PROCESSD_SOURCE.
                mutated = flags + ['-DPROCESSD_SOURCE="%s"' % mutant,
                                   '-I' + str(ROOT / 'services/processd')]
                mutated += [str(ROOT / source) for source in sources] + ['-o', str(binary)]
            else:
                # requests.c is included by the startup server test. A copy
                # next to it would not be found, so the include is redirected.
                server = (ROOT / 'testing/process_startup_server_test.c').read_text()
                server = server.replace('#include "../services/processd/requests.c"',
                                        '#include "%s"' % mutant)
                server = server.replace('#include PROCESSD_SOURCE',
                                        '#include "%s"' % (ROOT / PROCESS))
                (output / 'process_startup_server_test.c').write_text(server)
                test = (ROOT / 'testing/processd_exit_test.c').read_text()
                (output / 'processd_exit_test.c').write_text(test)
                mutated = flags + [str(output / 'processd_exit_test.c')]
                mutated += [str(ROOT / source) for source in sources[1:]] + ['-o', str(binary)]
            subprocess.run(mutated, check=True)
            passed, result = run(binary, environment)
            if passed:
                raise RuntimeError('Undetected mutation: ' + name)
            print('Rejected mutation: %s (%s)' % (name, reason(result)))
