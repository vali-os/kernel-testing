#!/usr/bin/env python3
"""Check the ARM64 device tree console choice.

console_test.c is built twice from the real kernel sources: once as a normal
build, and once with __OSCONFIG_RPI5_EARLY_SERIAL (VALI_RPI5_EARLY_SERIAL=ON)
and a test value for __OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS. Each build runs
its own list of checks on the bundled Raspberry Pi trees and on small trees
built here. With --mutation-check, changed copies of the kernel sources must
make at least one check report a mismatch.
"""
import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / 'boot/rpi/firmware'
CONSOLE_DIRECTORY = ROOT / 'kernel/arch/aarch64/devicetree'
DEVICETREE_DIRECTORY = ROOT / 'kernel/devicetree'

# The early serial build gets an address that is not the default
# 0x1c00030000, so a kernel that ignores the build setting is caught.
EARLY_SERIAL_ADDRESS = '0xabc0030000'
EARLY_SERIAL_DEFINES = ['-D__OSCONFIG_RPI5_EARLY_SERIAL',
                        f'-D__OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS={EARLY_SERIAL_ADDRESS}ULL']

# RP1 UART0 in the BCM2712 trees sits below the PCI bus of a PCIe host. The
# kernel's translation refuses buses with three address cells, in both builds.
RP1_UART0 = '/axi/pcie@1000120000/rp1/serial@30000'
DEBUG_UART = '/soc@107c000000/serial@7d001000'
DEBUG_UART_ADDRESS = '0x107d001000'

# Paths of the small trees from fixture().
FIXTURE_UART = '/axi/pcie@0/rp1/serial@30000'
FIXTURE_PCI_ENDPOINT = '/axi/pcie@0/endpoint@0,0'
FIXTURE_SOC_UART = '/soc/serial@1000'


def fixture(mode='normal'):
    """Builds a small tree with a PCI bus, in the layout of the BCM2712 trees.

    mode changes one detail: 'no-aliases' leaves out the /aliases node,
    'no-ranges' leaves out the ranges of the soc bus, 'short-reg' gives the
    stdout-path port a 0x10 byte register range, 'chosen-disabled'
    disables the stdout-path port, and 'chosen-pci' makes stdout-path name
    the enabled PL011 below the PCI bus.
    """
    strings = bytearray()
    data = bytearray()

    def cells(*values):
        return struct.pack('>' + 'I' * len(values), *values)

    def begin(name):
        data.extend(cells(1) + name.encode() + b'\0')
        data.extend(bytes(-len(data) % 4))

    def end():
        data.extend(cells(2))

    def prop(name, value):
        data.extend(cells(3, len(value), len(strings)) + value)
        data.extend(bytes(-len(data) % 4))
        strings.extend(name.encode() + b'\0')

    def bus(name, address_cells, ranges):
        begin(name)
        prop('#address-cells', cells(address_cells))
        prop('#size-cells', cells(2))
        prop('ranges', ranges)

    begin('')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(2))

    if mode != 'no-aliases':
        begin('aliases')
        prop('serial10', b'/soc/serial@1000\0')
        prop('serial0', (FIXTURE_UART + '\0').encode())
        end()

    begin('chosen')
    prop('stdout-path', b'serial0:115200n8\0' if mode == 'chosen-pci' else b'serial10:115200n8\0')
    end()

    # One cell on the bus side, two on the CPU side: 0x0 maps to 0x10_7d000000.
    begin('soc')
    prop('#address-cells', cells(1))
    prop('#size-cells', cells(1))
    if mode != 'no-ranges':
        prop('ranges', cells(0, 0x10, 0x7d000000, 0x10000))
    begin('serial@1000')
    prop('compatible', b'arm,pl011-axi\0arm,pl011\0arm,primecell\0')
    prop('reg', cells(0x1000, 0x10 if mode == 'short-reg' else 0x200))
    if mode == 'chosen-disabled':
        prop('status', b'disabled\0')
    end()
    end()

    # An empty ranges maps the axi bus one to one onto the root bus.
    bus('axi', 2, b'')

    # PCI bus, <flags pci.hi pci.lo  cpu.hi cpu.lo  size.hi size.lo>:
    # 32-bit memory 0x0 -> 0x1f_00000000.
    bus('pcie@0', 3, cells(0x02000000, 0, 0, 0x1f, 0, 0, 0x10000000))

    # The rp1 bus of the real trees: 0xc0_40000000 -> PCI memory 0x0.
    bus('rp1', 2, cells(0xc0, 0x40000000, 0x02000000, 0, 0, 0, 0x410000))
    begin('serial@30000')
    if mode == 'chosen-pci':
        prop('compatible', b'arm,pl011-axi\0arm,pl011\0arm,primecell\0')
    else:
        prop('compatible', b'arm,pl011-axi\0')
        prop('status', b'disabled\0')
    prop('reg', cells(0xc0, 0x40030000, 0, 0x100))
    end()
    end()

    # A PCI device with a reg entry directly on the PCI bus (a config space address).
    begin('endpoint@0,0')
    prop('reg', cells(0, 0, 0, 0, 0))
    end()
    end()

    end()
    end()
    data.extend(cells(9))
    strings_offset = 56 + len(data)
    header = cells(0xd00dfeed, strings_offset + len(strings), 56, strings_offset,
                   40, 17, 16, 0, len(strings), len(data))
    return header + bytes(16) + data + strings


