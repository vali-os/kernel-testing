#!/usr/bin/env python3
"""Boot one native SMMUv3 self-test case in a fresh QEMU VM and check its result.

The kernel must come from a build configured with VALI_ARM64_SMMU_SELFTEST=ON
and the requested VALI_ARM64_SMMU_SELFTEST_CASE. The runner never configures
or rebuilds that build. It builds a small read-only UEFI disk, starts QEMU
paused, checks the PCI and SMMU topology over QMP, then lets the guest run
until it prints its terminal SMMU-SELFTEST record or the deadline passes.

Exit codes: 0 PASS, 1 FAIL, 77 NOT RUN (a prerequisite is missing). Usage
errors, including an unknown case, exit with 2.
Evidence goes to --output-dir: the resolved QEMU command, the QEMU version,
input hashes, the serial log, QEMU stderr, the QMP checks, the rebuilt
iort.bin with its hash, the kernel and its build settings.

--self-check runs only the log checks against built-in logs and needs no QEMU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

QEMU_VERSION = '11.1.3'
MACHINE = 'virt-11.1,gic-version=3'
DEADLINE_SECONDS = 180
CASES = ('positive', 'unmapped', 'write-denied', 'unknown-stream', 'other-domain', 'after-unmap',
         'after-disconnect')

# Every request the guest can make: the domain that sends it and what must
# happen. 'ok' must succeed. 'no-mapping' and 'permission' must be refused
# with exactly one expected fault record of that kind. 'abort' must be
# refused with no fault record, or with one expected 'no-route' record.
REQUESTS = {
    'write-only': ('domain-a', 'ok'),
    'page-boundary': ('domain-a', 'ok'),
    'same-iova-b': ('domain-b', 'ok'),
    'same-iova-a': ('domain-a', 'ok'),
    'after-reconnect': ('domain-a', 'ok'),
    'control-a': ('domain-a', 'ok'),
    'control-b': ('domain-b', 'ok'),
    'domain-a-page2': ('domain-a', 'ok'),
    'follow-up-a': ('domain-a', 'ok'),
    'follow-up-b': ('domain-b', 'ok'),
    'unmapped-write': ('domain-a', 'no-mapping'),
    'denied-write': ('domain-b', 'permission'),
    'unconnected-write': ('domain-b', 'abort'),
    'other-domain-write': ('domain-b', 'no-mapping'),
    'after-unmap-write': ('domain-a', 'no-mapping'),
    'after-disconnect-write': ('domain-a', 'abort'),
}

# The requests and connection events a PASS needs, in order, from the first
# connect to "cleanup complete". The cleanup disconnects show which domains
# were still connected at the end. "cleanup complete" follows the release of
# every mapping, space, test page and the unit itself.
CONNECT_BOTH = ('connect domain-a', 'connect domain-b')
CLEANUP_BOTH = ('disconnect domain-a', 'disconnect domain-b', 'cleanup complete')
FOLLOW_UP_BOTH = ('follow-up-a', 'follow-up-b')
CASE_SEQUENCES = {
    'positive': CONNECT_BOTH + ('write-only', 'page-boundary', 'same-iova-b', 'same-iova-a',
                                'disconnect domain-a', 'connect domain-a', 'after-reconnect') + CLEANUP_BOTH,
    'unmapped': CONNECT_BOTH + ('control-a', 'unmapped-write') + FOLLOW_UP_BOTH + CLEANUP_BOTH,
    'write-denied': CONNECT_BOTH + ('control-b', 'denied-write') + FOLLOW_UP_BOTH + CLEANUP_BOTH,
    'unknown-stream': ('connect domain-a', 'control-a', 'unconnected-write', 'follow-up-a',
                       'disconnect domain-a', 'cleanup complete'),
    'other-domain': CONNECT_BOTH + ('control-b', 'domain-a-page2', 'other-domain-write') + FOLLOW_UP_BOTH
    + CLEANUP_BOTH,
    'after-unmap': CONNECT_BOTH + ('control-a', 'release write-only', 'after-unmap-write') + FOLLOW_UP_BOTH
    + CLEANUP_BOTH,
    'after-disconnect': CONNECT_BOTH + ('control-a', 'disconnect domain-a', 'after-disconnect-write',
                                        'follow-up-b', 'disconnect domain-b', 'cleanup complete'),
}

# SMMUv3 event numbers that each expected fault kind must carry (FC.2).
FAULT_CODES = {'no-mapping': 0x10, 'permission': 0x13, 'no-route': 0x02}
# Device result when the SMMU refused the write. Any other nonzero result is
# a fixture failure and never counts as a refused write.
RESULT_TX_FAIL = 0xDEAD0002
PROBE_LENGTH = 4
# Kernel log lines hold 127 characters. Longer records could be cut off.
MAX_RECORD_LENGTH = 119

RECORD = re.compile(r'SMMU-SELFTEST (START|IORT|STEP|FAULT|PASS|FAIL)\b(.*)$')
TRANSFER = re.compile(r'^ (\S+) rid=([0-9a-f]{4}) sid=0x([0-9a-f]+) iova=0x([0-9a-f]+) '
                      r'pa=0x([0-9a-f]+) len=(\d+) result=0x([0-9a-f]+)$')
POLL = re.compile(r'^ (\S+) poll status=(\d+) records=(\d+)$')
MEMORY = re.compile(r'^ (\S+) memory written=(\S+)(?: len=\d+)? guard=unchanged other-bytes=unchanged$')
EVENT = re.compile(r'^ ((?:connect|disconnect) domain-[ab]) sid=0x[0-9a-f]+$|^ (release \S+) iova=0x[0-9a-f]+$'
                   r'|^ (cleanup complete)$')
EXPECTED_FAULT = re.compile(r'^ expected (\S+) kind=(\S+) sid=0x([0-9a-f]+) iova=0x([0-9a-f]+) dir=(\S+) '
                            r'code=0x([0-9a-f]+)$')
UNIT = re.compile(r'^ unit index=\d+ base=0x([0-9a-f]+) idr0=0x([0-9a-f]+) ')
ROUTE = re.compile(r'^ route (\S+) rid=([0-9a-f]{4}) sid=0x([0-9a-f]+) unit=\d+ ')
DEVICE = re.compile(r'^ device rid=([0-9a-f]{4}) bar=0x([0-9a-f]+) pa=0x[0-9a-f]+$')
ENDPOINT_IDENTITY = re.compile(r'^ endpoint rid=([0-9a-f]{4}) id=1b36:0005 class=0x00ff00 header=0x00 functions=1$')
ENDPOINT_COMMAND = re.compile(r'^ endpoint rid=([0-9a-f]{4}) bar0=0x([0-9a-f]{8}) command=0x([0-9a-f]{4}) '
                              r'decode=on master=off$')
IORT_LENGTH = re.compile(r'^ length=(\d+) chunk=(\d+)$')
IORT_CHUNK = re.compile(r'^ offset=0x([0-9a-f]+) data=([0-9a-f]*)$')
PANIC = ('DebugPanic', 'ARM64 exception')

# SMMU_IDR0 bit 0 is S2P (stage 2) and bit 1 is S1P (stage 1).
IDR0_STAGE2 = 1 << 0
IDR0_STAGE1 = 1 << 1

# PCI command register bits: I/O decode, memory decode and bus mastering.
COMMAND_IO = 1 << 0
COMMAND_MEMORY = 1 << 1
COMMAND_BUS_MASTER = 1 << 2


class NotRun(Exception):
    """A prerequisite is missing. The case did not run."""


class Failed(Exception):
    """The case ran and failed, or its evidence is incomplete."""


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rebuild_iort(records):
    """Rebuild the IORT from its records. Every chunk must appear once and
    cover the declared length, and the ACPI length and checksum must match."""
    lengths = [IORT_LENGTH.match(text) for text in records]
    lengths = [match for match in lengths if match]
    if len(lengths) != 1:
        raise Failed(f'expected one IORT length record, found {len(lengths)}')
    length, chunk = int(lengths[0][1]), int(lengths[0][2])
    if chunk == 0 or length < 36:
        raise Failed(f'invalid IORT length {length} or chunk size {chunk}')
    chunks = {}
    for text in records:
        match = IORT_CHUNK.match(text)
        if not match:
            continue
        offset = int(match[1], 16)
        if offset in chunks:
            raise Failed(f'duplicate IORT chunk at offset {offset:#x}')
        chunks[offset] = bytes.fromhex(match[2])
    data = bytearray()
    for offset in range(0, length, chunk):
        expected = min(chunk, length - offset)
        if offset not in chunks:
            raise Failed(f'missing IORT chunk at offset {offset:#x}')
        if len(chunks[offset]) != expected:
            raise Failed(f'IORT chunk at offset {offset:#x} is not {expected} bytes')
        data += chunks.pop(offset)
    if chunks:
        raise Failed(f'IORT chunks outside the declared length: {sorted(chunks)}')
    if bytes(data[0:4]) != b'IORT' or int.from_bytes(data[4:8], 'little') != length:
        raise Failed('IORT signature or table length does not match the length record')
    if sum(data) % 256 != 0:
        raise Failed('IORT checksum is wrong')
    return bytes(data)


class Request:
    """One guest request and the records that belong to it."""

    def __init__(self, match):
        self.name = match[1]
        self.rid, self.sid = match[2], int(match[3], 16)
        self.iova, self.physical = int(match[4], 16), int(match[5], 16)
        self.length, self.result = int(match[6]), int(match[7], 16)
        self.poll = None
        self.memory = None
        self.faults = []


def collect_requests(body):
    """Walk the records between START and the terminal record in order.
    Returns the sequence of requests and events and the requests by name.
    Each request must be followed by its poll record, then its expected
    fault records, then its memory record, before anything else happens."""
    sequence, requests, current = [], {}, None

    def finish(next_item):
        if current is not None and current.memory is None:
            raise Failed(f'request {current.name} has no memory record before {next_item}')

    for kind, detail in body:
        if kind == 'FAULT':
            match = EXPECTED_FAULT.match(detail)
            if not match:
                raise Failed(f'malformed fault record:{detail}')
            if current is None or match[1] != current.name or current.poll is None or current.memory is not None:
                raise Failed(f'expected fault record outside its request:{detail}')
            current.faults.append(match)
            continue
        if kind != 'STEP':
            continue
        transfer, poll, memory, event = (TRANSFER.match(detail), POLL.match(detail), MEMORY.match(detail),
                                         EVENT.match(detail))
        if transfer:
            finish(transfer[1])
            if transfer[1] not in REQUESTS:
                raise Failed(f'unknown request {transfer[1]}')
            if transfer[1] in requests:
                raise Failed(f'request {transfer[1]} appears twice')
            current = requests[transfer[1]] = Request(transfer)
            sequence.append(transfer[1])
        elif poll or memory:
            match = poll or memory
            if current is None or match[1] != current.name:
                raise Failed(f'record outside its request:{detail}')
            if poll and current.poll is None:
                current.poll = (int(poll[2]), int(poll[3]))
            elif memory and current.poll is not None and current.memory is None:
                current.memory = memory[2]
            else:
                raise Failed(f'duplicate or misplaced record for {current.name}:{detail}')
        elif event:
            item = next(group for group in event.groups() if group)
            finish(item)
            current = None
            sequence.append(item)
    finish('the terminal record')
    return sequence, requests


def check_request(request, routes):
    """Check one request against its domain's route and its expected outcome."""
    domain, outcome = REQUESTS[request.name]
    if (request.rid, request.sid) != routes.get(domain):
        raise Failed(f'{request.name}: sent by rid {request.rid} sid {request.sid:#x}, expected {domain}')
    if request.poll is None or request.poll[0] != 0:
        raise Failed(f'{request.name}: Poll status {request.poll}, the unit must stay healthy')
    if request.poll[1] != len(request.faults):
        raise Failed(f'{request.name}: Poll returned {request.poll[1]} records, '
                     f'{len(request.faults)} printed as expected')
    if outcome == 'ok':
        if request.poll[1] != 0:
            raise Failed(f'transfer {request.name} has {request.poll[1]} fault records')
        if request.result != 0:
            raise Failed(f'transfer {request.name} returned {request.result:#x}')
        if request.iova == request.physical:
            raise Failed(f'transfer {request.name} used an identity address {request.iova:#x}')
        if request.memory == 'none':
            raise Failed(f'transfer {request.name} reports no written range')
        return
    if request.result != RESULT_TX_FAIL:
        raise Failed(f'probe {request.name} returned {request.result:#x}, expected {RESULT_TX_FAIL:#x}')
    if request.iova != request.physical or request.length != PROBE_LENGTH:
        raise Failed(f'probe {request.name} must be a {PROBE_LENGTH}-byte write with its IOVA as readback address')
    if request.memory != 'none':
        raise Failed(f'probe {request.name} changed {request.memory}')
    # An abort may produce no record at all. Otherwise exactly one record of
    # the expected kind must follow the probe.
    if outcome == 'abort' and not request.faults:
        return
    expected_kind = 'no-route' if outcome == 'abort' else outcome
    if len(request.faults) != 1:
        raise Failed(f'probe {request.name}: {len(request.faults)} expected fault records, need 1')
    fault = request.faults[0]
    kind, sid, iova, direction, code = fault[2], int(fault[3], 16), int(fault[4], 16), fault[5], int(fault[6], 16)
    if kind != expected_kind or sid != request.sid or code != FAULT_CODES[expected_kind]:
        raise Failed(f'probe {request.name}: fault {kind} sid {sid:#x} code {code:#x} does not match')
    if kind != 'no-route' and (iova != request.iova or direction != 'write'):
        raise Failed(f'probe {request.name}: fault at {iova:#x} dir {direction} does not match the probe')


