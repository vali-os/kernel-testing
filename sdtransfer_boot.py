"""Boot a test image with the SD transfer test on QEMU's sdhci-pci and check the result.

The SD transfer test (testing/target/sdtransfer) writes a file on the SD card
in requests of several sizes, reads it back, compares every sector and logs
its throughput. This runner:

1. Builds a system disk without vali-data that starts the test program
   (testing/sdtransfer_image.py), unless --disk names one built earlier.
2. Builds an SD card with one MFS partition labelled vali-data and the
   generated files of testing/sdhci_card.py, so filed binds it at /data and
   the program writes its file there.
3. Boots AMD64 or generic AArch64 with the card on an sdhci-pci host, and
   waits for the program's result line.
4. Checks the bus the card negotiated, parses the program's result lines,
   and lists the size of every data command the card got from QEMU's trace.
5. Quits QEMU and checks the card image: the host checker
   (testing/sdtransfer_check.py) must find every sector of the file exactly
   once with a valid CRC-32, the file read through MFS must hold the pattern
   in order, and the generated files must be intact.

The run fails on a mismatch, a missing or duplicated sector, a FAIL line of
the program, an error line from the SD or SDHCI driver, a DMA or SHM error
line, an sdhci_error event in QEMU's trace, a bus other than the expected one,
or any failure marker of testing/sdhci_boot.py. Driver warnings fail too,
except a late interrupt (see sdhci_boot.py) and a device pool shortage that
the adapter waited out. Both are listed in results.json.

QEMU's interrupt counters ("info irq") are read when the program starts and
when it ends, where the machine provides them, and the difference is
recorded with the number of interrupts per MiB moved.

Throughput under QEMU's TCG says nothing about the hardware. QEMU emulates
the CPU and the card in software and models no bus timing. The numbers are
recorded only to show that the measurement works.

--memory-mib above 4096 on AMD64 puts guest memory above 4 GiB. QEMU's
sdhci-pci only does 32-bit ADMA2, so buffers above 4 GiB go through the
driver's bounce buffers, 128 KiB per chunk. The generic AArch64 machine has
its RAM at 1 GiB, so with 4096 MiB the top GiB already lies above 4 GiB.

Example:

    python3 testing/sdtransfer_boot.py --arch amd64 --build build/verify-amd64 \\
        --firmware /usr/share/edk2/x64/OVMF_CODE.4m.fd \\
        --firmware-vars /usr/share/edk2/x64/OVMF_VARS.4m.fd \\
        --output build/boot-tests/s7c/transfer-amd64
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import shutil
import subprocess
import threading
import time

import sdhci_boot
import sdhci_card
import sdtransfer_check
import sdtransfer_image

# The program's lines (see the top of testing/target/sdtransfer/sdtransfer.c).
START = re.compile(r'sdtransfer: start run 0x([0-9a-f]+), file (\S+), (\d+) bytes, (\d+) passes')
WRITE = re.compile(r'sdtransfer: pass (\d+) write (\d+) bytes in (\d+) B requests, (\d+)\.(\d+) s, '
                   r'(\d+)\.(\d+) MiB/s')
READ = re.compile(r'sdtransfer: pass (\d+) read (\d+) bytes in (\d+) B requests, (\d+)\.(\d+) s, '
                  r'(\d+)\.(\d+) MiB/s, (\d+) mismatches')
LEFT = re.compile(r'sdtransfer: file (\S+) left with run 0x([0-9a-f]+) pass (\d+), (\d+) sectors')
RESULT = re.compile(r'sdtransfer: result (PASS|FAIL), run 0x([0-9a-f]+), (\d+) passes, (\d+) mismatches, '
                    r'(\d+) errors')
PROGRAM_PROBLEM = re.compile(r'sdtransfer: (FAIL|mismatch) ')
# An open that filed refused because it had not yet closed the previous
# handle of the file (see __OpenFile in sdtransfer.c). The program tries
# again, so it does not fail the run, but the count is recorded.
OPEN_REFUSED = re.compile(r'sdtransfer: pass (\d+) open refused with (\d+) for the (write|read), retry (\d+)')

# The adapter waited for device pool memory and went on. Expected under
# bounce load, so it is listed but does not fail the run. The variant that
# gives up is an error line and fails it.
POOL_SHORT = re.compile(r'SD storage \d+: device pool short at sector')
# Error lines of the DMA and SHM layers, whoever logs them.
DMA_PROBLEM = re.compile(r'\[error-\d+-\d+\].*(DMA|SHM|Dma|Shm)')
# The first bounce of a process, logged at NOTICE level by DMAMapSHMChunk.
BOUNCE_NOTICE = re.compile(r'DMAMapSHMChunk: buffer \d+, offset \d+: direct map failed .*bouncing')

# QEMU trace events: commands, ADMA2 descriptors and errors, and the card
# model's commands for the bus switch checks. Per-block events are left out,
# they would make the trace of a large run grow to gigabytes.
TRACE_EVENTS = ('sdhci_send_command', 'sdhci_adma_loop', 'sdhci_adma_transfer_completed', 'sdhci_error*',
                'sdcard_app_command', 'sdcard_normal_command')

NOTE = ('Throughput under QEMU TCG says nothing about the hardware. QEMU emulates the CPU and the card in '
        'software and models no bus timing. It is recorded only to show that the measurement works.')


def data_command_sizes(trace):
    """Bytes and ADMA2 descriptors of every data command, from the trace.

    Each sdhci_adma_loop event is one descriptor with its length. They follow
    the sdhci_send_command of their command."""
    commands = []
    for line in trace.splitlines():
        match = re.search(r'sdhci_send_command CMD(\d+) ARG\[0x([0-9a-fA-F]+)\]', line)
        if match:
            commands.append({'cmd': int(match[1]), 'bytes': 0, 'descriptors': 0, 'highest_address': 0})
            continue
        match = re.search(r'sdhci_adma_loop addr=0x([0-9a-fA-F]+), len=(\d+)', line)
        if match and commands:
            commands[-1]['bytes'] += int(match[2])
            commands[-1]['descriptors'] += 1
            commands[-1]['highest_address'] = max(commands[-1]['highest_address'], int(match[1], 16))
    return [command for command in commands if command['cmd'] in (17, 18, 24, 25)]


def summarize_commands(commands):
    """Size histogram and largest data commands, by direction."""
    summary = {}
    for name, numbers in (('read', (17, 18)), ('write', (24, 25))):
        chosen = [command for command in commands if command['cmd'] in numbers]
        sizes = {}
        for command in chosen:
            sizes[command['bytes']] = sizes.get(command['bytes'], 0) + 1
        summary[name] = {
            'commands': len(chosen),
            'bytes': sum(command['bytes'] for command in chosen),
            'largest_bytes': max((command['bytes'] for command in chosen), default=0),
            'most_descriptors': max((command['descriptors'] for command in chosen), default=0),
            'above_64_sectors': sum(1 for command in chosen if command['bytes'] > 64 * 512),
            'sizes': {str(size): count for size, count in sorted(sizes.items())},
        }
    summary['highest_descriptor_address'] = f'0x{max((c["highest_address"] for c in commands), default=0):x}'
    return summary


def parse_irq(text):
    """QEMU's "info irq" output as {controller: {line: count}}."""
    counts = {}
    controller = 'irq'
    for line in text.splitlines():
        header = re.match(r'IRQ statistics for (\S+):', line)
        if header:
            controller = header[1]
            continue
        match = re.match(r'\s*(\d+):\s*(\d+)\s*$', line)
        if match:
            counts.setdefault(controller, {})[int(match[1])] = int(match[2])
    return counts


