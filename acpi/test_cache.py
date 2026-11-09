#!/usr/bin/env python3
"""Inspect actual ARM64 cache-flush instructions under privileged QEMU execution.

TCG does not model dirty cache lines. This verifies geometry decoding, completion,
register preservation and control-state preservation, not physical writeback.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output-dir', required=True, type=Path)
p.add_argument('--clang', default='clang')
p.add_argument('--ld', default='ld.lld')
p.add_argument('--gdb', default='gdb')
p.add_argument('--qemu', default='qemu-system-aarch64')
a = p.parse_args()
a.output_dir = a.output_dir.resolve()
a.output_dir.mkdir(parents=True, exist_ok=True)
objects = []
source = ROOT / 'kernel/arch/aarch64/common/cache.S'
for path in [ROOT / 'testing/acpi/cache_probe.S', source]:
    obj = a.output_dir / (path.stem + '.o')
    subprocess.run([a.clang, '--target=aarch64-none-elf', '-g', '-c', str(path), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = a.output_dir / 'cache.elf'
subprocess.run([a.ld, '-Ttext=0x40080000', '-e', 'CacheProbeStart', *objects, '-o', str(elf)], check=True)
lines = source.read_text().splitlines()
decode = next(i+1 for i, line in enumerate(lines) if line.strip() == 'and x14, x13, #7')
operand = next(i+1 for i, line in enumerate(lines) if line.strip() == 'dc cisw, x4')
for name, cpu, direct in [('legacy', 'cortex-a57', False), ('ccidx', 'cortex-a710', False),
                           ('direct-mapped', 'cortex-a57', True)]:
    script = a.output_dir / (name + '.gdb')
    commands = [
        f'target remote | {a.qemu} -M virt,gic-version=3 -cpu {cpu} -m 128 -smp 1 '
        f'-kernel {elf} -display none -serial null -monitor none -S -gdb stdio',
        'break CacheProbeReady', 'break CacheProbeFailed', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&CacheProbeReady"))',
        'set $old_selector = $CSSELR',
        'set $old_mask = $cpsr & 0x3c0', 'set $old_control = $SCTLR_EL1',
        f'tbreak {source}:{decode}', 'continue',
        *(['set $x13 = (3 << 13) | 2'] if direct else []),
        'set $geometry = $x13', 'set $level = $x12',
        f'tbreak {source}:{operand}', 'continue',
        'python c = int(gdb.parse_and_eval("$geometry"))',
        f'python ways = (c >> 3) & {0x1fffff if cpu == "cortex-a710" else 1023}',
        f'python sets = (c >> {32 if cpu == "cortex-a710" else 13}) & {0xffffff if cpu == "cortex-a710" else 32767}',
        'python expected = (ways << (32 - ways.bit_length())) | (sets << ((c & 7) + 4)) | int(gdb.parse_and_eval("$level"))',
        'python assert int(gdb.parse_and_eval("$x4")) == expected',
        'break CacheProbeDone', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&CacheProbeDone"))',
        'python assert int(gdb.parse_and_eval("$CSSELR")) == int(gdb.parse_and_eval("$old_selector"))',
        'python assert int(gdb.parse_and_eval("$cpsr")) & 0x3c0 == int(gdb.parse_and_eval("$old_mask"))',
        'python assert int(gdb.parse_and_eval("$SCTLR_EL1")) == int(gdb.parse_and_eval("$old_control"))',
        'python assert [int(gdb.parse_and_eval("$x%d" % n)) for n in [18, 19, 20]] == [0x1818, 0x1919, 0x2020]',
        'python print("CACHE FLUSH PASS")', 'monitor quit',
    ]
    script.write_text('\n'.join(commands) + '\n')
    log = a.output_dir / (name + '.log')
    with log.open('w') as output:
        process = subprocess.Popen([a.gdb, '-q', '-batch', str(elf), '-x', str(script)],
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=30)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    result = log.read_text()
    if 'CACHE FLUSH PASS' not in result or 'AssertionError' in result:
        raise RuntimeError(f'{name}: inspect {log}')
    print(f'{name}: CACHE FLUSH PASS', flush=True)