def check_requests(case, steps, body):
    """The requests and events must follow the case's sequence, and each
    request must have the outcome its table entry names."""
    routes = {}
    for text in steps:
        match = ROUTE.match(text)
        if match:
            routes[match[1]] = (match[2], int(match[3], 16))
    sequence, requests = collect_requests(body)
    if tuple(sequence) != CASE_SEQUENCES[case]:
        raise Failed(f'steps {sequence} do not match {list(CASE_SEQUENCES[case])}')
    for request in requests.values():
        check_request(request, routes)


def check_endpoints(steps):
    """Each endpoint must have one identity record and one BAR0/command
    record from the adapter. BAR0 must match the device record, memory
    decode must be on, and I/O decode and bus mastering must be off."""
    bars = {match[1]: int(match[2], 16) for match in map(DEVICE.match, steps) if match}
    identities, commands = [], {}
    for text in steps:
        if not text.startswith(' endpoint '):
            continue
        identity, command = ENDPOINT_IDENTITY.match(text), ENDPOINT_COMMAND.match(text)
        if identity:
            identities.append(identity[1])
        elif command:
            if command[1] in commands:
                raise Failed(f'endpoint {command[1]} has two command records')
            commands[command[1]] = (int(command[2], 16), int(command[3], 16))
        else:
            raise Failed(f'unexpected endpoint record:{text}')
    if len(bars) != 2 or sorted(identities) != sorted(bars) or sorted(commands) != sorted(bars):
        raise Failed(f'endpoint records {identities} {sorted(commands)} do not match devices {sorted(bars)}')
    for rid, (bar, command) in commands.items():
        if bar != bars[rid]:
            raise Failed(f'endpoint {rid} reads BAR0 {bar:#x}, the device record has {bars[rid]:#x}')
        if command & (COMMAND_IO | COMMAND_MEMORY | COMMAND_BUS_MASTER) != COMMAND_MEMORY:
            raise Failed(f'endpoint {rid} command {command:#x} is not memory decode only')


