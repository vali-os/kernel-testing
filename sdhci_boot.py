"""Boot the system image with a QEMU sdhci-pci host and check the SDHCI driver.

The system boots from its normal VirtIO disk. The runner adds one sdhci-pci
host and, unless --empty-slot is given, an SD card backed by a raw image that
it creates in the output directory. It then reads the serial log for the
driver's lines: the host summary (version, capabilities, ADMA2 format and
table size, interrupt kind) and the card setup outcome, either the identified
card with the expected capacity and addressing, or "no card in the slot".

QEMU's sdhci_* and sdcard_* trace events are written to trace.log, so the
commands, ADMA transfers and register accesses the driver made can be checked
after the run. QEMU snapshots the system disk, so the source image is never
modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time

# QEMU's SD card model switches from a standard capacity card (CSD version 1,
# byte addressing) to a high capacity card (CSD version 2, sector addressing)
# above 2 GiB. It also requires a power-of-two image size.
SDSC_LIMIT_MIB = 2048

# Lines that mean the guest or a driver failed, whatever this test checks.
FAILURE_MARKERS = ('DebugPanic(', 'FATAL EXCEPTION OCCURRED', 'MSSync timeout',
                   'did not complete startup', 'Crashed in module', 'Received signal',
                   'CapabilityVerify missing')

HOST_READY = re.compile(
    r'SDHCI (\d+): host ready, version 0x([0-9a-fA-F]+), capabilities 0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+), '
    r'base clock (\d+) Hz, card clock (\d+) to (\d+) Hz, voltages 0x([0-9a-fA-F]+), (\w+) slot, '
    r'ADMA2 (32|64)-bit with (\d+) entries, (\S+) interrupt')
CARD_READY = re.compile(
    r'SDHCI (\d+): SD card "([^"]*)" \(manufacturer 0x([0-9a-fA-F]+), serial 0x([0-9a-fA-F]+)\), '
    r'(\d+) sectors \((\d+) MiB\), (sector|byte) addressing, (\d+)-bit bus at (\d+) Hz')
NO_CARD = re.compile(r'SDHCI (\d+): no card in the slot')
SETUP_FAILED = re.compile(r'SDHCI (\d+): card setup failed')
DRIVER_PROBLEM = re.compile(r'\[(warn|error)-\d+-sdhci\.dll\]\s*(.*)')


def decode_capabilities(low, high):
    """Decode the capability register fields that bring-up records."""
    slot_types = {0: 'removable', 1: 'embedded', 2: 'shared bus', 3: 'reserved'}
    return {
        'base_clock_field_mhz': (low >> 8) & 0xff,
        'max_block_bytes': 512 << ((low >> 16) & 0x3),
        'bus_8bit': bool(low & (1 << 18)),
        'adma2': bool(low & (1 << 19)),
        'high_speed': bool(low & (1 << 21)),
        'sdma': bool(low & (1 << 22)),
        'voltage_3v3': bool(low & (1 << 24)),
        'voltage_3v0': bool(low & (1 << 25)),
        'voltage_1v8': bool(low & (1 << 26)),
        'system_bus_64bit': bool(low & (1 << 28)),
        'slot_type': slot_types[(low >> 30) & 0x3],
        'capabilities1': f'0x{high:08x}',
    }


def summarize_trace(trace):
    """Count what the trace shows about commands, DMA and register widths."""
    commands = [int(match) for match in re.findall(r'sdhci_send_command CMD(\d+)', trace)]
    return {
        'commands': commands,
        'adma_transfers_completed': trace.count('sdhci_adma_transfer_completed'),
        'adma_descriptors': trace.count('sdhci_adma_loop'),
        'end_transfers': trace.count('sdhci_end_transfer'),
        'errors': [line for line in trace.splitlines() if 'sdhci_error' in line],
        # The engine writes block size and count (0x04), and transfer mode and
        # command (0x0c), each as one 32-bit write.
        'writes_32bit_block_size_count': len(re.findall(r'wr32: addr\[0x0004\]', trace)),
        'writes_32bit_transfer_mode_command': len(re.findall(r'wr32: addr\[0x000c\]', trace)),
        'writes_16bit_command': len(re.findall(r'wr16: addr\[0x000e\]', trace)),
        'dataport_accesses': trace.count('sdhci_read_dataport') + trace.count('sdhci_write_dataport'),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--disk', type=Path, required=True, help='Packaged system disk (GPT image)')
    parser.add_argument('--output', type=Path, required=True, help='New directory for the run artifacts')
    parser.add_argument('--qemu', type=Path, help='QEMU executable. Defaults to qemu-system-x86_64 or '
                        'qemu-system-aarch64 from PATH')
    parser.add_argument('--firmware', type=Path, required=True, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, required=True, help='UEFI variables template, copied')
    parser.add_argument('--empty-slot', action='store_true', help='Add the host without an SD card')
    parser.add_argument('--card-size-mib', type=int, default=64,
                        help='Size of the SD card image, a power of two. Above 2048 the card is SDHC')
    parser.add_argument('--timeout', type=int, default=240)
    parser.add_argument('--observe-seconds', type=int, default=15,
                        help='Seconds to keep running after the outcome line, to catch late errors')
    args = parser.parse_args()
    size = args.card_size_mib
    if size <= 0 or size & (size - 1):
        parser.error('--card-size-mib must be a power of two')
    if args.timeout <= args.observe_seconds:
        parser.error('--timeout must be longer than --observe-seconds')

    out = args.output.resolve()
    disk = args.disk.resolve()
    out.mkdir(parents=True, exist_ok=False)
    executable = str(args.qemu or ('qemu-system-aarch64' if args.arch == 'arm64' else 'qemu-system-x86_64'))
    files = [disk, args.firmware.resolve(), args.firmware_vars.resolve()]
    hashes = {}
    for path in files:
        with path.open('rb') as source:
            hashes[str(path)] = hashlib.file_digest(source, 'sha256').hexdigest()
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    version = subprocess.run([executable, '--version'], check=True, capture_output=True, text=True).stdout
    (out / 'qemu-version.txt').write_text(version)
    shutil.copyfile(args.firmware_vars, out / 'vars.fd')
    (out / 'events.txt').write_text('sdhci_*\nsdcard_*\n')

    machine = 'virt-11.1,gic-version=3' if args.arch == 'arm64' else 'pc'
    command = [executable, '-machine', machine, '-accel', 'tcg',
               '-smp', '4', '-m', '4096', '-display', 'none', '-monitor', 'none',
               '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
               '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={args.firmware.resolve()}',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd']
    if args.arch == 'arm64':
        command += ['-cpu', 'cortex-a57']
    command += ['-drive', f'if=none,id=storage,format=raw,snapshot=on,file={disk.as_posix()}',
                '-device', 'virtio-blk-pci,drive=storage,bootindex=1',
                '-device', 'sdhci-pci,id=sdhci']
    if not args.empty_slot:
        # A sparse file, so a large SDHC card costs no disk space.
        card = out / 'sdcard.raw'
        with card.open('wb') as image:
            image.truncate(size * 1024 * 1024)
        command += ['-drive', f'if=none,id=sdcard,format=raw,file={card.as_posix()}',
                    '-device', 'sd-card,drive=sdcard']
    (out / 'command.json').write_text(json.dumps(command, indent=2))

    with (out / 'qemu.log').open('w') as log:
        # QEMU's temporary disk snapshots must be writable even when the
        # host's default /var/tmp is read-only in a restricted test environment.
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
                                   text=True, bufsize=1, env={**os.environ, 'TMPDIR': str(out)})
        replies = queue.Queue()

        def read_replies():
            for line in process.stdout:
                replies.put(line)
            replies.put(None)

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
            outcome_at = None
            text = ''
            failures = []
            while time.monotonic() - started < args.timeout:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited early')
                text = (out / 'serial.log').read_text(errors='replace')
                failures = [line for line in text.splitlines() if any(marker in line for marker in FAILURE_MARKERS)]
                if failures:
                    break
                outcome = CARD_READY.search(text) or NO_CARD.search(text) or SETUP_FAILED.search(text)
                if outcome and outcome_at is None:
                    outcome_at = time.monotonic()
                    print(f'SDHCI: {args.arch} card setup ended; observing {args.observe_seconds}s', flush=True)
                if outcome_at is not None and time.monotonic() - outcome_at >= args.observe_seconds:
                    break
                time.sleep(1)

            observed = time.monotonic() - outcome_at if outcome_at else 0
            qmp('stop')
            (out / 'pci.txt').write_text(qmp('human-monitor-command', {'command-line': 'info pci'}))
            trace = (out / 'trace.log').read_text(errors='replace')
            host = HOST_READY.search(text)
            card = CARD_READY.search(text)
            result = {
                'arch': args.arch,
                'slot': 'empty' if args.empty_slot else f'{size} MiB card',
                'observed_seconds': observed,
                'failures': failures,
                'driver_problems': [match.group(0) for match in DRIVER_PROBLEM.finditer(text)],
                'services': [name for name in ('process', 'file', 'device', 'net', 'usb', 'serve')
                             if f'path=/service/{name})' in text],
                'data_mounted': 'storage_ready_invocation(path=/data)' in text,
                'host_ready': host.group(0) if host else None,
                'card_ready': card.group(0) if card else None,
                'no_card': NO_CARD.search(text) is not None,
                'setup_failed': [line for line in text.splitlines() if SETUP_FAILED.search(line)],
                'trace': summarize_trace(trace),
            }
            if host:
                result['host'] = {
                    'version_register': f'0x{host.group(2)}',
                    'spec_version': f'{(int(host.group(2), 16) & 0xff) + 1}.00',
                    'base_clock_hz': int(host.group(5)),
                    'card_clock_hz': [int(host.group(6)), int(host.group(7))],
                    'slot': host.group(9),
                    'adma2_format': f'{host.group(10)}-bit',
                    'adma2_entries': int(host.group(11)),
                    'interrupt': host.group(12),
                    'capabilities': decode_capabilities(int(host.group(3), 16), int(host.group(4), 16)),
                }

            errors = [line for line in result['driver_problems'] if line.startswith('[error')]
            passed = host is not None and observed >= args.observe_seconds and not failures and not errors
            if args.empty_slot:
                passed &= result['no_card'] and card is None
            else:
                expected_sectors = size * 2048
                expected_addressing = 'sector' if size > SDSC_LIMIT_MIB else 'byte'
                result['expected'] = {'sectors': expected_sectors, 'addressing': expected_addressing}
                passed &= (card is not None and int(card.group(5)) == expected_sectors
                           and card.group(7) == expected_addressing)
                # The SCR read during card setup is an ADMA2 data transfer.
                passed &= result['trace']['adma_transfers_completed'] > 0
                passed &= result['trace']['dataport_accesses'] == 0
            result['pass'] = passed
            (out / 'results.json').write_text(json.dumps(result, indent=2))
            print(json.dumps(result, indent=2), flush=True)
            if not passed:
                raise RuntimeError('SDHCI boot acceptance failed; inspect artifacts')
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
