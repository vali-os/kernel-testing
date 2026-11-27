# DeviceTree reader and retained tree checks

The allocation-free parser lives in `librt/libfdt`. The loader, kernel, and
service compile the same source with their own ABI and register policy. Kernel
allocation and resource ownership remain in `kernel/devicetree`.

Run the retained-tree tests with Clang and sanitizer runtimes installed:

```sh
python3 testing/devicetree/test_tree.py
```

They exercise forward references, aliases, high-address translation, allocation
failure cleanup, malformed input, and real firmware trees. They also check the
interrupt controller lookup for a node under a PCIe host that has both
`interrupt-parent` and `#interrupt-cells`, like `pcie2` on the Pi 5. The child
must resolve to the host. The host's own interrupts must resolve to the GIC.

The ARM64 console choice (`kernel/arch/aarch64/devicetree/serial-console.c`) is
checked by:

```sh
python3 testing/devicetree/test_console.py --mutation-check
```

It builds `console_test.c` twice. One is a normal build. The other has
`__OSCONFIG_RPI5_EARLY_SERIAL` and a test value for
`__OSCONFIG_RPI5_EARLY_SERIAL_ADDRESS` that differs from the default. Each
build checks the console and a set of register reads on every bundled BCM2712
tree, the Pi 4B tree and small built trees.

- The early serial build must return the test address with a 0x100 byte range
  and no tree node, whatever the tree says. That includes a tree without
  `/aliases`. It must log one debug line that names
  `VALI_RPI5_EARLY_SERIAL_ADDRESS`.
- The normal build must pick the enabled `stdout-path` PL011 as before. It must
  return the error for a port behind a bus without ranges or with a register
  range that is too short, and it must not log.
- When `stdout-path` names a PL011 behind a PCI bus, the normal build must
  return OS_EOK with no console, so the boot goes on. It must log one warning
  that says the port is behind a bus the kernel can't translate.
- On the BCM2712 trees the normal build must return no console, because the
  syslog service prints the log through the PL011 driver there. It must log
  one debug line that names the PL011 driver. The Pi 4B tree and the built
  trees keep their `stdout-path` console.
- Both builds must refuse a register read through a PCI bus, and a `reg` entry
  directly on a PCI bus, with OS_ENOTSUPPORTED.

With `--mutation-check`, changed copies of the kernel sources must each fail
at least one check.

The loader's parser
consumers are covered by:

```sh
python3 testing/rpi-loader/test_platform.py
python3 testing/rpi-loader/test_resources.py
python3 testing/rpi-loader/test_pe.py
```

The service reader's borrowed views and publication boundary are covered by
`testing/firmware/reader_test.c`, built by `testing/firmware/test.sh`. This links
the shared parser without any kernel tree allocation or controller policy.

Parser callbacks may accumulate private state while traversing. They must not
publish it until the complete parse succeeds: a valid prefix cannot establish a
valid tree. Names and values borrow the immutable input. All consumers preserve
this contract even though they have different ownership and publication models.
