"""Boot a test image with the SD timing test on QEMU's sdhci-pci and collect its table.

The SD timing test (testing/target/sdtiming) times the parts of a file read
one by one: a call to filed, a grant token, reads straight from the SD
driver, file reads and writes through filed and MFS. This runner:

1. Builds a system disk without vali-data that starts the timing program
   (testing/sdtransfer_image.py --program sdtiming), unless --disk names one
   built earlier.
2. Builds an SD card with one MFS partition labelled vali-data and the
   generated files of testing/sdhci_card.py, so filed binds it at /data and
   the program creates its files there.
3. Boots AMD64 or generic AArch64 with the card on an sdhci-pci host, and
   waits for the program's result line.
4. Parses every probe line into results.json (testing/sdtiming_report.py),
   and writes the table of the probes and the derived split to stdout and to
   table.txt.
5. Quits QEMU and checks the card: the test file is there with its size, the
   file of the append probe was deleted, and the generated files and MFS's
   master record are intact.

The run fails on a FAIL line of the program, a probe that was asked for and
has no line, a result other than PASS, an error line from the SD or SDHCI
driver, a DMA or SHM error line, an sdhci_error event in QEMU's trace, a bus
other than the expected one, or any failure marker of testing/sdhci_boot.py.
Driver warnings fail too, except a late interrupt and a device pool shortage
that the adapter waited out, as in testing/sdtransfer_boot.py.

Timing under QEMU's TCG says nothing about the hardware. QEMU emulates the
CPU and the card in software and models no bus timing. The numbers are
recorded only to show that the measurement works. Use the program on the
board for real numbers, and testing/sdtiming_report.py for its log.

Example:

    python3 testing/sdtiming_boot.py --arch amd64 --build build/verify-amd64 \\
        --firmware /usr/share/edk2/x64/OVMF_CODE.4m.fd \\
        --firmware-vars /usr/share/edk2/x64/OVMF_VARS.4m.fd \\
        --output build/boot-tests/timing1/amd64
"""
import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time

import sdhci_boot
import sdhci_card
import sdtiming_report
import sdtransfer_boot
import sdtransfer_image

NOTE = ('Timing under QEMU TCG says nothing about the hardware. QEMU emulates the CPU and the card in software '
        'and models no bus timing. It is recorded only to show that the measurement works.')

# The files the program leaves on /data (see sdtiming.c): the test file
# stays, the file of the append probe is deleted.
APPEND_FILE_NAME = 'sdtiming-grow.bin'


