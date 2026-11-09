#!/usr/bin/env python3
"""Boot the real ARM64 UEFI loader and kernel through ACPI discovery, timer delivery and SMP startup.

Checks UART output, RAM/Device page attributes, a timer IRQ and the requested
number of online CPUs. Userspace may start while those events are observed; it is not validated.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
p.add_argument('--loader', type=Path, required=True)
p.add_argument('--phoenix', type=Path, required=True)
p.add_argument('--firmware', type=Path, required=True)
p.add_argument('--lzss', type=Path, required=True)
p.add_argument('--mkvafs', type=Path, required=True)
p.add_argument('--output-dir', type=Path, required=True)
p.add_argument('--qemu', default='qemu-system-aarch64')
p.add_argument('--gdb', default='gdb')
p.add_argument('--gic-version', type=int, choices=[2, 3], default=2)
p.add_argument('--cpus', type=int, default=4)
p.add_argument('--gic-selftest', action='store_true', help='Require the opt-in native GIC test before userspace')
a = p.parse_args()
directory = a.output_dir.resolve()
esp = directory / 'esp'
files = esp / 'EFI/VALI'
files.mkdir(parents=True, exist_ok=True)
(esp / 'EFI/BOOT').mkdir(parents=True, exist_ok=True)
shutil.copy2(a.loader, esp / 'EFI/BOOT/BOOTAA64.EFI')
shutil.copy2(a.phoenix, files / 'phoenix.mos')
ramdisk = directory / 'ramdisk'
(ramdisk / 'services').mkdir(parents=True, exist_ok=True)
(ramdisk / 'README').write_text('ACPI kernel checkpoint fixture.\n')
subprocess.run([str(a.mkvafs.resolve()), '--arch', 'arm64', '--out',
                str(files / 'initrd.mos'), str(ramdisk)], check=True)
subprocess.run([str(a.lzss.resolve()), 'c', str(a.build / 'bin/kernel.mos'), str(files / 'kernel.mos')], check=True)
data = (a.build / 'bin/kernel.mos').read_bytes()
optional = struct.unpack_from('<I', data, 60)[0] + 24
base = struct.unpack_from('<Q', data, optional + 24)[0]
mapping = (a.build / 'bin/kernel.map').read_text()
def symbol(name):
    return base + int(re.search(r'^([0-9a-fA-F]+).*?\s' + re.escape(name) + r'$', mapping, re.M)[1], 16)

for name, cpu, virtualization in [('el1', 'cortex-a57', 'off'), ('el2', 'max', 'on')]:
    serial = directory / (name + '.serial.log')
    debugger = directory / (name + '.debugger.log')
    script = directory / (name + '.gdb')
    script.write_text('\n'.join([
        f'target remote | {a.qemu} -M virt,gic-version={a.gic_version},virtualization={virtualization} '
        f'-cpu {cpu} -m 1024 -smp {a.cpus} -bios {a.firmware.resolve()} '
        f'-drive if=none,id=esp,format=raw,file=fat:{esp},readonly=on -device virtio-blk-pci,drive=esp '
        f'-display none -serial file:{serial} -monitor none -no-reboot -S -gdb stdio',
        f'break *{symbol("kentry")}', 'continue',
        f'python assert int(gdb.parse_and_eval("$pc")) == {symbol("kentry")}',
        'delete breakpoints', f'break *{symbol("ArchAcpiGetMappingFlags")}',
        f'break *{symbol("DebugPanic")}', 'continue',
        f'python assert int(gdb.parse_and_eval("$pc")) == {symbol("ArchAcpiGetMappingFlags")}',
        'set $rsdp = $x0', 'set $flags = $x2', 'set $mapping_return = $x30',
        'delete breakpoints', 'break *$mapping_return', 'continue',
        'python assert int(gdb.parse_and_eval("$w0")) == 0',
        'python assert int(gdb.parse_and_eval("*(unsigned int*)$flags")) == 0, "RSDP must use cached Normal RAM"',
        'delete breakpoints', f'break *{symbol("ArchAcpiInitialize")}',
        f'break *{symbol("DebugPanic")}', 'continue',
        f'python assert int(gdb.parse_and_eval("$pc")) == {symbol("ArchAcpiInitialize")}',
        'set $platform_return = $x30', 'delete breakpoints',
        'break *$platform_return', f'break *{symbol("DebugPanic")}', 'continue',
        'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("$platform_return"))',
        'python assert int(gdb.parse_and_eval("$w0")) == 0, "platform discovery failed"',
        'python',
        'def read64(address):',
        '    return int.from_bytes(gdb.selected_inferior().read_memory(address, 8).tobytes(), "little")',
        'def descriptor(root, address):',
        '    for shift in [39, 30, 21, 12]:',
        '        entry = read64(root + ((address >> shift) & 511) * 8)',
        '        if not entry & 1: return 0',
        '        if shift == 12 or entry & 3 == 1: return entry',
        '        root = entry & 0x0000fffffffff000',
        '    raise AssertionError("invalid table walk")',
        f'identity = read64({symbol("g_identityTable")})',
        f'kernel = read64({symbol("g_kernelTable")})',
        f'uart = read64({symbol("g_uart")})',
        # Arm64GicDescription occupies 48 bytes before the UART resource.
        f'assert read64({symbol("g_arm64Platform")} + 48) == 0x9000000',
        'assert uart != 0',
        'assert descriptor(identity, 0x9000000) == 0, "UART must have no Normal-memory alias"',
        'rsdp_entry = descriptor(identity, int(gdb.parse_and_eval("$rsdp")))',
        'assert rsdp_entry & 1 and (rsdp_entry >> 2) & 7 == 0, "RSDP identity RAM must remain mapped and cached"',
        'entry = descriptor(kernel, uart)',
        'assert (entry >> 2) & 7 == 1, "UART must be Device-nGnRnE"',
        'assert entry & (1 << 53) and entry & (1 << 54), "UART must be execute-never"',
        'end',
        'delete breakpoints',
        'python',
        # IRQ delivery may begin after SpawnBootstrapper. Observe both events
        # in either order instead of assuming the scheduler's first IRQ timing.
        f'pending = {{address: gdb.Breakpoint("*%d" % address) for address in [{symbol("__TimerInterrupt")}, {symbol("SpawnBootstrapper")}]}}',
        f'panic = gdb.Breakpoint("*{symbol("DebugPanic")}")',
        'while pending:',
        '    gdb.execute("continue")',
        '    pc = int(gdb.parse_and_eval("$pc"))',
        '    assert pc in pending, "panic before timer/SMP checkpoints completed"',
        '    pending.pop(pc).delete()',
        'end',
        'python',
        f'assert int(gdb.parse_and_eval("*(unsigned int*){symbol("g_pciRootStatus")}")) == 0, "PCI root scan rejected firmware"',
        f'assert int(gdb.parse_and_eval("*(unsigned int*){symbol("g_pciRootCount")}")) == 1, "QEMU namespace root missing"',
        f'root = read64({symbol("g_pciRoots")})',
        'assert root != 0 and read64(root) >> 32 > 0, "root _CRS windows missing"',
        f'assert read64({symbol("g_pciMcfg")}) != 0, "AML ECAM table unavailable"',
        f'assert int(gdb.parse_and_eval("*(unsigned int*){symbol("g_pciMcfgInvalid")}")) == 0',
        'print("ARM64 ACPI PCI ROOTS PASS")',
        'end',
        'python print("ARM64 ACPI PLATFORM PASS")', 'monitor quit',
    ]) + '\n')
    with debugger.open('w') as output:
        process = subprocess.Popen([a.gdb, '-q', '-batch', '-x', str(script)],
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            process.wait(timeout=55)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    result = debugger.read_text()
    if 'ARM64 ACPI PLATFORM PASS' not in result or 'AssertionError' in result:
        raise RuntimeError(f'{name}: inspect {debugger} and {serial}')
    text = serial.read_text(errors='replace')
    if ('SPCR PL011 console at' not in text or f'ARM64 ACPI platform: {a.cpus} CPUs, GICv{a.gic_version}' not in text
            or any(f'CpuCoreStart {i} is online' not in text for i in range(1, a.cpus))):
        raise RuntimeError(f'{name}: console checkpoint missing in {serial}')
    if a.gic_selftest and 'GIC SELFTEST PASS' not in text:
        raise RuntimeError(f'{name}: native GIC self-test did not finish: {serial}')
    print(f'{name}: real kernel ACPI/timer/{a.cpus}-CPU/GICv{a.gic_version} checkpoint PASS ({debugger})', flush=True)
