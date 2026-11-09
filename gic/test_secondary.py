#!/usr/bin/env python3
"""Execute the production ARM64 secondary entry from EL2 and VHE seed states."""
import argparse
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--clang', default='/usr/local/valicc/bin/clang')
p.add_argument('--ld', default='/usr/local/valicc/bin/ld.lld')
p.add_argument('--qemu', default='qemu-system-aarch64')
p.add_argument('--gdb', default='gdb')
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
objects = []
for source in ['testing/gic/secondary.S', 'kernel/arch/aarch64/common/secondary.S']:
    output = a.build / (source.replace('/', '_') + '.o')
    subprocess.run([a.clang, '--target=aarch64-none-elf', '-g', '-c', str(ROOT / source), '-o', str(output)], check=True)
    objects.append(output)
elf = a.build / 'secondary.elf'
subprocess.run([a.ld, '-T', str(ROOT / 'testing/arm64-boot/transition.ld'), *map(str, objects), '-o', str(elf)], check=True)
for name, hcr in [('el2', 0), ('vhe-host', (1 << 34) | (1 << 27)), ('vhe-guest', 1 << 34)]:
    script = a.build / (name + '.gdb')
    log = a.build / (name + '.log')
    script.write_text('\n'.join([
        f'target remote | {a.qemu} -M virt-9.2,virtualization=on,gic-version=3 -cpu max '
        f'-m 1024 -kernel {elf.resolve()} -display none -serial null -monitor none -S -gdb stdio',
        f'set $x20 = {hcr}', 'break ProbeDone', 'break Failed', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&ProbeDone"))',
        'python assert int(gdb.parse_and_eval("$cpsr")) & 0x3cf == 0x3c5',
        'python assert int(gdb.parse_and_eval("$SCTLR_EL1")) & 0x1005 == 0x1005',
        f'python assert int(gdb.parse_and_eval("$HCR_EL2")) == {(hcr & (1 << 34)) | (1 << 31)}',
        f'python assert int(gdb.parse_and_eval("$CNTHCTL_EL2")) == {0xc00 if hcr else 3}',
        'python assert int(gdb.parse_and_eval("$x0")) == 0',
        'python assert int(gdb.parse_and_eval("$x9")) != 0',
        'python assert int(gdb.parse_and_eval("$sp")) == int(gdb.parse_and_eval("&StackTop"))',
        'python print("SECONDARY GIC TRANSITION PASS")', 'monitor quit',
    ]) + '\n')
    with log.open('w') as output:
        process = subprocess.Popen([a.gdb, '-q', '-batch', str(elf), '-x', str(script)],
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=20)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    if 'SECONDARY GIC TRANSITION PASS' not in log.read_text():
        raise RuntimeError(f'{name}: inspect {log}')
    print(f'{name}: secondary GIC transition PASS ({log})', flush=True)
