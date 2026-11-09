#!/usr/bin/env python3
"""Boot a built AMD64 kernel through UEFI and BIOS to the post-MMU ACPI checkpoint.

Uses a minimal read-only MFS fixture for BIOS. No host disk builder or system
installation is required. This checks loader handoff and kernel resource/stack
mapping; it does not assert that later ACPI or userspace initialization succeeds.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess


def run(*arguments):
    subprocess.run([str(argument) for argument in arguments], check=True,
                   stdout=subprocess.DEVNULL)


def symbol(mapping, name):
    return int(re.search(r'^([0-9a-fA-F]+).*?\s' + re.escape(name) + r'$', mapping, re.M)[1], 16)


def bios_image(output, build, files):
    # The loader only reads this fixture. A single root bucket contains three
    # ordinary file records, each backed by one consecutive run of 4 KiB buckets.
    # Offsets match boot/bios/stage{1,2}'s MFS reader and the MFS record format.
    sector_size = 512
    sectors_per_bucket = 8
    partition_sector = 1
    partition_sectors = 65535
    stage2 = (build / 'deploy/stage2.sys').read_bytes()
    master_sector = 1 + (len(stage2) + 511) // 512
    reserved = (master_sector + 1 + 7) & ~7
    bucket_count = (partition_sectors - reserved) // sectors_per_bucket
    map_sectors = (bucket_count * 8 + 511) // 512
    map_sector = partition_sectors - map_sectors
    backup_sector = map_sector - 1
    data_sector = partition_sector + reserved
    image = bytearray((partition_sectors + partition_sector) * sector_size)
    image[:446] = (build / 'deploy/mbr.sys').read_bytes()[:446]
    struct.pack_into('<8BII', image, 446, 0x80, 0, 2, 0, 0x61, 15, 63, 64,
                     partition_sector, partition_sectors)
    image[510:512] = b'\x55\xaa'
    vbr = bytearray((build / 'deploy/stage1.sys').read_bytes())
    struct.pack_into('<4sBBBHHHQHHQQ', vbr, 3, b'MFS1', 1, 1, 0x80, 512, 63, 16,
                     partition_sectors, reserved, sectors_per_bucket, master_sector, backup_sector)
    image[512:1024] = vbr
    image[1024:1024 + len(stage2)] = stage2
    master = bytearray(512)
    master[:4] = b'MFS1'
    master[12:21] = b'boot-test'
    struct.pack_into('<I', master, 80, 0)
    struct.pack_into('<QQ', master, 92, map_sector, bucket_count * 8)
    map_base = (partition_sector + map_sector) * sector_size
    struct.pack_into('<II', image, map_base, 0xffffffff, 1)
    next_bucket = 1
    for i, name in enumerate(['kernel.mos', 'phoenix.mos', 'initrd.mos']):
        contents = (files / name).read_bytes()
        buckets = (len(contents) + 4095) // 4096
        record = data_sector * sector_size + i * 1024
        struct.pack_into('<III', image, record, 0x80000000, next_bucket, buckets)
        struct.pack_into('<QQ', image, record + 0x30, len(contents), buckets * 4096)
        encoded = name.encode() + b'\0'
        image[record + 0x44:record + 0x44 + len(encoded)] = encoded
        destination = data_sector * sector_size + next_bucket * 4096
        assert destination + len(contents) < backup_sector * sector_size
        image[destination:destination + len(contents)] = contents
        struct.pack_into('<II', image, map_base + next_bucket * 8, 0xffffffff, buckets)
        next_bucket += buckets
    # Leave a valid free run and master checksum even though this gate never
    # mounts the filesystem for writing or starts the filesystem service.
    free_buckets = (backup_sector - reserved) // 8 - next_bucket
    struct.pack_into('<II', image, map_base + next_bucket * 8, 0xffffffff, free_buckets)
    struct.pack_into('<I', master, 76, next_bucket)
    struct.pack_into('<I', master, 8, sum(master))
    for sector in [master_sector, backup_sector]:
        offset = (partition_sector + sector) * sector_size
        image[offset:offset + 512] = master
    output.write_bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--uefi-loader', required=True, type=Path)
    parser.add_argument('--firmware', required=True, type=Path)
    parser.add_argument('--lzss', required=True, type=Path)
    parser.add_argument('--mkvafs', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--acpi', action='store_true', help='Also require successful early ACPI initialization')
    parser.add_argument('--qemu', default='qemu-system-x86_64')
    parser.add_argument('--gdb', default='gdb')
    args = parser.parse_args()
    build = args.build.resolve()
    directory = args.output_dir.resolve()
    esp = directory / 'esp'
    files = esp / 'EFI/VALI'
    files.mkdir(parents=True, exist_ok=True)
    (esp / 'EFI/BOOT').mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.uefi_loader, esp / 'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(build / 'bin/phoenix.mos', files / 'phoenix.mos')
    run(args.lzss.resolve(), 'c', build / 'bin/kernel.mos', files / 'kernel.mos')
    ramdisk = directory / 'ramdisk'
    (ramdisk / 'services').mkdir(parents=True, exist_ok=True)
    (ramdisk / 'README').write_text('x86 boot resource fixture\n')
    run(args.mkvafs.resolve(), '--arch', 'amd64', '--out', files / 'initrd.mos', ramdisk)
    bios_image(directory / 'bios.img', build, files)
    data = (build / 'bin/kernel.mos').read_bytes()
    optional = struct.unpack_from('<I', data, 60)[0] + 24
    image_base = struct.unpack_from('<Q', data, optional + 24)[0]
    mapping = (build / 'bin/kernel.map').read_text()
    memory_entry = image_base + symbol(mapping, 'MmuLoadKernel')
    acpi_entry = image_base + symbol(mapping, 'AcpiInitializeEarly')
    panic = image_base + symbol(mapping, 'DebugPanic')
    for mode in ['uefi', 'bios']:
        serial = directory / (mode + '.serial.log')
        debugger = directory / (mode + '.debugger.log')
        script = directory / (mode + '.gdb')
        if mode == 'uefi':
            machine = 'q35'
            drive = (f'-bios {args.firmware.resolve()} '
                     f'-drive if=virtio,format=raw,file=fat:{esp},readonly=on')
        else:
            machine = 'pc'
            # Stage 1 uses CHS reads. Match the fixture's boot-sector geometry
            # explicitly instead of letting QEMU infer it from a small image.
            drive = (f'-drive if=none,id=hd0,format=raw,file={directory}/bios.img '
                     '-device ide-hd,drive=hd0,cyls=66,heads=16,secs=63,bios-chs-trans=none')
        script.write_text('\n'.join([
            f'target remote | {args.qemu} -machine {machine} -accel tcg -cpu qemu64 '
            f'-m 1024 -smp 1 {drive} -display none -serial file:{serial} -monitor none '
            '-no-reboot -S -gdb stdio',
            f'break *{memory_entry}', f'break *{panic}', 'continue',
            f'python assert int(gdb.parse_and_eval("$pc")) == {memory_entry}, "memory initialization not reached"',
            'delete breakpoints', f'break *{acpi_entry}', f'break *{panic}', 'continue',
            f'python assert int(gdb.parse_and_eval("$pc")) == {acpi_entry}, "page-table/stack switch failed"',
            *([
                'set $acpi_return = *(unsigned long long*)$rsp',
                'delete breakpoints', 'break *$acpi_return', f'break *{panic}', 'continue',
                'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("$acpi_return")), "ACPI did not return"',
                'python assert int(gdb.parse_and_eval("$eax")) == 0, "early ACPI failed"',
                'python print("X86 EARLY ACPI PASS")',
            ] if args.acpi else []),
            'python print("X86 KERNEL MEMORY PASS")', 'monitor quit',
        ]) + '\n')
        with debugger.open('w') as output:
            process = subprocess.Popen([args.gdb, '-q', '-batch', '-ex',
                                        'set architecture i386:x86-64', '-x', str(script)],
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                process.wait(timeout=55)
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
        result = debugger.read_text()
        if 'X86 KERNEL MEMORY PASS' not in result or 'AssertionError' in result:
            raise RuntimeError(f'{mode} failed; inspect {debugger} and {serial}')
        checkpoint = 'memory/stack and ACPI' if args.acpi else 'memory/stack'
        print(f'{mode}: kernel {checkpoint} checkpoint PASS ({debugger})', flush=True)


if __name__ == '__main__':
    main()
