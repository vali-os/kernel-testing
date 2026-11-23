"""Take SD cards out of a running system and put them in again, on QEMU's sdhci-pci host.

The system boots from its VirtIO disk, as in testing/sdhci_boot.py, with one
sdhci-pci host and an sd-card device on its bus. The card device has a
removable medium: the runner takes it out with the QMP command "eject" and
puts an image in with "blockdev-change-medium". QEMU's host then changes the
card detect bits in its present state register, raises the card insertion
or removal interrupt, and switches bus power and the SD clock off by itself
on a removal.

QEMU 11.1 gives every sd-card the same CID (manufacturer 0xaa, product
"QEMU!", serial 0xdeadbeef). Two cards are told apart only by their size and
contents, so card B is twice the size of card A and holds other files. The
slot compares the CID and the capacity, so it sees B as another card. filed
gets the same serial for both, /storage/SDAA-QEMU_-DEADBEEF.

Scenarios (--scenario):

- sequence: boot with an empty slot, then, in one boot:
  insert card A, take it out while idle, put A in again, take it out and put
  card B in, take B out and put it back faster than the 200 ms debounce,
  take B out and put it back 3 seconds later, and take B out the moment the
  driver sends its register request, then put it in once more. Both cards
  carry the sd-test partition, which filed mounts at
  /storage/<Serial>/sd-test and nothing writes to. Neither image may change.
  Use a system disk with its own vali-data partition (sdhci-storage.img).
- write: boot with a vali-data card. served's first start creates
  /data/served and three directories below it, which is the only writing
  the system does on its own (CMD24 and CMD25 of up to 64 sectors, spread
  over a few seconds). QEMU keeps no block statistics for an sd-card, so the
  runner takes the card out the moment MFS logs the creation of
  /data/served/apps (WRITE_TRIGGER), which falls among those writes.
  Whether the removal hits a command that runs, or the gap between two,
  depends on timing. results.json says which, from the engine's "card
  removed while CMDn ran" line and from QEMU's trace (removal_context: the
  last commands before the removal). Then card B goes in, and the driver
  must give it a new storage device ID. filed's mount of card B is a step of
  its own, and so is taking B out again, so a filed problem after the first
  removal does not hide whether the driver went on. Card A must not change
  after its removal. Use a system disk without vali-data (sdhci-nodata.img).

  A larger write needs a client inside the guest. served would copy a chef
  package from /data/setup into /data/served/apps, but the only package at
  hand (an old hello-world.pack) makes served crash while it reads it, see
  the stage 6 notes in docs/sdhci-driver-design.md.

Each step waits for the lines that prove it in the serial log and records
them with the time the host waited. results.json holds every step, the
driver's warning and error lines with the expected ones listed apart, and
the image checks. A step that times out fails the run.
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

import sdhci_boot
import sdhci_card

# Card sizes. A stays an SDSC card like the default boot test card, and B is
# twice as large, which makes it another card for the slot.
CARD_A_MIB = 64
CARD_B_MIB = 128

# Files of card B. Other paths and sizes than CARD_FILES, so the two cards
# differ in contents as well.
CARD_B_FILES = (
    ('card-b.txt', 900),
    ('other/blob-200k.bin', 200 * 1024),
)

# MFS's line for the second directory served creates on its first start.
# The card gets writes for that directory right after it.
WRITE_TRIGGER = re.compile(r'FsCreate\(parent=served, name=apps\)')

# The engine debounces card detect for 200 ms (SDHCI_CARD_DEBOUNCE_US).
DEBOUNCE_S = 0.2

# Slot, adapter, engine and filed lines.
SLOT_START = re.compile(r'SD slot (\d+): (card detect line reports (a card|no card)|'
                        r'no card detect line, looking for a card with commands)')
SLOT_INSERTED = re.compile(r'SD slot (\d+): card inserted')
SLOT_REMOVED = re.compile(r'SD slot (\d+): (card removed|the card does not answer, it was removed|'
                          r'no card answered, it was removed)')
SLOT_LEAVING = re.compile(r'SD slot (\d+): leaving state ([a-z ]+), (card removed|card replaced|card failed|'
                          r'host failed|slot stopped)')
SAME_CARD = re.compile(r'SD slot (\d+): the same card is back, storage device (\d+) goes on')
ENGINE_REMOVED_DURING = re.compile(r'SDHCI (\d+): card removed while CMD(\d+) ran')
ADAPTER_STOPPING = re.compile(r'SD storage (\d+): stopping, queued requests fail with (\d+)')
ADAPTER_GONE = re.compile(r'SD storage (\d+): the card of media generation (\d+) is gone, failing the request '
                          r'at sector (\d+)')
UNREGISTER_ACCEPTED = re.compile(r'SD storage (\d+): (?:filed accepted unregister|unregister ended with (\d+))')
FILED_UNREGISTER = re.compile(r'sys_storage_unregister_invocation\(driver=(\d+), device=(\d+)\)')
FILED_DEFERRED = re.compile(r'driver (\d+) device (\d+) (is still registering, teardown deferred|'
                            r'was unregistered during registration)')
FILED_UNMOUNT = re.compile(r'VFSFileSystemUnmount\(fs=(\d+)\)')
MOUNT = re.compile(r'__StorageMount mounting at /storage/(\S+)')
PARTITION = re.compile(r'Partition-name: (\S+)')

# Warnings and errors a step may cause on purpose. Anything else from the
# driver fails the run (see sdhci_boot.DRIVER_PROBLEM).
EXPECTED_PROBLEMS = (
    # A removal that ends a running command (request.c).
    re.compile(r'SDHCI \d+: card removed while CMD\d+ ran'),
    # The adapter fails the request of a card that has gone (storage.c).
    re.compile(r'SD storage \d+: the card of media generation \d+ is gone'),
    # A sector operation that the host refused or ended because the card
    # left: the sequence's failed step and the sector line, both with
    # OS_ENOENT (3). filed reads the card while it registers it, so a
    # removal at that time ends a read this way (sequence.c, sectors.c).
    re.compile(r'SD (?:read|write): step [^:]+ failed: 3(?:,|$)'),
    re.compile(r'SD: (?:read|write) of \d+ sectors at sector \d+ failed with \d+ sectors confirmed: 3$'),
    re.compile(r'SD: write of \d+ sectors at sector \d+ not stored, card status 0x[0-9a-f]+, '
               r'errors 0x[0-9a-f]+: 3$'),
    # A late interrupt under TCG, see sdhci_boot.LATE_INTERRUPT.
    sdhci_boot.LATE_INTERRUPT,
)

# served must keep running when /data goes away under its setup job. processd
# logs the process ID it gives served when it starts it, and the exit of a
# process with that ID. A crash also shows as a failure marker
# (sdhci_boot.FAILURE_MARKERS).
SERVED_STARTED = re.compile(r'__StartProcess\(process=served\.dll\)(?:\n.*){0,10}?\n.*'
                            r'PmCreateProcess registering process (\d+)/')
PROCESS_EXIT = re.compile(r'PmTerminateProcess process (\d+) exitted with code: (-?\d+)')


def served_exits(text):
    """The processd lines that report an exit of served."""
    started = SERVED_STARTED.search(text)
    if started is None:
        return []
    return [match.group(0) for match in PROCESS_EXIT.finditer(text) if match.group(1) == started.group(1)]


class Guest:
    """One QEMU process with its QMP connection on stdin and stdout."""

    def __init__(self, command, out):
        self.out = out
        self.log = (out / 'qemu.log').open('w')
        # QEMU's temporary disk snapshots must be writable even when the
        # host's default /var/tmp is read-only in a restricted test environment.
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                                        text=True, bufsize=1, env={**os.environ, 'TMPDIR': str(out)})
        self.replies = queue.Queue()
        self.events = []
        self.reader = threading.Thread(target=self.__read, daemon=True)
        self.reader.start()
        self.__receive()
        self.qmp('qmp_capabilities')

    def __read(self):
        for line in self.process.stdout:
            self.replies.put(line)
        self.replies.put(None)

    def __receive(self):
        line = self.replies.get(timeout=30)
        if line is None:
            raise RuntimeError('QMP connection closed')
        return json.loads(line)

    def qmp(self, execute, arguments=None):
        request = {'execute': execute}
        if arguments is not None:
            request['arguments'] = arguments
        self.process.stdin.write(json.dumps(request) + '\n')
        self.process.stdin.flush()
        while True:
            reply = self.__receive()
            if 'event' in reply:
                self.events.append(reply)
                continue
            if 'error' in reply:
                raise RuntimeError(f'{execute}: {reply["error"]}')
            if 'return' in reply:
                return reply['return']

    def text(self):
        return (self.out / 'serial.log').read_text(errors='replace')

    def alive(self):
        return self.process.poll() is None

    def eject(self):
        self.qmp('eject', {'id': 'card0'})
        return time.monotonic()

    def insert(self, image):
        self.qmp('blockdev-change-medium', {'id': 'card0', 'filename': str(image), 'format': 'raw'})
        return time.monotonic()

    def quit(self):
        try:
            if self.alive():
                self.process.stdin.write(json.dumps({'execute': 'quit'}) + '\n')
                self.process.stdin.flush()
                self.process.wait(timeout=30)
        finally:
            if self.alive():
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.reader.join(timeout=5)
            self.log.close()


class StepFailed(Exception):
    pass


class Run:
    """The steps of one scenario and what each one found in the log."""

    def __init__(self, guest, step_timeout):
        self.guest = guest
        self.step_timeout = step_timeout
        self.steps = []
        self.mark = 0

    def failures(self, text):
        return [line for line in text.splitlines()
                if any(marker in line for marker in sdhci_boot.FAILURE_MARKERS)]

    def wait(self, pattern, since=None, timeout=None, predicate=None):
        """Wait for pattern in the serial log after offset since (the step
        mark by default). Returns the match. Fails on a failure marker or a
        timeout."""
        start = self.mark if since is None else since
        deadline = time.monotonic() + (timeout or self.step_timeout)
        while time.monotonic() < deadline:
            if not self.guest.alive():
                raise StepFailed('QEMU exited')
            text = self.guest.text()
            failures = self.failures(text)
            if failures:
                raise StepFailed(f'failure marker: {failures[0]}')
            for match in pattern.finditer(text, start):
                if predicate is None or predicate(match):
                    return match
            time.sleep(0.02)
        raise StepFailed(f'no line matching {pattern.pattern!r} within {timeout or self.step_timeout} s')

    def step(self, name, function):
        """Run one step. The step's lines are searched from the mark at its
        start, so each step only sees what happened since."""
        self.mark = len(self.guest.text())
        record = {'name': name, 'pass': False, 'lines': []}
        self.steps.append(record)
        print(f'step: {name}', flush=True)
        started = time.monotonic()
        try:
            function(record)
            record['pass'] = True
        except StepFailed as failure:
            record['failure'] = str(failure)
        record['seconds'] = round(time.monotonic() - started, 3)
        text = self.guest.text()
        record['log'] = [line for line in text[self.mark:].splitlines()
                         if re.search(r'SD slot|SD storage|SDHCI|SD (?:setup|read|write):|storage_(?:un)?register|'
                                      r'__StorageMount|Partition-name|VFSFileSystemUnmount|teardown deferred|'
                                      r'during registration', line)]
        print(f'  {"pass" if record["pass"] else "FAIL: " + record.get("failure", "")} '
              f'({record["seconds"]} s)', flush=True)
        return record['pass']


LOG_LEVEL = re.compile(r'^\d+ \[(\w+)-\d+-\d+\]\s*(.*)$')
DRIVER_LINE = re.compile(r'^(SDHCI( \d+)?:|SD:|SD storage \d+:|SD slot \d+:|SD (?:setup|read|write):|RPI-FW)')


def log_volume(text):
    """Lines of a stretch of the serial log by level: all of them, and the
    driver's own (SDHCI, SD and the firmware driver that switches the card
    supply on the Pi 4)."""
    volume = {'all': {}, 'driver': {}}
    for line in text.splitlines():
        match = LOG_LEVEL.match(line)
        if not match:
            continue
        level = match.group(1)
        volume['all'][level] = volume['all'].get(level, 0) + 1
        if DRIVER_LINE.match(match.group(2)):
            volume['driver'][level] = volume['driver'].get(level, 0) + 1
    return volume


def removal_context(trace):
    """QEMU's view of the first removal: the last commands the host sent
    before the card left, with the blocks each moved, and the commands after
    it until the next insertion. QEMU runs an ADMA transfer to its end at
    once, so a write that the trace shows as written can still be running
    for the driver, which then waits for its completion, CMD12 and CMD13."""
    def describe(commands):
        return [f'CMD{c["cmd"]} arg 0x{c["arg"]:x} read {c["read"]} written {c["written"]}' for c in commands]

    lines = trace.splitlines()
    for index, line in enumerate(lines):
        if 'sdcard_ejected' in line:
            break
    else:
        return None
    end = next((later for later in range(index, len(lines)) if 'sdcard_inserted' in lines[later]), len(lines))
    before, _ = sdhci_boot.data_commands('\n'.join(lines[:index]))
    after, _ = sdhci_boot.data_commands('\n'.join(lines[index:end]))
    return {'last_commands_before': describe(before[-6:]), 'commands_after': describe(after[:6])}


def image_sha256(path):
    with path.open('rb') as image:
        return hashlib.file_digest(image, 'sha256').hexdigest()


def note(record, match):
    record['lines'].append(match.group(0))
    return match


def wait_disk(run, record, previous_id=None, sectors=None):
    """A card was put in: the adapter's disk line, the slot's register line,
    filed's register and mount of /storage/<Serial> and the partition name.
    Returns (storage device ID, media generation)."""
    disk = note(record, run.wait(sdhci_boot.DISK_CREATED))
    sent = note(record, run.wait(sdhci_boot.REGISTER_SENT))
    storage_id, generation = int(sent.group(2)), int(sent.group(3))
    if previous_id is not None and storage_id <= previous_id:
        raise StepFailed(f'storage device {storage_id} is not new (last {previous_id})')
    if sectors is not None and int(disk.group(3)) != sectors:
        raise StepFailed(f'disk has {disk.group(3)} sectors, expected {sectors}')
    note(record, run.wait(sdhci_boot.FILED_REGISTER,
                          predicate=lambda match: int(match.group(2)) == storage_id))
    note(record, run.wait(sdhci_boot.REGISTER_ACCEPTED,
                          predicate=lambda match: int(match.group(1)) == storage_id))
    mount = note(record, run.wait(MOUNT, predicate=lambda match: match.group(1) == disk.group(2)))
    # The partition name that follows the card's mount. The system disk logs
    # its own names too, possibly earlier.
    note(record, run.wait(PARTITION, since=mount.end()))
    record['storage_id'] = storage_id
    record['generation'] = generation
    return storage_id, generation


def wait_removed(run, record, storage_id, ejected_at):
    """A card was taken out: the slot's removal line, Leaving, the adapter
    stopping, filed's unregister for the storage device, the unregister
    answer and filed's unmount."""
    removed = note(record, run.wait(SLOT_REMOVED))
    record['removal_seen_after_s'] = round(time.monotonic() - ejected_at, 3)
    note(record, run.wait(SLOT_LEAVING))
    note(record, run.wait(ADAPTER_STOPPING, predicate=lambda match: int(match.group(1)) == storage_id))
    note(record, run.wait(FILED_UNREGISTER, predicate=lambda match: int(match.group(2)) == storage_id))
    answer = note(record, run.wait(UNREGISTER_ACCEPTED, predicate=lambda match: int(match.group(1)) == storage_id))
    if answer.group(2) is not None:
        raise StepFailed(f'unregister of storage device {storage_id} ended with {answer.group(2)}')
    note(record, run.wait(FILED_UNMOUNT))
    return removed


