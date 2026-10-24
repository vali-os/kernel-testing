"""Validate boot-connected USB keyboards on the packaged AMD64 QEMU image.

Requires an EFI tree (default build/driver-msi-boot/esp) and build/windows/disk.img.
All writable drives are disposable. --topology nested exercises two external hub
levels. Build the HID image with VALI_USB_INPUT_SELFTEST=ON to emit parsed keys.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--mode', choices=['preferred', 'msi', 'intx'], default='preferred')
parser.add_argument('--smp', type=int, default=4)
parser.add_argument('--name')
parser.add_argument('--qemu-dir', default='C:/Program Files/qemu')
parser.add_argument('--topology', choices=['direct','hub','nested'], default='direct')
parser.add_argument('--no-hpet', action='store_true')
parser.add_argument('--timeout', type=int, default=150)
parser.add_argument('--esp', type=Path, help='Use a separately packaged EFI tree')
parser.add_argument('--output-root', type=Path, help='Directory for isolated run artifacts')
parser.add_argument('--key-cycles', type=int, default=1, help='B/C press-release sequences per attachment')
parser.add_argument('--reconnect', action='store_true', help='Unplug and re-enumerate on the same port')
parser.add_argument('--observe-seconds', type=int, default=40)
args = parser.parse_args()
if args.key_cycles < 1 or args.observe_seconds < 40:
    parser.error('key-cycles must be positive and observe-seconds must be at least 40')
root = Path(__file__).resolve().parents[1]
base = root / 'build/driver-msi-boot'
esp = (args.esp or base / 'esp').resolve()
out = (args.output_root or base).resolve() / (args.name or f'{args.mode}-smp{args.smp}')
out.mkdir(exist_ok=False)
qemu = Path(args.qemu_dir)
image_hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                for p in (esp / 'EFI/vali').iterdir() if p.is_file()}
(out / 'image-hashes.json').write_text(json.dumps(image_hashes, indent=2))
shutil.copyfile(qemu / 'share/edk2-i386-vars.fd', out / 'vars.fd')
with (out / 'ahci.raw').open('wb') as disk:
    disk.truncate(16 * 1024 * 1024)
events = ['pci_cfg_write', 'msix_write_config', 'virtio_pci_notify',
          'virtio_notify', 'virtio_queue_notify', 'apic_deliver_irq', 'apic_local_deliver',
          'virtio_blk_req_complete', 'usb_xhci_irq_msix', 'usb_xhci_irq_msi',
          'usb_xhci_irq_intx', 'usb_xhci_fetch_trb', 'usb_xhci_queue_event', 'usb_xhci_slot_address', 'ahci_irq_raise', 'ahci_cmd_done']
events += ['usb_xhci_oper_read', 'usb_xhci_oper_write',
           'usb_xhci_runtime_read', 'usb_xhci_runtime_write']
(out / 'events.txt').write_text('\n'.join(events) + '\n')
virtio_options = ',vectors=0' if args.mode == 'intx' else ''
usb_devices = ['-device', 'usb-kbd,id=kbd,bus=xhci.0,port=1']
if args.topology == 'hub':
    usb_devices = ['-device', 'usb-hub,id=hub1,bus=xhci.0,port=1', '-device', 'usb-kbd,id=kbd,bus=xhci.0,port=1.3']
elif args.topology == 'nested':
    usb_devices = ['-device', 'usb-hub,id=hub1,bus=xhci.0,port=1', '-device', 'usb-hub,id=hub2,bus=xhci.0,port=1.3', '-device', 'usb-kbd,id=kbd,bus=xhci.0,port=1.3.2']
xhci_options = ',msix=off,msi=' + ('off' if args.mode == 'intx' else 'on') if args.mode != 'preferred' else ''
command = [str(qemu / 'qemu-system-x86_64.exe'), '-machine', 'pc,hpet=off' if args.no_hpet else 'pc', '-accel', 'tcg',
           '-smp', str(args.smp), '-m', '1024', '-display', 'none', '-monitor', 'none',
           '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
           '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
           '-drive', f'if=pflash,format=raw,readonly=on,file={qemu.as_posix()}/share/edk2-x86_64-code.fd',
           '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd',
           '-drive', f'if=none,id=esp,format=raw,snapshot=on,file=fat:ro:{esp.as_posix()}',
           '-device', 'ide-hd,drive=esp,bootindex=1',
           '-drive', f'if=none,id=osdisk,format=raw,snapshot=on,file={root.as_posix()}/build/windows/disk.img',
           '-device', 'virtio-blk-pci,id=vblk,drive=osdisk' + virtio_options,
           '-netdev', 'hubport,id=net0,hubid=0',
           '-device', 'virtio-net-pci,id=vnet,netdev=net0' + virtio_options,
           '-device', 'qemu-xhci,id=xhci' + xhci_options,
           *usb_devices, '-device', 'usb-ehci',
           '-device', 'pci-ohci', '-device', 'piix3-usb-uhci',
           '-device', 'ich9-ahci,id=ahci',
           '-drive', f'if=none,id=sata,format=raw,snapshot=on,file={out.as_posix()}/ahci.raw',
           '-device', 'ide-hd,bus=ahci.0,drive=sata']
(out / 'command.json').write_text(json.dumps(command, indent=2))
with (out / 'qemu.log').open('w') as log:
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    try:
        reader, writer = process.stdout, process.stdin
        if not reader.readline():
            raise RuntimeError('QEMU startup failed; see qemu.log')
        def qmp(execute, arguments=None):
            request = {'execute': execute}
            if arguments is not None:
                request['arguments'] = arguments
            writer.write((json.dumps(request) + '\n').encode())
            writer.flush()
            while True:
                line = reader.readline()
                if not line:
                    raise RuntimeError('QMP closed')
                reply = json.loads(line)
                if 'return' in reply or 'error' in reply:
                    return reply
        qmp('qmp_capabilities')
        normal_trbs = {}
        command_trbs = {}
        completed_commands = []
        reports = []
        trace_reader = {'offset': 0, 'pending': b''}
        expected = [[0, 0, 5, 0, 0, 0, 0, 0], [0] * 8,
                    [0, 0, 6, 0, 0, 0, 0, 0], [0] * 8]
        key_events = [('b', True), ('b', False), ('c', True), ('c', False)]
        expected *= args.key_cycles
        key_events *= args.key_cycles
        attachment_keys = len(key_events)
        if args.reconnect:
            expected *= 2
            key_events *= 2
        reconnect_state = 'initial'
        disable_baseline = 0
        reattached_at = None
        key_index = 0
        key_waiting = False
        key_report_start = 0
        key_guest_start = 0
        stimulus = []

        def guest_keys(serial):
            return [(int(usage), bool(int(released))) for usage, released in
                    re.findall(r'HID keyboard usage=(\d+) released=([01])', serial)]

        def collect_reports():
            # Periodic transfers reuse one staging page. Read each completed
            # report before injecting the next key transition; inspecting all
            # old TRBs after the run would read the final release repeatedly.
            with (out / 'trace.log').open('rb') as trace:
                trace.seek(trace_reader['offset'])
                data = trace_reader['pending'] + trace.read()
                trace_reader['offset'] = trace.tell()
            lines = data.split(b'\n')
            trace_reader['pending'] = lines.pop()
            for line in lines:
                line = line.decode(errors='replace')
                command = re.search(r'usb_xhci_fetch_trb addr (0x[0-9a-f]+), (CR_[A-Z_]+),', line)
                if command:
                    command_trbs[int(command[1], 16)] = command[2]
                completion = re.search(r'ER_COMMAND_COMPLETE, CC_SUCCESS, p (0x[0-9a-f]+),', line)
                if completion:
                    completed_commands.append(command_trbs.get(int(completion[1], 16), 'unknown'))
                match = re.search(r'usb_xhci_fetch_trb addr (0x[0-9a-f]+), TR_NORMAL, p (0x[0-9a-f]+), s (0x[0-9a-f]+)', line)
                if match and int(match[3], 16) & 0x1ffff >= 8:
                    normal_trbs[int(match[1], 16)] = int(match[2], 16)
                match = re.search(r'ER_TRANSFER, CC_(?:SUCCESS|SHORT_PACKET), p (0x[0-9a-f]+), s 0x[0-9a-f]+, c (0x[0-9a-f]+)', line)
                if match and (int(match[2], 16) >> 16) & 31 == 3:
                    address = normal_trbs.get(int(match[1], 16))
                    if address is not None:
                        reply = qmp('human-monitor-command', {'command-line': f'xp /8bx 0x{address:x}'})
                        raw = reply.get('return', '')
                        payload = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})(?=\s|$)', raw)]
                        reports.append({'trb': match[1], 'buffer': hex(address), 'bytes': payload, 'raw': raw})

        start = time.monotonic()
        ready_at = None
        failure = False
        trace_start = 0
        while time.monotonic() - start < args.timeout:
            if process.poll() is not None:
                raise RuntimeError('QEMU exited early')
            text = (out / 'serial.log').read_text(errors='replace')
            guest_events = guest_keys(text)
            collect_reports()
            failure = any(x in text for x in ('DebugPanic(', 'MSSync timeout', 'did not complete startup'))
            if failure:
                break
            ready = all(x in text for x in ('InstallBundledApplications no bundled apps!',
                        'storage_ready_invocation(path=/boot)', 'storage_ready_invocation(path=/data)',
                        'virtio-net ready device='))
            if ready and ready_at is None:
                ready_at = time.monotonic()
                trace_start = (out / 'trace.log').stat().st_size
                print(f'{args.mode}: boot complete, observing {args.observe_seconds} seconds', flush=True)
                stimulus = [qmp('set_link', {'name': 'vnet', 'up': False}),
                            qmp('set_link', {'name': 'vnet', 'up': True})]
            if key_waiting:
                key, down = key_events[key_index]
                guest_event = (5 if key == 'b' else 6, not down)
                if (any(r['bytes'] == expected[key_index] for r in reports[key_report_start:]) and
                        guest_event in guest_events[key_guest_start:]):
                    key_index += 1
                    key_waiting = False
            if args.reconnect and key_index == attachment_keys and reconnect_state == 'initial':
                disable_baseline = completed_commands.count('CR_DISABLE_SLOT')
                reply = qmp('device_del', {'id': 'kbd'})
                if 'error' in reply:
                    raise RuntimeError(f'Keyboard removal failed: {reply}')
                stimulus.append({'remove': 'kbd', 'reply': reply})
                reconnect_state = 'deleting'
                print(f'{args.mode}: reports verified; waiting for guest Disable Slot', flush=True)
            if reconnect_state == 'deleting' and completed_commands.count('CR_DISABLE_SLOT') > disable_baseline:
                port = {'direct': '1', 'hub': '1.3', 'nested': '1.3.2'}[args.topology]
                reply = qmp('device_add', {'driver': 'usb-kbd', 'id': 'kbd2', 'bus': 'xhci.0', 'port': port})
                if 'error' in reply:
                    raise RuntimeError(f'Keyboard reattachment failed: {reply}')
                stimulus.append({'reattach': 'kbd2', 'port': port, 'reply': reply})
                reconnect_state = 'attached'
                reattached_at = time.monotonic()
                print(f'{args.mode}: slot disabled; keyboard reattached to port {port}', flush=True)
            can_inject = reconnect_state != 'deleting' and (reattached_at is None or time.monotonic() - reattached_at >= 8)
            if ready_at is not None and time.monotonic() - ready_at >= 12 and can_inject and not key_waiting and key_index < len(key_events):
                key, down = key_events[key_index]
                reply = qmp('input-send-event', {'events': [
                    {'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': key}}}]})
                if 'error' in reply:
                    raise RuntimeError(f'Keyboard stimulus failed: {reply}')
                stimulus.append({'key': key, 'down': down, 'reply': reply})
                key_report_start = len(reports)
                key_guest_start = len(guest_events)
                key_waiting = True
            if ready_at is not None and time.monotonic() - ready_at >= args.observe_seconds:
                break
            time.sleep(1)
        stopped_at = time.monotonic()
        qmp('stop')
        collect_reports()
        text = (out / 'serial.log').read_text(errors='replace')
        failure = any(x in text for x in ('DebugPanic(', 'MSSync timeout', 'did not complete startup'))
        guest_events = guest_keys(text)
        (out / 'guest-key-events.json').write_text(json.dumps(guest_events, indent=2))
        (out / 'stimulus.json').write_text(json.dumps(stimulus, indent=2))
        for name, cmd in [('pci', 'info pci'), ('irq', 'info irq'), ('registers', 'info registers -a')]:
            reply = qmp('human-monitor-command', {'command-line': cmd})
            (out / f'{name}.txt').write_text(reply.get('return', str(reply)))
        pci = (out / 'pci.txt').read_text()
        xhci_bar = re.search(r'USB controller: PCI device 1b36:000d.*?BAR0:.*? at (0x[0-9a-f]+)', pci, re.S)
        scratchpads = None
        if xhci_bar:
            bar = int(xhci_bar[1], 16)
            reply = qmp('human-monitor-command', {'command-line': f'xp /8wx {hex(bar)}'})
            raw = reply.get('return', '')
            (out / 'xhci-capabilities.txt').write_text(raw)
            capabilities = re.findall(r'0x([0-9a-fA-F]{8})(?=\s|$)', raw)
            if len(capabilities) == 8:
                operational = bar + (int(capabilities[0], 16) & 0xff)
                interrupter = bar + (int(capabilities[6], 16) & ~31) + 32
                for name, address in [('operational', operational), ('interrupter', interrupter)]:
                    reply = qmp('human-monitor-command', {'command-line': f'xp /8wx {hex(address)}'})
                    (out / f'xhci-{name}.txt').write_text(reply.get('return', str(reply)))
            reply = qmp('human-monitor-command', {'command-line': f'xp /1wx {hex(int(xhci_bar[1], 16) + 8)}'})
            raw = reply.get('return', '')
            (out / 'xhci-hcsparams2.txt').write_text(raw)
            words = re.findall(r'0x([0-9a-fA-F]{8})(?=\s|$)', raw)
            if len(words) == 1:
                hcsparams2 = int(words[0], 16)
                scratchpads = (((hcsparams2 >> 21) & 31) << 5) | ((hcsparams2 >> 27) & 31)
        for cpu in range(args.smp):
            reply = qmp('human-monitor-command', {'command-line': 'info lapic', 'cpu-index': cpu})
            (out / f'lapic-{cpu}.txt').write_text(reply.get('return', str(reply)))
        for queue, available, used in re.findall(r'__ProgramQueue desc=(0x[0-9a-f]+), avail=(0x[0-9a-f]+), used=(0x[0-9a-f]+)', text):
            reply = qmp('human-monitor-command', {'command-line': f'xp /136hx {available}'})
            (out / f'queue-{queue}-available.txt').write_text(reply.get('return', str(reply)))
            reply = qmp('human-monitor-command', {'command-line': f'xp /4hx {used}'})
            (out / f'queue-{queue}-used.txt').write_text(reply.get('return', str(reply)))
        registers = (out / 'registers.txt').read_text()
        for cpu, stack in re.findall(r'CPU#(\d+).*?RSP=([0-9a-fA-F]+)', registers, re.S):
            reply = qmp('human-monitor-command', {'command-line': f'x /64gx 0x{stack}', 'cpu-index': int(cpu)})
            (out / f'stack-{cpu}.txt').write_text(reply.get('return', str(reply)))
        for cpu, rsi in re.findall(r'CPU#(\d+).*?RSI=([0-9a-fA-F]+)', registers, re.S):
            reply = qmp('human-monitor-command', {'command-line': f'x /128gx 0x{rsi}', 'cpu-index': int(cpu)})
            (out / f'rsi-{cpu}.txt').write_text(reply.get('return', str(reply)))
        online = 1 + len(set(re.findall(r'CpuCoreStart (\d+) is online', text)))
        result = {'mode': args.mode, 'cpus_online': online, 'boot_complete': ready_at is not None,
                  'observed_seconds': stopped_at - ready_at if ready_at else 0,
                  'panic': failure, 'virtio_completions': text.count('VirtioSplitQueuePoll queue='),
                  'virtio_net_failed': text.count('virtio-net initialization failed'),
                  'capability_denials': text.count('CapabilityVerify missing')}
        usb_trace = (out / 'trace.log').read_bytes()[trace_start:].decode(errors='replace')
        result['usb_successful_transfers'] = usb_trace.count('ER_TRANSFER, CC_SUCCESS')
        irq_mode = {'preferred': 'msix', 'msi': 'msi', 'intx': 'intx'}[args.mode]
        result['usb_irq_mode'] = irq_mode
        all_trace = (out / 'trace.log').read_bytes().decode(errors='replace')
        result['virtio_device_completions'] = all_trace.count('virtio_blk_req_complete')
        result['virtio_guest_completions'] = len(re.findall(
            r'\[trace-\d+-virtio-blk\.dll\] VirtioSplitQueuePoll queue=', text))
        result['virtio_completion_errors'] = re.findall(
            r'virtio_blk_req_complete .* status ([1-9][0-9]*)', all_trace)
        result['virtio_completions_balanced'] = (
            result['virtio_device_completions'] > 0 and
            result['virtio_guest_completions'] == result['virtio_device_completions'])
        result['usb_command_errors'] = len(re.findall(r'ER_COMMAND_COMPLETE, (?!CC_SUCCESS)[A-Z_]+,', all_trace))
        result['usb_control_completions'] = sum(
            ((int(control, 16) >> 16) & 31) == 1
            for control in re.findall(r'ER_TRANSFER, CC_SUCCESS, p 0x[0-9a-f]+, s 0x[0-9a-f]+, c (0x[0-9a-f]+)', all_trace))
        result['usb_short_transfers'] = all_trace.count('ER_TRANSFER, CC_SHORT_PACKET')
        result['usb_transfer_errors'] = re.findall(
            r'ER_TRANSFER, (?!CC_SUCCESS,|CC_SHORT_PACKET,|CC_STOPPED(?:_LENGTH_INVALID|_SHORT_PACKET)?,)(CC_[A-Z_]+),', all_trace)
        result['disable_slot_completions'] = completed_commands.count('CR_DISABLE_SLOT')
        if args.mode == 'preferred':
            result['usb_mode_verified'] = 'msix_write_config dev qemu-xhci enabled 1 masked 0' in all_trace
        else:
            command_writes = re.findall(r'pci_cfg_write qemu-xhci .*? @0x4 <- (0x[0-9a-f]+)', all_trace)
            result['usb_mode_verified'] = bool(command_writes) and not (int(command_writes[-1], 16) & 0x400)
            if args.mode == 'msi':
                result['usb_mode_verified'] = bool(command_writes) and bool(int(command_writes[-1], 16) & 0x400)
        (out / 'keyboard-reports.json').write_text(json.dumps(reports, indent=2))
        payloads = [r['bytes'] for r in reports]
        matched = 0
        for payload in payloads:
            if matched < len(expected) and payload == expected[matched]:
                matched += 1
        result['topology'] = args.topology
        result['xhci_scratchpad_count'] = scratchpads
        result['guest_key_transitions'] = len(guest_events)
        result['guest_key_sequence'] = guest_events == [(5 if key == 'b' else 6, not down) for key, down in key_events]
        result['keyboard_sequence'] = matched == len(expected) and key_index == len(key_events)
        result['keyboard_completions'] = len(reports)
        result['key_cycles_per_attachment'] = args.key_cycles
        result['reconnected'] = reconnect_state == 'attached' and key_index == len(key_events)
        result['hid_report_dispatches'] = text.count('HidHandleReportEvent(')
        result['hid_setup_failed'] = 'HidDeviceCreate failed' in text
        result['pass'] = (result['boot_complete'] and result['observed_seconds'] >= args.observe_seconds and
                          not failure and online == args.smp and not result['virtio_net_failed'] and
                          not result['capability_denials'] and result['virtio_completions'] > 0 and
                          result['virtio_completions_balanced'] and not result['virtio_completion_errors'] and
                          not result['usb_command_errors'] and not result['usb_transfer_errors'] and result['usb_mode_verified'] and
                          result['keyboard_sequence'] and result['guest_key_sequence'] and not result['hid_setup_failed'] and
                          (not args.reconnect or result['reconnected']))
        (out / 'results.json').write_text(json.dumps(result, indent=2))
        print(json.dumps(result), flush=True)
        if not result['pass']:
            raise RuntimeError('Boot acceptance failed; inspect artifacts')
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
