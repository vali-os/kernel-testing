#!/usr/bin/env python3
"""Run the real UEFI CPU checks and trampoline from synthetic MMU-on firmware."""
import argparse
import os
import signal
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--build', type=Path, required=True)
p.add_argument('--edk2-source', type=Path, required=True)
p.add_argument('--clang', default='clang')
p.add_argument('--ld', default='ld.lld')
p.add_argument('--qemu', default='qemu-system-aarch64')
p.add_argument('--gdb', default='gdb')
a = p.parse_args()
a.build = a.build.resolve()
a.build.mkdir(parents=True, exist_ok=True)
objects = []
for source in ['testing/arm64-boot/transition.S', 'testing/arm64-boot/transition.c',
               'boot/uefi/arm64/trampoline.S', 'boot/uefi/arm64/platform.c']:
    obj = a.build / (Path(source).name + '.o')
    subprocess.run([a.clang, '--target=aarch64-none-elf', '-g', '-O2', '-ffreestanding',
                    '-fno-stack-protector', '-fshort-wchar', '-mgeneral-regs-only',
                    '-mstrict-align', '-ffunction-sections', '-fdata-sections',
                    '-I' + str(ROOT / 'boot/include'), '-I' + str(ROOT / 'boot/uefi/include'),
                    '-I' + str(a.edk2_source / 'MdePkg/Include'),
                    '-I' + str(a.edk2_source / 'MdePkg/Include/AArch64'),
                    '-c', str(ROOT / source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = a.build / 'transition.elf'
subprocess.run([a.ld, '-T', str(ROOT / 'testing/arm64-boot/transition.ld'),
                '--gc-sections', *objects, '-o', str(elf)], check=True)
# Resolve line breakpoints from instructions so documentation edits do not move
# the probe to a different cache operation. No test hooks enter production code.
trampoline = ROOT / 'boot/uefi/arm64/trampoline.S'
lines = trampoline.read_text().splitlines()
decode_line = next(i + 1 for i, line in enumerate(lines) if line.strip() == 'and x14, x13, #7')
operand_line = next(i + 1 for i, line in enumerate(lines) if line.strip() == 'dc cisw, x4')

for name, cpu, hcr in [('legacy', 'cortex-a57', 0),
                       ('ccidx', 'cortex-a710', 0),
                       ('vhe-host', 'cortex-a710', (1 << 34) | (1 << 27)),
                       ('vhe-guest', 'cortex-a710', 1 << 34),
                       ('zero-frequency', 'cortex-a710', (1 << 34) | (1 << 27))]:
    # The command executes a seed routine, rather than assigning system
    # registers through GDB. At SeedReady, translation and caches are active.
    commands = [
        f'target remote | {a.qemu} -M virt-9.2,virtualization=on,gic-version=3 '
        f'-cpu {cpu} -m 1024 -smp 1 -kernel {elf} '
        '-display none -serial null -monitor none -S -gdb stdio',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&SeedStart"))',
        f'set $x20 = {hcr}', f'set $x21 = {int(name == "zero-frequency")}',
        'break SeedReady', 'break SeedFailed', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&SeedReady"))',
        f'python assert int(gdb.parse_and_eval("$HCR_EL2")) == {hcr | (1 << 31)}',
        'python assert int(gdb.parse_and_eval("$SCTLR_EL2")) & 0x1005 == 0x1005',
        f'python assert (int(gdb.parse_and_eval("$ID_AA64MMFR2_EL1")) >> 20) & 15 == {int(cpu == "cortex-a710")}',
        f'tbreak {trampoline}:{decode_line}', 'continue',
        'set $cache_id = $x13', 'set $cache_level = $x12',
        f'tbreak {trampoline}:{operand_line}', 'continue',
        # Check the actual first set/way operand against the architected layout.
        # TCG treats cache maintenance as a no-op, so boot success alone cannot
        # catch decoding CCIDX geometry with legacy field positions.
        'python c = int(gdb.parse_and_eval("$cache_id"))',
        f'python ways = ((c >> 3) & {(1 << 21) - 1 if cpu == "cortex-a710" else 1023})',
        f'python sets = ((c >> {32 if cpu == "cortex-a710" else 13}) & {(1 << 24) - 1 if cpu == "cortex-a710" else 32767})',
        'python expected = (ways << (32 - ways.bit_length())) | (sets << ((c & 7) + 4)) | int(gdb.parse_and_eval("$cache_level"))',
        'python assert int(gdb.parse_and_eval("$x4")) == expected',
        'break KernelEntry', 'break Arm64Vectors', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&KernelEntry"))',
        'python assert int(gdb.parse_and_eval("$cpsr")) & 0x3cf == 0x3c5',
        'python assert int(gdb.parse_and_eval("$SCTLR_EL1")) & 0x1005 == 0',
        'python assert int(gdb.parse_and_eval("$SCTLR_EL2")) & 0x1005 == 0',
        'python assert int(gdb.parse_and_eval("$CPACR_EL1")) == 0',
        'python assert int(gdb.parse_and_eval("$VBAR_EL1")) == int(gdb.parse_and_eval("&Arm64Vectors"))',
        'python assert int(gdb.parse_and_eval("$sp")) == int(gdb.parse_and_eval("&StackTop"))',
        'python assert int(gdb.parse_and_eval("$x0")) == 0x12345678',
        'python assert all(int(gdb.parse_and_eval("$x%d" % n)) == 0 for n in [1, 2, 3, 18, 29])',
        'python assert int(gdb.parse_and_eval("$x30")) == int(gdb.parse_and_eval("&Arm64Vectors"))',
        f'python assert int(gdb.parse_and_eval("$HCR_EL2")) == {(hcr & (1 << 34)) | (1 << 31)}',
        f'python assert int(gdb.parse_and_eval("$CNTHCTL_EL2")) == {0xc00 if hcr else 3}',
        'python assert int(gdb.parse_and_eval("$CNTVOFF_EL2")) == 0',
        'python assert int(gdb.parse_and_eval("$CNTHP_CTL_EL2")) & 3 == 0',
        *(['python assert int(gdb.parse_and_eval("$CNTHV_CTL_EL2")) & 3 == 0'] if hcr else []),
        'break ProbeDone', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&ProbeDone"))',
        'python assert all(int(gdb.parse_and_eval("$x%d" % n)) != 0 for n in [9, 10, 11])',
        'python assert all(int(gdb.parse_and_eval("$x%d" % n)) & 3 == 0 for n in [12, 13])',
        'python assert int(gdb.parse_and_eval("$x14")) == 0',
        'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&Arm64Vectors"))',
        'python print("TRANSITION PASS")', 'monitor quit',
    ]
    if name == 'zero-frequency':
        # SeedFailed receives CheckFirmwareState's failure code, proving that
        # the capability gate rejected the state before Arm64Enter executed.
        commands = commands[:commands.index(f'tbreak {trampoline}:{decode_line}')] + [
            'break Arm64Enter', 'continue',
            'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&SeedFailed"))',
            'python assert int(gdb.parse_and_eval("$x0")) == 1',
            'python print("TRANSITION PASS")', 'monitor quit',
        ]
    script = a.build / (name + '.gdb')
    script.write_text('\n'.join(commands) + '\n')
    log = a.build / (name + '.log')
    with log.open('w') as output:
        process = subprocess.Popen([a.gdb, '-q', '-batch', str(elf), '-x', str(script)],
                                   stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True, env={**os.environ, 'LC_ALL': 'C'})
        try:
            process.wait(timeout=30)
        finally:
            # GDB may exit on an assertion before executing monitor quit. Stop
            # its QEMU child too, so failing tests cannot leave a running VM.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
    output = log.read_text()
    if 'TRANSITION PASS' not in output or 'AssertionError' in output:
        raise RuntimeError(f'{name}: transition failed; inspect {log}')
    print(f'{name}: TRANSITION PASS ({log})', flush=True)
