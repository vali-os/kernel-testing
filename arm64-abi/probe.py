#!/usr/bin/env python3
"""Collect compiler evidence; exit 1 when the Vali compiler gate is blocked.
Windows/ELF controls never satisfy the Vali gate. No target code is executed.
"""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--clang', required=True, type=Path)
p.add_argument('--tools', required=True, type=Path, help='LLVM bin directory')
p.add_argument('--out', required=True, type=Path, help='fresh output directory')
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
src = Path(__file__).resolve().parent
results = {}


def run(name, args):
    cp = subprocess.run([str(x) for x in args], text=True, capture_output=True)
    (a.out / (name + '.log')).write_text('$ ' + ' '.join(map(str, args)) + '\n' + cp.stdout + cp.stderr)
    results[name] = cp.returncode
    return cp.returncode == 0


run('compiler', [a.clang, '--version'])
run('targets', [a.clang, '--print-targets'])
for name, triple in [('existing', 'amd64-uml-vali'),
                     ('vali', 'aarch64-uml-vali'),
                     ('windows-control', 'aarch64-pc-windows-msvc'),
                     ('elf-control', 'aarch64-none-elf')]:
    flags = [a.clang, '--target=' + triple, '-ffreestanding', '-nostdlibinc',
             '-fms-extensions', '-O1']
    if name != 'existing':
        flags += ['-march=armv8-a', '-mno-outline-atomics', '-ffixed-x18']
    run(name + '-macros', flags + ['-dM', '-E', '-x', 'c', '/dev/null'])
    run(name + '-model', flags + ['-S', '-emit-llvm', src / 'model.c', '-o', a.out / (name + '-model.ll')])
    if name == 'vali':
        run('vali-contract', flags + ['-DVALI_ABI_CONTRACT', '-fsyntax-only', src / 'model.c'])
        run('vali-driver', flags + ['-###', '-nostdlib', src / 'image.c', '-o', a.out / 'driver.exe'])
    if name == 'existing':
        continue
    for fixture in ['library', 'image', 'features', 'kernel', 'assembler']:
        source = src / (fixture + ('.S' if fixture == 'assembler' else '.c'))
        obj = a.out / (name + '-' + fixture + '.obj')
        extra = ['-mgeneral-regs-only'] if fixture == 'kernel' else []
        ok = run(name + '-' + fixture, flags + extra + ['-c', source, '-o', obj])
        if ok:
            run(name + '-' + fixture + '-inspect', [a.tools / 'llvm-readobj',
                '--file-headers', '--sections', '--symbols', '--relocations', '--unwind', obj])
            run(name + '-' + fixture + '-disasm', [a.tools / 'llvm-objdump', '-dr', obj])
            run(name + '-' + fixture + '-undefined', [a.tools / 'llvm-nm', '--undefined-only', obj])
    if name == 'elf-control' or results[name + '-library'] or results[name + '-image']:
        continue
    # Attempt the full feature link without hiding missing runtime helpers.
    if results[name + '-features'] == 0:
        run(name + '-full-features-link', [a.tools / 'lld-link', '/machine:arm64',
            '/nodefaultlib', '/dll', '/noentry', '/manifest:no',
            '/out:' + str(a.out / (name + '-features.dll')),
            a.out / (name + '-features.obj')])
    dll = a.out / (name + '.dll')
    lib = a.out / (name + '.lib')
    exe = a.out / (name + '.exe')
    common = [a.tools / 'lld-link', '/machine:arm64', '/nodefaultlib', '/manifest:no', '/dynamicbase']
    if run(name + '-dll', common + ['/dll', '/noentry', '/base:0x180000000',
            '/out:' + str(dll), '/implib:' + str(lib), a.out / (name + '-library.obj')]):
        run(name + '-exe', common + ['/entry:entry', '/subsystem:console',
            '/out:' + str(exe), a.out / (name + '-image.obj'), lib])
        for image in [dll, exe]:
            if image.exists():
                run(image.name + '-inspect', [a.tools / 'llvm-readobj', '--file-headers',
                    '--coff-imports', '--coff-exports', '--coff-basereloc', '--coff-tls-directory', image])
        run(name + '-rebase', ['python3', src / 'rebase.py', dll])

# This preflight cannot certify TLS, startup or execution on the Vali loader.
compiler_ready = all(results.get(k, 1) == 0 for k in
                     ['vali-contract', 'vali-library', 'vali-image', 'vali-features',
                      'vali-kernel', 'vali-assembler', 'vali-dll', 'vali-exe', 'vali-rebase'])
summary = {'compiler_gate': 'PASS' if compiler_ready else 'BLOCKED',
           'runtime_gate': 'NOT RUN: Vali ARM64 loader/runtime required', 'commands': results}
(a.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
raise SystemExit(0 if compiler_ready else 1)
