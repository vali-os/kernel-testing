#!/usr/bin/env python3
"""Boot the real service stack and require EDU's device-generated PCI INTx test.

Build with VALI_PCI_INTX_SELFTEST=ON. A kernel checkpoint alone cannot pass:
Phoenix must start deviced, deviced must enumerate and bind EDU, and the driver
must receive and acknowledge all 32 hardware assertions through the public DDK.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--loader', type=Path, required=True)
parser.add_argument('--firmware', type=Path, required=True)
parser.add_argument('--lzss', type=Path, required=True)
parser.add_argument('--mkvafs', type=Path, required=True)
parser.add_argument('--output-dir', type=Path, required=True)
parser.add_argument('--arch', choices=['arm64', 'amd64'], default='arm64')
parser.add_argument('--gic-version', choices=[2, 3], type=int, default=3)
parser.add_argument('--bridge', action='store_true', help='Place EDU behind a PCIe root port')
parser.add_argument('--timeout', type=int, default=90)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
directory = args.output_dir.resolve()
root = directory / 'root'
esp = directory / 'esp'
for path in [root / 'bin', root / 'services', root / 'modules', esp / 'EFI/BOOT', esp / 'EFI/VALI']:
    path.mkdir(parents=True, exist_ok=True)
for filename in ['c.dll', 'gracht.dll']:
    shutil.copy2(args.build / 'bin' / filename, root / 'bin' / filename)
shutil.copy2(args.build / 'bin/deviced.dll', root / 'services/deviced.dll')
shutil.copy2(repo / 'services/deviced/deviced.yaml', root / 'services/deviced.yaml')
shutil.copy2(args.build / 'bin/edu-intx.dll', root / 'modules/edu-intx.dll')
shutil.copy2(repo / 'testing/modules/edu-intx/edu-intx.yaml', root / 'modules/edu-intx.yaml')
shutil.copy2(args.build / 'bin/phoenix.mos', esp / 'EFI/VALI/phoenix.mos')
loader_name = 'BOOTAA64.EFI' if args.arch == 'arm64' else 'BOOTX64.EFI'
shutil.copy2(args.loader, esp / 'EFI/BOOT' / loader_name)
subprocess.run([str(args.mkvafs.resolve()), '--arch', args.arch, '--out',
                str(esp / 'EFI/VALI/initrd.mos'), str(root)], check=True)
subprocess.run([str(args.lzss.resolve()), 'c', str(args.build / 'bin/kernel.mos'),
                str(esp / 'EFI/VALI/kernel.mos')], check=True)
serial = directory / 'serial.log'
serial.write_text('')
if args.arch == 'arm64':
    command = ['qemu-system-aarch64', '-M', f'virt,gic-version={args.gic_version},virtualization=on',
               '-cpu', 'max']
else:
    command = ['qemu-system-x86_64', '-M', 'q35', '-accel', 'tcg']
command += ['-m', '1024', '-smp', '4', '-bios', str(args.firmware.resolve()),
            '-drive', f'if=none,id=esp,format=raw,file=fat:{esp},readonly=on',
            '-device', 'virtio-blk-pci,drive=esp', '-display', 'none', '-monitor', 'none',
            '-serial', f'file:{serial}', '-no-reboot']
if args.bridge:
    command += ['-device', 'pcie-root-port,id=bridge,chassis=1,slot=1', '-device', 'edu,bus=bridge']
else:
    command += ['-device', 'edu']
with (directory / 'qemu.log').open('w') as output:
    process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            log = serial.read_text(errors='replace')
            if 'DebugPanic' in log or 'EDU INTx self-test: FAILED' in log:
                raise RuntimeError(f'Boot/INTx failed; see {serial}')
            if 'EDU INTx self-test: PASS (32 device-generated interrupts)' in log:
                print(f'{args.arch}: deviced enumeration and device-generated INTx PASS ({serial})')
                break
            if process.poll() is not None:
                raise RuntimeError(f'QEMU exited before the INTx test; see {directory}')
            time.sleep(0.5)
        else:
            raise RuntimeError(f'No completed userspace INTx test; see {serial}')
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