class QemuSession:
    """A QEMU process with QMP on its standard input and output. QMP is only
    used to quit QEMU, so the card image is complete when QEMU exits."""

    def __init__(self, command, out):
        self.log = (out / 'qemu.log').open('w')
        # QEMU's temporary disk snapshots must be writable even when the
        # host's default /var/tmp is read-only in a restricted test
        # environment.
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, bufsize=1, env={**os.environ, 'TMPDIR': str(out)})
        self.replies = queue.Queue()
        self.reader = threading.Thread(target=self._read_replies, daemon=True)
        self.reader.start()
        self._receive()
        self.execute('qmp_capabilities')

    def _read_replies(self):
        for line in self.process.stdout:
            self.replies.put(line)
        self.replies.put(None)

    def _receive(self):
        line = self.replies.get(timeout=30)
        if line is None:
            raise RuntimeError('QMP connection closed')
        return json.loads(line)

    def execute(self, name):
        self.process.stdin.write(json.dumps({'execute': name}) + '\n')
        self.process.stdin.flush()
        while True:
            reply = self._receive()
            if 'error' in reply:
                raise RuntimeError(str(reply['error']))
            if 'return' in reply:
                return reply['return']

    def running(self):
        return self.process.poll() is None

    def quit(self):
        """Quit QEMU through QMP, and end it by force if that does not work."""
        try:
            if self.running():
                self.process.stdin.write(json.dumps({'execute': 'quit'}) + '\n')
                self.process.stdin.flush()
                self.process.wait(timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            pass
        finally:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.reader.join(timeout=5)
            self.log.close()


def qemu_command(executable, arch, memory_mib, out, firmware, disk, card, emmc):
    """The QEMU command line, the same machine as testing/sdtransfer_boot.py."""
    machine = 'virt-11.1,gic-version=3' if arch == 'arm64' else 'pc'
    command = [executable, '-machine', machine, '-accel', 'tcg',
               '-smp', '4', '-m', str(memory_mib), '-display', 'none', '-monitor', 'none',
               '-net', 'none', '-no-reboot', '-serial', f'file:{out.as_posix()}/serial.log',
               '-qmp', 'stdio', '-trace', f'events={out.as_posix()}/events.txt,file={out.as_posix()}/trace.log',
               '-drive', f'if=pflash,format=raw,readonly=on,file={firmware}',
               '-drive', f'if=pflash,format=raw,file={out.as_posix()}/vars.fd']
    if arch == 'arm64':
        command += ['-cpu', 'cortex-a57']
    command += ['-drive', f'if=none,id=storage,format=raw,snapshot=on,file={disk.as_posix()}',
                '-device', 'virtio-blk-pci,drive=storage,bootindex=1',
                '-device', 'sdhci-pci,id=sdhci',
                '-drive', f'if=none,id=sdcard,format=raw,file={card.as_posix()}',
                '-device', f'{"emmc" if emmc else "sd-card"},drive=sdcard']
    return command


def wait_for_result(session, out, timeout, observe_seconds, arch):
    """Follow the serial log until the program's result line, then observe a
    little longer. Returns the failure marker lines and the timeline."""
    failures = []
    timeline = {}
    started = time.monotonic()
    result_at = None
    offset = 0
    watched = []
    while time.monotonic() - started < timeout:
        if not session.running():
            raise RuntimeError('QEMU exited early')
        # Only the new part of the serial log is read.
        with (out / 'serial.log').open('rb') as serial:
            serial.seek(offset)
            fresh = serial.read()
        complete = fresh.rfind(b'\n') + 1
        offset += complete
        for line in fresh[:complete].decode(errors='replace').splitlines():
            if any(marker in line for marker in sdhci_boot.FAILURE_MARKERS):
                failures.append(line)
            elif 'sdtiming: ' in line:
                watched.append(line)
        if failures:
            break
        text = '\n'.join(watched)
        if 'start' not in timeline and sdtiming_report.START.search(text):
            timeline['start'] = time.monotonic() - started
            print(f'sdtiming: {arch} probes started after {timeline["start"]:.0f}s', flush=True)
        for match in sdtiming_report.PROBE.finditer(text):
            timeline.setdefault(f'probe {match[1]}', time.monotonic() - started)
        if result_at is None and sdtiming_report.RESULT.search(text):
            result_at = time.monotonic()
            timeline['result'] = result_at - started
            print(f'sdtiming: {arch} result after {timeline["result"]:.0f}s, observing {observe_seconds}s',
                  flush=True)
        if result_at is not None and time.monotonic() - result_at >= observe_seconds:
            break
        time.sleep(2)
    return failures, timeline


def card_files(card):
    """The test files on the card's vali-data partition after the run, by
    name, with their sizes."""
    data = card.read_bytes()
    partition = sdhci_card.read_gpt(data)['partitions'][0]
    volume = sdhci_card.MfsVolume(data, partition['first'])
    return {path: entry['size'] for path, entry in volume.walk()
            if not entry['directory'] and path.startswith('sdtiming')}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--build', type=Path, help='Built OS build directory to make the test image from')
    parser.add_argument('--disk', type=Path, help='A test image from sdtransfer_image.py --program sdtiming, '
                        'instead of --build. Its built-in arguments must match the program options here')
    parser.add_argument('--output', type=Path, required=True, help='New directory for the run artifacts')
    parser.add_argument('--qemu', type=Path, help='QEMU executable. Defaults to qemu-system-x86_64 or '
                        'qemu-system-aarch64 from PATH')
    parser.add_argument('--firmware', type=Path, required=True, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, required=True, help='UEFI variables template, copied')
    parser.add_argument('--mkcdk', type=Path, help='mkcdk executable. Defaults to mkcdk from PATH')
    parser.add_argument('--card-size-mib', type=int, default=512,
                        help='SD card size, a power of two. Above 2048 the card is SDHC')
    parser.add_argument('--memory-mib', type=int, default=4096, help='Guest memory')
    parser.add_argument('--file-mib', type=int, default=8, help='Size of the program\'s test file')
    parser.add_argument('--calls', type=int, default=2000, help='Calls of every probe but file-append-4k')
    parser.add_argument('--append-calls', type=int, default=500, help='Calls of file-append-4k')
    parser.add_argument('--probes', help='Probes to run, as the program\'s --probes. Default all')
    parser.add_argument('--timeout', type=int, default=3600, help='Seconds to wait for the result line')
    parser.add_argument('--observe-seconds', type=int, default=10,
                        help='Seconds to keep running after the result line, to catch late errors')
    parser.add_argument('--emmc', action='store_true',
                        help='Attach the card image as QEMU\'s eMMC device instead of an SD card. Give '
                        '--expect-clock-hz 52000000 with it')
    parser.add_argument('--expect-width', type=int, choices=[1, 4], default=4)
    parser.add_argument('--expect-timing', choices=['default speed', 'high speed'], default='high speed')
    parser.add_argument('--expect-clock-hz', type=int, default=26000000)
    args = parser.parse_args()
    size = args.card_size_mib
    if size <= 0 or size & (size - 1):
        parser.error('--card-size-mib must be a power of two')
    if (args.file_mib + args.append_calls * 4 // 1024 + 1) * 2 > size:
        parser.error('the card must be at least twice the size of both test files')
    if (args.build is None) == (args.disk is None):
        parser.error('give either --build or --disk')
    try:
        expected_probes = sdtiming_report.expand_probes(args.probes)
    except ValueError as error:
        parser.error(str(error))

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    mkcdk = sdhci_card.find_mkcdk(args.mkcdk)
    arguments = f'--file-mib {args.file_mib} --calls {args.calls} --append-calls {args.append_calls}'
    if args.probes:
        arguments += f' --probes {args.probes}'
    if args.disk:
        disk = args.disk.resolve()
        image_manifest = json.loads(disk.with_suffix('.json').read_text())
        if image_manifest.get('program') != 'sdtiming' or image_manifest['arguments'] != arguments:
            parser.error(f'{disk} was built for {image_manifest.get("program")} with '
                         f'"{image_manifest["arguments"]}", not for sdtiming with "{arguments}"')
    else:
        print(f'sdtiming: building the test image from {args.build}', flush=True)
        disk = out / 'system.img'
        image_manifest = sdtransfer_image.build_image(args.build, disk, arguments, mkcdk, program_name='sdtiming')
    (out / 'image-manifest.json').write_text(json.dumps(image_manifest, indent=2))

    executable = str(args.qemu or ('qemu-system-aarch64' if args.arch == 'arm64' else 'qemu-system-x86_64'))
    hashes = {}
    for path in (disk, args.firmware.resolve(), args.firmware_vars.resolve()):
        hashes[str(path)] = sdtransfer_image.sha256(path)
    (out / 'image-hashes.json').write_text(json.dumps(hashes, indent=2))
    version = subprocess.run([executable, '--version'], check=True, capture_output=True, text=True).stdout
    (out / 'qemu-version.txt').write_text(version)
    shutil.copyfile(args.firmware_vars, out / 'vars.fd')
    (out / 'events.txt').write_text('\n'.join(sdtransfer_boot.TRACE_EVENTS) + '\n')

    print(f'sdtiming: building a {size} MiB vali-data card', flush=True)
    manifest = sdhci_card.build_card(out, size, 'vali-data', sdhci_card.MFS_DATA_TYPE, True, mkcdk)
    (out / 'card-manifest.json').write_text(json.dumps(manifest, indent=2))
    card = out / 'sdcard.raw'

    command = qemu_command(executable, args.arch, args.memory_mib, out, args.firmware.resolve(), disk, card,
                           args.emmc)
    (out / 'command.json').write_text(json.dumps(command, indent=2))
    session = QemuSession(command, out)
    try:
        failures, timeline = wait_for_result(session, out, args.timeout, args.observe_seconds, args.arch)
        session.execute('stop')
    finally:
        session.quit()

    text = (out / 'serial.log').read_text(errors='replace')
    trace = (out / 'trace.log').read_text(errors='replace')
    parsed = sdtiming_report.parse_log(text)
    table = sdtiming_report.format_table(parsed)
    (out / 'table.txt').write_text(f'{NOTE}\n\n{table}\n')
    card_line = sdhci_boot.CARD_READY.search(text)
    slot_bus = sdhci_boot.SLOT_BUS.search(text)
    driver_lines = [line for line in text.splitlines() if sdhci_boot.DRIVER_PROBLEM.search(line)]
    files = card_files(card)
    result = {
        'arch': args.arch,
        'memory_mib': args.memory_mib,
        'card': f'{size} MiB vali-data {"eMMC device" if args.emmc else "card"}',
        'arguments': arguments,
        'note': NOTE,
        'timeline_seconds': timeline,
        'failures': failures,
        'program': sdtiming_report.report(parsed),
        'expected_probes': expected_probes,
        'card_ready': card_line.group(0) if card_line else None,
        'slot_bus': slot_bus.group(0) if slot_bus else None,
        'driver_problems': [line for line in driver_lines if not sdhci_boot.LATE_INTERRUPT.search(line)
                            and not sdtransfer_boot.POOL_SHORT.search(line)],
        'late_interrupts': [line for line in driver_lines if sdhci_boot.LATE_INTERRUPT.search(line)],
        'pool_short_waits': [line for line in driver_lines if sdtransfer_boot.POOL_SHORT.search(line)],
        'dma_problems': [line for line in text.splitlines() if sdtransfer_boot.DMA_PROBLEM.search(line)],
        'data_mounted': 'storage_ready_invocation(path=/data)' in text,
        'trace_errors': [line for line in trace.splitlines() if line.startswith('sdhci_error')][:50],
        'data_commands': sdtransfer_boot.summarize_commands(sdtransfer_boot.data_command_sizes(trace)),
        'card_test_files': files,
    }
    image = sdhci_card.compare_images(out / 'sdcard.orig', card, manifest)
    result['card_files'] = {path: entry['present'] for path, entry in image['files'].items()}
    result['master_record_problems_after'] = image['master_record_problems_after']

    expected_bus = (args.expect_width, args.expect_timing, args.expect_clock_hz)
    verdict = parsed['result']
    checks = {
        'no_failures': not failures,
        'data_mounted': result['data_mounted'],
        'negotiated_bus': (card_line is not None and slot_bus is not None
                           and (int(card_line.group(5)), card_line.group(6), int(card_line.group(7))) == expected_bus
                           and (int(slot_bus.group(2)), slot_bus.group(3), int(slot_bus.group(4))) == expected_bus),
        'program_pass': (verdict is not None and verdict['verdict'] == 'PASS' and verdict['errors'] == 0
                         and not parsed['failures']),
        'every_probe_reported': sorted(parsed['probes']) == sorted(expected_probes) and not parsed['skipped'],
        'no_driver_problems': not result['driver_problems'],
        'no_dma_problems': not result['dma_problems'],
        # The SD setup runs first and gets no answer to CMD8 and CMD55 from an
        # eMMC device. QEMU's host logs each as a command response timeout.
        'no_trace_errors': (result['trace_errors'] == ['sdhci_error timeout waiting for command response'] * 2
                            if args.emmc else not result['trace_errors']),
        'test_file_left': files.get('sdtiming.bin') == args.file_mib * 1024 * 1024,
        'append_file_deleted': APPEND_FILE_NAME not in files,
        'card_files_intact': all(result['card_files'].values()),
        'master_record_valid': not result['master_record_problems_after'],
    }
    result['checks'] = checks
    result['pass'] = all(checks.values())
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result[key] for key in ('checks', 'pass', 'card_ready')}, indent=2))
    print(NOTE)
    print(table)
    if not result['pass']:
        raise SystemExit('SD timing test failed: ' + ', '.join(name for name, passed in checks.items() if not passed))


if __name__ == '__main__':
    main()
