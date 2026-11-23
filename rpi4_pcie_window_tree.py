"""Give a QEMU raspi4b boot the PCIe inbound window of the real Pi 4 firmware.

On the board, the firmware moves the PCIe host's dma-ranges window to PCI
address 0x4_00000000 and keeps the 32-bit memory tag:

    dma-ranges = <0x02000000 0x4 0x0  0x0 0x0  0x1 0x0>;

The kernel's early device pool scan once rejected that window, and the board
booted with no device pool. QEMU's raspi4b machine deletes the pcie@7d500000
node before the guest sees it, so the window never reaches the guest there.
This script adds a PCI node under another name, which QEMU keeps, with the
same window. The kernel scan treats any enabled PCI node the same way, and no
driver matches the node's compatible.

The tree to edit is the bcm2711-rpi-4-b.dtb in a copy of the boot inputs, the
file that build/boot-tests/run_dma_2d_boot.py --arch rpi4 copies and boots.
The file is changed in place. With --tag, the window uses another PCI space
code, for example 0x43000000 for 64-bit memory.
"""
import argparse
from pathlib import Path
import subprocess

# Name and address chosen outside the nodes QEMU's raspi4b machine removes.
NODE = "/scb/dma-window-check@7d600000"

# The real firmware's window: PCI 0x4_00000000 reaches CPU 0x0 for 4 GiB.
WINDOW = ("0x4", "0x0", "0x0", "0x0", "0x1", "0x0")


def fdtput(tree, *arguments):
    subprocess.run(["fdtput", str(tree), *arguments], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tree", type=Path, help="bcm2711-rpi-4-b.dtb in a copy of the boot inputs")
    parser.add_argument("--tag", default="0x02000000", help="PCI space code of the window (default: 32-bit memory)")
    args = parser.parse_args()

    fdtput(args.tree, "-c", NODE)
    fdtput(args.tree, "-t", "s", NODE, "compatible", "vali,dma-window-check")
    fdtput(args.tree, "-t", "s", NODE, "device_type", "pci")
    fdtput(args.tree, "-t", "x", NODE, "#address-cells", "3")
    fdtput(args.tree, "-t", "x", NODE, "#size-cells", "2")
    fdtput(args.tree, "-t", "x", NODE, "dma-ranges", args.tag, *WINDOW)
    print(f"{args.tree}: added {NODE} with dma-ranges <{args.tag} {' '.join(WINDOW)}>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
