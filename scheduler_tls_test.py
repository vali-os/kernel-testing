"""Execute scheduler TLS catch-up and native-stack handoff under QEMU.

Uses actual scheduler, TLS registry, TLS switching and ARM64 context assembly.
Kernel notifications, mutexes and allocation are deterministic test adapters.
The handoff fixture also runs production job, mutex and condition routines.
Execution-unit interleavings are deterministic; this is not a multicore stress test.
"""
import argparse
import json
from pathlib import Path
import resource
import struct
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--bin', type=Path, required=True, help='Vali LLVM tool binaries')
p.add_argument('--llvm', type=Path, required=True, help='Vali LLVM source with PE fixture mapper')
p.add_argument('--out', type=Path, required=True)
p.add_argument('--qemu', default='qemu-aarch64')
p.add_argument('--mutation-check', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.bin, a.llvm, a.out = a.bin.resolve(), a.llvm.resolve(), a.out.resolve()
a.out.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
sys.path.insert(0, str(a.llvm / 'libunwind/utils'))
from vali_pe import PE

def run(name, args, expected=0, must_fail=False):
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=60)
    (a.out / (name + '.log')).write_text(' '.join(map(str, args)) + '\n' + result.stdout)
    if (must_fail and result.returncode == 0) or (not must_fail and result.returncode != expected):
        raise RuntimeError(f'{name}: exit {result.returncode}, expected {expected}\n{result.stdout}')
    return result.stdout

headers = run('resource', [a.bin / 'clang', '-print-resource-dir']).strip()
flags = ['--target=aarch64-uml-vali', '-O2', '-ffreestanding', '-nostdinc',
         '-fms-extensions', '-ffunction-sections', '-fdata-sections', '-I' + headers + '/include']
flags += ['-I' + str(root / f'librt/{lib}/include') for lib in ['libc', 'libos', 'libddk', 'libds']]
def compile(source, name, extra=()):
    obj = a.out / (name + '.obj')
    run(name, [a.bin / 'clang', *flags, *extra, '-c', source, '-o', obj])
    return obj

objects = [compile(root / src, name) for name, src in [
    ('tls', 'librt/libc/os/tls.c'), ('modules', 'librt/libc/os/tls_modules.c'),
    ('spinlock', 'librt/libos/spinlock.c'), ('setjmp', 'librt/libc/arch/aarch64/_setjmp.S'),
    ('context', 'librt/libos/uthreads/aarch64/context.S'),
    ('platform', 'testing/scheduler_tls_platform.c')]]
objects.append(compile(a.llvm / 'compiler-rt/lib/builtins/aarch64/chkstk.S', 'chkstk'))
tls_unavailable = compile(root / 'testing/scheduler_tls_unavailable.S', 'tls-unavailable')
handoff_unavailable = compile(root / 'testing/scheduler_handoff_unavailable.S', 'handoff-unavailable')
xunit_unavailable = compile(root / 'testing/scheduler_xunit_unavailable.S', 'xunit-unavailable')
# Compile every production translation unit independently for each supported ABI.
for arch in ['aarch64', 'x86_64', 'i686']:
    for source in sorted((root / 'librt/libos/uthreads').glob('*.c')):
        compile(source, source.stem + '-' + arch,
                ['--target=' + arch + '-uml-vali', '-D__OSLIB_C_IMPLEMENTATION'])
def map_image(image, base, name):
    pe = PE(image, base)
    pe.resolve({})
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    hdr = ident + struct.pack('<HHIQQQIHHHHHH', 2, 183, 1, pe.entry,
                              64, 0, 0, 64, 56, 1, 0, 0, 0)
    size = len(pe.mapped)
    phdr = struct.pack('<IIQQQQQQ', 1, 7, 4096, base, base, size, size, 4096)
    elf = a.out / f'{name}-{base:x}.elf'
    elf.write_bytes((hdr + phdr).ljust(4096, b'\0') + pe.mapped)
    elf.chmod(0o755)
    return elf


def link_image(name, test, unavailable):
    image = a.out / (name + '.exe')
    run(name + '-link', [a.bin / 'lld', '-flavor', 'link', '-lldvpe',
        '/machine:arm64', '/nodefaultlib', '/subsystem:console', '/opt:ref',
        '/entry:entry', '/out:' + str(image), test, unavailable, *objects])
    return image


results = []
for case in ['normal', 'FAIL_CATCHUP', 'handoff', 'shared-waits', 'xunit']:
    source = {
        'handoff': 'scheduler_handoff_test.c',
        'shared-waits': 'scheduler_handoff_test.c',
        'xunit': 'scheduler_xunit_test.c',
    }.get(case, 'scheduler_tls_test.c')
    test = compile(root / 'testing' / source, case,
                   ['-DFAIL_CATCHUP'] if case == 'FAIL_CATCHUP' else
                   ['-DSHARED_WAITS'] if case == 'shared-waits' else [])
    unavailable = {
        'handoff': handoff_unavailable,
        'shared-waits': handoff_unavailable,
        'xunit': xunit_unavailable,
    }.get(case, tls_unavailable)
    image = link_image(case, test, unavailable)
    imports = run(case + '-imports', [a.bin / 'llvm-readobj', '--coff-imports', image])
    assert 'Import {' not in imports, imports
    for base in [0x180000000, 0x190000000, 0x170000000]:
        elf = map_image(image, base, case)
        run(elf.stem, [a.qemu, '-cpu', 'cortex-a53', elf],
            expected=-5 if case == 'FAIL_CATCHUP' else 0)
        results.append({'case': case, 'base': hex(base), 'result': 'PASS'})
if a.mutation_check:
    source = (root / 'librt/libos/uthreads/scheduler.c').read_text()
    replacements = {
        'completion-winner': ('if (wait->Result != UschedWaitStatus_PENDING) {', 'if (false) {'),
        'signal-removes-timer': ('    __remove_timer_locked(wait);', '    (void)wait;'),
        'syscall-unlink': ('            *link = syscall->next;', '            link = &syscall->next;'),
        'retire-with-waits': ('if (atomic_load(&execution->stop_requested)) {',
                              'if (atomic_load(&execution->stop_requested) && '
                              'execution->scheduler->Timers == NULL && '
                              'execution->scheduler->syscalls_pending == NULL) {'),
    }
    for name, (before, after) in replacements.items():
        assert source.count(before) == 1, name
        mutant = a.out / (name + '.c')
        mutant.write_text(source.replace(before, after))
        test = compile(root / 'testing/scheduler_handoff_test.c', name,
                       ['-DSHARED_WAITS', f'-DSCHEDULER_SOURCE="{mutant}"',
                        '-I' + str(root / 'librt/libos/uthreads')])
        image = link_image(name, test, handoff_unavailable)
        elf = map_image(image, 0x180000000, name)
        run(name + '-rejected', [a.qemu, '-cpu', 'cortex-a53', elf], must_fail=True)
        results.append({'mutation': name, 'result': 'REJECTED'})

(a.out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
