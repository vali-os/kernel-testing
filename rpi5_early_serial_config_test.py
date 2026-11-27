#!/usr/bin/env python3
"""Check the CMake rule of VALI_RPI5_EARLY_SERIAL.

Configures the tree in scratch build directories and checks that:
- the option is refused with a clear message for Raspberry Pi variant 4 and
  for the generic AArch64 platform,
- a variant 5 build with the option stages config.txt with enable_rp1_uart=1
  and pciex4_reset=0 in its [pi5] section, and passes
  __OSCONFIG_RPI5_EARLY_SERIAL to the kernel,
- a variant 5 build without the option stages enable_rp1_uart=0 and
  pciex4_reset=1, and does not pass the define,
- both variant 5 builds load the uart0-pi5 overlay in their [pi5] section,
- VALI_RPI5_EARLY_SERIAL_ADDRESS reaches the kernel flags as
  __OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS=<value>ULL, with the default
  0x1c00030000 and with a value given on the command line,
- an address without the 0x prefix, with a non-hex digit, of zero or of 48
  bits or more is refused with a clear message.

Nothing is built. config.txt is staged with the rpi-firmware install component,
the same step install_prepare runs. VALICC must point at the Vali compiler root
(default /usr/local/valicc).
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
# The option define is also the start of the address define, so match it as
# a whole flag.
DEFINE = re.compile(r'-D__OSCONFIG_RPI5_EARLY_SERIAL(\s|$)')
ADDRESS_DEFINE = re.compile(r'-D__OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS=(\S+)')
DEFAULT_ADDRESS = '0x1c00030000'
# The [pi5] line that enables RP1 UART0 for the PL011 driver.
UART0_OVERLAY = 'dtoverlay=uart0-pi5'
REFUSAL = 'VALI_RPI5_EARLY_SERIAL requires VALI_ARCH=aarch64, VALI_PLATFORM=rpi and VALI_PLATFORM_VARIANT=5'
ADDRESS_REFUSAL = 'VALI_RPI5_EARLY_SERIAL_ADDRESS must'


def configure(build, platform, variant, early_serial, address=None):
    command = ['cmake', '-S', str(ROOT), '-B', str(build), '-DVALI_ARCH=aarch64',
               f'-DVALI_PLATFORM={platform}', f'-DVALI_PLATFORM_VARIANT={variant}',
               '-DCMAKE_BUILD_TYPE=Release', f'-DVALI_RPI5_EARLY_SERIAL={"ON" if early_serial else "OFF"}']
    if address is not None:
        command.append(f'-DVALI_RPI5_EARLY_SERIAL_ADDRESS={address}')
    return subprocess.run(command, capture_output=True, text=True, timeout=600)


def pi5_section(config):
    """Returns the lines of the [pi5] section, without comments and blank lines."""
    match = re.search(r'^\[pi5\]\n(.*?)^\[', config, re.S | re.M)
    assert match, 'config.txt has no [pi5] section'
    return [line for line in match.group(1).splitlines() if line and not line.startswith('#')]


def kernel_flags(build):
    """Returns the compile flags CMake wrote for the kernel sources."""
    flags = list((build / 'kernel').rglob('flags.make'))
    assert flags, f'no kernel flags.make below {build}'
    return '\n'.join(path.read_text() for path in flags)


def check_refused(work, name, platform, variant, address=None, message=REFUSAL):
    build = work / name
    completed = configure(build, platform, variant, True, address)
    output = completed.stdout + completed.stderr
    if completed.returncode == 0:
        raise AssertionError(f'{name}: configure accepted the settings')
    # CMake wraps long messages, so compare the text without line breaks.
    if message not in ' '.join(output.split()):
        raise AssertionError(f'{name}: configure failed without the expected message:\n{output}')
    print(f'Refused as expected: {name}', flush=True)


def check_address(build, name, expected):
    """Checks the address define in the kernel flags. expected None means no define."""
    values = set(ADDRESS_DEFINE.findall(kernel_flags(build)))
    wanted = set() if expected is None else {expected + 'ULL'}
    if values != wanted:
        raise AssertionError(f'{name}: kernel address defines are {sorted(values)}, expected {sorted(wanted)}')


def check_accepted(work, name, address):
    build = work / name
    completed = configure(build, 'rpi', '5', True, address)
    if completed.returncode != 0:
        raise AssertionError(f'{name}: configure failed:\n{completed.stdout}{completed.stderr}')
    check_address(build, name, address)
    print(f'Accepted as expected: {name} ({address})', flush=True)


def check_staged(work, name, early_serial):
    build = work / name
    completed = configure(build, 'rpi', '5', early_serial)
    if completed.returncode != 0:
        raise AssertionError(f'{name}: configure failed:\n{completed.stdout}{completed.stderr}')

    completed = subprocess.run(['cmake', '--install', str(build), '--component', 'rpi-firmware'],
                               capture_output=True, text=True, timeout=600)
    if completed.returncode != 0:
        raise AssertionError(f'{name}: staging failed:\n{completed.stdout}{completed.stderr}')

    staged = build / 'boot-assets'
    config = (staged / 'config.txt').read_text()
    # Both builds load the uart0-pi5 overlay, which enables RP1 UART0 for the
    # PL011 driver. The early serial build never starts RP1, so there the
    # port stays with the kernel.
    # pciex4_reset=0 keeps the firmware's PCIe setup of RP1, so the early
    # console can reach RP1 UART0.
    expected = 'enable_rp1_uart=1' if early_serial else 'enable_rp1_uart=0'
    reset = 'pciex4_reset=0' if early_serial else 'pciex4_reset=1'
    if pi5_section(config) != [expected, reset, UART0_OVERLAY]:
        raise AssertionError(f'{name}: [pi5] section is {pi5_section(config)}, '
                             f'expected [{expected!r}, {reset!r}, {UART0_OVERLAY!r}]')
    if '@' in config:
        raise AssertionError(f'{name}: config.txt still holds a template placeholder')
    if (staged / 'config.txt.in').exists():
        raise AssertionError(f'{name}: the template was staged next to config.txt')

    if bool(DEFINE.search(kernel_flags(build))) != early_serial:
        raise AssertionError(f'{name}: kernel flags {"lack" if early_serial else "contain"} '
                             '-D__OSCONFIG_RPI5_EARLY_SERIAL')
    check_address(build, name, DEFAULT_ADDRESS if early_serial else None)
    print(f'Staged as expected: {name} ({expected})', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work-dir', type=pathlib.Path,
                        help='directory for the scratch build trees (default: a temporary directory)')
    args = parser.parse_args()
    os.environ.setdefault('VALICC', '/usr/local/valicc')

    with tempfile.TemporaryDirectory(prefix='vali-rpi5-serial-', dir=args.work_dir) as directory:
        work = pathlib.Path(directory)
        check_refused(work, 'rpi4-with-option', 'rpi', '4')
        check_refused(work, 'generic-with-option', 'generic', '')
        check_staged(work, 'rpi5-with-option', True)
        check_staged(work, 'rpi5-without-option', False)
        check_accepted(work, 'rpi5-custom-address', '0x1f00030000')
        check_accepted(work, 'rpi5-largest-address', '0x0000ffffffffffff')
        not_hex = ADDRESS_REFUSAL + ' be a hexadecimal number with the 0x prefix'
        for name, address, message in [
                ('no-prefix', '1c00030000', not_hex),
                ('not-hex', '0x1c0003g000', not_hex),
                ('zero', '0x0', ADDRESS_REFUSAL + ' not be zero'),
                ('zero-digits', '0x0000', ADDRESS_REFUSAL + ' not be zero'),
                ('above-48-bits', '0x1000000000000', ADDRESS_REFUSAL + ' be below 0x1000000000000')]:
            check_refused(work, f'address-{name}', 'rpi', '5', address, message)


if __name__ == '__main__':
    main()