def irq_difference(before, after):
    """Interrupts per controller line between two "info irq" readings."""
    difference = {}
    for controller, lines in after.items():
        for line, count in lines.items():
            delta = count - before.get(controller, {}).get(line, 0)
            if delta:
                difference.setdefault(controller, {})[str(line)] = delta
    return difference


def sdhci_irq_line(pci_text):
    """The legacy interrupt line of the sdhci-pci function from "info pci"."""
    blocks = pci_text.split('Bus ')
    for block in blocks:
        if 'SD Host controller' in block or '1b36:0007' in block:
            match = re.search(r'IRQ (\d+)', block)
            if match:
                return int(match[1])
    return None


def parse_program(text):
    """The program's result lines, by pass."""
    passes = {}
    for match in WRITE.finditer(text):
        passes.setdefault(int(match[1]), {})['write'] = {
            'bytes': int(match[2]), 'request_bytes': int(match[3]),
            'seconds': float(f'{match[4]}.{match[5]}'), 'mib_per_second': float(f'{match[6]}.{match[7]}')}
    for match in READ.finditer(text):
        passes.setdefault(int(match[1]), {})['read'] = {
            'bytes': int(match[2]), 'request_bytes': int(match[3]),
            'seconds': float(f'{match[4]}.{match[5]}'), 'mib_per_second': float(f'{match[6]}.{match[7]}'),
            'mismatches': int(match[8])}
    start = START.search(text)
    left = LEFT.search(text)
    result = RESULT.search(text)
    return {
        'start': start.group(0) if start else None,
        'run': start[1] if start else None,
        'file_bytes': int(start[3]) if start else None,
        'passes_planned': int(start[4]) if start else None,
        'passes': [{'pass': number, **passes[number]} for number in sorted(passes)],
        'left': {'path': left[1], 'run': left[2], 'pass': int(left[3]), 'sectors': int(left[4])} if left else None,
        'result': {'line': result.group(0), 'verdict': result[1], 'run': result[2], 'passes': int(result[3]),
                   'mismatches': int(result[4]), 'errors': int(result[5])} if result else None,
        'problems': [line for line in text.splitlines() if PROGRAM_PROBLEM.search(line)],
        'open_refused': [match.group(0) for match in OPEN_REFUSED.finditer(text)],
    }