def scenario_sequence(run, args, cards, report):
    guest = run.guest
    ids = {'last': None, 'generation': None}
    detect_line = {}

    def empty_start(record):
        start = note(record, run.wait(SLOT_START, since=0))
        # Group 3 is what the detect line reports. It is empty for a slot
        # without one.
        detect_line['present'] = start.group(3) is not None
        if detect_line['present'] and start.group(3) != 'no card':
            raise StepFailed('the slot reports a card in an empty slot')
        # Nothing may register while the slot is empty. What the empty slot
        # writes to the log meanwhile is recorded by level.
        start = len(guest.text())
        time.sleep(args.empty_seconds)
        text = guest.text()
        record['log_volume'] = {'seconds': args.empty_seconds, **log_volume(text[start:])}
        if sdhci_boot.REGISTER_SENT.search(text, run.mark):
            raise StepFailed('a disk was registered for an empty slot')

    def insert_a(record):
        guest.insert(cards['a'])
        if detect_line['present']:
            note(record, run.wait(SLOT_INSERTED))
        ids['last'], ids['generation'] = wait_disk(run, record, sectors=CARD_A_MIB * 2048)

    def remove_a_idle(record):
        # What a mounted card that nobody uses writes to the log.
        start = len(guest.text())
        time.sleep(args.idle_seconds)
        record['log_volume'] = {'seconds': args.idle_seconds, **log_volume(guest.text()[start:])}
        ejected = guest.eject()
        wait_removed(run, record, ids['last'], ejected)
        # Without a detect line the idle check (CMD13 once a second) finds
        # the removal, so it takes up to about a second.
        if detect_line['present'] and record['removal_seen_after_s'] < DEBOUNCE_S:
            raise StepFailed(f'removal seen after {record["removal_seen_after_s"]} s, before the debounce')
        report['card_a_after_removal'] = image_sha256(cards['a'])

    def insert_a_again(record):
        time.sleep(1)
        guest.insert(cards['a'])
        storage_id, generation = wait_disk(run, record, ids['last'], CARD_A_MIB * 2048)
        if generation <= ids['generation']:
            raise StepFailed(f'media generation {generation} is not new (last {ids["generation"]})')
        ids['last'], ids['generation'] = storage_id, generation

    def swap_to_b(record):
        time.sleep(2)
        ejected = guest.eject()
        wait_removed(run, record, ids['last'], ejected)
        report['card_a_after_second_removal'] = image_sha256(cards['a'])
        time.sleep(1)
        guest.insert(cards['b'])
        ids['last'], ids['generation'] = wait_disk(run, record, ids['last'], CARD_B_MIB * 2048)

    def fast_pair(record):
        time.sleep(2)
        ejected = guest.eject()
        time.sleep(0.05)
        guest.insert(cards['b'])
        record['host_gap_s'] = round(time.monotonic() - ejected, 3)
        if not detect_line['present']:
            # The idle check sees a card that does not answer as it did, and
            # card setup finds the same CID and capacity. Without a detect
            # line that is the same card, and its disk goes on.
            note(record, run.wait(SAME_CARD, predicate=lambda match: int(match.group(2)) == ids['last']))
            return
        wait_removed(run, record, ids['last'], ejected)
        ids['last'], ids['generation'] = wait_disk(run, record, ids['last'], CARD_B_MIB * 2048)

    def far_pair(record):
        time.sleep(2)
        ejected = guest.eject()
        wait_removed(run, record, ids['last'], ejected)
        time.sleep(3)
        guest.insert(cards['b'])
        ids['last'], ids['generation'] = wait_disk(run, record, ids['last'], CARD_B_MIB * 2048)

    def remove_while_registering(record):
        time.sleep(2)
        ejected = guest.eject()
        wait_removed(run, record, ids['last'], ejected)
        time.sleep(1)
        guest.insert(cards['b'])
        sent = note(record, run.wait(sdhci_boot.REGISTER_SENT))
        seen = time.monotonic()
        ejected = guest.eject()
        storage_id = int(sent.group(2))
        record['ejected_after_register_line_s'] = round(ejected - seen, 3)
        note(record, run.wait(SLOT_REMOVED))
        note(record, run.wait(FILED_UNREGISTER, predicate=lambda match: int(match.group(2)) == storage_id))
        note(record, run.wait(UNREGISTER_ACCEPTED, predicate=lambda match: int(match.group(1)) == storage_id))
        # filed defers the teardown only when its registration still runs.
        # The time it needs decides that, so the outcome is recorded, not
        # required.
        time.sleep(2)
        deferred = FILED_DEFERRED.search(guest.text(), run.mark)
        record['filed_deferred'] = deferred.group(0) if deferred else None
        ids['last'] = storage_id

    def insert_b_last(record):
        time.sleep(2)
        guest.insert(cards['b'])
        ids['last'], ids['generation'] = wait_disk(run, record, ids['last'], CARD_B_MIB * 2048)

    steps = (('start with an empty slot', empty_start),
             ('insert card A', insert_a),
             ('remove card A while idle', remove_a_idle),
             ('insert card A again', insert_a_again),
             ('remove card A, insert card B', swap_to_b),
             ('remove and insert card B within 50 ms', fast_pair),
             ('remove card B, insert it 3 s later', far_pair),
             ('remove card B as its register request goes out', remove_while_registering),
             ('insert card B once more', insert_b_last))
    for name, function in steps:
        if not run.step(name, function):
            break


