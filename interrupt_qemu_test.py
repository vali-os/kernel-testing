"""Boot a staged EFI tree without modifying the user's disk image.

Build with VALI_INTERRUPT_SELFTEST=ON. Stage BOOTX64.EFI in EFI/BOOT and
kernel.mos (aPLib), phoenix.mos and initrd.mos in EFI/vali under --esp.
The APIC self-test validates kernel delivery. With --edu, also build
VALI_EDU_SELFTEST=ON and stage its module to validate real PCI MSI delivery.
"""
import argparse
import json
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--code', type=Path, required=True)
    parser.add_argument('--vars', type=Path, required=True)
    parser.add_argument('--esp', type=Path, required=True)
    parser.add_argument('--disk', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--smp', type=int, default=1)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--no-hpet', action='store_true')
    parser.add_argument('--edu', action='store_true',
                        help='Attach EDU and require the PCI MSI driver self-test')
    parser.add_argument('--debug-exceptions', action='store_true')
    parser.add_argument('--debug-state', action='store_true', help='Capture QMP state without an exception trace')
    parser.add_argument('--require', default='PmCreateProcess',
                        help='Serial marker required after the APIC self-test')
    args = parser.parse_args()
    debug = args.debug_state or args.debug_exceptions
    args.output.mkdir(parents=True, exist_ok=True)
    variables = args.output / 'vars.fd'
    serial = args.output / 'serial.log'
    shutil.copyfile(args.vars, variables)
    serial.write_text('')
    command = [str(args.qemu), '-machine', 'pc,hpet=off' if args.no_hpet else 'pc', '-accel', 'tcg', '-smp', str(args.smp),
               '-m', '1024', '-display', 'none', '-monitor', 'none', '-net', 'none',
               '-no-reboot', '-serial', f'file:{serial.resolve().as_posix()}',
               '-drive', f'if=pflash,format=raw,readonly=on,file={args.code.resolve().as_posix()}',
               '-drive', f'if=pflash,format=raw,file={variables.resolve().as_posix()}',
               '-drive', f'if=none,id=esp,format=raw,snapshot=on,file=fat:ro:{args.esp.resolve().as_posix()}',
               '-device', 'ide-hd,drive=esp,bootindex=1']
    if args.disk:
        command += ['-drive', f'if=virtio,format=raw,snapshot=on,file={args.disk.resolve().as_posix()}']
    if args.edu:
        command += ['-device', 'edu']
    if debug:
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            qmp_port = listener.getsockname()[1]
        command += ['-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off']
        if args.debug_exceptions:
            command += ['-d', 'int,guest_errors', '-D', str((args.output / 'exceptions.log').resolve())]
    (args.output / 'command.txt').write_text(subprocess.list2cmdline(command))
    with (args.output / 'qemu.log').open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=output,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            deadline = time.monotonic() + args.timeout
            passed = None
            inspected = False
            if debug:
                for attempt in range(30):
                    try:
                        qmp_socket = socket.create_connection(('127.0.0.1', qmp_port), timeout=2)
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    raise RuntimeError('Cannot connect to the QEMU localhost debug socket')
                qmp = qmp_socket.makefile('rwb')
                qmp.readline()
                def monitor(request):
                    qmp.write((json.dumps(request) + '\n').encode())
                    qmp.flush()
                    while True:
                        result = json.loads(qmp.readline())
                        if 'return' in result or 'error' in result:
                            return result
                monitor({'execute': 'qmp_capabilities'})
            while time.monotonic() < deadline:
                text = serial.read_text(errors='replace')
                if any(marker in text for marker in ('Interrupt allocation self-test FAILED',
                        'DebugPanic(', 'MSSync timeout', 'did not complete startup',
                        'EDU MSI self-test: FAILED')):
                    if debug:
                        monitor({'execute': 'stop'})
                        state = monitor({'execute': 'human-monitor-command',
                                         'arguments': {'command-line': 'info registers -a'}})
                        (args.output / 'failure-registers.txt').write_text(state.get('return', str(state)))
                        stacks = re.findall(r'\[debug-(\d+)-[^\]]+\] RSP 0x[0-9a-f]+ \(UserRSP (0x[0-9a-f]+)\)', text)
                        if stacks:
                            cpu, stack = stacks[-1]
                            state = monitor({'execute': 'human-monitor-command',
                                             'arguments': {'command-line': f'x /96gx {stack}', 'cpu-index': int(cpu)}})
                            (args.output / 'failure-stack.txt').write_text(state.get('return', str(state)))
                    raise RuntimeError(f'Kernel boot failed; see {serial}')
                if 'Interrupt allocation self-test: PASS' in text and passed is None:
                    passed = time.monotonic()
                    print(f'SMP={args.smp}: real APIC interrupt allocation self-test PASS', flush=True)
                # Continue after the kernel checks to observe userspace boot.
                online = set(re.findall(r'CpuCoreStart (\d+) is online', text))
                if debug and passed is not None and not inspected and time.monotonic() - passed >= 25:
                    monitor({'execute': 'stop'})
                    state = monitor({'execute': 'human-monitor-command',
                                     'arguments': {'command-line': 'info registers -a'}})
                    (args.output / 'registers.txt').write_text(state.get('return', str(state)))
                    registers = state.get('return', '')
                    for cpu, stack in re.findall(r'CPU#(\d+).*?RSP=([0-9a-fA-F]+)', registers, re.S):
                        state = monitor({'execute': 'human-monitor-command',
                                         'arguments': {'command-line': f'x /96gx 0x{stack}', 'cpu-index': int(cpu)}})
                        (args.output / f'stack-{cpu}.txt').write_text(state.get('return', str(state)))
                    commands = [('info pic', 0), ('info irq', 0)]
                    commands += [('info lapic', cpu) for cpu in range(args.smp)]
                    with (args.output / 'interrupt-state.txt').open('w') as state_file:
                        for command, cpu in commands:
                            state = monitor({'execute': 'human-monitor-command',
                                             'arguments': {'command-line': command, 'cpu-index': cpu}})
                            state_file.write(f'{command} (CPU {cpu})\n' + state.get('return', str(state)) + '\n')
                    monitor({'execute': 'cont'})
                    inspected = True
                if (passed is not None and len(online) == args.smp - 1 and
                        f'Interrupt allocation self-test: CPUs={args.smp} ' in text and
                        (not args.edu or 'EDU MSI self-test: PASS' in text) and
                        args.require in text and time.monotonic() - passed >= 40):
                    print(f'SMP={args.smp}: boot observation complete; serial log: {serial}', flush=True)
                    return
                if process.poll() is not None:
                    raise RuntimeError(f'QEMU exited ({process.returncode}); see {args.output}')
                time.sleep(1)
            raise RuntimeError(f'No completed self-test boot before timeout; see {serial}')
        finally:
            if debug and 'qmp' in locals():
                qmp.close()
                qmp_socket.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
