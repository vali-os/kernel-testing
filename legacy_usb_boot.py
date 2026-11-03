"""Boot each legacy USB HCI with an emulated keyboard and capture DMA traffic.

Use freshly packaged binaries. All disks use disposable QEMU snapshots.
The capture verifies hardware report traffic; it does not assert desktop input.
"""
import argparse
import hashlib
import struct
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controller', choices=['uhci', 'ohci', 'ehci', 'xhci'], required=True)
    parser.add_argument('--esp', type=Path, required=True)
    parser.add_argument('--disk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--usb-storage', action='store_true', help='Mount the system disk through USB mass storage')
    parser.add_argument('--late-keyboard', action='store_true', help='Attach the keyboard after the storage boot completes')
    parser.add_argument('--timeout', type=int, default=150)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    hashes = {str(p.relative_to(args.esp)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in args.esp.rglob('*') if p.is_file()}
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    qemu = Path('C:/Program Files/qemu')
    shutil.copyfile(qemu / 'share/edk2-i386-vars.fd', out / 'vars.fd')
    events = {
        'uhci': ['usb_uhci_packet_complete_success', 'usb_uhci_packet_complete_shortxfer', 'usb_uhci_packet_complete_error'],
        'ohci': ['usb_ohci_td_packet_status', 'usb_ohci_die', 'usb_ohci_td_read_error',
                 'usb_ohci_td_bad_pid', 'usb_ohci_td_bad_buf'],
        'xhci': ['usb_xhci_fetch_trb', 'usb_xhci_queue_event', 'usb_xhci_slot_address'],
        'ehci': ['usb_ehci_packet_submit', 'usb_ehci_execute_complete', 'usb_ehci_doorbell_ring', 'usb_ehci_doorbell_ack'],
    }[args.controller]
    (out / 'events.txt').write_text('\n'.join(events) + '\n')
    device = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}[args.controller]
    command = [str(qemu / 'qemu-system-x86_64.exe'), '-machine', 'pc', '-accel', 'tcg',
               '-smp', '4', '-m', '1024', '-display', 'none', '-monitor', 'none', '-net', 'none',
               '-no-reboot', '-d', 'guest_errors', '-serial', f'file:{out.as_posix()}/serial.log', '-qmp', 'stdio',
               '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={qemu.as_posix()}/share/edk2-x86_64-code.fd',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd',
               '-drive', f'if=none,id=esp,format=raw,snapshot=on,file=fat:ro:{args.esp.resolve().as_posix()}',
               '-device', 'ide-hd,drive=esp,bootindex=1',
               '-drive', f'if=none,id=osdisk,format=raw,snapshot=on,file={args.disk.resolve().as_posix()}',
               '-netdev', 'hubport,id=net0,hubid=0', '-device', 'virtio-net-pci,netdev=net0',
               '-device', f'{device},id=usb']
    if not args.late_keyboard:
        command += ['-device', f'usb-kbd,id=kbd,bus=usb.0,port=1,pcap={out.as_posix()}/keyboard.pcap']
    command += ['-device', ('usb-storage,drive=osdisk,bus=usb.0,port=2' if args.usb_storage else
                            'virtio-blk-pci,drive=osdisk')]
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            if not process.stdout.readline():
                raise RuntimeError('QEMU startup failed; inspect qemu.log')

            def qmp(execute, arguments=None):
                request = {'execute': execute}
                if arguments is not None:
                    request['arguments'] = arguments
                process.stdin.write((json.dumps(request) + '\n').encode())
                process.stdin.flush()
                while True:
                    reply = json.loads(process.stdout.readline())
                    if 'error' in reply:
                        raise RuntimeError(str(reply['error']))
                    if 'return' in reply:
                        return reply['return']

            qmp('qmp_capabilities')
            start = time.monotonic()
            ready_at = None
            last_key = 0
            transitions = 0
            failures = []
            text = ''
            while time.monotonic() - start < args.timeout:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited early')
                text = (out / 'serial.log').read_text(errors='replace')
                failures = [m for m in ('DebugPanic(', 'MSSync timeout', 'did not complete startup', 'Crashed in module', 'Received signal') if m in text]
                if failures:
                    break
                if ready_at is None and all(m in text for m in ('InstallBundledApplications no bundled apps!',
                        'storage_ready_invocation(path=/boot)', 'storage_ready_invocation(path=/data)')):
                    if args.late_keyboard:
                        qmp('device_add', {'driver': 'usb-kbd', 'id': 'kbd', 'bus': 'usb.0',
                                          'port': '1', 'pcap': f'{out.as_posix()}/keyboard.pcap'})
                    ready_at = time.monotonic()
                    print(f'{args.controller}: boot complete; sending keyboard transitions', flush=True)
                if ready_at is not None:
                    if time.monotonic() - last_key > 0.75:
                        key = 'b' if (transitions // 2) % 2 == 0 else 'c'
                        qmp('input-send-event', {'events': [{'type': 'key', 'data': {
                            'down': transitions % 2 == 0, 'key': {'type': 'qcode', 'data': key}}}]})
                        transitions += 1
                        last_key = time.monotonic()
                    if time.monotonic() - ready_at >= 40:
                        break
                time.sleep(0.1)
            qmp('stop')
            trace = (out / 'trace.log').read_text(errors='replace')
            capture = (out / 'keyboard.pcap').read_bytes() if (out / 'keyboard.pcap').exists() else b''
            # QEMU writes DLT_USB_LINUX_MMAPPED records with a 64-byte USB
            # header. Match completed interrupt-IN payloads, never PCAP headers.
            if capture and (capture[:4] != bytes.fromhex('d4c3b2a1') or struct.unpack_from('<I', capture, 20)[0] != 220):
                raise RuntimeError('Unexpected USB capture format')
            reports = []
            position = 24
            while position + 16 <= len(capture):
                length = struct.unpack_from('<I', capture, position + 8)[0]
                position += 16
                record = capture[position:position + length]
                position += length
                if len(record) >= 64 and record[8:11] == bytes([ord('C'), 1, 0x81]):
                    reports.append(record[64:])
            reports_b = reports.count(bytes([0, 0, 5, 0, 0, 0, 0, 0]))
            reports_c = reports.count(bytes([0, 0, 6, 0, 0, 0, 0, 0]))
            guest_reports = text.count('HidHandleReportEvent(')
            result = {'controller': args.controller, 'usb_storage': args.usb_storage, 'boot_complete': ready_at is not None,
                      'observed_seconds': time.monotonic() - ready_at if ready_at else 0,
                      'failures': failures, 'key_transitions': transitions,
                      'keyboard_reports_b': reports_b, 'keyboard_reports_c': reports_c,
                      'guest_hid_reports': guest_reports,
                      'events': {e: trace.count(e + ' ') for e in events}}
            result['pass'] = (result['boot_complete'] and result['observed_seconds'] >= 40
                              and not failures and reports_b >= 4 and reports_c >= 4 and guest_reports >= 10)
            (out / 'results.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result), flush=True)
            if not result['pass']:
                raise RuntimeError('Legacy USB boot verification failed; inspect artifacts')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