def scenario_write(run, args, cards, report):
    guest = run.guest
    state = {}

    def mounted(record):
        state['id'], state['generation'] = wait_disk(run, record, sectors=CARD_A_MIB * 2048)

    def remove_during_writes(record):
        note(record, run.wait(WRITE_TRIGGER))
        ejected = guest.eject()
        report['card_a_after_removal'] = image_sha256(cards['a'])
        removed = wait_removed(run, record, state['id'], ejected)
        text = guest.text()
        # What the removal did to the request that ran and to the queue.
        record['engine_removed_during'] = [match.group(0) for match in ENGINE_REMOVED_DURING.finditer(text, run.mark)]
        record['adapter_gone'] = [match.group(0) for match in ADAPTER_GONE.finditer(text, run.mark)]
        record['adapter_stopping'] = [match.group(0) for match in ADAPTER_STOPPING.finditer(text, run.mark)]
        record['removal_line'] = removed.group(0)

    def insert_b(record):
        time.sleep(3)
        guest.insert(cards['b'])
        sent = note(record, run.wait(sdhci_boot.REGISTER_SENT))
        if int(sent.group(2)) <= state['id']:
            raise StepFailed(f'storage device {sent.group(2)} is not new (last {state["id"]})')
        state['previous'], state['id'] = state['id'], int(sent.group(2))

    def card_b_mounted(record):
        note(record, run.wait(sdhci_boot.FILED_REGISTER, since=0,
                              predicate=lambda match: int(match.group(2)) == state['id']))
        note(record, run.wait(sdhci_boot.REGISTER_ACCEPTED, since=0,
                              predicate=lambda match: int(match.group(1)) == state['id']))
        note(record, run.wait(PARTITION))

    def remove_b(record):
        # The driver must still follow the slot, whatever filed did with B.
        guest.eject()
        note(record, run.wait(SLOT_REMOVED))
        note(record, run.wait(SLOT_LEAVING))
        note(record, run.wait(ADAPTER_STOPPING, predicate=lambda match: int(match.group(1)) == state['id']))

    # The driver's part comes first and is required in full. filed's mount
    # of card B is its own step, so a filed problem after the removal does
    # not hide whether the driver went on.
    for name, function in (('card A mounted', mounted),
                           ('remove card A while served writes to it', remove_during_writes),
                           ('insert card B', insert_b)):
        if not run.step(name, function):
            return
    run.step('filed registers and mounts card B', card_b_mounted)
    run.step('remove card B', remove_b)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--arch', choices=['amd64', 'arm64'], default='amd64')
    parser.add_argument('--scenario', choices=['sequence', 'write'], required=True)
    parser.add_argument('--disk', type=Path, required=True, help='Packaged system disk (GPT image)')
    parser.add_argument('--output', type=Path, required=True, help='New directory for the run artifacts')
    parser.add_argument('--qemu', type=Path, help='QEMU executable. Defaults to qemu-system-x86_64 or '
                        'qemu-system-aarch64 from PATH')
    parser.add_argument('--firmware', type=Path, required=True, help='Read-only UEFI code image')
    parser.add_argument('--firmware-vars', type=Path, required=True, help='UEFI variables template, copied')
    parser.add_argument('--mkcdk', type=Path, help='mkcdk executable that builds the cards. Defaults to mkcdk '
                        'from PATH')
    parser.add_argument('--empty-seconds', type=int, default=5,
                        help='Sequence: seconds the slot stays empty after boot, with its log volume recorded')
    parser.add_argument('--idle-seconds', type=int, default=3,
                        help='Sequence: seconds card A stays mounted and idle before its removal, with its log '
                        'volume recorded')
    parser.add_argument('--boot-timeout', type=int, default=600, help='Seconds for the boot to reach the slot')
    parser.add_argument('--step-timeout', type=int, default=120, help='Seconds for each step')
    parser.add_argument('--observe-seconds', type=int, default=15,
                        help='Seconds to keep running after the last step, to catch late errors')
    args = parser.parse_args()

    out = args.output.resolve()
    disk = args.disk.resolve()
    if args.scenario == 'write':
        sdhci_boot.check_data_card_disk(disk)
    out.mkdir(parents=True, exist_ok=False)
    executable = str(args.qemu or ('qemu-system-aarch64' if args.arch == 'arm64' else 'qemu-system-x86_64'))
    (out / 'qemu-version.txt').write_text(
        subprocess.run([executable, '--version'], check=True, capture_output=True, text=True).stdout)
    shutil.copyfile(args.firmware_vars, out / 'vars.fd')
    (out / 'events.txt').write_text('sdhci_*\nsdcard_*\n')

    # Card A: sd-test for the sequence, vali-data for the write scenario.
    # Card B: sd-test with its own files.
    mkcdk = sdhci_card.find_mkcdk(args.mkcdk)
    manifests = {}
    if args.scenario == 'write':
        manifests['a'] = sdhci_card.build_card(out / 'card-a', CARD_A_MIB, 'vali-data', sdhci_card.MFS_DATA_TYPE,
                                               True, mkcdk)
    else:
        manifests['a'] = sdhci_card.build_card(out / 'card-a', CARD_A_MIB, 'sd-test', sdhci_card.MFS_USER_TYPE,
                                               False, mkcdk)
    manifests['b'] = sdhci_card.build_card(out / 'card-b', CARD_B_MIB, 'sd-test', sdhci_card.MFS_USER_TYPE,
                                           False, mkcdk, CARD_B_FILES)
    cards = {name: out / f'card-{name}' / 'sdcard.raw' for name in ('a', 'b')}
    (out / 'card-manifests.json').write_text(json.dumps(manifests, indent=2))

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
    # The sequence starts with an empty slot: the card device exists, with
    # no medium in it.
    if args.scenario == 'write':
        command += ['-drive', f'if=none,id=sdcard,format=raw,file={cards["a"].as_posix()}']
    else:
        command += ['-drive', 'if=none,id=sdcard']
    command += ['-device', 'sd-card,drive=sdcard,id=card0']
    (out / 'command.json').write_text(json.dumps(command, indent=2))

    report = {'arch': args.arch, 'scenario': args.scenario}
    guest = Guest(command, out)
    run = Run(guest, args.step_timeout)
    try:
        boot = run.step('boot until the slot starts',
                        lambda record: note(record, run.wait(SLOT_START, since=0, timeout=args.boot_timeout)))
        if boot:
            if args.scenario == 'sequence':
                scenario_sequence(run, args, cards, report)
            else:
                scenario_write(run, args, cards, report)
        if all(step['pass'] for step in run.steps):
            time.sleep(args.observe_seconds)
        guest.qmp('stop')
    finally:
        guest.quit()

    text = (out / 'serial.log').read_text(errors='replace')
    problems = [line for line in text.splitlines() if sdhci_boot.DRIVER_PROBLEM.search(line)]
    report['steps'] = run.steps
    report['failures'] = run.failures(text)
    report['served_exits'] = served_exits(text)
    report['expected_problems'] = [line for line in problems
                                   if any(pattern.search(line) for pattern in EXPECTED_PROBLEMS)]
    report['driver_problems'] = [line for line in problems if line not in report['expected_problems']]
    report['qmp_events'] = guest.events
    trace = (out / 'trace.log').read_text(errors='replace')
    report['trace'] = sdhci_boot.summarize_trace(trace)
    if args.scenario == 'write':
        report['removal_context'] = removal_context(trace)

    # Card A must not change after its (last) removal. A card nothing
    # writes to must not change at all, and card B is never written to.
    images = {}
    for name in ('a', 'b'):
        images[name] = sdhci_card.compare_images(out / f'card-{name}' / 'sdcard.orig', cards[name], manifests[name])
    report['card_images'] = images
    checks = {
        'steps_passed': bool(run.steps) and all(step['pass'] for step in run.steps),
        'no_failures': not report['failures'],
        'served_running': not report['served_exits'],
        'no_unexpected_driver_problems': not report['driver_problems'],
        'card_b_unchanged': images['b']['changed_sectors'] == 0,
    }
    final_a = image_sha256(cards['a'])
    if 'card_a_after_removal' in report:
        checks['card_a_unchanged_after_removal'] = report['card_a_after_removal'] == final_a
    if args.scenario == 'sequence':
        checks['card_a_unchanged'] = images['a']['changed_sectors'] == 0
        checks['card_a_unchanged_after_second_removal'] = report.get('card_a_after_second_removal') == final_a
    report['checks'] = checks
    report['pass'] = all(checks.values())
    (out / 'results.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'checks': checks, 'driver_problems': report['driver_problems'],
                      'expected_problems': report['expected_problems']}, indent=2), flush=True)
    if not report['pass']:
        raise SystemExit('SD card removal and insertion failed: ' +
                         ', '.join(name for name, passed in checks.items() if not passed))


if __name__ == '__main__':
    main()