def check_unit(steps):
    units = [UNIT.match(text) for text in steps]
    units = [match for match in units if match]
    if len(units) != 1:
        raise Failed(f'expected one unit profile record, found {len(units)}')
    idr0 = int(units[0][2], 16)
    if not idr0 & IDR0_STAGE2 or idr0 & IDR0_STAGE1:
        raise Failed(f'unit IDR0 {idr0:#x} is not the stage-2-only profile')


# IORT node types and layout (Arm DEN 0049). Every node starts with type (u8),
# length (u16), revision (u8), identifier (u32), mapping count (u32) and
# mapping offset (u32). An ID mapping is input base, ID count (number of IDs
# minus one), output base, output node offset and flags, all u32.
IORT_NODE_ROOT_COMPLEX = 2
IORT_NODE_SMMUV3 = 4
IORT_SMMUV3_BASE = 16


def resolve_iort(iort, requester):
    """Decode the stream ID and SMMUv3 base address that the IORT gives one
    PCI requester ID of the single root complex, independently of the
    kernel's decoder."""
    count, offset = struct.unpack_from('<II', iort, 36)
    nodes = {}
    for _ in range(count):
        kind, length = struct.unpack_from('<BH', iort, offset)
        nodes[offset] = kind
        if kind == IORT_NODE_ROOT_COMPLEX:
            root = offset
        offset += length
    mapping_count, mapping_offset = struct.unpack_from('<II', iort, root + 8)
    matches = []
    for index in range(mapping_count):
        base, ids, output, target, _ = struct.unpack_from('<IIIII', iort, root + mapping_offset + 20 * index)
        if base <= requester <= base + ids:
            matches.append((output + requester - base, target))
    if len(matches) != 1 or nodes.get(matches[0][1]) != IORT_NODE_SMMUV3:
        raise Failed(f'IORT does not route requester {requester:#06x} to one SMMUv3 node')
    stream, target = matches[0]
    return stream, struct.unpack_from('<Q', iort, target + IORT_SMMUV3_BASE)[0]


