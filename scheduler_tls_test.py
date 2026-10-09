"""Execute scheduler TLS catch-up using Vali ARM64 PE code under QEMU.

Uses actual scheduler, TLS registry, TLS switching and ARM64 context assembly.
Kernel notifications, mutexes and allocation are deterministic test adapters.
No concurrent execution units or asynchronous signals are exercised.
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
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
a.bin, a.llvm, a.out = a.bin.resolve(), a.llvm.resolve(), a.out.resolve()
a.out.mkdir(parents=True, exist_ok=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
sys.path.insert(0, str(a.llvm / 'libunwind/utils'))
from vali_pe import PE

def run(name, args, expected=0):
    result = subprocess.run(list(map(str, args)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=60)
    (a.out / (name + '.log')).write_text(' '.join(map(str, args)) + '\n' + result.stdout)
    if result.returncode != expected:
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
    ('unavailable', 'testing/scheduler_tls_unavailable.S'),
    ('platform', 'testing/scheduler_tls_platform.c')]]
objects.append(compile(a.llvm / 'compiler-rt/lib/builtins/aarch64/chkstk.S', 'chkstk'))
# Compile the production translation unit independently for each supported ABI.
for arch in ['aarch64', 'x86_64', 'i686']:
    compile(root / 'librt/libos/uthreads/scheduler.c', 'scheduler-' + arch,
            ['--target=' + arch + '-uml-vali'])
results = []
for case in ['normal', 'FAIL_CATCHUP']:
    test = compile(root / 'testing/scheduler_tls_test.c', case,
                   [] if case == 'normal' else ['-DFAIL_CATCHUP'])
    image = a.out / (case + '.exe')
    run(case + '-link', [a.bin / 'lld', '-flavor', 'link', '-lldvpe',
        '/machine:arm64', '/nodefaultlib', '/subsystem:console', '/opt:ref',
        '/entry:entry', '/out:' + str(image), test, *objects])
    imports = run(case + '-imports', [a.bin / 'llvm-readobj', '--coff-imports', image])
    assert 'Import {' not in imports, imports
    for base in [0x180000000, 0x190000000, 0x170000000]:
        pe = PE(image, base)
        pe.resolve({})
        ident = b'\x7fELF\x02\x01\x01' + bytes(9)
        hdr = ident + struct.pack('<HHIQQQIHHHHHH', 2, 183, 1, pe.entry,
                                  64, 0, 0, 64, 56, 1, 0, 0, 0)
        size = len(pe.mapped)
        phdr = struct.pack('<IIQQQQQQ', 1, 7, 4096, base, base, size, size, 4096)
        elf = a.out / f'{case}-{base:x}.elf'
        elf.write_bytes((hdr + phdr).ljust(4096, b'\0') + pe.mapped)
        elf.chmod(0o755)
        run(elf.stem, [a.qemu, '-cpu', 'cortex-a53', elf],
            expected=0 if case == 'normal' else -5)
        results.append({'case': case, 'base': hex(base), 'result': 'PASS'})
(a.out / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps(results, indent=2))