def register(path, status, address='0', length='0'):
    return ('register', path, '0', status, address, length)


def console(status, path='-', address='0', length='0'):
    return ('console', status, path, address, length)


def build_console():
    """The console of the early serial build: the build address, 0x100 bytes."""
    return console('OS_EOK', 'build', EARLY_SERIAL_ADDRESS, '0x100')


def bcm2712_cases(early_serial):
    # Both builds refuse the register read through the PCI bus.
    cases = [
        register(RP1_UART0, 'OS_ENOTSUPPORTED'),
        register('serial10', 'OS_EOK', DEBUG_UART_ADDRESS, '0x200'),
        register(DEBUG_UART, 'OS_EOK', DEBUG_UART_ADDRESS, '0x200'),
    ]
    if early_serial:
        return [build_console()] + cases
    # The normal build leaves the serial port of these boards to the PL011
    # driver, so the debug UART named by stdout-path is not used.
    return [console('OS_EOK', 'driver')] + cases


def bcm2711_cases(early_serial):
    # The Pi 4 stdout-path names the mini UART until the firmware applies
    # disable-bt, so the normal build finds no PL011 in the bundled file. The
    # early serial build does not read the tree at all. Register reads are the
    # same in both builds, and the reg entries on its PCI bus stay refused.
    cases = [
        register('/soc/serial@7e201000', 'OS_EOK', '0xfe201000', '0x200'),
        register('/scb/pcie@7d500000', 'OS_EOK', '0xfd500000', '0x9310'),
        register('/scb/pcie@7d500000/pci@0,0', 'OS_ENOTSUPPORTED'),
        register('/scb/pcie@7d500000/pci@0,0/usb@0,0', 'OS_ENOTSUPPORTED'),
    ]
    return [build_console() if early_serial else console('OS_EOK')] + cases


def fixture_cases(early_serial):
    registers = [
        register(FIXTURE_UART, 'OS_ENOTSUPPORTED'),
        register(FIXTURE_PCI_ENDPOINT, 'OS_ENOTSUPPORTED'),
        register(FIXTURE_SOC_UART, 'OS_EOK', '0x107d001000', '0x200'),
    ]
    if early_serial:
        # The tree never matters, so every fixture gives the build address.
        return {
            'normal': [build_console()] + registers,
            'no-aliases': [build_console()],
            'no-ranges': [build_console()],
            'short-reg': [build_console()],
            'chosen-disabled': [build_console()],
            'chosen-pci': [build_console()],
        }
    return {
        'normal': [console('OS_EOK', 'serial10:115200n8', '0x107d001000', '0x200')] + registers,
        # stdout-path names the alias serial10, which is gone with /aliases.
        'no-aliases': [console('OS_EOK')],
        # A broken register range on the stdout-path port is an error.
        'no-ranges': [console('OS_ENOENT')],
        'short-reg': [console('OS_EINVALPARAMS')],
        'chosen-disabled': [console('OS_EOK')],
        # The kernel can't translate an address through the PCI bus. The tree
        # is valid, so there is no console and the boot goes on.
        'chosen-pci': [console('OS_EOK', 'unreachable'), register(FIXTURE_UART, 'OS_ENOTSUPPORTED')],
    }