def check_routes(iort, steps):
    """The guest's resolved stream IDs and unit base must match the IORT."""
    base = int(UNIT.match([text for text in steps if UNIT.match(text)][0])[1], 16)
    routes = [ROUTE.match(text) for text in steps]
    routes = [match for match in routes if match]
    if len(routes) != 2:
        raise Failed(f'expected two route records, found {len(routes)}')
    for route in routes:
        requester, stream = int(route[2], 16), int(route[3], 16)
        try:
            expected = resolve_iort(iort, requester)
        except (struct.error, UnboundLocalError) as error:
            raise Failed(f'IORT layout could not be decoded: {error}')
        if expected != (stream, base):
            raise Failed(f'{route[1]}: guest route sid {stream:#x} at {base:#x}, IORT gives '
                         f'sid {expected[0]:#x} at {expected[1]:#x}')


def check_log(text, case):
    """Check one serial log. Returns (result, iort bytes).

    The log must hold exactly one START for this case, then exactly one
    terminal record for this case, and no self-test record outside them. A
    second START (an earlier boot in the same file), a record for another
    case, a panic, a record that may have been cut off or an unexpected fault
    all fail. PASS also needs the complete evidence: the IORT, the endpoint
    checks, the stage-2-only unit profile and every request and event of the
    case in order, each with its poll, fault and memory records."""
    records = []
    for line in text.splitlines():
        match = RECORD.search(line)
        if match:
            if len(line) - match.start() > MAX_RECORD_LENGTH:
                raise Failed(f'record longer than {MAX_RECORD_LENGTH} characters: {line[match.start():]}')
            records.append((match[1], match[2]))
        elif any(marker in line for marker in PANIC):
            raise Failed(f'kernel panic in serial log: {line.strip()}')
    starts = [index for index, (kind, _) in enumerate(records) if kind == 'START']
    terminals = [index for index, (kind, _) in enumerate(records) if kind in ('PASS', 'FAIL')]
    if len(starts) != 1:
        raise Failed(f'expected one START record, found {len(starts)}')
    if records[starts[0]][1].strip() != f'case={case}':
        raise Failed(f'START record is for another case: {records[starts[0]][1].strip()}')
    if len(terminals) != 1:
        raise Failed(f'expected one terminal record, found {len(terminals)}')
    if terminals[0] < starts[0]:
        raise Failed('terminal record appears before START')
    kind, detail = records[terminals[0]]
    if not detail.strip().startswith(f'case={case}'):
        raise Failed(f'terminal record is for another case: {detail.strip()}')
    faults = [detail for kind, detail in records if kind == 'FAULT' and not detail.startswith(' expected ')]
    if faults:
        raise Failed(f'unexpected fault records: {faults}')
    if kind == 'FAIL':
        raise Failed(f'guest reported FAIL:{detail}')
    if starts[0] != 0 or terminals[0] != len(records) - 1:
        raise Failed('self-test records appear before START or after the terminal record')
    body = records[starts[0] + 1:terminals[0]]
    iort = rebuild_iort([detail for kind, detail in body if kind == 'IORT'])
    steps = [detail for kind, detail in body if kind == 'STEP']
    check_endpoints(steps)
    check_unit(steps)
    check_routes(iort, steps)
    check_requests(case, steps, body)
    return 'PASS', iort


class QMP:
    """Minimal QMP client: one command at a time, events are skipped."""

    def __init__(self, path, deadline):
        self.socket = None
        while self.socket is None:
            try:
                connection = socket.socket(socket.AF_UNIX)
                connection.connect(str(path))
                self.socket = connection
            except OSError:
                if time.monotonic() > deadline:
                    raise Failed(f'QMP socket {path} did not open')
                time.sleep(0.1)
        self.socket.settimeout(10)
        self.stream = self.socket.makefile('rw')
        greeting = json.loads(self.stream.readline())
        self.version = greeting['QMP']['version']['qemu']
        self.command('qmp_capabilities')

    def command(self, name, **arguments):
        request = {'execute': name}
        if arguments:
            request['arguments'] = arguments
        self.stream.write(json.dumps(request) + '\n')
        self.stream.flush()
        while True:
            reply = json.loads(self.stream.readline())
            if 'error' in reply:
                raise Failed(f'QMP {name} failed: {reply["error"]}')
            if 'return' in reply:
                return reply['return']

    def close(self):
        self.socket.close()


def check_topology(qmp, log):
    """The paused VM must hold the host bridge and exactly the two test
    devices on bus 0, the SMMU with stage 2, and no IOMMU from the machine."""
    buses = qmp.command('query-pci')
    log.append(json.dumps(buses))
    if len(buses) != 1 or buses[0]['bus'] != 0:
        raise Failed(f'expected one PCI bus, found {[bus["bus"] for bus in buses]}')
    devices = {(device['slot'], device['function']): device for device in buses[0]['devices']}
    if sorted(devices) != [(0, 0), (1, 0), (2, 0)]:
        raise Failed(f'unexpected PCI functions {sorted(devices)}')
    host = devices[(0, 0)]['id']
    if (host['vendor'], host['device']) != (0x1B36, 0x0008):
        raise Failed(f'00:00.0 is not the QEMU host bridge: {host}')
    for slot, identifier in ((1, 'itd0'), (2, 'itd1')):
        device = devices[(slot, 0)]
        regions = device['regions']
        if (device['qdev_id'] != identifier or (device['id']['vendor'], device['id']['device']) != (0x1B36, 0x0005)
                or len(regions) != 1 or regions[0]['bar'] != 0 or regions[0]['size'] != 4096
                or regions[0]['type'] != 'memory' or regions[0]['mem_type_64'] or regions[0]['prefetch']):
            raise Failed(f'00:{slot:02x}.0 is not the expected test device: {device}')
        kind = qmp.command('qom-get', path=f'/machine/peripheral/{identifier}', property='type')
        log.append(f'{identifier} type={kind}')
        if kind != 'iommu-testdev':
            raise Failed(f'{identifier} has QOM type {kind}')
    for name, expected in (('type', 'arm-smmuv3'), ('stage', '2')):
        value = qmp.command('qom-get', path='/machine/peripheral/smmu0', property=name)
        log.append(f'smmu0 {name}={value}')
        if value != expected:
            raise Failed(f'smmu0 {name} is {value}, expected {expected}')
    machine = qmp.command('qom-get', path='/machine', property='iommu')
    log.append(f'machine iommu={machine}')
    if machine != 'none':
        raise Failed(f'machine iommu is {machine}, expected none')


