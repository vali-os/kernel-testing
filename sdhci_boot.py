"""Boot the system image with a QEMU sdhci-pci host and check the SDHCI driver.

The system boots from its normal VirtIO disk. The runner adds one sdhci-pci
host and, unless --empty-slot is given, an SD card backed by a raw image in the
output directory. The card can be:

- The default test card: a GPT disk with one MFS partition labelled sd-test
  that holds generated files (testing/sdhci_card.py). filed has no default
  path for that label, so it mounts it only at /storage/<Serial>/sd-test.
  Nothing in the system writes to it, so the card must come back unchanged.
- --data-card: the same card, but its partition is labelled vali-data, so
  filed binds it at /data and served's first start creates its directories
  there. That writes to the card. The system disk must then have no vali-data
  partition of its own (see "system-image" in testing/sdhci_card.py),
  otherwise the VirtIO disk could take /data first.
  The card carries files in its root and in subdirectories, and they must
  still be there by name with the same contents after the run. A mkcdk that
  writes a broken MFS (1.5.8) gets a card without files instead, see
  build_card in testing/sdhci_card.py. --mkcdk picks the image builder.
  --reuse-card boots again with the card an earlier --data-card run left
  behind. served then finds its directories and creates nothing, so the OS
  reads back what it wrote and the card must not change.
- --blank-card: a zero filled card, as the identification runs used. filed
  finds no partition table on it.

The runner reads the serial log for the driver's lines (host summary, card
setup outcome, disk registration and the stat answer) and for filed's lines
(registration, the /storage/<Serial> mount, partition parsing and the MFS
mount). QEMU's sdhci_* and sdcard_* trace events go to trace.log, and the
runner lists every data command the card received with its argument and
block count. After QEMU has quit, the card image is compared with the copy
made before the boot: changed sectors are listed in runs together with the
MFS structure each belongs to, and every generated file must still read back
with its original hash. QEMU snapshots the system disk, so the source image is
never modified.
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

import sdhci_card

# QEMU's SD card model switches from a standard capacity card (CSD version 1,
# byte addressing) to a high capacity card (CSD version 2, sector addressing)
# above 2 GiB. It also requires a power-of-two image size.
SDSC_LIMIT_MIB = 2048

# Lines that mean the guest or a driver failed, whatever this test checks.
FAILURE_MARKERS = ('DebugPanic(', 'FATAL EXCEPTION OCCURRED', 'MSSync timeout',
                   'did not complete startup', 'Crashed in module', 'Received signal',
                   'CapabilityVerify missing')

# The driver's summary lines. The system log cuts messages at about 128
# characters, so the host summary takes two lines.
HOST_REGISTERS = re.compile(
    r'SDHCI (\d+): host version 0x([0-9a-fA-F]+), capabilities 0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+), '
    r'base clock (\d+) Hz')
HOST_READY = re.compile(
    r'SDHCI (\d+): host ready, (\w+) slot, card clock (\d+) to (\d+) Hz, '
    r'ADMA2 (32|64)-bit with (\d+) entries, (\S+) interrupt')
# The card layer's lines (modules/storage/sd): the card that setup found, and
# what the slot saw on its card detect line when it started.
CARD_READY = re.compile(
    r'SD: card ready, manufacturer 0x([0-9a-fA-F]+), product "([^"]*)", (\d+) MiB, (sector|byte) addressing, '
    r'(\d+) data lines?, (default speed|high speed), (\d+) Hz')
# The slot's line with the bus the card runs on after setup.
SLOT_BUS = re.compile(
    r'SD slot (\d+): card ready, (\d+) data lines?, (default speed|high speed), (\d+) Hz(?:, read-only)?')
NO_CARD = re.compile(r'SD slot (\d+): card detect line reports no card')
SETUP_FAILED = re.compile(r'SD setup: step \S+ failed|SD slot (\d+): card setup (?:failed|could not start)')
# The disk: the adapter's identity line, the slot's register request, filed's
# answer as the adapter reports it, and the stat answer the driver sent. The
# slot's line also carries the media generation and the bus the card runs on.
DISK_CREATED = re.compile(r'SD storage (\d+): disk (\S+), (\d+) sectors')
REGISTER_SENT = re.compile(
    r'SD slot (\d+): storage device (\d+), generation (\d+), register sent to filed')
REGISTER_ACCEPTED = re.compile(r'SD storage (\d+): filed accepted register')
STAT_ANSWER = re.compile(r'SDHCI: stat for storage device (\d+): "([^"]*)", (\d+) sectors of (\d+) bytes, '
                         r'flags 0x([0-9a-fA-F]+)')
# filed's lines for one disk.
FILED_REGISTER = re.compile(r'sys_storage_register_invocation\(driver=(\d+), device=(\d+)\)')
# Warnings and errors of the driver. Log tags carry the level, CPU and
# thread, not the module, so the driver's lines are found by their prefix.
DRIVER_PROBLEM = re.compile(
    r'\[(warn|error)-\d+-\d+\]\s*(SDHCI( \d+)?:|SD:|SD storage \d+:|SD slot \d+:|SD (?:setup|read|write):)')
# The engine's recovery from a late interrupt: a deadline expired, and the
# completion was found before the deadline counted. The request then
# completes normally (see __ReadStatusDirectly in
# modules/storage/sdhci/request.c). There are two wordings. "found it by
# reading the register" means the status register still held the bits. "its
# handler saved it as the deadline passed" means a fast handler had already
# acknowledged the bits and saved them while the worker waited for it. Under
# TCG the whole guest can stall for tens of milliseconds while the kernel
# prints warning lines to the serial port, and QEMU's sdhci-pci runs a whole
# ADMA2 transfer inside the command register write. Both can take longer than
# the 20 ms response deadline. These lines are listed in late_interrupts and
# do not fail a run. Every other driver warning or error does.
LATE_INTERRUPT = re.compile(r'SDHCI \d+: (?:no interrupt for status 0x[0-9a-fA-F]+ of CMD\d+, found it by reading '
                            r'the register|late interrupt for status 0x[0-9a-fA-F]+ of CMD\d+, its handler saved it '
                            r'as the deadline passed)')
# served's marker after it has created /data/served and found no bundled
# applications to install, the end of its first start.
SERVED_SETUP_DONE = 'InstallBundledApplications no bundled apps!'


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


def data_commands(trace):
    """Every command the host sent, with its argument and the blocks the card moved.

    sdhci_send_command is the host writing the command register. The card
    model logs sdcard_read_block and sdcard_write_block once per block. A
    CMD12 that the card model receives without a matching
    sdhci_send_command would be an Auto CMD12 sent by the controller itself.
    """
    commands = []
    card_stops = 0
    for line in trace.splitlines():
        match = re.search(r'sdhci_send_command CMD(\d+) ARG\[0x([0-9a-fA-F]+)\]', line)
        if match:
            commands.append({'cmd': int(match[1]), 'arg': int(match[2], 16), 'read': 0, 'written': 0})
            continue
        if 'sdcard_read_block' in line and commands:
            commands[-1]['read'] += 1
        elif 'sdcard_write_block' in line and commands:
            commands[-1]['written'] += 1
        elif re.search(r'sdcard_normal_command .*CMD12 ', line):
            card_stops += 1
    return commands, card_stops


def feature_commands(trace):
    """The commands of the four-line and high speed steps as the card model got them.

    QEMU's card model logs every application command as sdcard_app_command and
    every other command as sdcard_normal_command. ACMD6 sets the bus width,
    ACMD13 reads the SD status and CMD6 checks or switches a function.
    """
    seen = []
    for line in trace.splitlines():
        match = re.search(r'sdcard_app_command .*ACMD0?(6|13)\b.*arg 0x([0-9a-fA-F]+)', line)
        if match:
            seen.append(f'ACMD{match[1]} arg 0x{int(match[2], 16):x}')
            continue
        match = re.search(r'sdcard_normal_command .*CMD0?6\b.*arg 0x([0-9a-fA-F]+)', line)
        if match:
            seen.append(f'CMD6 arg 0x{int(match[1], 16):x}')
    return seen


def summarize_trace(trace):
    """Count what the trace shows about commands, DMA and register widths."""
    commands, card_stops = data_commands(trace)
    counts = {}
    for command in commands:
        counts[command['cmd']] = counts.get(command['cmd'], 0) + 1
    data = [command for command in commands if command['cmd'] in (17, 18, 24, 25)]
    # Each multiple-block command must be followed by the host's own CMD12,
    # with no other command in between.
    stops_after_multiblock = sum(
        1 for index, command in enumerate(commands)
        if command['cmd'] in (18, 25) and index + 1 < len(commands) and commands[index + 1]['cmd'] == 12)
    return {
        'command_counts': {f'CMD{cmd}': count for cmd, count in sorted(counts.items())},
        'data_commands': [f'CMD{c["cmd"]} arg 0x{c["arg"]:x} read {c["read"]} written {c["written"]}'
                          for c in data],
        'blocks_read': sum(c['read'] for c in data),
        'blocks_written': sum(c['written'] for c in data),
        'largest_read_blocks': max([c['read'] for c in data if c['cmd'] == 18], default=0),
        'largest_write_blocks': max([c['written'] for c in data if c['cmd'] == 25], default=0),
        'multiblock_commands': sum(1 for c in data if c['cmd'] in (18, 25)),
        'host_cmd12_after_multiblock': stops_after_multiblock,
        'card_cmd12_received': card_stops,
        'host_cmd12_sent': counts.get(12, 0),
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
        # PCI command register values written to the host, in order. Bit 1
        # enables memory decoding and bit 2 bus mastering.
        'pci_command_writes': re.findall(r'pci_cfg_write sdhci-pci \S+ @0x4 <- (0x[0-9a-fA-F]+)', trace),
        'feature_commands': feature_commands(trace),
    }


def disk_evidence(text, label):
    """Collect the driver's and filed's lines for the SD disk.

    filed's lines carry no disk name, so the ones after the driver's register
    request are taken in order: the register for the SD storage device ID,
    the mount of /storage/<Serial>, and the MFS partition name.
    """
    created = DISK_CREATED.search(text)
    sent = REGISTER_SENT.search(text)
    evidence = {
        'disk': created.group(0) if created else None,
        'register_sent': sent.group(0) if sent else None,
        'register_accepted': None, 'filed_register': None, 'stat': None,
        'storage_mount': None, 'partition_name': None, 'filed_problems': [],
    }
    if not created or not sent:
        return evidence
    storage_id = sent.group(2)
    serial = created.group(2)
    after = text[sent.start():]
    accepted = REGISTER_ACCEPTED.search(after)
    evidence['register_accepted'] = accepted.group(0) if accepted else None
    for match in FILED_REGISTER.finditer(after):
        if match.group(2) == storage_id:
            evidence['filed_register'] = match.group(0)
            evidence['driver_id'] = int(match.group(1))
            break
    for match in STAT_ANSWER.finditer(after):
        if match.group(1) == storage_id:
            evidence['stat'] = {'line': match.group(0), 'model': match.group(2), 'sectors': int(match.group(3)),
                                'sector_size': int(match.group(4)), 'flags': int(match.group(5), 16)}
            break
    mount = re.search(rf'__StorageMount mounting at /storage/{re.escape(serial)}\b', after)
    evidence['storage_mount'] = mount.group(0) if mount else None
    evidence['serial'] = serial
    if label:
        name = re.search(rf'Partition-name: {re.escape(label)}\b', after)
        evidence['partition_name'] = name.group(0) if name else None
    # filed's own failure lines for this disk name the driver and device.
    evidence['filed_problems'] = [line for line in after.splitlines()
                                  if re.search(r'\[(warn|error)-', line)
                                  and (f'device {storage_id}' in line or f'/storage/{serial}' in line)]
    return evidence


def check_data_card_disk(disk):
    """The system disk of a --data-card run must not have a vali-data partition."""
    with disk.open('rb') as image:
        head = image.read(64 * 1024)
    names = [partition['name'] for partition in sdhci_card.read_gpt(head)['partitions']]
    if 'vali-data' in names:
        raise SystemExit(f'{disk} has a vali-data partition ({names}). Build one without it with '
                         'testing/sdhci_card.py system-image')
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--disk', type=Path, required=True, help='Packaged system disk (GPT image)')
    parser.add_argument('--output', type=Path, required=True, help='New directory for the run artifacts')
    parser.add_argument('--qemu', type=Path, help='QEMU executable. Defaults to qemu-system-x86_64 or '
                        'qemu-system-aarch64 from PATH')
    parser.add_argument('--firmware', type=Path, required=True, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, required=True, help='UEFI variables template, copied')
    cards = parser.add_mutually_exclusive_group()
    cards.add_argument('--empty-slot', action='store_true', help='Add the host without an SD card')
    cards.add_argument('--blank-card', action='store_true', help='Use a zero filled card without partitions')
    cards.add_argument('--data-card', action='store_true',
                       help='Label the card partition vali-data, so /data and served\'s writes go to the card')
    parser.add_argument('--reuse-card', type=Path,
                        help='Output directory of an earlier --data-card run. Boot with its card as that run left '
                        'it, so the OS reads back what it wrote. Nothing may change on the card this time')
    parser.add_argument('--mkcdk', type=Path,
                        help='mkcdk executable that builds the card. Defaults to mkcdk from PATH')
    parser.add_argument('--card-size-mib', type=int, default=64,
                        help='Size of the SD card image, a power of two. Above 2048 the card is SDHC')
    parser.add_argument('--timeout', type=int, default=300)
    parser.add_argument('--observe-seconds', type=int, default=20,
                        help='Seconds to keep running after the outcome, to catch late errors and writes')
    # QEMU's sdhci-pci has a 52 MHz base clock and a version 2 divider (powers
    # of two), so high speed with its 50 MHz limit runs at 26 MHz.
    parser.add_argument('--expect-width', type=int, choices=[1, 4], default=4,
                        help='Data lines the card must use after setup')
    parser.add_argument('--expect-timing', choices=['default speed', 'high speed'], default='high speed',
                        help='Bus timing the card must use after setup')
    parser.add_argument('--expect-clock-hz', type=int, default=26000000,
                        help='Card clock the host must have selected after setup')
    args = parser.parse_args()
    size = args.card_size_mib
    if size <= 0 or size & (size - 1):
        parser.error('--card-size-mib must be a power of two')
    if args.timeout <= args.observe_seconds:
        parser.error('--timeout must be longer than --observe-seconds')
    if args.reuse_card and not args.data_card:
        parser.error('--reuse-card needs --data-card')
    formatted = not args.empty_slot and not args.blank_card
    label = 'vali-data' if args.data_card else ('sd-test' if formatted else None)

    out = args.output.resolve()
    disk = args.disk.resolve()
    system_partitions = check_data_card_disk(disk) if args.data_card else None
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
    # pci_cfg_write shows the command register writes of DeviceSetStage.
    (out / 'events.txt').write_text('sdhci_*\nsdcard_*\npci_cfg_write\n')

    manifest = None
    card = out / 'sdcard.raw'
    if args.reuse_card:
        # The card of an earlier run, as that run left it.
        previous = args.reuse_card.resolve()
        manifest = json.loads((previous / 'card-manifest.json').read_text())
        shutil.copyfile(previous / 'sdcard.raw', card)
        shutil.copyfile(card, out / 'sdcard.orig')
        size = card.stat().st_size // (1024 * 1024)
        (out / 'card-manifest.json').write_text(json.dumps({**manifest, 'reused_from': str(previous)}, indent=2))
    elif formatted:
        # The data card is written to. With a defective mkcdk it then carries
        # no files (see build_card).
        manifest = sdhci_card.build_card(
            out, size, label, sdhci_card.MFS_DATA_TYPE if args.data_card else sdhci_card.MFS_USER_TYPE,
            args.data_card, sdhci_card.find_mkcdk(args.mkcdk))
        (out / 'card-manifest.json').write_text(json.dumps(manifest, indent=2))
    elif args.blank_card:
        # A sparse file, so a large SDHC card costs no disk space.
        with card.open('wb') as image:
            image.truncate(size * 1024 * 1024)

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

        def outcome_reached(text):
            # Identification ends the empty and blank runs. A formatted card
            # waits for the MFS mount, and the data card also for served.
            if NO_CARD.search(text) or SETUP_FAILED.search(text):
                return True
            if not CARD_READY.search(text):
                return False
            if not formatted:
                return True
            if f'Partition-name: {label}' not in text:
                return False
            return not args.data_card or SERVED_SETUP_DONE in text

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
                if outcome_at is None and outcome_reached(text):
                    outcome_at = time.monotonic()
                    print(f'SDHCI: {args.arch} outcome reached; observing {args.observe_seconds}s', flush=True)
                if outcome_at is not None and time.monotonic() - outcome_at >= args.observe_seconds:
                    break
                time.sleep(1)

            observed = time.monotonic() - outcome_at if outcome_at else 0
            qmp('stop')
            (out / 'pci.txt').write_text(qmp('human-monitor-command', {'command-line': 'info pci'}))
            (out / 'block-stats.json').write_text(json.dumps(qmp('query-blockstats'), indent=2))
            # Quit so QEMU has written everything to the card image before
            # it is compared.
            process.stdin.write(json.dumps({'execute': 'quit'}) + '\n')
            process.stdin.flush()
            process.wait(timeout=30)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            reader.join(timeout=5)

    text = (out / 'serial.log').read_text(errors='replace')
    trace = (out / 'trace.log').read_text(errors='replace')
    registers = HOST_REGISTERS.search(text)
    host = HOST_READY.search(text)
    card_line = CARD_READY.search(text)
    slot_bus = SLOT_BUS.search(text)
    result = {
        'arch': args.arch,
        'slot': 'empty' if args.empty_slot else
                f'{size} MiB {"blank" if args.blank_card else label} card',
        'system_partitions': system_partitions,
        'observed_seconds': observed,
        'failures': failures,
        'driver_problems': [line for line in text.splitlines()
                            if DRIVER_PROBLEM.search(line) and not LATE_INTERRUPT.search(line)],
        'late_interrupts': [line for line in text.splitlines() if LATE_INTERRUPT.search(line)],
        'services': [name for name in ('process', 'file', 'device', 'net', 'usb', 'serve')
                     if f'path=/service/{name})' in text],
        'data_mounted': 'storage_ready_invocation(path=/data)' in text,
        'served_setup_done': SERVED_SETUP_DONE in text,
        'host_registers': registers.group(0) if registers else None,
        'host_ready': host.group(0) if host else None,
        'card_ready': card_line.group(0) if card_line else None,
        'slot_bus': slot_bus.group(0) if slot_bus else None,
        'no_card': NO_CARD.search(text) is not None,
        'setup_failed': [line for line in text.splitlines() if SETUP_FAILED.search(line)],
        'disk': disk_evidence(text, label),
        'trace': summarize_trace(trace),
    }
    if registers and host:
        result['host'] = {
            'version_register': f'0x{registers.group(2)}',
            # Host version bits 7:0 count from 0 for version 1.00.
            'spec_version': f'{(int(registers.group(2), 16) & 0xff) + 1}.00',
            'base_clock_hz': int(registers.group(5)),
            'capabilities': decode_capabilities(int(registers.group(3), 16), int(registers.group(4), 16)),
            'slot': host.group(2),
            'card_clock_hz': [int(host.group(3)), int(host.group(4))],
            'adma2_format': f'{host.group(5)}-bit',
            'adma2_entries': int(host.group(6)),
            'interrupt': host.group(7),
        }
    if manifest is not None:
        result['card_image'] = sdhci_card.compare_images(out / 'sdcard.orig', card, manifest)

    # Any driver warning or error fails the run, so a problem the driver only
    # reports cannot hide behind a good outcome line.
    checks = {
        'host_found': registers is not None and host is not None,
        'observed_long_enough': observed >= args.observe_seconds,
        'no_failures': not failures,
        'no_driver_problems': not result['driver_problems'],
        'no_dataport_access': result['trace']['dataport_accesses'] == 0,
    }
    if args.empty_slot:
        checks['no_card'] = result['no_card'] and card_line is None
        checks['nothing_registered'] = result['disk']['register_sent'] is None
    else:
        expected_sectors = size * 2048
        expected_addressing = 'sector' if size > SDSC_LIMIT_MIB else 'byte'
        result['expected'] = {'sectors': expected_sectors, 'addressing': expected_addressing}
        # The card line gives the size in MiB, the adapter's disk line the
        # exact sector count.
        created = DISK_CREATED.search(text)
        checks['card_identified'] = (card_line is not None and int(card_line.group(3)) == size
                                     and card_line.group(4) == expected_addressing
                                     and created is not None and int(created.group(3)) == expected_sectors)
        # The SCR read during card setup is an ADMA2 data transfer.
        checks['adma2_used'] = result['trace']['adma_transfers_completed'] > 0
        # The bus the card ended up on, from the card layer's and the slot's
        # lines. Both must name the expected lines, timing and clock.
        expected_bus = (args.expect_width, args.expect_timing, args.expect_clock_hz)
        result['expected']['bus'] = {'data_lines': expected_bus[0], 'timing': expected_bus[1],
                                     'clock_hz': expected_bus[2]}
        if card_line is not None:
            result['negotiated'] = {'data_lines': int(card_line.group(5)), 'timing': card_line.group(6),
                                    'clock_hz': int(card_line.group(7))}
        checks['negotiated_bus'] = (
            card_line is not None and slot_bus is not None
            and (int(card_line.group(5)), card_line.group(6), int(card_line.group(7))) == expected_bus
            and (int(slot_bus.group(2)), slot_bus.group(3), int(slot_bus.group(4))) == expected_bus)
        # The card model must have seen the commands of each expected switch:
        # ACMD6 with four lines and the SD status read, CMD6 in check mode and
        # in switch mode for high speed.
        wanted = []
        if args.expect_width == 4:
            wanted += ['ACMD6 arg 0x2', 'ACMD13 arg 0x0']
        if args.expect_timing == 'high speed':
            wanted += ['CMD6 arg 0xfffff1', 'CMD6 arg 0x80fffff1']
        checks['switch_commands_seen'] = all(command in result['trace']['feature_commands'] for command in wanted)
        disk_lines = result['disk']
        stat = disk_lines['stat']
        checks['registered'] = all(disk_lines[key] for key in
                                   ('disk', 'register_sent', 'register_accepted', 'filed_register', 'storage_mount'))
        checks['stat_answer'] = (stat is not None and stat['sectors'] == expected_sectors
                                 and stat['sector_size'] == 512 and stat['flags'] == 0
                                 and stat['model'].startswith('SD card '))
        checks['no_filed_problems'] = not disk_lines['filed_problems']
        checks['single_block_reads'] = result['trace']['command_counts'].get('CMD17', 0) > 0
        if formatted:
            image = result['card_image']
            checks['mfs_mounted'] = disk_lines['partition_name'] is not None
            checks['multi_block_reads'] = result['trace']['command_counts'].get('CMD18', 0) > 0
            checks['explicit_stop_after_each_multiblock'] = (
                result['trace']['host_cmd12_after_multiblock'] == result['trace']['multiblock_commands'])
            checks['files_intact'] = all(entry['present'] for entry in image['files'].values())
            # The master record and its mirror must agree and point at free
            # space after the run. A card from a defective mkcdk that the OS
            # never writes to keeps the broken record it was built with.
            if args.data_card or manifest.get('writer_correct'):
                checks['master_record_valid'] = not image['master_record_problems_after']
            if args.data_card:
                # Every changed sector must belong to MFS metadata or to a
                # directory that exists after the run. A change in the GPT,
                # the boot record, a file or free space has no explanation.
                bad = [run for run in image['changed_runs']
                       if any(owner in ('GPT', 'outside the partition', 'MFS boot record', 'free space')
                              or owner.startswith('file /') for owner in run['owners'])]
                checks['data_mounted'] = result['data_mounted'] and result['served_setup_done']
                checks['changes_explained'] = not bad
                checks['served_directories'] = all(
                    name in image['entries_after'] for name in ('served/', 'served/apps/', 'served/mount/',
                                                                'served/cache/'))
                if args.reuse_card:
                    # served finds its directories, so it creates nothing.
                    checks['card_unchanged'] = (image['changed_sectors'] == 0
                                                and result['trace']['blocks_written'] == 0)
                else:
                    checks['writes_reached_card'] = (result['trace']['blocks_written'] > 0
                                                     and image['changed_sectors'] > 0)
            else:
                checks['card_unchanged'] = (image['changed_sectors'] == 0
                                            and result['trace']['blocks_written'] == 0)
    result['checks'] = checks
    result['pass'] = all(checks.values())
    (out / 'results.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2), flush=True)
    if not result['pass']:
        raise SystemExit('SDHCI boot acceptance failed: ' +
                         ', '.join(name for name, passed in checks.items() if not passed))


if __name__ == '__main__':
    main()
