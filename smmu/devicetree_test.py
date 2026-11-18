#!/usr/bin/env python3
"""Exercise DT SMMU discovery with real tree parsing, sanitizers and mutations."""
import argparse
import copy
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile

import discovery_test

ROOT = Path(__file__).resolve().parents[2]
OK, ABSENT, INVALID, UNSUPPORTED = 0, 3, 4, 8
MAX64 = (1 << 64) - 1
MAX32 = (1 << 32) - 1
SOURCES = ("testing/smmu/devicetree_test.c", "kernel/arch/aarch64/smmu/discovery.c",
           "kernel/arch/aarch64/smmu/devicetree.c", "kernel/arch/aarch64/smmu/devicetree-unit.c",
           "kernel/arch/aarch64/smmu/devicetree-route.c", "kernel/arch/aarch64/devicetree/gic-interrupt.c",
           "kernel/devicetree/tree.c",
           "kernel/devicetree/resources.c", "librt/libfdt/parser.c", "librt/libfdt/dma.c")


def cells(*values):
    return struct.pack(">" + "I" * len(values), *values)


def node(name, properties=None, children=None):
    return [name, properties or {}, children or []]


def encode(tree):
    strings = bytearray()
    data = bytearray()

    def visit(entry):
        name, properties, children = entry
        data.extend(cells(1) + name.encode() + b"\0")
        data.extend(bytes(-len(data) % 4))
        for key, value in properties.items():
            data.extend(cells(3, len(value), len(strings)) + value)
            data.extend(bytes(-len(data) % 4))
            strings.extend(key.encode() + b"\0")
        for child in children:
            visit(child)
        data.extend(cells(2))

    visit(tree)
    data.extend(cells(9))
    offset = 56 + len(data)
    return cells(0xd00dfeed, offset + len(strings), 56, offset, 40,
                 17, 16, 0, len(strings), len(data)) + bytes(16) + data + strings


def fixture():
    v3 = node("iommu@100000", {
        "compatible": b"arm,smmu-v3\0", "phandle": cells(1), "#iommu-cells": cells(1),
        "reg": cells(0, 0x100000, 0, 0x20000), "dma-coherent": b"",
        "interrupts": cells(0, 40, 1, 0, 41, 4), "interrupt-names": b"eventq\0gerror\0"})
    mmu = node("iommu@200000", {
        "compatible": b"qcom,glymur-smmu-500\0qcom,smmu-500\0arm,mmu-500\0",
        "phandle": cells(2), "#iommu-cells": cells(2), "reg": cells(0, 0x200000, 0, 0x40000),
        "#global-interrupts": cells(1), "interrupts": cells(0, 60, 4, 0, 61, 4, 0, 61, 4)})
    first = node("first", {"iommus": cells(1, 7, 1, 3, 1, 7)})
    masked = node("masked", {"iommus": cells(2, 5, 0x8001), "dma-coherent": b""})
    pci = node("pci", {"device_type": b"pci\0", "linux,pci-domain": cells(9),
                       "#address-cells": cells(3), "#size-cells": cells(2), "dma-ranges": b"",
                       "bus-range": cells(2, 2), "iommu-map-mask": cells(0xfff8),
                       "iommu-map": cells(0x200, 1, 0x100, 0x100)})
    bus = node("soc", {"compatible": b"simple-bus\0", "#address-cells": cells(2),
                       "#size-cells": cells(2), "ranges": b"", "dma-ranges": b""},
               [first, masked, pci])
    gic = node("gic", {"compatible": b"arm,gic-v3\0", "phandle": cells(3),
                       "#interrupt-cells": cells(3), "interrupt-controller": b""})
    tree = node("", {"#address-cells": cells(2), "#size-cells": cells(2),
                     "interrupt-parent": cells(3)}, [v3, mmu, gic, bus])
    return tree, {entry[0]: entry[1] for entry in [v3, mmu, gic, bus, first, masked, pci]}


UNITS = ["0 1048576 131072 1 72 1 73 3 0", "1 2097152 262144 0 92 3 4294967295 0 2 93 3 93 3"]


def query(path="-", segment=9, rid=0x20f, error=OK, unit=0, cache=1, limit=MAX64, ids=(0x108,)):
    if error != OK:
        ids = ()
    return " ".join(map(str, (path, segment, rid, error, unit, cache, limit, len(ids), *ids)))