def read_build(build, case):
    """Read the build settings. The build must already be configured for
    this case, since the runner never changes it."""
    cache = build / 'CMakeCache.txt'
    kernel = build / 'bin/kernel.mos'
    if not cache.is_file() or not kernel.is_file():
        raise NotRun(f'{build} has no CMakeCache.txt or bin/kernel.mos')
    settings = {}
    for line in cache.read_text().splitlines():
        match = re.match(r'^(VALI_[A-Z0-9_]+|CMAKE_BUILD_TYPE):[A-Z]+=(.*)$', line)
        if match:
            settings[match[1]] = match[2]
    expected = {'VALI_ARCH': 'aarch64', 'VALI_PLATFORM': 'generic',
                'VALI_ARM64_SMMU_SELFTEST': 'ON', 'VALI_ARM64_SMMU_SELFTEST_CASE': case}
    for name, value in expected.items():
        if settings.get(name) != value:
            raise Failed(f'build {name} is {settings.get(name)!r}, expected {value!r}')
    return settings


def check_prerequisites(arguments):
    for name in ('loader', 'phoenix', 'firmware', 'lzss', 'mkvafs'):
        if not getattr(arguments, name).is_file():
            raise NotRun(f'--{name} {getattr(arguments, name)} does not exist')
    if shutil.which(arguments.qemu) is None:
        raise NotRun(f'{arguments.qemu} is not installed')
    version = subprocess.run([arguments.qemu, '--version'], capture_output=True, text=True, check=True).stdout
    if f'version {QEMU_VERSION}' not in version:
        raise NotRun(f'QEMU {QEMU_VERSION} is required, found: {version.splitlines()[0]}')
    return version


def build_disk(arguments, directory):
    """Build the read-only ESP: loader, compressed kernel, Phoenix and an
    almost empty ramdisk. Userspace never starts in this build mode."""
    esp = directory / 'esp'
    shutil.rmtree(esp, ignore_errors=True)
    (esp / 'EFI/BOOT').mkdir(parents=True)
    (esp / 'EFI/VALI').mkdir(parents=True)
    shutil.copy2(arguments.loader, esp / 'EFI/BOOT/BOOTAA64.EFI')
    shutil.copy2(arguments.phoenix, esp / 'EFI/VALI/phoenix.mos')
    ramdisk = directory / 'ramdisk'
    shutil.rmtree(ramdisk, ignore_errors=True)
    (ramdisk / 'services').mkdir(parents=True)
    (ramdisk / 'README').write_text('Native SMMUv3 self-test fixture.\n')
    subprocess.run([str(arguments.mkvafs.resolve()), '--arch', 'arm64', '--out',
                    str(esp / 'EFI/VALI/initrd.mos'), str(ramdisk)], check=True, capture_output=True)
    subprocess.run([str(arguments.lzss.resolve()), 'c', str(arguments.build / 'bin/kernel.mos'),
                    str(esp / 'EFI/VALI/kernel.mos')], check=True, capture_output=True)
    return esp


def qemu_command(arguments, esp, serial, qmp_path):
    # The SMMU is created with -device so that its stage property applies.
    # With the machine option iommu=smmuv3, QEMU 11.1.3 forces stage=nested.
    # The firmware disk uses virtio-mmio, so no firmware storage function
    # sits on the PCI root behind the SMMU.
    return [arguments.qemu, '-M', MACHINE, '-cpu', 'max', '-smp', '4', '-m', '1024',
            '-nodefaults', '-display', 'none', '-no-reboot', '-S',
            '-bios', str(arguments.firmware.resolve()),
            '-device', 'arm-smmuv3,id=smmu0,primary-bus=pcie.0,stage=2',
            '-device', 'iommu-testdev,id=itd0,bus=pcie.0,addr=01.0',
            '-device', 'iommu-testdev,id=itd1,bus=pcie.0,addr=02.0',
            '-drive', f'if=none,id=esp,format=raw,file=fat:{esp},readonly=on',
            '-device', 'virtio-blk-device,drive=esp',
            '-serial', f'file:{serial}', '-monitor', 'none',
            '-qmp', f'unix:{qmp_path},server=on,wait=off']


def wait_for_terminal(process, serial, deadline):
    """Wait until the serial log has a terminal record, QEMU exits or the
    deadline passes. The log is checked fully afterwards."""
    while time.monotonic() < deadline:
        if serial.exists() and re.search(r'SMMU-SELFTEST (PASS|FAIL)', serial.read_text(errors='replace')):
            # Let the final log line reach the file.
            time.sleep(0.5)
            return
        if process.poll() is not None:
            return
        time.sleep(0.5)
    raise Failed(f'no terminal record within {DEADLINE_SECONDS} seconds')


def stop_qemu(process, qmp):
    if qmp is not None:
        try:
            qmp.command('quit')
        except (Failed, OSError, ValueError):
            pass
        qmp.close()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def record_inputs(arguments, directory, version, settings, command):
    shutil.copy2(arguments.build / 'bin/kernel.mos', directory / 'kernel.mos')
    if (arguments.build / 'bin/kernel.map').is_file():
        shutil.copy2(arguments.build / 'bin/kernel.map', directory / 'kernel.map')
    (directory / 'build-settings.txt').write_text(
        ''.join(f'{name}={value}\n' for name, value in sorted(settings.items())))
    (directory / 'qemu-version.txt').write_text(version)
    (directory / 'qemu-command.txt').write_text(' '.join(command) + '\n')
    hashes = {name: sha256(getattr(arguments, name)) for name in ('loader', 'phoenix', 'firmware')}
    hashes['kernel'] = sha256(arguments.build / 'bin/kernel.mos')
    hashes['qemu'] = sha256(shutil.which(arguments.qemu))
    (directory / 'hashes.txt').write_text(''.join(f'{name} {value}\n' for name, value in sorted(hashes.items())))


