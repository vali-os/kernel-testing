"""Build the SD transfer test program and a test image that starts it.

The program lives in testing/target/sdtransfer and is never part of the
normal build or install tree. This script builds it against a configured
and built OS build directory, puts it into a copy of that build's initial
ramdisk as a service, and builds a disk image from the copy. The build
directory itself is only read: the image goes to --output, and nothing in the
build directory changes, build/disk.img included.

processd starts every service it finds in the initial ramdisk during boot,
so the program runs on every boot of the test image. It gets no command line
(services only get their API path), so its arguments are built into it as
one text line, --arguments here. See the top of
testing/target/sdtransfer/sdtransfer.c for the options and the result lines.

Images:

- Generic AMD64 or AArch64 build: a system disk without the vali-data
  partition, the same as `testing/sdhci_card.py system-image` builds, with the
  program added. An SD card with a vali-data partition is then mounted at
  /data, and the program writes its file there. testing/sdtransfer_boot.py
  builds and boots these.
- Raspberry Pi 4 build: the card image for the board, with the boot partition
  and a vali-data partition that the system mounts at /data. The program
  writes its file to /data on the same card. The vali-data partition is
  larger than in the normal image (--data-size), so a large test file fits.

Examples:

    python3 testing/sdtransfer_image.py --build build/verify-amd64 \\
        --output build/verify-amd64/sdtransfer-nodata.img
    python3 testing/sdtransfer_image.py --build build \\
        --output build/rpi4-transfer-test.img \\
        --arguments "--file-mib 64 --request-kib 1024,256,1024 --passes 3"

The script refuses to write build/disk.img or any disk.img directly in a
build directory. Next to the image it writes <image>.json with the arguments
and the SHA-256 of the program, the ramdisk and the image.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import sdhci_card

ROOT = Path(__file__).resolve().parents[1]
PROGRAM_DIRECTORY = ROOT / 'testing/target/sdtransfer'
PROGRAM_SOURCES = ('pattern.c', 'sdtransfer.c')
PROGRAM_NAME = 'sdtransfer'

# Libraries a service links against, as in the link line of the services
# in the OS build (for example build/<arch>/services/served).
LINK_LIBRARIES = ('libddk.lib', 'libds.lib', 'librt.lib', 'gracht.dll.lib', 'c.dll.lib')

# Defaults of the generic and Raspberry Pi images.
GENERIC_IMAGE_SIZE = '4GB'
RPI_IMAGE_SIZE = '1GB'
RPI_DATA_SIZE = '880MB'


def sha256(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def read_cache(build):
    """The VALI_ARCH and VALI_PLATFORM the build directory was configured with."""
    values = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        match = re.match(r'(VALI_ARCH|VALI_PLATFORM|VALI_PLATFORM_VARIANT):STRING=(.*)', line)
        if match:
            values[match[1]] = match[2]
    if 'VALI_ARCH' not in values or 'VALI_PLATFORM' not in values:
        raise SystemExit(f'{build}/CMakeCache.txt has no VALI_ARCH or VALI_PLATFORM')
    return values


def service_flags(build):
    """Compiler and target flags of a service in this build.

    They are taken from the compile command of served's main.c in
    compile_commands.json, so the program is built for the same target with
    the same configuration defines as the system it runs on."""
    commands = json.loads((build / 'compile_commands.json').read_text())
    for entry in commands:
        if entry['file'].endswith('services/served/main.c'):
            words = entry['command'].split() if 'command' in entry else entry['arguments']
            compiler = words[0]
            kept = []
            for word in words[1:]:
                if (word.startswith('--target=') or word.startswith('-march=') or word.startswith('-D__OSCONFIG_')
                        or word == '-mno-outline-atomics') and word not in kept:
                    kept.append(word)
            return compiler, kept
    raise SystemExit(f'{build}/compile_commands.json has no compile command for services/served/main.c')


def check_arguments(arguments):
    """The arguments become a C string literal, so they must not contain quotes or backslashes."""
    if '"' in arguments or '\\' in arguments:
        raise SystemExit('--arguments must not contain " or \\')
    if len(arguments) >= 256:
        raise SystemExit('--arguments must be shorter than 256 characters')


def build_program(build, work, arguments, log):
    """Compile and link the program as a service DLL. Returns its path."""
    check_arguments(arguments)
    compiler, target = service_flags(build)
    flags = [compiler] + target + [
        '-fms-extensions', '-ffreestanding', '-nostdlib', '-nostdlibinc', '-O2', '-Wall', '-Werror',
        '-Wno-address-of-packed-member', '-Wno-unused-function',
        f'-DTRANSFER_ARGUMENTS="{arguments}"']
    includes = ['-I' + str(ROOT / path) for path in (
        'librt/libc/include', 'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
        'librt/libgracht/include')] + ['-I' + str(build / 'protocols')]
    objects = []
    with log.open('w') as handle:
        for source in PROGRAM_SOURCES:
            output = work / (source + '.obj')
            command = flags + includes + ['-c', str(PROGRAM_DIRECTORY / source), '-o', str(output)]
            handle.write(' '.join(command) + '\n')
            handle.flush()
            subprocess.run(command, check=True, stdout=handle, stderr=subprocess.STDOUT, timeout=300)
            objects.append(str(output))
        dll = work / f'{PROGRAM_NAME}.dll'
        command = flags + ['-D_DLL', '-shared', '-Xlinker', '-lldmap', '-Xlinker', '-entry:__CrtServiceEntry',
                           '-Xlinker', f'-implib:{work / (PROGRAM_NAME + ".dll.lib")}', '-o', str(dll)] + objects
        command += [str(build / 'lib' / library) for library in LINK_LIBRARIES]
        handle.write(' '.join(command) + '\n')
        handle.flush()
        subprocess.run(command, check=True, stdout=handle, stderr=subprocess.STDOUT, timeout=300)
    return dll


def build_initrd(build, work, dll, ramdisk_arch, log):
    """Copy the build's ramdisk tree, add the program as a service, and pack it.

    The tree is build/deploy/hdd/system, the same input the build's own
    install_prepare target packs."""
    tree = work / 'initrd-root'
    shutil.copytree(build / 'deploy/hdd/system', tree, symlinks=True)
    shutil.copyfile(dll, tree / 'services' / f'{PROGRAM_NAME}.dll')
    shutil.copyfile(PROGRAM_DIRECTORY / f'{PROGRAM_NAME}.yaml', tree / 'services' / f'{PROGRAM_NAME}.yaml')
    initrd = work / 'initrd.mos'
    with log.open('w') as handle:
        subprocess.run([str(build / 'tools/host/bin/mkvafs'), '--arch', ramdisk_arch, '--compression', 'aplib',
                        '--out', str(initrd), str(tree)],
                       check=True, stdout=handle, stderr=subprocess.STDOUT, timeout=300)
    return initrd


def rpi_model(model_text, data_size):
    """The Raspberry Pi model with another size for the vali-data partition."""
    head, separator, data = model_text.partition('  - label: vali-data\n')
    if not separator:
        raise SystemExit('the Raspberry Pi model has no vali-data partition')
    data, count = re.subn(r'(\n    size: )\S+', rf'\g<1>{data_size}', '\n' + data, count=1)
    if count != 1:
        raise SystemExit('the vali-data partition of the Raspberry Pi model has no size')
    return head + separator + data[1:]


def stage_generic(build, stage, initrd):
    """The files the generic GPT models read, with the new ramdisk."""
    shutil.copytree(build / 'boot-assets', stage / 'boot-assets', symlinks=True)
    shutil.copytree(build / 'deploy', stage / 'deploy', symlinks=True)
    shutil.copyfile(initrd, stage / 'deploy/hdd/boot/initrd.mos')


def stage_rpi(build, stage, initrd, log):
    """The files the Raspberry Pi model reads, with the new ramdisk.

    The firmware loads boot-payload.bin, which holds phoenix and the ramdisk.
    It is packed again with tools/rpi/bundle.py, as the build's
    install_prepare target does."""
    shutil.copytree(build / 'boot-assets', stage / 'boot-assets', symlinks=True)
    shutil.copytree(build / 'deploy/hdd/system', stage / 'deploy/hdd/system', symlinks=True)
    shutil.copyfile(initrd, stage / 'boot-assets/initrd.mos')
    with log.open('w') as handle:
        subprocess.run(['python3', str(ROOT / 'tools/rpi/bundle.py'), '--phoenix', str(build / 'bin/phoenix.mos'),
                        '--ramdisk', str(initrd), '--output', str(stage / 'boot-assets/boot-payload.bin')],
                       check=True, stdout=handle, stderr=subprocess.STDOUT, timeout=300)


def check_output(output, build):
    """Never write build/disk.img or any image the OS build itself makes."""
    resolved = output.resolve()
    for directory in {build.resolve(), (ROOT / 'build').resolve()}:
        if resolved == directory / 'disk.img':
            raise SystemExit(f'refusing to write {resolved}: that is the build\'s own image')
    if resolved.suffix != '.img':
        raise SystemExit('--output must end in .img')


def build_image(build, output, arguments, mkcdk=None, size=None, data_size=RPI_DATA_SIZE, keep_work=None):
    """Build the program and the test image. Returns the manifest that is
    also written to <output>.json."""
    build = build.resolve()
    output = output.resolve()
    check_output(output, build)
    cache = read_cache(build)
    rpi = cache['VALI_PLATFORM'] == 'rpi'
    arch = cache['VALI_ARCH']
    ramdisk_arch = 'arm64' if arch == 'aarch64' else arch
    mkcdk = sdhci_card.find_mkcdk(mkcdk)
    if rpi:
        model_source = ROOT / 'cmake/models/vali-rpi-gpt.yaml'
        size = size or RPI_IMAGE_SIZE
    elif arch == 'aarch64':
        model_source = ROOT / 'cmake/models/vali-arm64-gpt.yaml'
        size = size or GENERIC_IMAGE_SIZE
    else:
        model_source = ROOT / 'cmake/models/vali-gpt.yaml'
        size = size or GENERIC_IMAGE_SIZE

    output.parent.mkdir(parents=True, exist_ok=True)
    logs = output.with_suffix('.logs')
    logs.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='vali-sdtransfer-', dir=keep_work) as directory:
        work = Path(directory)
        dll = build_program(build, work, arguments, logs / 'program.log')
        initrd = build_initrd(build, work, dll, ramdisk_arch, logs / 'mkvafs.log')
        stage = work / 'stage'
        stage.mkdir()
        model = work / 'model.yaml'
        if rpi:
            stage_rpi(build, stage, initrd, logs / 'bundle.log')
            model.write_text(rpi_model(model_source.read_text(), data_size))
        else:
            stage_generic(build, stage, initrd)
            model.write_text(sdhci_card.strip_data_partition(model_source.read_text()))
        if output.exists():
            output.unlink()
        sdhci_card.run_mkcdk(stage, output, size, model, logs / 'mkcdk.log', mkcdk)
        # mkcdk can report a partition that does not fit and still exit
        # with 0, leaving an empty image behind.
        if not output.exists() or output.stat().st_size == 0:
            raise SystemExit(f'mkcdk did not build {output}, see {logs / "mkcdk.log"}')
        shutil.copyfile(model, logs / 'model.yaml')
        shutil.copyfile(dll, logs / f'{PROGRAM_NAME}.dll')
        manifest = {
            'image': str(output),
            'image_bytes': output.stat().st_size,
            'image_sha256': sha256(output),
            'build': str(build),
            'arch': arch,
            'platform': cache['VALI_PLATFORM'] + cache.get('VALI_PLATFORM_VARIANT', ''),
            'arguments': arguments,
            'program_sha256': sha256(dll),
            'initrd_sha256': sha256(initrd),
            'model': model_source.name,
            'vali_data': f'vali-data resized to {data_size}' if rpi else 'removed, the SD card provides /data',
            'mkcdk': mkcdk,
            'mkcdk_version': sdhci_card.mkcdk_version(mkcdk),
        }
    output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', type=Path, required=True, help='Built OS build directory (make and install_prepare)')
    parser.add_argument('--output', type=Path, required=True, help='Image to write, ending in .img')
    parser.add_argument('--arguments', default='', help='Argument line built into the program (see sdtransfer.c)')
    parser.add_argument('--mkcdk', type=Path, help='mkcdk executable. Defaults to mkcdk from PATH')
    parser.add_argument('--size', help=f'Image size. Default {GENERIC_IMAGE_SIZE}, or {RPI_IMAGE_SIZE} for the Pi')
    parser.add_argument('--data-size', default=RPI_DATA_SIZE, help='Size of the Pi image\'s vali-data partition')
    args = parser.parse_args()
    manifest = build_image(args.build, args.output, args.arguments, args.mkcdk, args.size, args.data_size)
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