def all_cases(directory, early_serial):
    """Returns (dtb path, arguments) for every check of one build."""
    cases = []
    # Every bundled BCM2712 tree, with the CM5 IO board and the Pi 5 B first.
    names = ['bcm2712-rpi-cm5-cm5io.dtb', 'bcm2712-rpi-5-b.dtb']
    names += sorted(path.name for path in FIRMWARE.glob('bcm2712*.dtb') if path.name not in names)
    for name in names:
        cases += [(FIRMWARE / name, case) for case in bcm2712_cases(early_serial)]
    cases += [(FIRMWARE / 'bcm2711-rpi-4-b.dtb', case) for case in bcm2711_cases(early_serial)]
    for mode, mode_cases in fixture_cases(early_serial).items():
        path = directory / f'fixture-{mode}.dtb'
        path.write_bytes(fixture(mode))
        cases += [(path, case) for case in mode_cases]
    return cases


def build(output, console_directory, devicetree_directory, early_serial):
    binary = output / ('console-early' if early_serial else 'console-normal')
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-fms-extensions', '-Wall', '-Wextra',
               '-Werror', '-DKERNELAPI=', '-DKERNELABI=', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-g']
    if early_serial:
        command += EARLY_SERIAL_DEFINES
    for include in ['kernel/include', 'librt/libfdt/include', 'librt/libos/include', 'librt/libc/include']:
        command.extend(['-I', str(ROOT / include)])
    command.extend(['-I', str(console_directory)])
    command += [str(ROOT / 'testing/devicetree/console_test.c'), str(ROOT / 'librt/libfdt/parser.c'),
                str(devicetree_directory / 'tree.c'), str(devicetree_directory / 'resources.c'),
                str(console_directory / 'serial-console.c')]
    subprocess.run(command + ['-o', str(binary)], check=True)
    return binary


def run_cases(binary, cases):
    """Runs every check. Returns the first mismatch, or None when all pass.

    Any other failure, such as a sanitizer report or a usage error, raises,
    because it does not show that a check caught a change in behaviour.
    """
    environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
    for dtb, arguments in cases:
        completed = subprocess.run([str(binary), str(dtb), *arguments], capture_output=True,
                                   env=environment, timeout=60)
        output = completed.stdout.decode() + completed.stderr.decode()
        if completed.returncode == 0:
            continue
        if completed.returncode == 1 and 'mismatch' in completed.stdout.decode() \
                and 'Sanitizer' not in output and 'runtime error:' not in output:
            return f'{dtb.name} {" ".join(arguments)}: {output.strip()}'
        raise AssertionError(f'{dtb.name} {" ".join(arguments)} failed: {output}')
    return None