def run_case(arguments):
    settings = read_build(arguments.build, arguments.case)
    version = check_prerequisites(arguments)
    directory = arguments.output_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    serial = directory / 'serial.log'
    stderr_path = directory / 'qemu-stderr.log'
    # Remove old results first, so a marker from an earlier boot cannot be read.
    for stale in (serial, stderr_path, directory / 'iort.bin', directory / 'result.txt'):
        stale.unlink(missing_ok=True)
    esp = build_disk(arguments, directory)
    socket_directory = Path(tempfile.mkdtemp(prefix='vali-smmu-qmp-'))
    qmp_path = socket_directory / 'qmp.sock'
    command = qemu_command(arguments, esp, serial, qmp_path)
    record_inputs(arguments, directory, version, settings, command)
    topology = []
    qmp = None
    deadline = time.monotonic() + DEADLINE_SECONDS
    with stderr_path.open('w') as stderr:
        process = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=stderr)
        try:
            qmp = QMP(qmp_path, deadline)
            topology.append(f'qemu {qmp.version}')
            check_topology(qmp, topology)
            qmp.command('cont')
            wait_for_terminal(process, serial, deadline)
        finally:
            (directory / 'qmp-topology.txt').write_text('\n'.join(topology) + '\n')
            stop_qemu(process, qmp)
            shutil.rmtree(socket_directory, ignore_errors=True)
    if not serial.exists():
        raise Failed('QEMU produced no serial log')
    result, iort = check_log(serial.read_text(errors='replace'), arguments.case)
    (directory / 'iort.bin').write_bytes(iort)
    (directory / 'iort.sha256').write_text(hashlib.sha256(iort).hexdigest() + '  iort.bin\n')
    return result


# Sample addresses for the self-check logs: the guard (IOVA range) at
# 0x7fc00000 and the data pages behind it, as in the QEMU fixture.
SAMPLE_SIDS = {'domain-a': ('0008', 0x8), 'domain-b': ('0010', 0x10)}
SAMPLE_GUARD = 0x7FC00000
SAMPLE_DATA = 0x7FC04000


def sample_iort():
    """A small IORT: one SMMUv3 node at 0xc000000 and one root complex that
    maps requester IDs 0-255 to the same stream IDs on it."""
    smmu = struct.pack('<BHBIII', IORT_NODE_SMMUV3, 24, 4, 1, 0, 0) + struct.pack('<Q', 0xC000000)
    root = (struct.pack('<BHBIII', IORT_NODE_ROOT_COMPLEX, 56, 3, 2, 1, 36) + bytes(20)
            + struct.pack('<IIIII', 0, 255, 0, 48, 0))
    table = bytearray(b'IORT' + (48 + len(smmu) + len(root)).to_bytes(4, 'little') + bytes(28)
                      + struct.pack('<III', 2, 48, 0) + smmu + root)
    table[9] = (256 - sum(table) % 256) % 256
    return bytes(table)


def sample_request(name, index):
    """The records a passing guest prints for one request."""
    domain, outcome = REQUESTS[name]
    rid, sid = SAMPLE_SIDS[domain]
    iova = SAMPLE_GUARD + 0x100 * (index + 1)
    if outcome == 'ok':
        lines = [f'STEP {name} rid={rid} sid={sid:#x} iova={iova:#x} pa={iova + SAMPLE_DATA - SAMPLE_GUARD:#x} '
                 f'len=64 result=0x0', f'STEP {name} poll status=0 records=0',
                 f'STEP {name} memory written=data-a+0x100 len=64 guard=unchanged other-bytes=unchanged']
    else:
        records = 0 if outcome == 'abort' else 1
        lines = [f'STEP {name} rid={rid} sid={sid:#x} iova={iova:#x} pa={iova:#x} len=4 result=0xdead0002',
                 f'STEP {name} poll status=0 records={records}']
        if records:
            lines.append(f'FAULT expected {name} kind={outcome} sid={sid:#x} iova={iova:#x} dir=write '
                         f'code={FAULT_CODES[outcome]:#x}')
        lines.append(f'STEP {name} memory written=none guard=unchanged other-bytes=unchanged')
    return lines


def sample_log(case):
    """A log that a passing guest of this case prints."""
    table = sample_iort()
    lines = [f'START case={case}', f'IORT length={len(table)} chunk=32']
    lines += [f'IORT offset=0x{offset:05x} data={table[offset:offset + 32].hex()}'
              for offset in range(0, len(table), 32)]
    for index, (rid, _) in enumerate(SAMPLE_SIDS.values()):
        lines += [f'STEP endpoint rid={rid} id=1b36:0005 class=0x00ff00 header=0x00 functions=1',
                  f'STEP endpoint rid={rid} bar0=0x1000{index}000 command=0x0002 decode=on master=off']
    for index, (rid, _) in enumerate(SAMPLE_SIDS.values()):
        lines.append(f'STEP device rid={rid} bar=0x1000{index}000 pa=0x1000{index}000')
    for domain, (rid, sid) in SAMPLE_SIDS.items():
        lines.append(f'STEP route {domain} rid={rid} sid={sid:#x} unit=0 limit=0xffffffffffffffff')
    lines.append('STEP unit index=0 base=0xc000000 idr0=0xd441019 idr1=0x0 idr5=0x0 aidr=0x1')
    for index, item in enumerate(CASE_SEQUENCES[case]):
        if item in REQUESTS:
            lines += sample_request(item, index)
        elif item.startswith('release '):
            lines.append(f'STEP {item} iova={SAMPLE_GUARD:#x}')
        elif item == 'cleanup complete':
            lines.append(f'STEP {item}')
        else:
            lines.append(f'STEP {item} sid={SAMPLE_SIDS[item.split()[1]][1]:#x}')
    lines.append(f'PASS case={case}')
    return ['SMMU-SELFTEST ' + line for line in lines]


