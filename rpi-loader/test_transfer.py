#!/usr/bin/env python3
"""Boot real packaged PE probes through the Pi wrapper and inspect the EL1 handoff."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test_platform import fixture

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOADER = ROOT / 'boot/rpi/loader'


def tool(name):
    result = shutil.which(name)
    bundled = pathlib.Path('/usr/local/valicc/bin') / name
    if result:
        return result
    if bundled.exists():
        return str(bundled)
    raise unittest.SkipTest(f'{name} is required for the handoff probe')


def build_probe_kernel(directory):
    """Build the handoff probe PE (transfer_entry.S and transfer_fixture.c)
    in directory and return its path. The probe checks the entry state and
    then stops with BRK #0x42, which the loader's vectors catch."""
    clang = tool('clang')
    objects = []
    for source in ['transfer_entry.S', 'transfer_fixture.c']:
        obj = directory / (source + '.obj')
        subprocess.run([clang, '--target=aarch64-pc-windows-msvc', '-ffreestanding',
                        '-fno-stack-protector', '-mgeneral-regs-only', '-mstrict-align', '-O2',
                        '-I', str(ROOT / 'boot/include'), '-c',
                        str(ROOT / 'testing/rpi-loader' / source), '-o', str(obj)], check=True)
        objects.append(str(obj))
    pe = directory / 'fixture.exe'
    subprocess.run([tool('lld-link'), '/entry:kentry', '/subsystem:native', '/nodefaultlib',
                    '/base:0x48000000', '/opt:noref', *objects, '/out:' + str(pe)], check=True)
    return pe


def build_loader(build, pe, options=()):
    """Build the Pi 4 loader in build with extra CMake options, pack pe behind
    it and return the path of the packed kernel8.img."""
    clang = tool('clang')
    subprocess.run(['cmake', '-S', str(LOADER), '-B', str(build),
                    '-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_ASM_COMPILER=' + clang,
                    '-DRPI_LD=' + tool('ld.lld'), '-DVALI_PLATFORM_VARIANT=4',
                    '-DCMAKE_BUILD_TYPE=Debug', *options], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(build), '-j4'], check=True, stdout=subprocess.DEVNULL)
    flat = build / 'rpi-loader.bin'
    subprocess.run([tool('llvm-objcopy'), '-O', 'binary',
                    str(build / 'rpi-loader.elf'), str(flat)], check=True)
    image = build / 'kernel8.img'
    subprocess.run(['python3', str(ROOT / 'tools/rpi/pack.py'),
                    '--loader', str(flat), '--kernel', str(pe),
                    '--output', str(image)], check=True)
    return image


class TransferTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clang = tool('clang')
        cls.gdb = tool('gdb')
        cls.qemu = tool('qemu-system-aarch64')
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-rpi-transfer-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.build = cls.directory / 'build'
        cls.dtb = cls.directory / 'fixture.dtb'
        cls.dtb.write_bytes(fixture(initrd=None))
        cls.seed = cls.directory / 'seed.bin'
        seed_obj = cls.directory / 'seed.o'
        subprocess.run([cls.clang, '--target=aarch64-none-elf', '-c',
                        str(ROOT / 'testing/rpi-loader/transfer_seed.S'),
                        '-o', str(seed_obj)], check=True)
        subprocess.run([tool('llvm-objcopy'), '-O', 'binary', '--only-section=.text',
                        str(seed_obj), str(cls.seed)], check=True)
        cls.pe = build_probe_kernel(cls.directory)
        build_loader(cls.build, cls.pe)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_probe(self, mode):
        commands = [
            f'target remote | {self.qemu} -M raspi4b -m 2G -smp 4 '
            f'-kernel {self.build}/kernel8.img -dtb {self.dtb} '
            '-display none -serial null -monitor none -S -gdb stdio',
            'break _rpi_initialize', 'continue',
        ]
        # Execute a firmware shim so both initial EL and inherited registers
        # are real CPU state, rather than debugger pseudo-register assignments.
        # RpiLoaderStop has a stack frame, and x0 changes after its prologue,
        # so its breakpoint goes on the first instruction (break *).
        commands += [f'restore {self.seed} binary 0x70000',
                     'set $x16 = &_rpi_initialize', f'set $x17 = {int(mode == "el1")}',
                     'set $pc = 0x70000', 'delete 1',
                     'break __JumpToKernel', 'break *RpiLoaderStop', 'break _rpi_stop', 'continue',
                     f'python assert (int(gdb.parse_and_eval("$cpsr")) & 0xf) == {5 if mode == "el1" else 9}',
                     'python assert int(gdb.parse_and_eval("$TPIDR_EL1")) == 0x1111',
                     f'python assert int(gdb.parse_and_eval("$SCTLR_EL{1 if mode == "el1" else 2}")) & 0x1000']
        if mode == 'invalid':
            # Check the C gate by re-entering it with an invalid published marker.
            commands += ['set g_context.BootInformation.Magic = 0',
                         'set $x0 = &g_context', 'set $pc = __PrepareJumpToKernel', 'continue',
                         'python assert int(gdb.parse_and_eval("$x0")) == 3',
                         'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&RpiLoaderStop"))']
        elif mode == 'return':
            # Inject RET into the loaded entry to verify LR is a safe stop
            # address. The production transfer still performs cache maintenance.
            commands += ['set *(unsigned int*)$x1 = 0xd65f03c0', 'set $saved_boot = $x0',
                         'set $saved_entry = $x1', 'break *$saved_entry', 'continue',
                         'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("$saved_entry"))',
                         'continue', 'python assert int(gdb.parse_and_eval("$x0")) == int(gdb.parse_and_eval("$saved_boot"))',
                         'python assert (int(gdb.parse_and_eval("$cpsr")) & 0xf) == 5',
                         'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&_rpi_stop"))']
        else:
            commands += ['continue', 'python assert int(gdb.parse_and_eval("$x0")) == 0x600d',
                         'python assert int(gdb.parse_and_eval("$ESR_EL1")) == 0xf2000042',
                         'python assert int(gdb.parse_and_eval("$pc")) == int(gdb.parse_and_eval("&_rpi_stop"))']
            if mode == 'el2':
                commands += ['python assert int(gdb.parse_and_eval("$HCR_EL2")) == 0x80000000',
                             'python assert int(gdb.parse_and_eval("$CNTHCTL_EL2")) == 3',
                             'python assert int(gdb.parse_and_eval("$CNTVOFF_EL2")) == 0',
                             'python assert int(gdb.parse_and_eval("$VPIDR_EL2")) == '
                             'int(gdb.parse_and_eval("*(unsigned long*)0x70200"))',
                             'python assert int(gdb.parse_and_eval("$VMPIDR_EL2")) == '
                             'int(gdb.parse_and_eval("*(unsigned long*)0x70208"))',
                             'python assert int(gdb.parse_and_eval("$MDCR_EL2")) == '
                             '(int(gdb.parse_and_eval("$PMCR_EL0")) >> 11) & 0x1f',
                             'python assert int(gdb.parse_and_eval("$MDCR_EL2")) != 0']
        commands += ['python print("HANDOFF PASS")', 'monitor quit']
        script = self.directory / 'probe.gdb'
        script.write_text('\n'.join(commands) + '\n')
        result = subprocess.run([self.gdb, '-q', '-batch', str(self.build / 'rpi-loader.elf'),
                                 '-x', str(script)], capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'LC_ALL': 'C'})
        self.assertIn('HANDOFF PASS', result.stdout, result.stdout + result.stderr)
        self.assertNotIn('AssertionError', result.stderr)

    def test_el2_handoff(self):
        self.run_probe('el2')

    def test_el1_handoff(self):
        self.run_probe('el1')

    def test_unexpected_kernel_return(self):
        self.run_probe('return')

    def test_invalid_contract_stops_before_transition(self):
        self.run_probe('invalid')


if __name__ == '__main__':
    unittest.main()
