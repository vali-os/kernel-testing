"""Check the kernel log ring buffer in kernel/output/log.c and
SpinlockTryAcquireIrq in kernel/sync/spinlock.c on a POSIX host with ASan and
UBSan.

Set CC to the compiler. --mutation-check builds changed copies of the sources
and fails if the test does not catch each change.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOG_SOURCE = ROOT / 'kernel/output/log.c'
SPINLOCK_SOURCE = ROOT / 'kernel/sync/spinlock.c'

# A mutant that hangs (for example by waiting on a stuck lock) counts as
# caught once this many seconds have passed.
MUTANT_TIMEOUT = 30

# Each mutation must make an assertion in kernel_log_test.c fail, make the
# sanitizers stop the test, or make the test hang.
MUTATIONS = (
    (LOG_SOURCE, 'LOG_SOURCE',
     '        atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_IDLE);\n'
     '        if (!__HasPendingOutput()) {\n            return;\n        }',
     '        atomic_store(&g_kernelLog.OutputState, __LOG_OUTPUT_IDLE);\n'
     '        return;',
     'a message added while giving up OutputState is never printed'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    if (bytesToEnd < recordSize) {',
     '    if (false) {',
     'a record is written across the end of the buffer'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    if (record->Level != __LOG_PADDING_LEVEL) {\n        log->TailSequence++;\n    }',
     '    log->TailSequence++;',
     'dropping a padding record changes the oldest sequence number'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '        dropped  = g_kernelLog.TailSequence - sequence;',
     '        dropped  = 0;',
     'LogRead does not report dropped messages'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    if (log->OutputPosition >= log->Tail) {\n        return;\n    }',
     '    return;',
     'the output cursor keeps pointing at a dropped record'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    } else if (written > OSKERNELLOG_MESSAGE_MAX) {\n        written = OSKERNELLOG_MESSAGE_MAX;\n    }',
     '    }',
     'the full length of a cut off message is stored'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '        if (SpinlockTryAcquireIrq(&g_kernelLog.SyncObject) == OS_EOK) {',
     '        SpinlockAcquireIrq(&g_kernelLog.SyncObject);\n        if (true) {',
     'panic mode waits on a stuck lock'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    // for OutputState. Print whatever has not been printed yet.\n    __PrintPendingRecords();',
     '    // for OutputState. Print whatever has not been printed yet.',
     'panic mode does not print waiting messages'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    upgraded.HeadSequence   = g_kernelLog.TailSequence;',
     '    upgraded.HeadSequence   = 0;',
     'moving to the full buffer renumbers the messages'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '        if (record->Sequence == g_kernelLog.OutputSequence) {\n'
     '            upgraded.OutputPosition = upgraded.Head;\n        }',
     '',
     'moving to the full buffer loses the output cursor'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    if (level >= sizeof(g_levelNames) / sizeof(g_levelNames[0])) {',
     '    if (false) {',
     'an unknown level reads past the name table'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '    while (__SerialAvailable()) {',
     '    while (true) {',
     'messages are marked printed before any output exists'),
    (LOG_SOURCE, 'LOG_SOURCE',
     '        irqState = InterruptDisable();\n        __PrintPendingRecords();',
     '        irqState = 0;\n        __PrintPendingRecords();',
     'printing runs with interrupts enabled'),
    (SPINLOCK_SOURCE, 'SPINLOCK_SOURCE',
     '        InterruptRestoreState(irqState);\n        return OS_EBUSY;',
     '        return OS_EBUSY;',
     'a failed try leaves interrupts disabled'),
    (SPINLOCK_SOURCE, 'SPINLOCK_SOURCE',
     '    expected = current;',
     '    expected = atomic_load(&spinlock->Next);',
     'a try takes a ticket behind the holder'),
)


def compile_command(binary):
    command = [os.environ.get('CC', 'clang'), '-std=gnu11', '-fms-extensions',
               '-DMOLLENOS', '-DVALI', '-D__LIBDS_KERNEL__', '-DKERNELAPI=',
               '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=', '-D__time64_t=int64_t',
               '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-Wno-unused-function', '-Wno-sign-compare', '-Wno-macro-redefined',
               '-Wno-unknown-warning-option', '-g', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-pthread', '-fuse-ld=lld']
    for include in ('kernel/include', 'kernel/components', 'boot/include',
                    'librt/libc/include', 'librt/libos/include', 'librt/libddk/include',
                    'librt/libds/include'):
        command += ['-I', str(ROOT / include)]
    command += [str(ROOT / 'testing/kernel_log_test.c'), '-o', str(binary)]
    return command


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-kernel-log-') as directory:
        output = Path(directory)
        binary = output / 'test'
        command = compile_command(binary)
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
        if not args.mutation_check:
            return
        for source, macro, before, after, reason in MUTATIONS:
            text = source.read_text()
            assert text.count(before) == 1, before
            mutant = output / source.name
            mutant.write_text(text.replace(before, after))
            subprocess.run(command + [f'-D{macro}="{mutant}"'], check=True)
            try:
                result = subprocess.run([str(binary)], capture_output=True,
                                        timeout=MUTANT_TIMEOUT)
                if result.returncode == 0:
                    raise RuntimeError('Undetected regression: ' + reason)
            except subprocess.TimeoutExpired:
                pass
            print('Rejected mutation: ' + reason, flush=True)


if __name__ == '__main__':
    main()