def replace(lines, marker, old, new):
    """Replace old with new in every line that contains marker."""
    return [line.replace(old, new) if marker in line else line for line in lines]


def without(lines, marker):
    """Drop every line that contains marker."""
    return [line for line in lines if marker not in line]


def insert_after(lines, marker, new_lines):
    """Insert new_lines after the first line that contains marker."""
    index = next(index for index, line in enumerate(lines) if marker in line)
    return lines[:index + 1] + new_lines + lines[index + 1:]


def move_after(lines, moved, marker):
    """Move the first line that contains moved to after the first line that
    contains marker."""
    line = next(line for line in lines if moved in line)
    rest = [other for other in lines if other is not line]
    return insert_after(rest, marker, [line])


def self_check():
    """Feed built-in logs to check_log. The sample log of every case must
    pass, and each broken variant must be rejected for the expected reason."""
    table = sample_iort()
    for case in CASES:
        result, data = check_log('\n'.join(sample_log(case)), case)
        assert result == 'PASS' and data == table, f'good {case} log rejected'

    # A NoRoute record is the one record an abort case may also accept.
    unknown = sample_log('unknown-stream')
    no_route_record = ('SMMU-SELFTEST FAULT expected unconnected-write kind=no-route sid=0x10 iova=0x0 '
                       'dir=none code=0x2')
    no_route = insert_after(replace(unknown, 'unconnected-write poll', 'records=0', 'records=1'),
                            'unconnected-write poll', [no_route_record])
    assert check_log('\n'.join(no_route), 'unknown-stream')[0] == 'PASS', 'no-route record rejected'

    good = sample_log('positive')
    second = [line for line in good if ' IORT offset=' in line][1]
    denied = sample_log('write-denied')
    fault = next(line for line in denied if 'FAULT expected' in line)
    reconnect = [index for index, line in enumerate(good) if 'STEP connect domain-a' in line][1]
    unmapped_fault = ('SMMU-SELFTEST FAULT expected unconnected-write kind=no-mapping sid=0x10 '
                      'iova=0x7fc00400 dir=write code=0x10')
    variants = {
        # Terminal records, boots and cases.
        'wrong case': ('positive', [line.replace('case=positive', 'case=unmapped') for line in good]),
        'missing terminal': ('positive', good[:-1]),
        'stale success marker': ('positive', ['SMMU-SELFTEST PASS case=positive'] + good[:-1]),
        'earlier boot in the same log': ('positive', good + good),
        'guest FAIL': ('positive', good[:-1] + ['SMMU-SELFTEST FAIL case=positive step=x status=1']),
        'unexpected fault': ('positive', insert_after(good, 'cleanup complete',
                                                      ['SMMU-SELFTEST FAULT unexpected kind=no-mapping'])),
        'panic': ('positive', insert_after(good, 'cleanup complete', ['[error-1-8] DebugPanic(Scope 1)'])),
        'record after the terminal record': ('positive', good + ['SMMU-SELFTEST STEP cleanup complete']),
        'record longer than the log line': ('positive', insert_after(good, 'cleanup complete',
                                                                     ['SMMU-SELFTEST STEP ' + 'x' * 110])),
        # IORT, routes and unit.
        'missing IORT chunk': ('positive', [line for line in good if line != second]),
        'duplicate IORT chunk': ('positive', insert_after(good, second, [second])),
        'bad IORT checksum': ('positive', [line[:-2] + ('00' if line[-2:] != '00' else '01') if line == second
                                           else line for line in good]),
        'route that disagrees with the IORT': ('positive', replace(good, 'route domain-b', 'sid=0x10', 'sid=0x11')),
        'unit base that disagrees with the IORT': ('positive', replace(good, 'unit index', 'base=0xc000000',
                                                                       'base=0xd000000')),
        'missing route': ('positive', without(good, 'route domain-b')),
        'stage 1 profile': ('positive', replace(good, 'unit index', 'idr0=0xd441019', 'idr0=0xd44101b')),
        # Endpoint checks.
        'missing endpoint identity': ('positive', without(good, 'endpoint rid=0010 id=')),
        'endpoint with bus mastering on': ('positive', replace(good, 'endpoint rid=0008 bar0', 'command=0x0002',
                                                               'command=0x0006')),
        'endpoint BAR0 that disagrees with the device': ('positive', replace(good, 'endpoint rid=0010 bar0',
                                                                             'bar0=0x10001000', 'bar0=0x10002000')),
        'endpoint with another identity': ('positive', replace(good, 'endpoint rid=0008 id=', 'class=0x00ff00',
                                                               'class=0x010000')),
        # Transfers that must succeed.
        'identity transfer': ('positive', replace(good, 'STEP write-only rid', 'pa=0x7fc04300', 'pa=0x7fc00300')),
        'refused transfer': ('positive', replace(good, 'page-boundary rid', 'result=0x0', 'result=0xdead0002')),
        'missing transfer': ('positive', without(good, 'same-iova-b')),
        'missing cleanup record': ('positive', without(good, 'cleanup complete')),
        'missing reconnect': ('positive', good[:reconnect] + good[reconnect + 1:]),
        'transfer without its poll record': ('positive', without(good, 'write-only poll')),
        'transfer without its memory record': ('positive', without(good, 'write-only memory')),
        'transfer with a fault record': ('positive', replace(good, 'write-only poll', 'records=0', 'records=1')),
        'transfer with an expected fault record': ('positive', insert_after(
            replace(good, 'write-only poll', 'records=0', 'records=1'), 'write-only poll',
            ['SMMU-SELFTEST FAULT expected write-only kind=no-mapping sid=0x8 iova=0x7fc00300 dir=write code=0x10'])),
        'Poll that reports a failed unit': ('positive', replace(good, 'same-iova-a poll', 'status=0', 'status=19')),
        # Expected fault records of the fault cases.
        'missing expected fault record': ('write-denied', without(denied, 'FAULT expected')),
        'duplicated expected fault record': ('write-denied', insert_after(denied, 'FAULT expected', [fault])),
        'expected fault record of another case': ('write-denied', replace(denied, 'FAULT expected',
                                                                          'denied-write', 'unmapped-write')),
        'expected fault record from an earlier boot': ('write-denied', [fault] + denied),
        'expected fault record after the memory check': ('write-denied', move_after(denied, 'FAULT expected',
                                                                                    'denied-write memory')),
        'fault of the wrong kind': ('write-denied', replace(replace(denied, 'FAULT expected', 'kind=permission',
                                                                    'kind=no-mapping'),
                                                            'FAULT expected', 'code=0x13', 'code=0x10')),
        'fault with another event code': ('write-denied', replace(denied, 'FAULT expected', 'code=0x13',
                                                                  'code=0x12')),
        'fault from the other stream': ('write-denied', replace(denied, 'FAULT expected', 'sid=0x10', 'sid=0x8')),
        'fault at another address': ('write-denied', replace(denied, 'FAULT expected', 'iova=0x', 'iova=0x1')),
        'fault for a read': ('write-denied', replace(denied, 'FAULT expected', 'dir=write', 'dir=read')),
        'Poll count that disagrees with the records': ('write-denied', replace(denied, 'denied-write poll',
                                                                               'records=1', 'records=2')),
        'unexpected fault during a probe': ('write-denied', replace(denied, 'FAULT expected', 'expected',
                                                                    'unexpected')),
        # Probe results that are not a refused write.
        'probe that reached memory': ('write-denied', replace(denied, 'denied-write rid', 'result=0xdead0002',
                                                              'result=0x0')),
        'probe that failed its readback': ('write-denied', replace(denied, 'denied-write rid', 'result=0xdead0002',
                                                                   'result=0xdead0003')),
        'probe with a bad length result': ('write-denied', replace(denied, 'denied-write rid', 'result=0xdead0002',
                                                                   'result=0xdead0001')),
        'probe longer than 4 bytes': ('write-denied', replace(denied, 'denied-write rid', 'len=4', 'len=64')),
        'probe without its identity readback address': ('write-denied', replace(denied, 'denied-write rid',
                                                                                'pa=0x7fc00', 'pa=0x7fc04')),
        'probe that changed memory': ('write-denied', replace(denied, 'denied-write memory', 'written=none',
                                                              'written=read-only+0x100')),
        'probe from the wrong endpoint': ('write-denied', replace(denied, 'denied-write rid',
                                                                  'rid=0010 sid=0x10', 'rid=0008 sid=0x8')),
        # Control and follow-up transfers.
        'missing control transfer': ('write-denied', without(denied, 'control-b')),
        'missing follow-up of the faulting endpoint': ('write-denied', without(denied, 'follow-up-b')),
        'missing follow-up of the peer': ('write-denied', without(denied, 'follow-up-a')),
        'log of another case': ('write-denied', sample_log('unmapped')),
        'steps of another case': ('write-denied', [line.replace('case=unmapped', 'case=write-denied')
                                                   for line in sample_log('unmapped')]),
        # Abort cases and the case-specific steps.
        'abort case with a translation fault': ('unknown-stream', insert_after(
            replace(unknown, 'unconnected-write poll', 'records=0', 'records=1'),
            'unconnected-write poll', [unmapped_fault])),
        'abort case with two records': ('unknown-stream', insert_after(
            replace(no_route, 'unconnected-write poll', 'records=1', 'records=2'),
            'FAULT expected', [no_route_record])),
        'unknown stream that was connected': ('unknown-stream', insert_after(
            unknown, 'connect domain-a', ['SMMU-SELFTEST STEP connect domain-b sid=0x10'])),
        'after-unmap without the release': ('after-unmap', without(sample_log('after-unmap'), 'release ')),
        'disconnect after the probe': ('after-disconnect', move_after(sample_log('after-disconnect'),
                                                                      'STEP disconnect domain-a',
                                                                      'after-disconnect-write memory')),
    }
    for name, (case, lines) in variants.items():
        try:
            check_log('\n'.join(lines), case)
        except Failed as error:
            print(f'Rejected {name}: {error}', flush=True)
            continue
        raise AssertionError(f'log check accepted: {name}')
    print(f'boot_smmuv3 self-check PASS: {len(CASES)} good logs, {len(variants)} broken logs rejected', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--self-check', action='store_true', help='only test the log checks')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--case', default='positive')
    parser.add_argument('--loader', type=Path)
    parser.add_argument('--phoenix', type=Path)
    parser.add_argument('--firmware', type=Path)
    parser.add_argument('--lzss', type=Path)
    parser.add_argument('--mkvafs', type=Path)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--qemu', default='qemu-system-aarch64')
    arguments = parser.parse_args()
    if arguments.self_check:
        self_check()
        return 0
    missing = [name for name in ('build', 'loader', 'phoenix', 'firmware', 'lzss', 'mkvafs', 'output_dir')
               if getattr(arguments, name) is None]
    if missing:
        parser.error('missing ' + ', '.join('--' + name.replace('_', '-') for name in missing))
    if arguments.case not in CASES:
        parser.error(f'unknown case {arguments.case!r}, known: {", ".join(CASES)}')
    try:
        result = run_case(arguments)
        code = 0
    except NotRun as error:
        result, code = f'NOT RUN: {error}', 77
    except Failed as error:
        result, code = f'FAIL: {error}', 1
    print(f'SMMU self-test {arguments.case}: {result}', flush=True)
    if arguments.output_dir is not None and arguments.output_dir.is_dir():
        (arguments.output_dir / 'result.txt').write_text(result + '\n')
    return code


if __name__ == '__main__':
    sys.exit(main())