def cases():
    result = []

    def add(name, change=None, error=OK, units=None, queries=None):
        tree, props = fixture()
        if change:
            change(tree, props)
        expected_units = UNITS if units is None else units
        expected = f"{error} {len(expected_units)}\n"
        if error == OK:
            expected += "\n".join(expected_units + (queries or [])) + "\n"
        result.append((name, encode(tree), expected))

    add("mixed", queries=[query(), query(rid=0x208), query(rid=0x2ff, ids=(0x1f8,)),
                           query(rid=0x300, error=ABSENT), query(segment=10, error=ABSENT),
                           query("/soc/first", ids=(3, 7)),
                           query("/soc/masked", unit=1, cache=2, ids=(4, 5, 0x8004, 0x8005)),
                           query("/gic", error=ABSENT)])
    add("variable-pci-width", lambda t, p: p["pci"].update({
        "iommu-map-mask": cells(0xffff), "iommu-map": cells(0x200, 2, 0x40, 1, 8, 0x208, 1, 0x80, 0xf8)}),
        queries=[query(rid=0x202, unit=1, ids=(0x42, 0x43)), query(rid=0x208, ids=(0x80,))])
    add("bus-before-mask", lambda t, p: p["pci"].update({
        "iommu-map-mask": cells(0xff), "iommu-map": cells(0, 1, 0, 256)}),
        queries=[query(rid=0x201, ids=(1,)), query(rid=1, error=ABSENT)])
    add("pci-count-end", lambda t, p: p["pci"].update({"iommu-map": cells(0x200, 1, 0, 8)}),
        queries=[query(rid=0x207, ids=(0,)), query(rid=0x208, error=ABSENT)])
    add("sid-upper-bound", lambda t, p: p["pci"].update({
        "iommu-map-mask": cells(0xffff), "iommu-map": cells(0x200, 1, MAX32 - 255, 256)}),
        queries=[query(rid=0x2ff, ids=(MAX32,))])
    add("high-mask-bit", lambda t, p: p["masked"].update({"iommus": cells(2, 1, 0x80000000)}),
        queries=[query("/soc/masked", unit=1, cache=2, ids=(1, 0x80000001))])
    add("capacity-and-duplicate", lambda t, p: p["masked"].update({"iommus": cells(2, 0, 255, 2, 3, 255)}),
        queries=[query("/soc/masked", unit=1, cache=2, ids=tuple(range(256)))])
    add("mask-too-wide", lambda t, p: p["masked"].update({"iommus": cells(2, 0, 511)}),
        queries=[query("/soc/masked", error=UNSUPPORTED), query()])
    add("aggregate-too-wide", lambda t, p: p["masked"].update({"iommus": cells(2, 0, 255, 2, 256, 0)}),
        queries=[query("/soc/masked", error=UNSUPPORTED)])
    add("multiple-units", lambda t, p: p["first"].update({"iommus": cells(1, 7, 2, 7, 0)}),
        queries=[query("/soc/first", error=UNSUPPORTED)])
    add("global-mask", lambda t, p: (p["iommu@200000"].update({
        "#iommu-cells": cells(1), "stream-match-mask": cells(5)}),
        p["masked"].update({"iommus": cells(2, 0x11)})),
        queries=[query("/soc/masked", unit=1, cache=2, ids=(0x10, 0x11, 0x14, 0x15))])
    add("two-cell-ignores-global-mask", lambda t, p: p["iommu@200000"].update({"stream-match-mask": cells(7)}),
        queries=[query("/soc/masked", unit=1, cache=2, ids=(4, 5, 0x8004, 0x8005))])
    add("coherency-override", lambda t, p: (p["soc"].update({"dma-coherent": b""}),
        p["first"].update({"dma-noncoherent": b""})),
        queries=[query(cache=2), query("/soc/first", ids=(3, 7))])
    add("bounded-identity", lambda t, p: p["pci"].update({
        "dma-ranges": cells(0x02000000, 0, 0, 0, 0, 0, 0x100000)}),
        queries=[query(limit=0xfffff)])
    add("parent-address-width", lambda t, p: (p["soc"].update({"#address-cells": cells(1)})),
        queries=[query(limit=MAX32), query("/soc/first", limit=MAX32, ids=(3, 7))])
    def direct_root(tree, props):
        tree[1]["#address-cells"] = cells(1)
        props["iommu@100000"]["reg"] = cells(0x100000, 0, 0x20000)
        props["iommu@200000"]["reg"] = cells(0x200000, 0, 0x40000)
        first = tree[2][-1][2].pop(0)
        tree[2].append(first)

    add("direct-root-width", direct_root, queries=[query("/first", limit=MAX32, ids=(3, 7))])
    add("translated-register", lambda t, p: (t[2].remove(t[2][0]),
        t[2][-1][2].append(node("iommu@100000", p["iommu@100000"])),
        p["soc"].update({"ranges": cells(0, 0, 1, 0, 0, 0x400000)})),
        units=[UNITS[1], "0 4296015872 131072 1 72 1 73 3 0"], queries=[query(unit=1)])
    add("combined-interrupt", lambda t, p: p["iommu@100000"].update({
        "interrupts": cells(0, 44, 4), "interrupt-names": b"combined\0"}),
        units=["0 1048576 131072 1 76 3 76 3 0", UNITS[1]])
    add("extended-interrupts", lambda t, p: p["iommu@100000"].update({
        "interrupts-extended": cells(3, 0, 42, 1, 3, 0, 43, 4)}),
        units=["0 1048576 131072 1 74 1 75 3 0", UNITS[1]])
    add("absent-v3-interrupts", lambda t, p: (p["iommu@100000"].pop("interrupts"),
        p["iommu@100000"].pop("interrupt-names")),
        units=["0 1048576 131072 1 4294967295 0 4294967295 0 0", UNITS[1]])
    add("unknown-target", lambda t, p: p["iommu@100000"].update({"compatible": b"test,other-iommu\0"}),
        units=[UNITS[1]], queries=[query(error=UNSUPPORTED), query("/soc/first", error=UNSUPPORTED)])
    add("unsupported-target-width", lambda t, p: (p["iommu@100000"].update({"#iommu-cells": cells(3)}),
        p["first"].update({"iommus": cells(1, 3, 0, 0)}),
        p["pci"].update({"iommu-map": cells(0x200, 1, 0x100, 0, 0, 256)})),
        units=[UNITS[1]], queries=[query(error=UNSUPPORTED), query("/soc/first", error=UNSUPPORTED)])
    for name, target, prop, value in [
        ("empty-iommus", "first", "iommus", b""),
        ("truncated-tail", "first", "iommus", cells(1, 7, 2, 5)),
        ("bad-phandle-tail", "first", "iommus", cells(1, 7, 99, 5)),
        ("partial-cell", "first", "iommus", cells(1, 7) + b"x"),
        ("conflicting-masks", "masked", "iommus", cells(2, 0, 3, 2, 0, 1)),
        ("zero-pci-length", "pci", "iommu-map", cells(0x200, 1, 0, 0)),
        ("rid-overflow", "pci", "iommu-map", cells(0xffff, 1, 0, 2)),
        ("sid-overflow", "pci", "iommu-map", cells(0x200, 1, MAX32, 2)),
        ("overlap", "pci", "iommu-map", cells(0x200, 1, 0, 8, 0x207, 1, 8, 8)),
        ("truncated-map", "pci", "iommu-map", cells(0x200, 1, 0, 8, 0x210)),
        ("bad-map-target", "pci", "iommu-map", cells(0x200, 99, 0, 8)),
        ("bad-mask-width", "pci", "iommu-map-mask", cells(0, 0xffff)),
        ("bad-rid-mask", "pci", "iommu-map-mask", cells(0x10000)),
        ("bad-domain-width", "pci", "linux,pci-domain", cells(0, 9)),
        ("bad-bus-range", "pci", "bus-range", cells(5, 2)),
        ("bad-cell-width", "iommu@100000", "#iommu-cells", cells(0, 1)),
        ("bad-compatible-tail", "iommu@100000", "compatible", b"arm,smmu-v3\0bad"),
        ("bad-status", "iommu@100000", "status", b"okay"),
        ("status-list", "iommu@100000", "status", b"okay\0disabled\0"),
        ("device-type-list", "pci", "device_type", b"pci\0other\0"),
        ("disabled-target", "iommu@100000", "status", b"disabled\0"),
        ("overlapping-mmio", "iommu@200000", "reg", cells(0, 0x110000, 0, 0x40000)),
        ("bad-irq-names", "iommu@100000", "interrupt-names", b"eventq\0gerror"),
        ("short-irq-list", "iommu@100000", "interrupts", cells(0, 40, 1)),
        ("duplicate-irq-name", "iommu@100000", "interrupt-names", b"eventq\0eventq\0"),
        ("mixed-combined-irq", "iommu@100000", "interrupt-names", b"combined\0eventq\0"),
        ("bad-boolean", "first", "dma-coherent", cells(1)),
        ("conflicting-coherency", "masked", "dma-noncoherent", b""),
        ("truncated-window", "soc", "dma-ranges", cells(0)),
        ("zero-window", "soc", "dma-ranges", cells(0, 0, 0, 0, 0, 0)),
    ]:
        add(name, lambda t, p, target=target, prop=prop, value=value: p[target].update({prop: value}), error=INVALID)
    for name, change in [
        ("missing-window", lambda t, p: p["soc"].pop("dma-ranges")),
        ("offset-window", lambda t, p: p["soc"].update({"dma-ranges": cells(0, 0, 0, 4096, 0, 4096)})),
        ("nonzero-window", lambda t, p: p["soc"].update({"dma-ranges": cells(0, 4096, 0, 4096, 0, 4096)})),
        ("sparse-window", lambda t, p: p["soc"].update({"dma-ranges": cells(0, 0, 0, 0, 0, 4096, 0, 8192, 0, 8192, 0, 4096)})),
        ("dma-parent", lambda t, p: p["soc"].update({"dma-parent": cells(3)})),
        ("reserved-memory", lambda t, p: p["soc"].update({"memory-region": cells(3)})),
    ]:
        add(name, change, queries=[query(error=UNSUPPORTED), query("/soc/first", error=UNSUPPORTED)])
    add("missing-segment", lambda t, p: p["pci"].pop("linux,pci-domain"), error=UNSUPPORTED)
    add("duplicate-segment", lambda t, p: t[2][-1][2].append(node("pci2", copy.deepcopy(p["pci"]))), error=INVALID)
    add("unsupported-mmio-overlap", lambda t, p: p["iommu@200000"].update({
        "#iommu-cells": cells(3), "reg": cells(0, 0x110000, 0, 0x40000)}), error=INVALID)
    add("untranslated-segment-collision", lambda t, p: t[2][-1][2].append(node("pci2", {
        "device_type": b"pci\0", "linux,pci-domain": cells(9)})), error=INVALID)
    add("cross-device-mask-conflict", lambda t, p: p["first"].update({"iommus": cells(2, 4, 1)}), error=INVALID)
    add("cross-device-mask-alias", lambda t, p: p["first"].update({"iommus": cells(2, 4, 0x8001)}),
        queries=[query("/soc/first", unit=1, ids=(4, 5, 0x8004, 0x8005))])
    add("two-global-interrupts", lambda t, p: p["iommu@200000"].update({"#global-interrupts": cells(2)}),
        units=[UNITS[0]], queries=[query("/soc/masked", error=UNSUPPORTED)])
    add("no-global-interrupt", lambda t, p: p["iommu@200000"].update({"#global-interrupts": cells(0)}),
        units=[UNITS[0], "1 2097152 262144 0 4294967295 0 4294967295 0 3 92 3 93 3 93 3"])
    add("register-overflow", lambda t, p: p["iommu@100000"].update({
        "reg": cells(MAX32, MAX32, 0, 0x20000)}), error=INVALID)
    add("context-edge-interrupt", lambda t, p: p["iommu@200000"].update({
        "interrupts": cells(0, 60, 4, 0, 61, 1, 0, 61, 4)}),
        units=[UNITS[0], "1 2097152 262144 0 92 3 4294967295 0 2 93 1 93 3"])
    add("disabled-source", lambda t, p: p["first"].update({"status": b"disabled\0"}),
        queries=[query("/soc/first", error=ABSENT)])
    add("missing-interrupt-controller", lambda t, p: p["gic"].pop("interrupt-controller"), error=INVALID)
    add("node-limit", lambda t, p: t[2].extend(node(f"unused{i}") for i in range(256)), error=UNSUPPORTED)
    add("range-limit", lambda t, p: p["first"].update({"iommus": cells(*([1, 0] * 257))}), error=UNSUPPORTED)
    def extra_units(tree, props, count):
        for index in range(count):
            unit = copy.deepcopy(props["iommu@100000"])
            unit["phandle"] = cells(10 + index)
            unit["reg"] = cells(0, 0x400000 + index * 0x20000, 0, 0x20000)
            tree[2].append(node(f"extra-iommu{index}", unit))

    add("unit-capacity", lambda t, p: extra_units(t, p, 6),
        units=UNITS + [f"0 {0x400000 + i * 0x20000} 131072 1 72 1 73 3 0" for i in range(6)])
    add("unit-limit", lambda t, p: extra_units(t, p, 7), error=UNSUPPORTED)
    add("context-capacity", lambda t, p: p["iommu@200000"].update({
        "interrupts": cells(0, 60, 4, *([0, 61, 4] * 128))}),
        units=[UNITS[0], "1 2097152 262144 0 92 3 4294967295 0 128 " + "93 3 " * 128])
    add("context-limit", lambda t, p: p["iommu@200000"].update({
        "interrupts": cells(0, 60, 4, *([0, 61, 4] * 129))}),
        units=[UNITS[0]], queries=[query("/soc/masked", error=UNSUPPORTED)])
    return result


