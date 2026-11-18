"""Boot the system disk through VirtIO and check filesystem mounts and block I/O.

Requires a freshly packaged EFI tree and system disk with no /data/served
directories, so service startup exercises writes. QEMU snapshots all writable
drives; the source disk and EFI files are never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import struct
import subprocess
import threading
import time


def capture_acpi(qmp, serial, output):
    """Copy the actual ARM64 firmware tables from the loader's reported RSDP."""
    match = re.search(r"ARM64 ACPI ([0-9A-Fa-f]+),", serial)
    if match is None:
        raise RuntimeError("Loader did not report an ACPI root address")
    directory = output / "acpi"
    directory.mkdir()

    def read_physical(address, length, path):
        qmp('pmemsave', {'val': address, 'size': length, 'filename': str(path)})
        data = path.read_bytes()
        if len(data) != length:
            raise RuntimeError(f"Short physical memory capture: {path}")
        return data

    def read_table(address, name):
        header = read_physical(address, 36, directory / 'header.tmp')
        length = struct.unpack_from('<I', header, 4)[0]
        if length < 36 or length > 1024 * 1024:
            raise RuntimeError(f"Invalid captured ACPI table length: {length}")
        data = read_physical(address, length, directory / name)
        if sum(data) & 0xff:
            raise RuntimeError(f"Captured ACPI table checksum failed: {name}")
        return data

    address = int(match[1], 16)
    rsdp = read_physical(address, 36, directory / 'RSDP.bin')
    if (rsdp[:8] != b'RSD PTR ' or rsdp[15] < 2 or struct.unpack_from('<I', rsdp, 20)[0] != 36
            or sum(rsdp[:20]) & 0xff or sum(rsdp) & 0xff):
        raise RuntimeError("Captured RSDP failed validation")
    xsdt = read_table(struct.unpack_from('<Q', rsdp, 24)[0], 'XSDT.bin')
    if xsdt[:4] != b'XSDT' or (len(xsdt) - 36) % 8 or (len(xsdt) - 36) // 8 > 256:
        raise RuntimeError("Captured XSDT has an invalid layout")
    tables = {}
    for index, offset in enumerate(range(36, len(xsdt), 8)):
        entry = struct.unpack_from('<Q', xsdt, offset)[0]
        data = read_table(entry, f'table-{index}.bin')
        signature = data[:4].decode('ascii')
        if re.fullmatch(r'[A-Z0-9_]{4}', signature) is None:
            raise RuntimeError(f"Invalid captured ACPI signature: {signature!r}")
        target = directory / f'{signature}-{index}.bin'
        (directory / f'table-{index}.bin').rename(target)
        tables[target.name] = {'address': entry, 'length': len(data)}
        if signature == 'FACP':
            if len(data) < 44:
                raise RuntimeError("Captured FADT has no DSDT pointer")
            dsdt = struct.unpack_from('<Q', data, 140)[0] if len(data) >= 148 else 0
            if dsdt == 0:
                dsdt = struct.unpack_from('<I', data, 40)[0]
            namespace = read_table(dsdt, 'DSDT.bin')
            if namespace[:4] != b'DSDT':
                raise RuntimeError("FADT did not point to a DSDT")
            tables['DSDT.bin'] = {'address': dsdt, 'length': len(namespace)}
    (directory / 'header.tmp').unlink()
    if 'DSDT.bin' not in tables or not any(name.startswith('IORT-') for name in tables):
        raise RuntimeError("Boot firmware capture is missing DSDT or IORT")
    (directory / 'tables.json').write_text(json.dumps(tables, indent=2))
    return tables


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--esp', type=Path, help='Separate EFI tree. Omit when --disk contains its EFI partition.')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disk', type=Path, required=True, help='Fresh system disk without service-created directories')
    parser.add_argument('--qemu-dir', type=Path, default=Path('C:/Program Files/qemu'))
    parser.add_argument('--qemu', type=Path, help='Explicit QEMU executable')
    parser.add_argument('--firmware', type=Path, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, help='UEFI variables template copied for this run')
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--iommu', choices=['none', 'smmuv3'], default='none',
                        help='ARM64 smmuv3 requires DMA rejection instead of storage success')
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--observe-seconds', type=int, default=45)
    parser.add_argument('--blank-ahci-disk', action='store_true',
                        help='Also verify filed handles an unformatted AHCI disk without crashing')
    args = parser.parse_args()
    if args.observe_seconds < 45 or args.timeout <= args.observe_seconds:
        parser.error('Observe at least 45 seconds, with a longer total timeout')
    if args.arch != 'arm64' and args.iommu != 'none':
        parser.error('--iommu=smmuv3 requires --arch=arm64')
    if args.arch == 'arm64' and args.blank_ahci_disk:
        parser.error('--blank-ahci-disk requires AMD64')
    out = args.output.resolve()
    esp = args.esp.resolve() if args.esp else None
    disk = args.disk.resolve()
    out.mkdir(parents=True, exist_ok=False)
    qemu = args.qemu_dir
    executable = args.qemu or qemu / ('qemu-system-aarch64.exe' if args.arch == 'arm64' else 'qemu-system-x86_64.exe')
    firmware = args.firmware or qemu / 'share/edk2-x86_64-code.fd'
    variables = args.firmware_vars or qemu / 'share/edk2-i386-vars.fd'
    if args.arch == 'arm64' and (args.firmware is None or args.firmware_vars is None):
        parser.error('ARM64 requires --firmware and --firmware-vars')
    files = [disk, firmware, variables]
    if esp is not None:
        files += [path for path in (esp / 'EFI/vali').iterdir() if path.is_file()]
    hashes = {}
    for path in files:
        with path.open('rb') as source:
            hashes[str(path)] = hashlib.file_digest(source, 'sha256').hexdigest()
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    version = subprocess.run([str(executable), '--version'], check=True, capture_output=True, text=True).stdout
    (out / 'qemu-version.txt').write_text(version)
    shutil.copyfile(variables, out / 'vars.fd')
    (out / 'events.txt').write_text('virtio_blk_req_complete\n')
    machine = f'virt-11.1,gic-version=3,iommu={args.iommu}' if args.arch == 'arm64' else 'pc'
    if args.iommu == 'smmuv3':
        # QEMU 11.1.3 gives its default ITS and SMMUv3 nodes identifier zero.
        # Omit ITS so this case reaches route rejection with a valid IORT.
        # The captured tables preserve the exact firmware used for the test.
        machine += ',its=off'
    command = [str(executable), '-machine', machine, '-accel', 'tcg',
               '-smp', '4', '-m', '4096', '-display', 'none', '-monitor', 'none',
               '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
               '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={firmware.resolve()}',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd']
    if args.arch == 'arm64':
        command += ['-cpu', 'cortex-a57']
    if esp is not None:
        command += ['-drive', f'if=none,id=esp,format=raw,snapshot=on,file=fat:ro:{esp.as_posix()}',
                    '-device', ('virtio-blk-pci' if args.arch == 'arm64' else 'ide-hd') + ',drive=esp,bootindex=1']
    command += ['-drive', f'if=none,id=storage,format=raw,snapshot=on,file={disk.as_posix()}',
                '-device', 'virtio-blk-pci,drive=storage' + (',bootindex=1' if esp is None else '')]
    # Preserve the original AMD64 device exercise when using a separate ESP.
    # Whole-disk boots exercise storage without additional network/USB devices.
    legacy_devices = args.arch == 'amd64' and esp is not None
    if legacy_devices:
        command += ['-netdev', 'hubport,id=net0,hubid=0', '-device', 'virtio-net-pci,netdev=net0',
                    '-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,port=1']
    if args.blank_ahci_disk:
        with (out / 'blank.raw').open('wb') as blank:
            blank.truncate(16 * 1024 * 1024)
        command += ['-device', 'ich9-ahci,id=ahci',
                    '-drive', f'if=none,id=blank,format=raw,snapshot=on,file={out.as_posix()}/blank.raw',
                    '-device', 'ide-hd,bus=ahci.0,drive=blank']
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    with (out / 'qemu.log').open('w') as log:
        # QEMU's temporary disk snapshots must be writable even when the
        # host's default /var/tmp is read-only in a restricted test environment.
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                   text=True, bufsize=1,
                                   env={**os.environ, 'TMPDIR': str(out)},
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        replies = queue.Queue()

        def read_replies():
            for line in process.stdout:
                replies.put(line)
            replies.put(None)

        # A reader thread keeps QMP deadlines effective on both Windows pipes
        # and POSIX. It exits when QEMU closes stdout during final cleanup.
        reader = threading.Thread(target=read_replies, daemon=True)
        reader.start()

        def receive():
            line = replies.get(timeout=10)
            if line is None:
                raise RuntimeError('QMP connection closed')
            return json.loads(line)

        def qmp(execute, arguments=None):
            request = {'execute': execute}
            if arguments is not None:
                request['arguments'] = arguments
            process.stdin.write(json.dumps(request) + '\n')
            process.stdin.flush()
            while True:
                reply = receive()
                if 'error' in reply:
                    raise RuntimeError(str(reply['error']))
                if 'return' in reply:
                    return reply['return']

        try:
            receive()
            qmp('qmp_capabilities')
            started = time.monotonic()
            ready_at = None
            failures = []
            services = ('process', 'file', 'device', 'net', 'usb', 'serve')
            text = ''
            negative = args.iommu == 'smmuv3'
            boot_mounted = data_mounted = direct_policy = dma_rejected = False
            while time.monotonic() - started < args.timeout:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited early')
                text = (out / 'serial.log').read_text(errors='replace')
                failures = [line for line in text.splitlines() if any(marker in line for marker in
                            ('DebugPanic(', 'MSSync timeout', 'did not complete startup',
                             'Crashed in module', 'FATAL EXCEPTION OCCURRED', 'Received signal', 'CapabilityVerify missing'))
                            or (not negative and re.search(r'\[(?:warn|error)-\d+-virtio-(?:blk|net)\.dll\]', line))]
                if not negative:
                    failures += [line for line in text.splitlines()
                                 if 'Failed to initialize Virtio block device:' in line]
                boot_mounted = 'storage_ready_invocation(path=/boot)' in text
                data_mounted = 'storage_ready_invocation(path=/data)' in text
                direct_policy = 'coherent direct DMA limit=' in text
                dma_rejected = ('Virtio DMA context creation failed: 8' in text and
                                'DMA policy unavailable (8)' in text and
                                'has no supported direct policy (8)' in text)
                if failures:
                    break
                ready = all(f'path=/service/{service})' in text for service in services)
                if negative:
                    ready &= dma_rejected and not direct_policy and not boot_mounted and not data_mounted
                else:
                    ready &= data_mounted and 'InstallBundledApplications no bundled apps!' in text
                    if args.arch == 'arm64':
                        # The ARM64 recipe has EFI FAT and MFS /data partitions.
                        # Only the AMD64 recipe includes an MFS /boot partition.
                        ready &= direct_policy
                    else:
                        ready &= boot_mounted
                    if legacy_devices:
                        ready &= 'virtio-net ready device=' in text
                if not ready:
                    ready_at = None
                if ready and ready_at is None:
                    ready_at = time.monotonic()
                    print(f'VirtIO: {args.arch}/{args.iommu} checkpoints reached; observing {args.observe_seconds}s', flush=True)
                if ready_at is not None and time.monotonic() - ready_at >= args.observe_seconds:
                    break
                time.sleep(1)
            observed = time.monotonic() - ready_at if ready_at else 0
            qmp('stop')
            if args.arch == 'arm64':
                capture_acpi(qmp, text, out)
            blocks = qmp('query-blockstats')
            (out / 'block-stats.json').write_text(json.dumps(blocks, indent=2))
            storage = next(block['stats'] for block in blocks if block.get('device') == 'storage')
            trace = (out / 'trace.log').read_text(errors='replace')
            result = {'boot_complete': ready_at is not None, 'observed_seconds': observed,
                      'arch': args.arch, 'iommu': args.iommu,
                      'boot_mounted': boot_mounted, 'data_mounted': data_mounted,
                      'direct_policy': direct_policy, 'dma_rejected': dma_rejected,
                      'cpus_online': 1 + len(set(re.findall(r'CpuCoreStart (\d+) is online', text))),
                      'failures': failures,
                      'virtio_completions': trace.count('virtio_blk_req_complete '),
                      'virtio_errors': re.findall(r'virtio_blk_req_complete .* status ([1-9][0-9]*)', trace),
                      'storage_reads': storage['rd_operations'], 'storage_writes': storage['wr_operations'],
                      'storage_read_bytes': storage['rd_bytes'], 'storage_write_bytes': storage['wr_bytes'],
                      # MFS logs this only after MfsCreateRecord succeeds. The
                      # entry trace alone also appears when a write fails.
                      'guest_created_directories': re.search(
                          r'FsCreate returned 0 \(0x[0-9a-fA-F]+\) apps\r?$', text, re.MULTILINE) is not None}
            result['pass'] = (result['boot_complete'] and observed >= args.observe_seconds
                              and result['cpus_online'] == 4 and not failures
                              and not result['virtio_errors'] and result['virtio_completions'] > 0
                              and result['storage_reads'] > 0 and result['storage_writes'] > 0
                              and result['guest_created_directories'])
            if negative:
                # UEFI uses block I/O to load the OS. Its trace is not evidence
                # that a production driver obtained a DMA context.
                result['pass'] = (result['boot_complete'] and observed >= args.observe_seconds
                                  and result['cpus_online'] == 4 and not failures and dma_rejected
                                  and not direct_policy and not boot_mounted and not data_mounted)
            if args.blank_ahci_disk:
                blank = next(block['stats'] for block in blocks if block.get('device') == 'blank')
                result['blank_disk_reads'] = blank['rd_operations']
                result['blank_disk_writes'] = blank['wr_operations']
                result['unknown_filesystem_reported'] = 'Unknown filesystem detected' in text
                result['pass'] &= (result['unknown_filesystem_reported']
                                   and result['blank_disk_reads'] >= 3
                                   and result['blank_disk_writes'] == 0)
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
            reader.join(timeout=5)


if __name__ == '__main__':
    main()
