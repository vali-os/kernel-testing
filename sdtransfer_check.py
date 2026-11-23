"""Check the test file of the SD transfer test on a raw card, without a file system.

The target program (testing/target/sdtransfer) writes a file in which every
512-byte sector carries a marker, the run identifier, the pass number, its own
sector index within the file and a CRC-32 over the sector. The layout is
described in testing/target/sdtransfer/pattern.h, and this script reads its
TRANSFER_PATTERN_ values from that header, so both always agree.

The input is a raw card image (from a QEMU run) or a block device (a real card
in a card reader, for example /dev/sdX or /dev/mmcblk0). It is opened
read-only and never written. Reading a block device usually needs root:

    sudo python3 testing/sdtransfer_check.py /dev/sdX --partition vali-data

The script reads every sector of the input (or of one GPT partition) and
collects the pattern sectors. A sector counts as a pattern sector when it
starts with the two marker words. For each one it checks the CRC-32 and the
fill words. The run identifier and pass come from the program's log line
"file ... left with run 0x<run> pass <p>". When they are not given, the
script takes the only run on the card and its highest pass.

For that run and pass the check passes when every sector index of the file,
0 to the sector count minus 1, is present exactly once with a valid CRC-32
and the expected fill, and every sector agrees on the sector count. Sectors of
earlier passes and runs may remain in free space. They are counted but do not
fail the check. A sector with the marker and a bad CRC-32 or fill that names
the checked run and pass fails it.

The exit code is 0 when the check passes, 1 when it fails, and 2 when the
input cannot be read or the run cannot be chosen. --json writes the full
result.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import sys
import zlib

import sdhci_card

ROOT = Path(__file__).resolve().parents[1]
PATTERN_HEADER = ROOT / 'testing/target/sdtransfer/pattern.h'

# Bytes read from the input at a time. A whole number of sectors.
READ_CHUNK = 8 * 1024 * 1024

# How many missing, duplicate or corrupt entries the text summary lists.
SHOW_LIMIT = 20


def read_layout(header=PATTERN_HEADER):
    """The TRANSFER_PATTERN_ values of pattern.h, by the name after the prefix."""
    values = {}
    for match in re.finditer(r'#define\s+TRANSFER_PATTERN_(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b', header.read_text()):
        values[match[1]] = int(match[2], 0)
    needed = ('SECTOR_SIZE', 'MARKER_0', 'MARKER_1', 'OFFSET_MARKER_0', 'OFFSET_MARKER_1', 'OFFSET_RUN',
              'OFFSET_PASS', 'OFFSET_INDEX', 'OFFSET_COUNT', 'OFFSET_REQUEST', 'OFFSET_FILL', 'FILL_WORDS',
              'OFFSET_CHECKSUM', 'SEED_PASS', 'SEED_INDEX', 'CRC_POLYNOMIAL')
    missing = [name for name in needed if name not in values]
    if missing:
        raise SystemExit(f'{header} lacks {missing}')
    return values


class Pattern:
    """Builds and decodes pattern sectors with the layout of pattern.h."""

    def __init__(self, layout):
        self.layout = layout
        self.size = layout['SECTOR_SIZE']
        self.marker = struct.pack('<II', layout['MARKER_0'], layout['MARKER_1'])
        if layout['OFFSET_MARKER_1'] != layout['OFFSET_MARKER_0'] + 4:
            raise SystemExit('pattern.h: the marker words must follow each other')
        # zlib's CRC-32 is the one pattern.h describes. Check the polynomial,
        # so a change in the header cannot go unnoticed.
        if layout['CRC_POLYNOMIAL'] != 0xEDB88320:
            raise SystemExit('pattern.h names another CRC polynomial than zlib uses')

    def fill(self, run, pass_number, index):
        """The fill words of a sector, as bytes."""
        seed = (run ^ ((pass_number * self.layout['SEED_PASS']) & 0xFFFFFFFF)
                ^ ((index * self.layout['SEED_INDEX']) & 0xFFFFFFFF)) & 0xFFFFFFFF
        value = seed or 1
        words = []
        for _ in range(self.layout['FILL_WORDS']):
            words.append(value)
            value ^= (value << 13) & 0xFFFFFFFF
            value ^= value >> 17
            value ^= (value << 5) & 0xFFFFFFFF
        return struct.pack(f'<{len(words)}I', *words)

    def build(self, run, pass_number, index, count, request):
        """One whole pattern sector, as the target program writes it."""
        layout = self.layout
        data = bytearray(self.size)
        data[layout['OFFSET_MARKER_0']:layout['OFFSET_MARKER_0'] + 8] = self.marker
        for name, value in (('RUN', run), ('PASS', pass_number), ('INDEX', index), ('COUNT', count),
                            ('REQUEST', request)):
            struct.pack_into('<I', data, layout['OFFSET_' + name], value)
        fill = self.fill(run, pass_number, index)
        data[layout['OFFSET_FILL']:layout['OFFSET_FILL'] + len(fill)] = fill
        struct.pack_into('<I', data, layout['OFFSET_CHECKSUM'], zlib.crc32(bytes(data[:layout['OFFSET_CHECKSUM']])))
        return bytes(data)

    def decode(self, data):
        """The header words of a sector and whether its CRC-32 and fill hold."""
        layout = self.layout
        fields = {name.lower(): struct.unpack_from('<I', data, layout['OFFSET_' + name])[0]
                  for name in ('RUN', 'PASS', 'INDEX', 'COUNT', 'REQUEST', 'CHECKSUM')}
        fields['checksum_valid'] = zlib.crc32(data[:layout['OFFSET_CHECKSUM']]) == fields['checksum']
        fill = self.fill(fields['run'], fields['pass'], fields['index'])
        fields['fill_valid'] = data[layout['OFFSET_FILL']:layout['OFFSET_FILL'] + len(fill)] == fill
        return fields


def scan_range(path, pattern, first_sector=0, sector_count=None):
    """Every sector in the range that starts with the marker, as
    (disk sector, decoded fields). The input is opened read-only."""
    found = []
    size = pattern.size
    with open(path, 'rb') as source:
        source.seek(0, 2)
        total = source.tell() // size
        last = total if sector_count is None else min(total, first_sector + sector_count)
        sector = first_sector
        source.seek(sector * size)
        while sector < last:
            chunk = source.read(min(READ_CHUNK, (last - sector) * size))
            if not chunk:
                break
            position = chunk.find(pattern.marker)
            while position != -1:
                # Only a marker at the start of a sector counts.
                if position % size == 0 and position + size <= len(chunk):
                    found.append((sector + position // size, pattern.decode(chunk[position:position + size])))
                    position = chunk.find(pattern.marker, position + size - (position % size))
                else:
                    position = chunk.find(pattern.marker, position + 1)
            sector += len(chunk) // size
    return found, total


def partition_range(path, name):
    """First sector and sector count of the GPT partition with this name."""
    with open(path, 'rb') as source:
        head = source.read(64 * 1024)
    for partition in sdhci_card.read_gpt(head)['partitions']:
        if partition['name'] == name:
            return partition['first'], partition['last'] - partition['first'] + 1
    raise SystemExit(f'{path} has no GPT partition named {name}')


def ranges(numbers):
    """Sorted numbers as [first, last] runs, for short reports."""
    runs = []
    for number in sorted(numbers):
        if runs and number == runs[-1][1] + 1:
            runs[-1][1] = number
        else:
            runs.append([number, number])
    return runs


def check(found, run=None, pass_number=None):
    """Check the sectors of one run and pass. Returns the result dictionary."""
    runs = {}
    for _, fields in found:
        if fields['checksum_valid'] and fields['fill_valid']:
            runs.setdefault(fields['run'], set()).add(fields['pass'])
    result = {'runs_found': {f'0x{key:08x}': sorted(value) for key, value in sorted(runs.items())},
              'pattern_sectors': len(found)}
    if run is None:
        if len(runs) != 1:
            result['error'] = ('no valid pattern sector found' if not runs else
                               'more than one run on the input, choose one with --run')
            return result
        run = next(iter(runs))
    if pass_number is None:
        if run not in runs:
            result['error'] = f'run 0x{run:08x} has no valid pattern sector'
            return result
        pass_number = max(runs[run])
    result.update({'run': f'0x{run:08x}', 'pass': pass_number})

    positions = {}
    corrupt = []
    counts = set()
    requests = set()
    other_pass = 0
    other_run = 0
    for disk_sector, fields in found:
        valid = fields['checksum_valid'] and fields['fill_valid']
        if fields['run'] != run:
            other_run += 1
            continue
        if fields['pass'] != pass_number:
            other_pass += 1
            continue
        if not valid:
            corrupt.append({'disk_sector': disk_sector, 'index': fields['index'],
                            'checksum_valid': fields['checksum_valid'], 'fill_valid': fields['fill_valid']})
            continue
        positions.setdefault(fields['index'], []).append(disk_sector)
        counts.add(fields['count'])
        requests.add(fields['request'])

    count = max(counts) if counts else 0
    missing = [index for index in range(count) if index not in positions]
    duplicates = {index: sectors for index, sectors in positions.items() if len(sectors) > 1}
    outside = sorted(index for index in positions if index >= count)
    # Where the file lies on the disk: runs of disk sectors that hold
    # consecutive sector indexes. One extent per run of MFS buckets.
    extents = []
    for index in range(count):
        if index not in positions or index in duplicates:
            continue
        sector = positions[index][0]
        if extents and extents[-1]['last_index'] == index - 1 and extents[-1]['last_sector'] == sector - 1:
            extents[-1]['last_index'] = index
            extents[-1]['last_sector'] = sector
        else:
            extents.append({'first_index': index, 'last_index': index, 'first_sector': sector,
                            'last_sector': sector})
    result.update({
        'sector_count': count,
        'sector_counts_seen': sorted(counts),
        'request_bytes': sorted(requests),
        'present': len(positions),
        'missing': len(missing),
        'missing_ranges': ranges(missing),
        'duplicates': len(duplicates),
        'duplicate_sectors': {str(index): sectors for index, sectors in sorted(duplicates.items())},
        'corrupt': len(corrupt),
        'corrupt_sectors': corrupt,
        'index_outside_file': outside,
        'other_pass_sectors': other_pass,
        'other_run_sectors': other_run,
        'extents': len(extents),
        'extent_list': extents,
    })
    result['pass_check'] = (count > 0 and len(counts) == 1 and not missing and not duplicates
                            and not corrupt and not outside)
    return result


def summary(result, limit=SHOW_LIMIT):
    """A short text report of a check result."""
    lines = [f'pattern sectors found: {result["pattern_sectors"]}, runs: {result["runs_found"]}']
    if 'error' in result:
        lines.append(f'error: {result["error"]}')
        return '\n'.join(lines)
    lines.append(f'run {result["run"]} pass {result["pass"]}: {result["present"]} of {result["sector_count"]} '
                 f'sectors present, {result["missing"]} missing, {result["duplicates"]} duplicated, '
                 f'{result["corrupt"]} corrupt, {result["extents"]} extents on disk')
    if result['missing']:
        lines.append(f'missing index ranges: {result["missing_ranges"][:limit]}')
    if result['duplicates']:
        lines.append(f'duplicated indexes (disk sectors): {dict(list(result["duplicate_sectors"].items())[:limit])}')
    if result['corrupt']:
        lines.append(f'corrupt sectors: {result["corrupt_sectors"][:limit]}')
    if len(result['sector_counts_seen']) > 1:
        lines.append(f'sectors disagree on the sector count: {result["sector_counts_seen"]}')
    if result['index_outside_file']:
        lines.append(f'indexes outside the file: {result["index_outside_file"][:limit]}')
    lines.append(f'sectors of other passes: {result["other_pass_sectors"]}, of other runs: '
                 f'{result["other_run_sectors"]}')
    lines.append('PASS' if result['pass_check'] else 'FAIL')
    return '\n'.join(lines)


def run_check(path, run=None, pass_number=None, partition=None, header=PATTERN_HEADER):
    """Scan path and check it. Returns the result dictionary."""
    pattern = Pattern(read_layout(header))
    first, count = (0, None)
    if partition:
        first, count = partition_range(path, partition)
    found, total = scan_range(path, pattern, first, count)
    result = check(found, run, pass_number)
    result.update({'input': str(path), 'input_sectors': total, 'scan_first_sector': first,
                   'scan_sectors': (total - first) if count is None else count})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path, help='Raw card image or block device, opened read-only')
    parser.add_argument('--run', type=lambda text: int(text, 16), help='Run identifier in hex, from the log')
    parser.add_argument('--pass', dest='pass_number', type=int, help='Pass to check. Default: the highest')
    parser.add_argument('--partition', help='Scan only the GPT partition with this name, for example vali-data')
    parser.add_argument('--json', type=Path, help='Write the full result to this file')
    args = parser.parse_args()
    try:
        result = run_check(args.input, args.run, args.pass_number, args.partition)
    except OSError as error:
        print(f'cannot read {args.input}: {error}', file=sys.stderr)
        sys.exit(2)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + '\n')
    print(summary(result))
    if 'error' in result:
        sys.exit(2)
    sys.exit(0 if result['pass_check'] else 1)


if __name__ == '__main__':
    main()