# Each mutation must make at least one check report a mismatch. The file is
# changed in a copy of its directory.
MUTATIONS = (
    ('accept a bus with three address cells', 'resources.c',
     '*addressCells > 2 || *sizeCells > 2', '*addressCells > 3 || *sizeCells > 2'),
    ('ignore the address the build was given', 'serial-console.c',
     'consoleOut->Address = __OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS;',
     'consoleOut->Address = 0x1c00030000ULL;'),
    ('give the early serial port a range too short for FR', 'serial-console.c',
     '#define __EARLY_SERIAL_LENGTH 0x100ULL', '#define __EARLY_SERIAL_LENGTH 0x10ULL'),
    ('depend on the aliases in the early serial build', 'serial-console.c',
     '    (void)tree;\n',
     '    if (!DeviceTreeFindPath(tree, "/aliases")) {\n        return OS_ENOENT;\n    }\n'),
    ('leave the build setting out of the console log line', 'serial-console.c',
     '"RP1 UART0 console at 0x%llx (VALI_RPI5_EARLY_SERIAL_ADDRESS)"', '"RP1 UART0 console at 0x%llx"'),
    ('accept a disabled stdout-path port', 'serial-console.c',
     '!node || !DeviceTreeIsEnabled(node) || !DeviceTreeIsCompatible(node, "arm,pl011")',
     '!node || !DeviceTreeIsCompatible(node, "arm,pl011")'),
    ('ignore an unreadable stdout-path register range', 'serial-console.c',
     '    } else if (status != OS_EOK) {\n        return status;\n    }\n',
     '    } else if (status != OS_EOK) {\n        return OS_EOK;\n    }\n'),
    ('fail on a stdout-path port behind a PCI bus', 'serial-console.c',
     'if (status == OS_ENOTSUPPORTED) {', 'if (status == OS_ENOTSUPPORTED && 0) {'),
    ('treat every unreadable stdout-path register range as unreachable', 'serial-console.c',
     'if (status == OS_ENOTSUPPORTED) {', 'if (status != OS_EOK) {'),
    ('drop the warning for an unreachable stdout-path port', 'serial-console.c',
     '"%s: kernel serial console is off, the port is behind a bus the kernel can\'t translate (such as PCI)"',
     '"%s: kernel serial console is off"'),
    ('skip the register range length check', 'serial-console.c',
     'if (length < minimumLength) {', 'if (length < minimumLength / 0x1000) {'),
    ('keep the debug UART as the console on BCM2712 boards', 'serial-console.c',
     '    return DeviceTreeIsCompatible(root, __BCM2712_COMPATIBLE);',
     '    return 0;'),
    ('leave the console to the driver on every board', 'serial-console.c',
     '    return DeviceTreeIsCompatible(root, __BCM2712_COMPATIBLE);',
     '    return 1;'),
    ('drop the log line that says why BCM2712 has no console', 'serial-console.c',
     '"BCM2712: kernel serial console is off, syslogd prints the log through the PL011 driver "',
     '"BCM2712: kernel serial console is off "'),
    ('report a console from a tree without one', 'serial-console.c',
     '        memset(consoleOut, 0, sizeof(*consoleOut));\n        return OS_EOK;',
     '        memset(consoleOut, 0, sizeof(*consoleOut));\n        consoleOut->Node = tree->Root;\n'
     '        consoleOut->Path = "/";\n        return OS_EOK;'),
)


def run_build(output, console_directory, devicetree_directory, early_serial):
    binary = build(output, console_directory, devicetree_directory, early_serial)
    return run_cases(binary, all_cases(output, early_serial))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vali-dt-console-') as directory:
        output = pathlib.Path(directory)
        for early_serial in [False, True]:
            mismatch = run_build(output, CONSOLE_DIRECTORY, DEVICETREE_DIRECTORY, early_serial)
            if mismatch:
                raise AssertionError(mismatch)
            print('Console checks passed:', 'early serial build' if early_serial else 'normal build',
                  flush=True)
        if not args.mutation_check:
            return

        for name, file_name, before, after in MUTATIONS:
            mutant = output / 'mutant'
            if mutant.exists():
                shutil.rmtree(mutant)
            shutil.copytree(CONSOLE_DIRECTORY, mutant / 'console')
            shutil.copytree(DEVICETREE_DIRECTORY, mutant / 'devicetree')
            target = mutant / ('console' if file_name == 'serial-console.c' else 'devicetree') / file_name
            original = target.read_text()
            assert original.count(before) == 1, name
            target.write_text(original.replace(before, after))

            mismatch = None
            for early_serial in [False, True]:
                mismatch = run_build(output, mutant / 'console', mutant / 'devicetree', early_serial)
                if mismatch:
                    break
            if not mismatch:
                raise AssertionError(f'Mutation was not caught: {name}')
            reason = next(line for line in mismatch.splitlines() if 'mismatch' in line)
            print('Console mutation rejected:', name, '->', reason, flush=True)


if __name__ == '__main__':
    main()
