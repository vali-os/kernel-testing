"""Host test of the SD transfer test's sector pattern and of its host checker.

The target program's pattern code (testing/target/sdtransfer/pattern.c) is
built for the host with testing/sdtransfer_pattern_host.c. The test checks:

- The C code and the checker's own copy of the layout (sdtransfer_check.py,
  which reads its values from pattern.h) build the same bytes, the seed that
  would be 0 included, and the CRC-32 is the common one.
- The checker on small images built from the C code's sectors:
  - a good file spread over three extents in mixed order, with sectors of an
    earlier pass and of another run in free space,
  - a missing sector, a duplicated sector, a sector with damaged fill, a
    sector with a damaged index and a damaged extra copy,
  - sectors that disagree on the sector count,
  - two runs without --run, and a run and pass given by hand,
  - a scan limited to one GPT partition.
- The checker opens its input read-only: the image is made read-only on the
  host and its SHA-256 must not change.
- The target program and pattern.c compile for i386, AMD64 and AArch64 with
  the Vali compiler, when it is installed.

With --mutation-check, each mutation of pattern.c or of the checker must
make at least one case fail.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
TESTING = ROOT / 'testing'
PROGRAM_DIRECTORY = TESTING / 'target/sdtransfer'
PATTERN_HEADER = PROGRAM_DIRECTORY / 'pattern.h'
CHECKER = TESTING / 'sdtransfer_check.py'
HELPER_SOURCE = TESTING / 'sdtransfer_pattern_host.c'

SECTOR = 512
RUN = 0x1234ABCD
OTHER_RUN = 0x0BADF00D
REQUEST = 65536

# Each mutation must make at least one case fail. (name, file, before, after)
MUTATIONS = (
    ('pattern: pass not mixed into the seed', 'pattern.c',
     '^ (sector->Pass * (uint32_t)TRANSFER_PATTERN_SEED_PASS)', '^ 0'),
    ('pattern: zero seed kept', 'pattern.c', '        fill = 1;', '        fill = 0;'),
    ('pattern: words stored big endian', 'pattern.c',
     'data[0] = (uint8_t)(value & 0xFF);\n    data[1] = (uint8_t)((value >> 8) & 0xFF);',
     'data[1] = (uint8_t)(value & 0xFF);\n    data[0] = (uint8_t)((value >> 8) & 0xFF);'),
    ('pattern: xorshift shift changed', 'pattern.c', 'value ^= value >> 17;', 'value ^= value >> 15;'),
    ('pattern: CRC start value 0', 'pattern.c', 'uint32_t crc = 0xFFFFFFFFU;', 'uint32_t crc = 0;'),
    ('pattern: checksum over one byte less', 'pattern.c',
     'TransferPatternChecksum(data, TRANSFER_PATTERN_OFFSET_CHECKSUM)',
     'TransferPatternChecksum(data, TRANSFER_PATTERN_OFFSET_CHECKSUM - 1)'),
    ('checker: duplicates ignored', 'sdtransfer_check.py',
     'duplicates = {index: sectors for index, sectors in positions.items() if len(sectors) > 1}',
     'duplicates = {}'),
    ('checker: missing sectors ignored', 'sdtransfer_check.py',
     'missing = [index for index in range(count) if index not in positions]', 'missing = []'),
    ('checker: CRC not checked', 'sdtransfer_check.py',
     "fields['checksum_valid'] = zlib.crc32(data[:layout['OFFSET_CHECKSUM']]) == fields['checksum']",
     "fields['checksum_valid'] = True"),
    ('checker: fill not checked', 'sdtransfer_check.py',
     "fields['fill_valid'] = data[layout['OFFSET_FILL']:layout['OFFSET_FILL'] + len(fill)] == fill",
     "fields['fill_valid'] = True"),
    ('checker: corrupt sectors of the run ignored', 'sdtransfer_check.py',
     'and not corrupt and not outside)', 'and not outside)'),
    ('checker: sector count disagreement ignored', 'sdtransfer_check.py',
     'result[\'pass_check\'] = (count > 0 and len(counts) == 1 and', 'result[\'pass_check\'] = (count > 0 and'),
    ('checker: unaligned markers taken', 'sdtransfer_check.py',
     'if position % size == 0 and position + size <= len(chunk):', 'if position + size <= len(chunk):'),
    ('checker: partition limit ignored', 'sdtransfer_check.py',
     '        first, count = partition_range(path, partition)', '        partition_range(path, partition)'),
    ('checker: oldest pass chosen', 'sdtransfer_check.py',
     'pass_number = max(runs[run])', 'pass_number = min(runs[run])'),
)


def load_checker(path):
    """Import a copy of sdtransfer_check.py. It imports sdhci_card from testing/."""
    if str(TESTING) not in sys.path:
        sys.path.insert(0, str(TESTING))
    spec = importlib.util.spec_from_file_location(f'checker_{abs(hash(str(path)))}', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_helper(pattern_directory, output):
    """Build the host helper from the pattern.c in pattern_directory."""
    binary = output / 'pattern_host'
    compiler = os.environ.get('CC', 'clang')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-g', '-I' + str(pattern_directory), str(HELPER_SOURCE),
                    str(pattern_directory / 'pattern.c'), '-o', str(binary)], check=True)
    return binary


class Writer:
    """Pattern sectors from the C helper."""

    def __init__(self, binary):
        self.binary = binary

    def sectors(self, run, pass_number, first, count, total, request=REQUEST):
        completed = subprocess.run([str(self.binary), 'sectors', str(run), str(pass_number), str(first),
                                    str(count), str(total), str(request)], capture_output=True, check=True)
        data = completed.stdout
        return [data[i:i + SECTOR] for i in range(0, len(data), SECTOR)]

    def crc(self, text):
        return int(subprocess.run([str(self.binary), 'crc', text], capture_output=True, check=True,
                                  text=True).stdout, 16)


def noise(index):
    """A sector of other data. Some carry the marker at an odd offset."""
    data = bytearray(hashlib.sha256(f'noise {index}'.encode()).digest() * (SECTOR // 32))
    if index % 3 == 0:
        data[100:108] = struct.pack('<II', 0x54445356, 0x314E4152)
    return bytes(data)


def good_image(writer, file_sectors=96):
    """A raw image: the file of pass 2 in three extents in mixed order,
    pass 1 of the same run and another run's sectors in free space, and
    noise. Returns (sector list, file sector disk positions)."""
    image = [noise(i) for i in range(400)]
    file_data = writer.sectors(RUN, 2, 0, file_sectors, file_sectors)
    extents = ((300, 0, 40), (20, 40, 30), (150, 70, file_sectors - 70))
    positions = {}
    for disk, first, count in extents:
        for offset in range(count):
            image[disk + offset] = file_data[first + offset]
            positions[first + offset] = disk + offset
    # The earlier pass and another run, left in free space.
    for offset, data in enumerate(writer.sectors(RUN, 1, 0, 30, file_sectors)):
        image[200 + offset] = data
    for offset, data in enumerate(writer.sectors(OTHER_RUN, 3, 0, 10, 10)):
        image[250 + offset] = data
    return image, positions


def write_image(path, sectors):
    path.write_bytes(b''.join(sectors))
    return path


def gpt_image(writer, output):
    """A GPT image with two partitions. The file lies in vali-data, and a
    duplicate of one of its sectors lies in the other partition."""
    sectors = [bytes(SECTOR) for _ in range(200)]
    header = bytearray(SECTOR)
    header[0:8] = b'EFI PART'
    struct.pack_into('<QQ', header, 40, 34, 190)
    struct.pack_into('<QII', header, 72, 2, 4, 128)
    entries = bytearray(4 * 128)

    def entry(index, first, last, name):
        offset = index * 128
        entries[offset:offset + 16] = bytes(range(1, 17))
        struct.pack_into('<QQ', entries, offset + 32, first, last)
        encoded = name.encode('utf-16le')
        entries[offset + 56:offset + 56 + len(encoded)] = encoded

    entry(0, 40, 79, 'vali-boot')
    entry(1, 80, 189, 'vali-data')
    sectors[1] = bytes(header)
    sectors[2] = bytes(entries[:SECTOR])
    file_data = writer.sectors(RUN, 1, 0, 50, 50)
    for offset, data in enumerate(file_data):
        sectors[100 + offset] = data
    sectors[45] = file_data[7]
    return write_image(output / 'gpt.img', sectors)


class Result(dict):
    """A check result where a key the result lacks reads as None, so a case
    that looks at a result with an error fails instead of raising."""

    def __missing__(self, key):
        return None


def run_cases(checker, writer, output):
    """Run every case. Returns the names of the cases that failed."""
    failures = []
    header = PATTERN_HEADER
    pattern = checker.Pattern(checker.read_layout(header))

    def case(name, condition):
        if not condition:
            failures.append(name)
        print(('passed: ' if condition else 'FAILED: ') + name, flush=True)

    def checked(sectors, name, **options):
        path = write_image(output / f'{name}.img', sectors)
        return Result(checker.run_check(path, header=header, **options))

    # The C writer and the checker's builder agree byte for byte.
    c_sectors = writer.sectors(RUN, 2, 0, 64, 64)
    case('C and Python build the same sectors',
         all(c_sectors[i] == pattern.build(RUN, 2, i, 64, REQUEST) for i in range(64)))
    # pass 1, index 0: the seed is run ^ SEED_PASS, so this run makes it 0.
    zero_run = 0x9E3779B9
    zero = writer.sectors(zero_run, 1, 0, 1, 1)[0]
    case('zero seed becomes 1 in both', zero == pattern.build(zero_run, 1, 0, 1, REQUEST)
         and struct.unpack_from('<I', zero, 28)[0] == 1)
    case('CRC-32 of "123456789" is 0xcbf43926', writer.crc('123456789') == 0xCBF43926
         and zlib.crc32(b'123456789') == 0xCBF43926)
    case('every sector checks out', all(pattern.decode(data)['checksum_valid'] and pattern.decode(data)['fill_valid']
                                        for data in c_sectors))

    image, positions = good_image(writer)
    result = checked(image, 'good', run=RUN)
    case('good image passes', result.get('pass_check') is True and result['present'] == 96
         and result['pass'] == 2 and result['extents'] == 3 and result['other_pass_sectors'] == 30
         and result['other_run_sectors'] == 10 and result['run'] == f'0x{RUN:08x}'
         and result['runs_found'] == {f'0x{OTHER_RUN:08x}': [3], f'0x{RUN:08x}': [1, 2]})

    single = [noise(i) for i in range(120)]
    for offset, data in enumerate(writer.sectors(RUN, 1, 0, 64, 64)):
        single[10 + offset] = data
    for offset, data in enumerate(writer.sectors(RUN, 2, 0, 64, 64)):
        single[30 + offset] = data if offset < 50 else single[30 + offset]
    result = checked(single, 'auto')
    case('one run, highest pass chosen without --run', result.get('pass') == 2 and result['run'] == f'0x{RUN:08x}')
    case('highest pass with sectors missing fails', result.get('pass_check') is False and result['missing'] == 14)
    result = checked(single, 'auto-pass1', pass_number=1)
    case('pass given by hand', result.get('pass') == 1 and result['present'] == 20)

    two = [noise(i) for i in range(60)]
    two[5:15] = writer.sectors(RUN, 1, 0, 10, 10)
    two[30:40] = writer.sectors(OTHER_RUN, 1, 0, 10, 10)
    result = checked(two, 'two-runs')
    case('two runs without --run is an error', 'error' in result)
    result = checked(two, 'two-runs-chosen', run=OTHER_RUN)
    case('run given by hand passes', result.get('pass_check') is True and result['run'] == f'0x{OTHER_RUN:08x}')

    missing = list(image)
    missing[positions[57]] = noise(9001)
    result = checked(missing, 'missing', run=RUN)
    case('missing sector fails', result['pass_check'] is False and result['missing'] == 1
         and result['missing_ranges'] == [[57, 57]])

    duplicate = list(image)
    duplicate[5] = image[positions[12]]
    result = checked(duplicate, 'duplicate', run=RUN)
    case('duplicated sector fails', result['pass_check'] is False and result['duplicates'] == 1
         and result['duplicate_sectors'] == {'12': sorted([5, positions[12]])})

    corrupt = list(image)
    damaged = bytearray(image[positions[33]])
    damaged[200] ^= 0x40
    corrupt[positions[33]] = bytes(damaged)
    result = checked(corrupt, 'corrupt-fill', run=RUN)
    case('damaged fill fails', result['pass_check'] is False and result['corrupt'] == 1
         and result['missing'] == 1 and result['corrupt_sectors'][0]['checksum_valid'] is False)

    # A sector with valid CRC but wrong fill can only come from a writer that
    # puts one sector's fill under another's header. Build one by hand.
    swapped = bytearray(pattern.build(RUN, 2, 70, 96, REQUEST))
    swapped[28:508] = pattern.build(RUN, 2, 71, 96, REQUEST)[28:508]
    struct.pack_into('<I', swapped, 508, zlib.crc32(bytes(swapped[:508])))
    wrong_fill = list(image)
    wrong_fill[positions[70]] = bytes(swapped)
    result = checked(wrong_fill, 'wrong-fill', run=RUN)
    case('valid CRC with another sector\'s fill fails', result['pass_check'] is False and result['corrupt'] == 1
         and result['corrupt_sectors'][0]['fill_valid'] is False)

    index = list(image)
    damaged = bytearray(image[positions[80]])
    damaged[16] ^= 0x01
    index[positions[80]] = bytes(damaged)
    result = checked(index, 'corrupt-index', run=RUN)
    case('damaged index fails', result['pass_check'] is False and result['missing'] == 1 and result['corrupt'] == 1)

    # Every index is present, and a damaged copy of one of them lies in free
    # space. Damage to a sector of the checked pass still fails the check.
    extra = list(image)
    damaged = bytearray(image[positions[10]])
    damaged[300] ^= 0x10
    extra[390] = bytes(damaged)
    result = checked(extra, 'corrupt-extra', run=RUN)
    case('damaged extra copy fails', result['pass_check'] is False and result['missing'] == 0
         and result['corrupt'] == 1)

    count = list(image)
    count[positions[90]] = writer.sectors(RUN, 2, 90, 1, 95)[0]
    result = checked(count, 'count', run=RUN)
    case('sector count disagreement fails', result['pass_check'] is False
         and result['missing'] == 0 and result['sector_counts_seen'] == [95, 96])

    shifted = list(image)
    shifted[positions[3]] = noise(77)[:7] + image[positions[3]][:SECTOR - 7]
    result = checked(shifted, 'unaligned', run=RUN)
    case('a sector shifted by 7 bytes is not taken', result['pass_check'] is False and result['missing'] == 1
         and result['corrupt'] == 0)

    gpt = gpt_image(writer, output)
    result = Result(checker.run_check(gpt, partition='vali-data', header=header))
    case('partition scan passes', result.get('pass_check') is True and result['scan_first_sector'] == 80
         and result['scan_sectors'] == 110)
    result = Result(checker.run_check(gpt, header=header))
    case('whole disk scan sees the copy in the other partition', result.get('pass_check') is False
         and result['duplicates'] == 1)

    # The checker must never write. The image is read-only on the host, and
    # its hash must not change.
    readonly = write_image(output / 'readonly.img', image)
    before = hashlib.sha256(readonly.read_bytes()).hexdigest()
    readonly.chmod(stat.S_IRUSR)
    result = Result(checker.run_check(readonly, run=RUN, header=header))
    case('read-only input is checked and unchanged', result.get('pass_check') is True
         and hashlib.sha256(readonly.read_bytes()).hexdigest() == before)
    readonly.chmod(stat.S_IRUSR | stat.S_IWUSR)
    return failures


def cross_compile(output):
    """Compile the target program for each architecture, without linking."""
    compiler = os.environ.get('VALI_CLANG', '/usr/local/valicc/bin/clang')
    if not Path(compiler).exists():
        print('Vali compiler not found, cross-compilation skipped:', compiler, flush=True)
        return
    includes = ['-I' + str(ROOT / path) for path in (
        'librt/libc/include', 'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
        'librt/libgracht/include')]
    for arch in ('i386', 'amd64', 'aarch64'):
        flags = [compiler, f'--target={arch}-uml-vali', '-fms-extensions', '-ffreestanding', '-nostdlibinc', '-O2',
                 '-Wall', '-Werror', '-Wno-address-of-packed-member', '-Wno-unused-function',
                 '-DTRANSFER_ARGUMENTS="--file-mib 8 --passes 2"']
        if arch == 'aarch64':
            flags += ['-march=armv8-a', '-mno-outline-atomics']
        for source in ('pattern.c', 'sdtransfer.c'):
            subprocess.run(flags + includes + ['-c', str(PROGRAM_DIRECTORY / source),
                                               '-o', str(output / f'{arch}-{source}.o')], check=True)
        print('Target program compilation:', arch, 'passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-sdtransfer-check-') as directory:
        output = Path(directory)
        writer = Writer(build_helper(PROGRAM_DIRECTORY, output))
        failures = run_cases(load_checker(CHECKER), writer, output)
        if failures:
            raise SystemExit(f'sdtransfer check test failed: {failures}')
        cross_compile(output)
        if not args.mutation_check:
            return

        for name, file_name, before, after in MUTATIONS:
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            mutant.mkdir()
            shutil.copy(PROGRAM_DIRECTORY / 'pattern.c', mutant / 'pattern.c')
            shutil.copy(PROGRAM_DIRECTORY / 'pattern.h', mutant / 'pattern.h')
            shutil.copy(CHECKER, mutant / 'sdtransfer_check.py')
            target = mutant / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))
            cases = output / 'mutant-cases'
            if cases.exists():
                shutil.rmtree(cases)
            cases.mkdir()
            mutant_writer = Writer(build_helper(mutant, cases))
            print(f'--- mutation: {name}', flush=True)
            failures = run_cases(load_checker(mutant / 'sdtransfer_check.py'), mutant_writer, cases)
            if not failures:
                raise SystemExit(f'Mutation did not fail a case: {name}')
            print('sdtransfer mutation rejected:', name, flush=True)


if __name__ == '__main__':
    main()
