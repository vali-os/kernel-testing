#!/usr/bin/env python3
"""Boot the Pi loader built with early serial output and check what it shows.

A VALI_RPI5_EARLY_SERIAL loader writes to RP1 UART0 on a Raspberry Pi 5, and
shows its progress on the ACT LED through the SoC's always-on GPIO block.
QEMU has no Pi 5 machine, so these tests build the same loader code for the
raspi4b machine:

* The early serial address is the machine's PL011 (UART0 at 0xfe201000). The
  loader only reads the PL011 ID registers and sends bytes, and RP1 UART0 is a
  PL011 too, so the code under test is the same.
* The LED GPIO bank is unused low RAM (LED_BANK), which the loader keeps as
  firmware memory. gdb reads the DATA and IODIR words there to see the LED.
* The list of other serial addresses (RPI_EARLY_SERIAL_CANDIDATES) is empty,
  as the RP1 addresses are not mapped on raspi4b. One build lists the real
  PL011 there and builds with a wrong address, to test the fallback.

Each test boots kernel8.img with QEMU's -kernel loader, which enters the
loader at EL2 with the MMU off, as the Pi firmware does. gdb stops the run
where the loader blinks its stop code (RpiLedBlinkForever), checks the code
and the LED, and quits QEMU. The serial output goes to a file.
"""
import pathlib
import subprocess
import tempfile
import unittest

from test_platform import fixture
from test_transfer import build_loader, build_probe_kernel, tool

# PL011 UART0 of the QEMU raspi4b machine (peripheral base 0xfe000000).
RASPI4B_UART0 = '0xfe201000'

# Stand-in for the GPIO bank of the ACT LED. QEMU's raspi4b boot code uses
# only the first 0x400 bytes of RAM, and the loader reserves everything below
# its image at 0x80000.
LED_BANK = 0x1000
LED_DATA = LED_BANK + 0x04
LED_IODIR = LED_BANK + 0x08
LED_BIT = 1 << 9

# RAM that reads back zero, used as a serial address where no PL011 answers.
NO_UART = '0x3000'

# How long one gdb run may take.
BOOT_TIMEOUT = 30


def led_check(lit):
    """gdb commands that check the LED pin is an output and lit or off.
    A high level lights the LED, so a lit LED has its DATA bit set."""
    data = LED_BIT if lit else 0
    return [
        f'python assert int(gdb.parse_and_eval("*(unsigned int*){LED_IODIR}")) & {LED_BIT} == 0',
        f'python assert int(gdb.parse_and_eval("*(unsigned int*){LED_DATA}")) & {LED_BIT} == {data}',
    ]


class EarlySerialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.qemu = tool('qemu-system-aarch64')
        cls.gdb = tool('gdb')
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-rpi-serial-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.pe = build_probe_kernel(cls.directory)
        led = '-DRPI_EARLY_LED_ADDRESS=' + hex(LED_BANK)
        no_candidates = '-DRPI_EARLY_SERIAL_CANDIDATES='
        cls.build = cls.directory / 'build'
        build_loader(cls.build, cls.pe, [
            '-DVALI_RPI5_EARLY_SERIAL=ON',
            '-DVALI_RPI5_EARLY_SERIAL_ADDRESS=' + RASPI4B_UART0, led, no_candidates,
        ])
        cls.no_uart_build = cls.directory / 'build-no-uart'
        build_loader(cls.no_uart_build, cls.pe, [
            '-DVALI_RPI5_EARLY_SERIAL=ON',
            '-DVALI_RPI5_EARLY_SERIAL_ADDRESS=' + NO_UART, led, no_candidates,
        ])
        cls.fallback_build = cls.directory / 'build-fallback'
        build_loader(cls.fallback_build, cls.pe, [
            '-DVALI_RPI5_EARLY_SERIAL=ON',
            '-DVALI_RPI5_EARLY_SERIAL_ADDRESS=' + NO_UART, led,
            '-DRPI_EARLY_SERIAL_CANDIDATES=' + NO_UART + ',' + RASPI4B_UART0,
        ])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_gdb(self, dtb_data, commands, build=None):
        """Boot the loader in build (the default build when None) under gdb,
        run the gdb commands after the stop at _rpi_initialize, and return the
        serial output. PASS is printed and QEMU is quit after the commands."""
        build = build or self.build
        dtb = self.directory / 'input.dtb'
        dtb.write_bytes(dtb_data)
        output = self.directory / 'serial.txt'
        output.unlink(missing_ok=True)
        qemu = ' '.join([
            self.qemu, '-M', 'raspi4b', '-m', '2G', '-smp', '4',
            '-kernel', str(build / 'kernel8.img'), '-dtb', str(dtb),
            '-display', 'none', '-monitor', 'none', '-no-reboot',
            '-serial', 'file:' + str(output), '-S', '-gdb', 'stdio',
        ])
        script = self.directory / 'run.gdb'
        script.write_text('\n'.join([
            f'target remote | {qemu}',
            'break _rpi_initialize', 'continue', 'delete 1',
            *commands,
            'python print("PASS")', 'monitor quit',
        ]) + '\n')
        result = subprocess.run([self.gdb, '-q', '-batch', str(build / 'rpi-loader.elf'),
                                 '-x', str(script)], capture_output=True, text=True,
                                timeout=BOOT_TIMEOUT)
        self.assertIn('PASS', result.stdout, result.stdout + result.stderr)
        # Read bytes, as read_text would turn "\r\n" into "\n".
        return output.read_bytes().decode(errors='replace')

    def test_handoff_and_kernel_exception(self):
        # The probe kernel ends with BRK #0x42. The loader's vector table is
        # still in VBAR_EL1 then, so the loader prints the exception and
        # blinks fast. The LED is lit in the loader and off in the kernel.
        text = self.run_gdb(fixture(initrd=None), [
            'break RpiLoaderInit', 'continue', *led_check(lit=True),
            'break RpiLedBlinkForever', 'continue',
            'python assert int(gdb.parse_and_eval("$x0")) == 0',
            *led_check(lit=False),
        ])
        lines = [line for line in text.replace('\r\n', '\n').split('\n') if line]
        self.assertTrue(lines[0].startswith('vali loader: started, device tree at 0x'), text)
        self.assertEqual(lines[1:7], [
            'vali loader: validate payload',
            'vali loader: discover platform',
            'vali loader: load kernel',
            'vali loader: load resources',
            'vali loader: build boot contract',
            'vali loader: transfer to kernel',
        ], text)
        self.assertRegex(lines[7], r'^vali loader: kernel entry 0x[0-9a-f]{16}, '
                                   r'VBoot 0x[0-9a-f]{16}, stack top 0x[0-9a-f]{16}$')
        self.assertEqual(lines[8], 'vali loader: running at EL1, entering the kernel', text)
        # ESR_EL1 for BRK #0x42 at EL1: EC 0x3c, IL set, immediate 0x42.
        self.assertRegex(lines[9], r'^vali loader: exception, ESR=0x00000000f2000042 '
                                   r'ELR=0x[0-9a-f]{16} FAR=0x[0-9a-f]{16}$')
        self.assertEqual(len(lines), 10, text)
        # Every line ends with a carriage return before the line feed.
        self.assertNotRegex(text, r'[^\r]\n')

    def test_stop_reports_stage_and_status(self):
        # A Pi 5 tree fails platform discovery in a Pi 4 loader. QEMU adds its
        # own memory node to the tree, so a tree without RAM can't be used.
        # The LED blinks the stage number, 2 for discover platform.
        text = self.run_gdb(fixture(board=5, initrd=None), [
            'break RpiLedBlinkForever', 'continue',
            'python assert int(gdb.parse_and_eval("$x0")) == 2',
        ])
        self.assertIn('vali loader: discover platform\r\n', text)
        self.assertIn("vali loader: stopped in stage 'discover platform' "
                      "with status 'invalid platform'\r\n", text)
        self.assertNotIn('vali loader: load kernel', text)

    def test_entry_check_reports_reason(self):
        # The assembly entry checks run before any C code. A small seed program
        # turns the data cache on (SCTLR_EL2.C, bit 2) and then enters the
        # loader, so the cache check stops it through RPI_STOP. The register is
        # set by real instructions, as QEMU's gdb stub can't write it.
        seed_source = self.directory / 'cache_on.S'
        seed_source.write_text('\n'.join([
            '.text',
            'mrs x9, SCTLR_EL2',
            'orr x9, x9, #4',
            'msr SCTLR_EL2, x9',
            'isb',
            'br x16',
        ]) + '\n')
        seed_object = self.directory / 'cache_on.o'
        seed = self.directory / 'cache_on.bin'
        subprocess.run([tool('clang'), '--target=aarch64-none-elf', '-c', str(seed_source),
                        '-o', str(seed_object)], check=True)
        subprocess.run([tool('llvm-objcopy'), '-O', 'binary', '--only-section=.text',
                        str(seed_object), str(seed)], check=True)

        text = self.run_gdb(fixture(initrd=None), [
            f'restore {seed} binary 0x70000',
            'set $x16 = &_rpi_initialize', 'set $pc = 0x70000',
            'break RpiLedBlinkForever', 'continue',
            'python assert int(gdb.parse_and_eval("$x0")) == 0',
        ])
        self.assertEqual(text, '\r\nvali loader: stopped, the data cache is on at entry\r\n')

    def test_missing_port_blinks_code(self):
        # No PL011 answers at the serial address of this build. The loader
        # stops before its first line, sends nothing and blinks code 7.
        text = self.run_gdb(fixture(initrd=None), [
            'break RpiLedBlinkForever', 'continue',
            'python assert int(gdb.parse_and_eval("$x0")) == 7',
            'python assert int(gdb.parse_and_eval("g_rpi_serial_stalled")) == 1',
        ], build=self.no_uart_build)
        self.assertEqual(text, '')

    def test_other_address_is_used_and_named(self):
        # No PL011 answers at the build address, but the second candidate is
        # the real port. The loader prints there, names the address to build
        # with, and boots on to the probe kernel.
        text = self.run_gdb(fixture(initrd=None), [
            'break RpiLedBlinkForever', 'continue',
            'python assert int(gdb.parse_and_eval("$x0")) == 0',
            f'python assert int(gdb.parse_and_eval("g_rpi_serial_address")) == {RASPI4B_UART0}',
        ], build=self.fallback_build)
        lines = [line for line in text.replace('\r\n', '\n').split('\n') if line]
        self.assertTrue(lines[0].startswith('vali loader: started'), text)
        self.assertEqual(lines[1], 'vali loader: no PL011 at 0x0000000000003000, using '
                                   '0x00000000fe201000. The kernel will print nothing, rebuild '
                                   'with VALI_RPI5_EARLY_SERIAL_ADDRESS=0x00000000fe201000', text)
        self.assertEqual(lines[-2], 'vali loader: running at EL1, entering the kernel', text)
        self.assertTrue(lines[-1].startswith('vali loader: exception, ESR=0x00000000f2000042'), text)


if __name__ == '__main__':
    unittest.main()