def build(output, replacement=None):
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions", "-DKERNELAPI=", "-DKERNELABI=",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-DVALI",
             "-DLIBC_KERNEL", "-D__LIBDS_KERNEL__", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    for directory in ("testing/include", "kernel/include", "librt/libos/include", "librt/libfdt/include",
                      "librt/libddk/include", "librt/libds/include", "boot/include"):
        flags += ["-I" + str(ROOT / directory)]
    sources = [str(replacement[1]) if replacement and source == replacement[0] else str(ROOT / source)
               for source in SOURCES]
    binary = output / "devicetree-test"
    subprocess.run(flags + sources + ["-o", str(binary)], check=True)
    return binary


def run_cases(binary, output, fixtures, mutation=None):
    for name, blob, expected in fixtures:
        path = output / f"{name}.dtb"
        path.write_bytes(blob)
        answers = output / f"{name}.txt"
        answers.write_text(expected)
        result = subprocess.run([str(binary), str(path), str(answers)], timeout=10, capture_output=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if mutation and result.returncode == -signal.SIGABRT and b"Assertion" in result.stderr:
            print(f"Rejected DT mutation: {mutation} ({name})", flush=True)
            return
        if result.returncode:
            raise AssertionError(f"{name}: unexpected failure\n" + result.stderr.decode(errors="replace"))
    if mutation:
        raise AssertionError(f"Mutation remained green: {mutation}")
    print(f"Arm SMMU DT: {len(fixtures)} fixtures passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    parser.add_argument("--qemu-fixture", action="store_true", help="Also decode a freshly generated QEMU virt DTB")
    args = parser.parse_args()
    fixtures = cases()
    with tempfile.TemporaryDirectory(prefix="vali-smmu-dt-") as directory:
        output = Path(directory)
        if args.qemu_fixture:
            dtb = output / "qemu-virt.dtb"
            subprocess.run(["qemu-system-aarch64", "-machine",
                            f"virt,gic-version=3,iommu=smmuv3,dumpdtb={dtb}",
                            "-cpu", "cortex-a72", "-m", "512M", "-nographic"], check=True, timeout=30)
            # QEMU describes one v3 unit but omits dma-ranges on the PCI host.
            # Preserve that unsupported route under Vali's explicit-window rule.
            expected = "0 1\n0 151322624 131072 1 106 1 109 1 0\n"
            expected += query(segment=0, rid=8, error=UNSUPPORTED) + "\n"
            fixtures.append(("qemu-virt", dtb.read_bytes(), expected))
        run_cases(build(output), output, fixtures)
        discovery_test.SOURCES = SOURCES
        discovery_test.compile_targets(output)
        if not args.mutation_check:
            return
        mutations = (
            ("RID mask", SOURCES[1], "input &= source->RequesterIdMask;", "input &= UINT16_MAX;"),
            ("SMR ignored bits", SOURCES[1], "requester &= ~mask;", "requester &= UINT32_MAX;"),
            ("complete platform list", SOURCES[4], "for (index = 0;; index++)", "for (index = 0;; index += 2)"),
            ("separate coherency", SOURCES[4], "source->CachePolicy = SHMDeviceCacheNonCoherent;", "source->CachePolicy = SHMDeviceCacheCoherent;"),
            ("disabled target", SOURCES[4], "if (!DeviceTreeIsEnabled(target))", "if (false)"),
            ("full SID arithmetic", SOURCES[4], "length - 1 > UINT32_MAX - range.OutputFirst", "false"),
            ("alias capacity", SOURCES[1], "count *= 2;\n        if (count >", "count *= 2;\n        if (count >="),
            ("GIC SPI base", SOURCES[5], "number + ARM64_GIC_SPI_BASE", "number"),
            ("PCI count convention", SOURCES[4], "range.InputLast = range.InputFirst + length - 1;", "range.InputLast = range.InputFirst + length;"),
        )
        for name, source, old, new in mutations:
            original = (ROOT / source).read_text()
            assert original.count(old) == 1, name
            changed = original.replace(old, new)
            changed = re.sub(r'#include "([^"]+)"',
                             lambda match: '#include "' + str((ROOT / source).parent / match[1]) + '"', changed)
            mutant = output / "mutant.c"
            mutant.write_text(changed)
            run_cases(build(output, (source, mutant)), output, fixtures, name)


if __name__ == "__main__":
    main()
