"""SD card images for testing/sdhci_boot.py, and the host side checks of them.

The boot test needs a card that filed can parse and mount, with known
contents, and a way to tell after the run which sectors the OS changed. This
module builds such a card with the system's own image builder (mkcdk): a GPT
disk with one MFS partition that holds generated files. It also reads the
image back on the host:

- read_gpt: the GPT header and partition entries.
- MfsVolume: the MFS boot record, master record, bucket map and directory
  records, enough to list every file and read it back.
- compare_images: the sectors that differ between two images, in runs, with
  the MFS structure each run belongs to.
- writer_problems: what is wrong with a card mkcdk just built. mkcdk 1.5.8
  writes broken MFS volumes, see build_card.

Run it as a script to build a system disk without the vali-data partition.
A boot test with --data-card needs one, so that /data comes from the SD card
and served's startup writes land on the card:

    python3 testing/sdhci_card.py system-image --build build/verify-amd64 \\
        --model cmake/models/vali-gpt.yaml --output build/verify-amd64/sdhci-nodata.img

MFS layout used here (see modules/filesystems/mfs/mfs.h). Sector numbers are
relative to the partition start:
- Sector 0: boot record. ReservedSectors, SectorsPerBucket, and the sectors
  of the master record and its mirror.
- Master record: label, first free bucket, root directory bucket, bucket map
  sector and size. The mirror holds a copy.
- Bucket map: 8 bytes per bucket, the next bucket of the chain (Link) and the
  number of buckets in this run (Length).
- Bucket n starts at sector ReservedSectors + n * SectorsPerBucket.
- A directory is a chain of buckets that holds 1024-byte file records.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

SECTOR_SIZE = 512

# GPT partition type that filed parses as MFS user data (vali-data).
MFS_DATA_TYPE = '80C6C62A-B0D6-4FF4-A69D-558AB6FD8B53'
# Another MFS type filed accepts. A card with this type and a label filed
# has no default path for is only mounted under /storage/<Serial>/<label>.
MFS_USER_TYPE = '8874F880-E7AD-4EE2-839E-6FFA54F19A72'

MFS_BOOT_MAGIC = 0x3153464D
MFS_MASTER_MAGIC = 0x3153464D
MFS_END_OF_CHAIN = 0xFFFFFFFF
MFS_RECORD_SIZE = 1024
MFS_RECORD_IN_USE = 0x80000000
MFS_RECORD_DIRECTORY = 0x1

# Files placed on the card: path, size in bytes. The sizes cover less than
# one sector, one bucket, several buckets, and more than the 128 KiB bounce
# chunk of the storage adapter. The files sit in the root and in directories
# up to three levels deep, and are found by name after the run.
CARD_FILES = (
    ('readme.txt', 300),
    ('one-bucket.bin', 4096),
    ('docs/notes.txt', 700),
    ('docs/manual/pattern-64k.bin', 64 * 1024),
    ('docs/manual/index.txt', 1500),
    ('data/blobs/deep/pattern-300k.bin', 300 * 1024),
)

# The same sizes, all in the root, for a defective mkcdk (see build_card).
# Such a card is found by the contents of its files, not by their names.
FLAT_CARD_FILES = (
    ('readme.txt', 300),
    ('one-bucket.bin', 4096),
    ('pattern-64k.bin', 64 * 1024),
    ('pattern-300k.bin', 300 * 1024),
)


def pattern_bytes(name, size):
    """Deterministic contents: SHA-256 in counter mode, seeded by the name."""
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{counter}'.encode()).digest()
        counter += 1
    return bytes(out[:size])


def write_card_files(root, files=CARD_FILES):
    """Write files (path, size) below root. Returns {path: sha256}."""
    hashes = {}
    for path, size in files:
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        data = pattern_bytes(path, size)
        target.write_bytes(data)
        hashes[path] = hashlib.sha256(data).hexdigest()
    return hashes


def write_model(path, label, type_guid, source):
    """A mkcdk model for a GPT card with one MFS partition over the whole card."""
    path.write_text(
        'schema: gpt\n\n'
        'partitions:\n'
        f'  - label: {label}\n'
        '    type: mfs\n'
        f'    id: {type_guid}\n'
        '    sources:\n'
        '      - type: dir\n'
        f'        source: {source}\n'
        '        target: /\n')


def find_mkcdk(path=None):
    """The mkcdk executable to use: path when given, otherwise mkcdk from PATH."""
    found = shutil.which(str(path) if path else 'mkcdk')
    if found is None:
        raise SystemExit(f'mkcdk not found: {path or "mkcdk on PATH"}')
    return str(Path(found).resolve())


def mkcdk_version(mkcdk):
    """The version line of mkcdk, for the record only. Defects are found
    by writer_problems, not by the version."""
    result = subprocess.run([mkcdk, '--version'], capture_output=True, text=True, timeout=30)
    return (result.stdout or result.stderr).strip()


def run_mkcdk(cwd, output, size_text, model, log, mkcdk='mkcdk'):
    """Run the image builder. mkcdk resolves model sources against cwd."""
    with log.open('w') as handle:
        subprocess.run([mkcdk, '-o', str(output), '-s', size_text, str(model)], cwd=cwd, check=True,
                       stdout=handle, stderr=subprocess.STDOUT, timeout=300)


def writer_problems(data, hashes):
    """Check an MFS card that mkcdk just built with the files in hashes.

    mkcdk 1.5.8 has three defects, and each one is visible in its image:
    - Every file name starts with garbage bytes. Here every file must be found
      by its own path, with its own contents.
    - Every file in a subdirectory creates the subdirectory again, so the
      parent holds one record per file. Here no path may appear twice.
    - The master record's first free bucket is never saved. It then points at
      the first bucket given to a file. Here it must point at a bucket that
      no structure uses, and the mirror must hold the same record.

    Returns a list of problems. An empty list means the image is correct.
    """
    problems = []
    volume = MfsVolume(data, read_gpt(data)['partitions'][0]['first'])
    entries = volume.walk()
    paths = [path for path, _ in entries]
    duplicates = sorted({path for path in paths if paths.count(path) > 1})
    if duplicates:
        problems.append(f'paths with more than one record: {duplicates}')
    unreadable = sorted(path for path in paths if not path.isprintable())
    if unreadable:
        problems.append(f'unreadable names: {[__entry_name(path, {"directory": False}) for path in unreadable]}')
    records = {path: record for path, record in entries}
    for path, expected in hashes.items():
        record = records.get(path)
        if record is None or record['directory']:
            problems.append(f'{path} not found by name')
        elif hashlib.sha256(volume.read_file(record)).hexdigest() != expected:
            problems.append(f'{path} has other contents')
    problems += master_record_problems(volume)
    return problems


def master_record_problems(volume):
    """The master record and its mirror must agree, and the first free bucket
    must lie in free space: inside the partition and used by no structure."""
    problems = []
    if volume.sector(volume.master_sector) != volume.sector(volume.mirror_sector):
        problems.append('master record and mirror differ')
    owners = volume.owners()
    first = volume.bucket_sector(volume.free)
    if volume.free == MFS_END_OF_CHAIN or first >= volume.sector_count:
        problems.append(f'first free bucket {volume.free:#x} lies outside the partition')
        return problems
    _, length = volume.link(volume.free)
    used = sorted({owners[sector] for sector in range(first, first + max(length, 1) * volume.per_bucket)
                   if sector in owners})
    if length == 0:
        problems.append(f'first free bucket {volume.free} has no free run in the bucket map')
    if used:
        problems.append(f'first free bucket {volume.free} is in use by {used}')
    return problems


def build_card(out, size_mib, label, type_guid, writable, mkcdk='mkcdk'):
    """Build out/sdcard.raw and keep a copy as out/sdcard.orig. Returns the manifest.

    The card first gets CARD_FILES, in the root and in subdirectories. If
    writer_problems finds the defects of mkcdk 1.5.8 in that image, the card is
    built again with the old workaround:
    - A card the OS writes to (writable) carries no files, because the first
      allocation the OS makes would fail or take a file's bucket.
    - A read-only card carries FLAT_CARD_FILES in the root, and the files are
      found by contents, because their names are garbage.
    The manifest records the problems found and how files are matched.
    """
    work = out / 'card-source'
    model = work / 'card.yaml'
    image = out / 'sdcard.raw'

    def build(files, log_name):
        if (work / 'files').exists():
            shutil.rmtree(work / 'files')
        (work / 'files').mkdir(parents=True)
        hashes = write_card_files(work / 'files', files)
        write_model(model, label, type_guid, 'files')
        run_mkcdk(work, image, f'{size_mib}MB', model, out / log_name, mkcdk)
        if image.stat().st_size != size_mib * 1024 * 1024:
            raise RuntimeError(f'mkcdk built {image.stat().st_size} bytes, expected {size_mib} MiB')
        return hashes

    work.mkdir(parents=True)
    hashes = build(CARD_FILES, 'mkcdk-card.log')
    problems = writer_problems(image.read_bytes(), hashes)
    manifest = {'label': label, 'type': type_guid, 'mkcdk': mkcdk, 'mkcdk_version': mkcdk_version(mkcdk),
                'writer_problems': problems, 'writer_correct': not problems}
    if problems:
        shutil.copyfile(image, out / 'sdcard.defective')
        hashes = build(() if writable else FLAT_CARD_FILES, 'mkcdk-card-workaround.log')
        manifest.update({'layout': 'none' if writable else 'flat', 'match': 'contents'})
    else:
        manifest.update({'layout': 'nested', 'match': 'name'})
    manifest['files'] = hashes
    shutil.copyfile(image, out / 'sdcard.orig')
    return manifest


def read_gpt(data):
    """Return the primary GPT header fields and the used partition entries."""
    header = data[SECTOR_SIZE:2 * SECTOR_SIZE]
    if header[:8] != b'EFI PART':
        raise ValueError('no GPT header at sector 1')
    first_usable, last_usable = struct.unpack_from('<QQ', header, 40)
    entries_lba, entry_count, entry_size = struct.unpack_from('<QII', header, 72)
    partitions = []
    for index in range(entry_count):
        offset = entries_lba * SECTOR_SIZE + index * entry_size
        entry = data[offset:offset + entry_size]
        if entry[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', entry, 32)
        name = entry[56:128].decode('utf-16le').split('\0')[0]
        partitions.append({'index': index, 'first': first, 'last': last, 'name': name})
    return {
        'first_usable': first_usable, 'last_usable': last_usable,
        'entries_lba': entries_lba, 'entries_sectors': (entry_count * entry_size + SECTOR_SIZE - 1) // SECTOR_SIZE,
        'backup_lba': struct.unpack_from('<Q', header, 32)[0], 'partitions': partitions,
    }


class MfsVolume:
    """Read-only view of one MFS partition in a disk image."""

    def __init__(self, data, first_sector):
        self.data = data
        self.first = first_sector
        boot = self.sector(0)
        (_, magic, _, _, _, sector_size, _, _, sector_count, reserved, per_bucket,
         master, mirror) = struct.unpack_from('<3sIBBBHHHQHHQQ', boot, 0)
        if magic != MFS_BOOT_MAGIC or sector_size != SECTOR_SIZE:
            raise ValueError('no MFS boot record at the partition start')
        self.sector_count = sector_count
        self.reserved = reserved
        self.per_bucket = per_bucket
        self.master_sector = master
        self.mirror_sector = mirror
        record = self.sector(master)
        (magic, _, _, name, free, root, _, _, map_sector, map_size) = struct.unpack_from('<III64sIIIIQQ', record, 0)
        if magic != MFS_MASTER_MAGIC:
            raise ValueError('no MFS master record')
        self.label = name.split(b'\0')[0].decode(errors='replace')
        self.free = free
        self.root = root
        self.map_sector = map_sector
        self.map_sectors = (map_size + SECTOR_SIZE - 1) // SECTOR_SIZE
        self.map = self.read(map_sector, self.map_sectors)[:map_size]

    def sector(self, relative):
        return self.read(relative, 1)

    def read(self, relative, count):
        start = (self.first + relative) * SECTOR_SIZE
        return self.data[start:start + count * SECTOR_SIZE]

    def bucket_sector(self, bucket):
        return self.reserved + bucket * self.per_bucket

    def link(self, bucket):
        return struct.unpack_from('<II', self.map, bucket * 8)

    def runs(self, bucket):
        """The (first bucket, bucket count) runs of a chain."""
        runs = []
        while bucket != MFS_END_OF_CHAIN and len(runs) < 100000:
            following, length = self.link(bucket)
            runs.append((bucket, length))
            bucket = following
        return runs

    def chain_bytes(self, bucket):
        return b''.join(self.read(self.bucket_sector(start), length * self.per_bucket)
                        for start, length in self.runs(bucket))

    def records(self, bucket):
        """The in-use file records of the directory that starts at bucket."""
        data = self.chain_bytes(bucket)
        for offset in range(0, len(data) - MFS_RECORD_SIZE + 1, MFS_RECORD_SIZE):
            flags, start, _, = struct.unpack_from('<III', data, offset)
            if not flags & MFS_RECORD_IN_USE:
                continue
            size = struct.unpack_from('<Q', data, offset + 0x30)[0]
            name = data[offset + 0x44:offset + 0x44 + 300].split(b'\0')[0].decode(errors='replace')
            yield {'name': name, 'flags': flags, 'start': start, 'size': size,
                   'directory': (flags & 0x3) == MFS_RECORD_DIRECTORY}

    def walk(self):
        """Every entry as a list of (path, record), directories included.
        A list, because a broken image can hold two records with one name."""
        entries = []
        pending = [('', self.root)]
        while pending:
            prefix, bucket = pending.pop()
            if bucket == MFS_END_OF_CHAIN:
                continue
            for record in self.records(bucket):
                path = f'{prefix}{record["name"]}'
                entries.append((path, record))
                if record['directory']:
                    pending.append((path + '/', record['start']))
        return entries

    def read_file(self, record):
        if record['start'] == MFS_END_OF_CHAIN:
            return b''
        return self.chain_bytes(record['start'])[:record['size']]

    def owners(self):
        """Partition-relative sector -> what holds it: a path, a directory or metadata."""
        owners = {0: 'MFS boot record', self.master_sector: 'MFS master record',
                  self.mirror_sector: 'MFS master record mirror'}
        for index in range(self.map_sectors):
            owners[self.map_sector + index] = 'MFS bucket map'
        for start, length in self.runs(self.root):
            for sector in range(self.bucket_sector(start), self.bucket_sector(start + length)):
                owners[sector] = 'directory /'
        for path, record in self.walk():
            if record['start'] == MFS_END_OF_CHAIN:
                continue
            kind = 'directory /' if record['directory'] else 'file /'
            for start, length in self.runs(record['start']):
                for sector in range(self.bucket_sector(start), self.bucket_sector(start + length)):
                    owners[sector] = kind + path
        return owners


def __entry_name(path, record):
    """Printable entry name with a trailing / for a directory."""
    name = ''.join(character if character.isprintable() else '?' for character in path)
    return name + '/' if record['directory'] else name


def changed_runs(before, after):
    """Runs of sectors that differ, as (first, count)."""
    if len(before) != len(after):
        raise ValueError('images differ in size')
    runs = []
    for sector in range(len(before) // SECTOR_SIZE):
        start = sector * SECTOR_SIZE
        if before[start:start + SECTOR_SIZE] == after[start:start + SECTOR_SIZE]:
            continue
        if runs and runs[-1][0] + runs[-1][1] == sector:
            runs[-1][1] += 1
        else:
            runs.append([sector, 1])
    return [tuple(run) for run in runs]


def compare_images(original_path, after_path, manifest):
    """Compare the card before and after the boot, and read the card back.

    Returns a report with: the SHA-256 of both images, the changed sector runs
    with the structure each sector belongs to (taken from the image after the
    run, so new directories are named), the SHA-256 of all unchanged sectors
    in both images, the files listed in the card after the run, and whether
    every generated file still reads back with its original hash.
    """
    before = original_path.read_bytes()
    after = after_path.read_bytes()
    runs = changed_runs(before, after)
    changed = {sector for first, count in runs for sector in range(first, first + count)}

    unchanged_before = hashlib.sha256()
    unchanged_after = hashlib.sha256()
    for sector in range(len(before) // SECTOR_SIZE):
        if sector in changed:
            continue
        start = sector * SECTOR_SIZE
        unchanged_before.update(before[start:start + SECTOR_SIZE])
        unchanged_after.update(after[start:start + SECTOR_SIZE])

    gpt = read_gpt(after)
    partition = gpt['partitions'][0]
    volume_before = MfsVolume(before, partition['first'])
    volume = MfsVolume(after, partition['first'])
    owners_before = volume_before.owners()
    owners_after = volume.owners()

    def describe(sector):
        if sector < gpt['first_usable'] or sector > gpt['last_usable']:
            return 'GPT'
        relative = sector - partition['first']
        if relative < 0 or sector > partition['last']:
            return 'outside the partition'
        owner = owners_after.get(relative) or owners_before.get(relative)
        return owner or 'free space'

    described = []
    for first, count in runs:
        parts = {}
        for sector in range(first, first + count):
            name = describe(sector)
            parts[name] = parts.get(name, 0) + 1
        described.append({'first': first, 'count': count, 'owners': parts})

    # Each generated file must still be on the card with its contents. A card
    # from a correct mkcdk is matched by path and then by SHA-256. A card from
    # a defective one, or a manifest from before this choice existed, only by
    # SHA-256 (see build_card).
    entries = volume.walk()
    by_name = manifest.get('match') == 'name'
    found = {}
    for path, record in entries:
        if not record['directory']:
            found.setdefault(path if by_name else hashlib.sha256(volume.read_file(record)).hexdigest(), record)
    files = {}
    for path, expected in manifest['files'].items():
        record = found.get(path if by_name else expected)
        sha256 = hashlib.sha256(volume.read_file(record)).hexdigest() if record else None
        files[path] = {'present': record is not None and sha256 == expected, 'sha256': expected,
                       'sha256_after': sha256, 'start_bucket': record['start'] if record else None}
    return {
        'sha256_before': hashlib.sha256(before).hexdigest(),
        'sha256_after': hashlib.sha256(after).hexdigest(),
        'changed_sectors': len(changed),
        'changed_runs': described,
        'unchanged_sectors': len(before) // SECTOR_SIZE - len(changed),
        'unchanged_sha256_before': unchanged_before.hexdigest(),
        'unchanged_sha256_after': unchanged_after.hexdigest(),
        'label': volume.label,
        'match': 'name' if by_name else 'contents',
        'free_bucket_before': volume_before.free,
        'free_bucket_after': volume.free,
        'master_record_problems_before': master_record_problems(volume_before),
        'master_record_problems_after': master_record_problems(volume),
        'entries_after': sorted(__entry_name(path, record) for path, record in entries),
        'entries_before': sorted(__entry_name(path, record) for path, record in volume_before.walk()),
        'files': files,
    }


def strip_data_partition(model_text):
    """Remove the vali-data partition from a GPT model."""
    head, separator, rest = model_text.partition('partitions:\n')
    if not separator:
        raise ValueError('model has no partitions list')
    blocks = []
    for block in rest.split('\n  - label: '):
        if block.strip():
            blocks.append(block if block.startswith('  - label: ') else '  - label: ' + block)
    kept = [block for block in blocks if not block.startswith('  - label: vali-data')]
    if len(kept) == len(blocks):
        raise ValueError('model has no vali-data partition')
    return head + separator + '\n'.join(block.rstrip('\n') + '\n' for block in kept)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    system = commands.add_parser('system-image', help='Build a system disk without the vali-data partition')
    system.add_argument('--build', type=Path, required=True, help='Build directory after install_prepare')
    system.add_argument('--model', type=Path, required=True, help='GPT model, for example cmake/models/vali-gpt.yaml')
    system.add_argument('--output', type=Path, required=True)
    system.add_argument('--size', default='4GB')
    system.add_argument('--mkcdk', type=Path, help='mkcdk executable. Defaults to mkcdk from PATH')
    inspect = commands.add_parser('inspect', help='List the MFS partition of a card image as JSON')
    inspect.add_argument('image', type=Path)
    args = parser.parse_args()

    if args.command == 'system-image':
        model = args.output.with_suffix('.yaml')
        model.write_text(strip_data_partition(args.model.read_text()))
        mkcdk = find_mkcdk(args.mkcdk)
        run_mkcdk(args.build.resolve(), args.output.resolve(), args.size, model.resolve(),
                  args.output.with_suffix('.log'), mkcdk)
        print(f'{args.output}: system disk without vali-data, model {model}, {mkcdk_version(mkcdk)} ({mkcdk})')
        return
    data = args.image.read_bytes()
    gpt = read_gpt(data)
    volume = MfsVolume(data, gpt['partitions'][0]['first'])
    print(json.dumps({'gpt': gpt, 'label': volume.label, 'free_bucket': volume.free,
                      'master_record_problems': master_record_problems(volume),
                      'entries': [[__entry_name(path, record), record] for path, record in volume.walk()]}, indent=2))


if __name__ == '__main__':
    main()
