"""Boot the system disk through VirtIO and verify guest filesystem reads and writes.

Requires a freshly packaged EFI tree and system disk with no /data/served
directories, so service startup exercises writes. QEMU snapshots all writable
drives; the source disk and EFI files are never modified.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--esp', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disk', type=Path, required=True, help='Fresh system disk without service-created directories')
    parser.add_argument('--qemu-dir', type=Path, default=Path('C:/Program Files/qemu'))
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--observe-seconds', type=int, default=45)
    args = parser.parse_args()
    if args.observe_seconds < 40 or args.timeout <= args.observe_seconds:
        parser.error('Observe at least 40 seconds, with a longer total timeout')
    out = args.output.resolve()
    esp = args.esp.resolve()
    disk = args.disk.resolve()
    out.mkdir(parents=True, exist_ok=False)
    qemu = args.qemu_dir
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (esp / 'EFI/vali').iterdir() if p.is_file()}
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    shutil.copyfile(qemu / 'share/edk2-i386-vars.fd', out / 'vars.fd')
    events = ['virtio_blk_req_complete']
    (out / 'events.txt').write_text('\n'.join(events) + '\n')
    command = [str(qemu / 'qemu-system-x86_64.exe'), '-machine', 'pc', '-accel', 'tcg',
               '-smp', '4', '-m', '1024', '-display', 'none', '-monitor', 'none',
               '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
               '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={qemu.as_posix()}/share/edk2-x86_64-code.fd',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd',
               '-drive', f'if=none,id=esp,format=raw,snapshot=on,file=fat:ro:{esp.as_posix()}',
               '-device', 'ide-hd,drive=esp,bootindex=1',
               '-drive', f'if=none,id=storage,format=raw,snapshot=on,file={disk.as_posix()}',
               '-device', 'virtio-blk-pci,drive=storage',
               '-netdev', 'hubport,id=net0,hubid=0', '-device', 'virtio-net-pci,netdev=net0',
               '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,port=1']
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        try:
            if not process.stdout.readline():
                raise RuntimeError('QEMU startup failed; inspect qemu.log')

            def qmp(execute):
                process.stdin.write((json.dumps({'execute': execute}) + '\n').encode())
                process.stdin.flush()
                while True:
                    line = process.stdout.readline()
                    if not line:
                        raise RuntimeError('QMP connection closed')
                    reply = json.loads(line)
                    if 'error' in reply:
                        raise RuntimeError(str(reply['error']))
                    if 'return' in reply:
                        return reply['return']

            qmp('qmp_capabilities')
            started = time.monotonic()
            ready_at = None
            failures = []
            services = ('process', 'file', 'device', 'net', 'usb', 'serve')
            text = ''
            while time.monotonic() - started < args.timeout:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited early')
                text = (out / 'serial.log').read_text(errors='replace')
                failures = [line for line in text.splitlines() if any(marker in line for marker in
                            ('DebugPanic(', 'MSSync timeout', 'did not complete startup',
                             'Crashed in module', 'Received signal', 'CapabilityVerify missing'))
                            or re.search(r'\[(?:warn|error)-\d+-virtio-(?:blk|net)\.dll\]', line)]
                if failures:
                    break
                ready = all(marker in text for marker in
                            ('storage_ready_invocation(path=/boot)', 'storage_ready_invocation(path=/data)',
                             'InstallBundledApplications no bundled apps!', 'virtio-net ready device='))
                ready &= all(f'path=/service/{service})' in text for service in services)
                if ready and ready_at is None:
                    ready_at = time.monotonic()
                    print(f'VirtIO: system partitions mounted and services ready; observing {args.observe_seconds}s', flush=True)
                if ready_at is not None and time.monotonic() - ready_at >= args.observe_seconds:
                    break
                time.sleep(1)
            qmp('stop')
            blocks = qmp('query-blockstats')
            (out / 'block-stats.json').write_text(json.dumps(blocks, indent=2))
            storage = next(block['stats'] for block in blocks if block.get('device') == 'storage')
            trace = (out / 'trace.log').read_text(errors='replace')
            observed = time.monotonic() - ready_at if ready_at else 0
            result = {'boot_complete': ready_at is not None, 'observed_seconds': observed,
                      'cpus_online': 1 + len(set(re.findall(r'CpuCoreStart (\d+) is online', text))),
                      'failures': failures,
                      'virtio_completions': trace.count('virtio_blk_req_complete '),
                      'virtio_errors': re.findall(r'virtio_blk_req_complete .* status ([1-9][0-9]*)', trace),
                      'storage_reads': storage['rd_operations'], 'storage_writes': storage['wr_operations'],
                      'storage_read_bytes': storage['rd_bytes'], 'storage_write_bytes': storage['wr_bytes'],
                      'guest_created_directories': 'FsCreate(parent=served, name=apps)' in text}
            result['pass'] = (result['boot_complete'] and observed >= args.observe_seconds
                              and result['cpus_online'] == 4 and not failures
                              and not result['virtio_errors'] and result['virtio_completions'] > 0
                              and result['storage_reads'] > 0 and result['storage_writes'] > 0
                              and result['guest_created_directories'])
            (out / 'results.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result), flush=True)
            if not result['pass']:
                raise RuntimeError('VirtIO boot acceptance failed; inspect artifacts')
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