def check_file_through_mfs(card, left):
    """Read the test file through the MFS structures of the card and compare
    it with the pattern in order. This is a second view next to the raw
    scan: the raw scan finds the sectors, this one checks that MFS's record
    points at them in the right order and has the right size."""
    data = card.read_bytes()
    partition = sdhci_card.read_gpt(data)['partitions'][0]
    volume = sdhci_card.MfsVolume(data, partition['first'])
    name = left['path'].rsplit('/', 1)[-1]
    record = None
    for path, entry in volume.walk():
        if path == name and not entry['directory']:
            record = entry
    if record is None:
        return {'found': False}
    contents = volume.read_file(record)
    pattern = sdtransfer_check.Pattern(sdtransfer_check.read_layout())
    run = int(left['run'], 16)
    wrong = []
    for index in range(len(contents) // pattern.size):
        sector = contents[index * pattern.size:(index + 1) * pattern.size]
        fields = pattern.decode(sector)
        if not (sector[:8] == pattern.marker and fields['checksum_valid'] and fields['fill_valid']
                and fields['run'] == run and fields['pass'] == left['pass'] and fields['index'] == index):
            wrong.append(index)
    return {'found': True, 'bytes': len(contents), 'sectors_wrong': len(wrong),
            'wrong_ranges': sdtransfer_check.ranges(wrong)[:20]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--build', type=Path, help='Built OS build directory to make the test image from')
    parser.add_argument('--disk', type=Path, help='A test image from sdtransfer_image.py, instead of --build. '
                        'Its built-in arguments must match --file-mib, --request-kib and --passes')
    parser.add_argument('--output', type=Path, required=True, help='New directory for the run artifacts')
    parser.add_argument('--qemu', type=Path, help='QEMU executable. Defaults to qemu-system-x86_64 or '
                        'qemu-system-aarch64 from PATH')
    parser.add_argument('--firmware', type=Path, required=True, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, required=True, help='UEFI variables template, copied')
    parser.add_argument('--mkcdk', type=Path, help='mkcdk executable. Defaults to mkcdk from PATH')
    parser.add_argument('--card-size-mib', type=int, default=512,
                        help='SD card size, a power of two. Above 2048 the card is SDHC')
    parser.add_argument('--memory-mib', type=int, default=4096, help='Guest memory')
    parser.add_argument('--file-mib', type=int, default=64)
    parser.add_argument('--request-kib', default='4,64,1024', help='Request sizes, one per pass in turn')
    parser.add_argument('--passes', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=7200, help='Seconds to wait for the result line')
    parser.add_argument('--observe-seconds', type=int, default=10,
                        help='Seconds to keep running after the result line, to catch late errors')
    parser.add_argument('--expect-width', type=int, choices=[1, 4], default=4)
    parser.add_argument('--expect-timing', choices=['default speed', 'high speed'], default='high speed')
    parser.add_argument('--expect-clock-hz', type=int, default=26000000)
    args = parser.parse_args()
    size = args.card_size_mib
    if size <= 0 or size & (size - 1):
        parser.error('--card-size-mib must be a power of two')
    if args.file_mib * 2 > size:
        parser.error('the card must be at least twice the file size')
    if (args.build is None) == (args.disk is None):
        parser.error('give either --build or --disk')

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    mkcdk = sdhci_card.find_mkcdk(args.mkcdk)
    arguments = f'--file-mib {args.file_mib} --request-kib {args.request_kib} --passes {args.passes}'
    if args.disk:
        disk = args.disk.resolve()
        image_manifest = json.loads(disk.with_suffix('.json').read_text())
        if image_manifest['arguments'] != arguments:
            parser.error(f'{disk} was built with "{image_manifest["arguments"]}", not "{arguments}"')
    else:
        print(f'sdtransfer: building the test image from {args.build}', flush=True)
        disk = out / 'system.img'
        image_manifest = sdtransfer_image.build_image(args.build, disk, arguments, mkcdk)
    (out / 'image-manifest.json').write_text(json.dumps(image_manifest, indent=2))

    executable = str(args.qemu or ('qemu-system-aarch64' if args.arch == 'arm64' else 'qemu-system-x86_64'))
    hashes = {}
    for path in (disk, args.firmware.resolve(), args.firmware_vars.resolve()):
        hashes[str(path)] = sdtransfer_image.sha256(path)
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    version = subprocess.run([executable, '--version'], check=True, capture_output=True, text=True).stdout
    (out / 'qemu-version.txt').write_text(version)
    shutil.copyfile(args.firmware_vars, out / 'vars.fd')
    (out / 'events.txt').write_text('\n'.join(TRACE_EVENTS) + '\n')

    print(f'sdtransfer: building a {size} MiB vali-data card', flush=True)
    manifest = sdhci_card.build_card(out, size, 'vali-data', sdhci_card.MFS_DATA_TYPE, True, mkcdk)
    (out / 'card-manifest.json').write_text(json.dumps(manifest, indent=2))
    card = out / 'sdcard.raw'

    machine = 'virt-11.1,gic-version=3' if args.arch == 'arm64' else 'pc'
    command = [executable, '-machine', machine, '-accel', 'tcg',
               '-smp', '4', '-m', str(args.memory_mib), '-display', 'none', '-monitor', 'none',
               '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
               '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={args.firmware.resolve()}',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd']
    if args.arch == 'arm64':
        command += ['-cpu', 'cortex-a57']
    command += ['-drive', f'if=none,id=storage,format=raw,snapshot=on,file={disk.as_posix()}',
                '-device', 'virtio-blk-pci,drive=storage,bootindex=1',
                '-device', 'sdhci-pci,id=sdhci',
                '-drive', f'if=none,id=sdcard,format=raw,file={card.as_posix()}',
                '-device', 'sd-card,drive=sdcard']
    (out / 'command.json').write_text(json.dumps(command, indent=2))

    irq = {}
    failures = []
    timeline = {}
    with (out / 'qemu.log').open('w') as log:
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
            line = replies.get(timeout=30)
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

        def monitor(command_line):
            try:
                return qmp('human-monitor-command', {'command-line': command_line})
            except RuntimeError as error:
                return f'error: {error}'

        try:
            receive()
            qmp('qmp_capabilities')
            started = time.monotonic()
            result_at = None
            offset = 0
            watched = []
            while time.monotonic() - started < args.timeout:
                if process.poll() is not None:
                    raise RuntimeError('QEMU exited early')
                # The serial log of a large run grows to hundreds of MiB, so
                # only the new part is read, and only the lines the loop looks
                # at are kept.
                with (out / 'serial.log').open('rb') as serial:
                    serial.seek(offset)
                    fresh = serial.read()
                complete = fresh.rfind(b'\n') + 1
                offset += complete
                for line in fresh[:complete].decode(errors='replace').splitlines():
                    if any(marker in line for marker in sdhci_boot.FAILURE_MARKERS):
                        failures.append(line)
                    elif 'sdtransfer: ' in line:
                        watched.append(line)
                text = '\n'.join(watched)
                if failures:
                    break
                if 'start' not in timeline and START.search(text):
                    timeline['start'] = time.monotonic() - started
                    irq['start'] = monitor('info irq')
                    print(f'sdtransfer: {args.arch} program started after {timeline["start"]:.0f}s', flush=True)
                # The host time each pass line first shows up, at the 2 second
                # polling resolution. Under TCG the guest clock need not run
                # at the host's pace, so this is a cross-check of the guest's
                # own seconds.
                for match in list(WRITE.finditer(text)) + list(READ.finditer(text)):
                    key = f'pass {match[1]} {"write" if " write " in match.group(0) else "read"}'
                    timeline.setdefault(key, time.monotonic() - started)
                if result_at is None and RESULT.search(text):
                    result_at = time.monotonic()
                    timeline['result'] = result_at - started
                    irq['end'] = monitor('info irq')
                    print(f'sdtransfer: {args.arch} result after {timeline["result"]:.0f}s; '
                          f'observing {args.observe_seconds}s', flush=True)
                if result_at is not None and time.monotonic() - result_at >= args.observe_seconds:
                    break
                time.sleep(2)

            qmp('stop')
            pci = monitor('info pci')
            (out / 'pci.txt').write_text(pci)
            (out / 'block-stats.json').write_text(json.dumps(qmp('query-blockstats'), indent=2))
            process.stdin.write(json.dumps({'execute': 'quit'}) + '\n')
            process.stdin.flush()
            process.wait(timeout=60)
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
    (out / 'irq.txt').write_text(f'--- at program start\n{irq.get("start")}\n--- at result\n{irq.get("end")}\n')
    program = parse_program(text)
    card_line = sdhci_boot.CARD_READY.search(text)
    slot_bus = sdhci_boot.SLOT_BUS.search(text)
    driver_lines = [line for line in text.splitlines() if sdhci_boot.DRIVER_PROBLEM.search(line)]
    commands = data_command_sizes(trace)
    result = {
        'arch': args.arch,
        'memory_mib': args.memory_mib,
        'card': f'{size} MiB vali-data card',
        'arguments': arguments,
        'note': NOTE,
        'timeline_seconds': timeline,
        'failures': failures,
        'program': program,
        'card_ready': card_line.group(0) if card_line else None,
        'slot_bus': slot_bus.group(0) if slot_bus else None,
        'host_ready': (lambda match: match.group(0) if match else None)(sdhci_boot.HOST_READY.search(text)),
        'driver_problems': [line for line in driver_lines
                            if not sdhci_boot.LATE_INTERRUPT.search(line) and not POOL_SHORT.search(line)],
        'late_interrupts': [line for line in driver_lines if sdhci_boot.LATE_INTERRUPT.search(line)],
        'pool_short_waits': [line for line in driver_lines if POOL_SHORT.search(line)],
        'dma_problems': [line for line in text.splitlines() if DMA_PROBLEM.search(line)],
        'bounce_notices': [line for line in text.splitlines() if BOUNCE_NOTICE.search(line)],
        'data_mounted': 'storage_ready_invocation(path=/data)' in text,
        'trace_errors': [line for line in trace.splitlines() if line.startswith('sdhci_error')][:50],
        'feature_commands': sdhci_boot.feature_commands(trace),
        'data_commands': summarize_commands(commands),
    }
    if card_line is not None:
        result['negotiated'] = {'data_lines': int(card_line.group(5)), 'timing': card_line.group(6),
                                'clock_hz': int(card_line.group(7))}

    # Interrupts while the program ran, per MiB moved by the program.
    moved = sum(step['bytes'] for entry in program['passes'] for step in (entry.get('write'), entry.get('read'))
                if step) / (1024 * 1024)
    line = sdhci_irq_line(pci)
    if irq.get('start') and irq.get('end'):
        difference = irq_difference(parse_irq(irq['start']), parse_irq(irq['end']))
        # QEMU's pc machine lists the same line under the IOAPIC and the
        # legacy i8259, and both count every interrupt. The largest count of
        # the line is taken, so an interrupt is not counted twice.
        sdhci_count = None
        if line is not None:
            sdhci_count = max((lines.get(str(line), 0) for lines in difference.values()), default=0)
        result['interrupts'] = {
            'source': 'QEMU info irq at program start and at the result line',
            'sdhci_irq_line': line,
            'difference': difference,
            'sdhci_interrupts': sdhci_count,
            'mib_moved': moved,
            'sdhci_interrupts_per_mib': (sdhci_count / moved) if sdhci_count and moved else None,
        }
    else:
        # The generic AArch64 machine answers "info irq" with nothing.
        result['interrupts'] = {'source': 'QEMU info irq gave no counters on this machine'}

    left = program['left']
    if left is not None:
        checked = sdtransfer_check.run_check(card, int(left['run'], 16), left['pass'], 'vali-data')
        (out / 'card-check.json').write_text(json.dumps(checked, indent=2) + '\n')
        (out / 'card-check.txt').write_text(sdtransfer_check.summary(checked) + '\n')
        result['card_check'] = {key: checked.get(key) for key in (
            'run', 'pass', 'sector_count', 'present', 'missing', 'duplicates', 'corrupt', 'extents',
            'other_pass_sectors', 'other_run_sectors', 'pass_check', 'error')}
        result['file_through_mfs'] = check_file_through_mfs(card, left)
    image = sdhci_card.compare_images(out / 'sdcard.orig', card, manifest)
    result['card_files'] = {path: entry['present'] for path, entry in image['files'].items()}
    result['master_record_problems_after'] = image['master_record_problems_after']

    expected_bus = (args.expect_width, args.expect_timing, args.expect_clock_hz)
    request_sizes = [int(item) * 1024 for item in args.request_kib.split(',')]
    planned = [request_sizes[index % len(request_sizes)] for index in range(args.passes)]
    verdict = program['result']
    checks = {
        'no_failures': not failures,
        'data_mounted': result['data_mounted'],
        'negotiated_bus': (card_line is not None and slot_bus is not None
                           and (int(card_line.group(5)), card_line.group(6), int(card_line.group(7))) == expected_bus
                           and (int(slot_bus.group(2)), slot_bus.group(3), int(slot_bus.group(4))) == expected_bus),
        'program_pass': (verdict is not None and verdict['verdict'] == 'PASS' and verdict['mismatches'] == 0
                         and verdict['errors'] == 0 and not program['problems']),
        'every_pass_reported': ([entry.get('write', {}).get('request_bytes') for entry in program['passes']] == planned
                                and [entry.get('read', {}).get('request_bytes') for entry in program['passes']]
                                == planned),
        'no_driver_problems': not result['driver_problems'],
        'no_dma_problems': not result['dma_problems'],
        'no_trace_errors': not result['trace_errors'],
        'card_check': result.get('card_check', {}).get('pass_check') is True,
        'file_through_mfs': (result.get('file_through_mfs', {}).get('found') is True
                             and result['file_through_mfs']['sectors_wrong'] == 0
                             and result['file_through_mfs']['bytes'] == args.file_mib * 1024 * 1024),
        'card_files_intact': all(result['card_files'].values()),
        'master_record_valid': not result['master_record_problems_after'],
    }
    # A request above 64 sectors must reach the card as a command above 64
    # sectors, in both directions. That is what this test adds over the
    # earlier runs.
    if max(request_sizes) > 64 * 512:
        checks['chunks_above_64_sectors'] = (result['data_commands']['read']['above_64_sectors'] > 0
                                             and result['data_commands']['write']['above_64_sectors'] > 0)
    result['checks'] = checks
    result['pass'] = all(checks.values())
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('checks', 'pass', 'card_ready', 'data_commands')}, indent=2))
    for entry in program['passes']:
        print(json.dumps(entry))
    if not result['pass']:
        raise SystemExit('SD transfer test failed: ' + ', '.join(name for name, passed in checks.items() if not passed))


if __name__ == '__main__':
    main()
