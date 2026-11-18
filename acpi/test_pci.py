#!/usr/bin/env python3
"""Exercise ACPI PCI roots, ECAM, routing and deviced swizzling with ASan/UBSan."""
import argparse
import os
import re
import signal
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
flags = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-fms-extensions',
         '-fasm-blocks', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=static inline',
         '-DSERVICEABI=', '-DLIBC_KERNEL', '-D__LIBDS_KERNEL__',
         '-DPRIuIN="zu"', '-DPRIiIN="zi"', '-DPRIxIN="zx"',
         '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-variable',
         '-Wno-unused-parameter', '-Wno-sign-compare', '-Wno-unused-but-set-variable',
         '-Wno-unused-but-set-global', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
         '-Wl,--gc-sections', '-g']
for directory in ['testing/include', 'kernel/include', 'librt/libos/include',
                  'librt/libacpi/source/include', 'librt/libfdt/include', 'services/deviced',
                  'services/deviced/include', 'librt/libgracht/include']:
    flags += ['-I' + str(ROOT / directory)]
for directory in ['librt/libds/include', 'librt/libddk/include', 'boot/include']:
    flags += ['-isystem', str(ROOT / directory)]
cases = [
    ('pci-dma', ['testing/acpi/pci_dma_test.c']),
    ('pci-dma-x86', ['testing/acpi/pci_dma_x86_test.c']),
    ('pci-dma-host', ['testing/acpi/pci_dma_host_test.c']),
    ('pci-dma-host-legacy', ['testing/acpi/pci_dma_host_test.c']),
    ('pci-dma-host-arm64', ['testing/acpi/pci_dma_host_test.c']),
    ('pci-links', ['testing/acpi/pci_links_test.c', 'librt/libds/list.c']),
    ('pci-swizzle', ['testing/acpi/pci_swizzle_test.c']),
    ('pci-config', ['testing/acpi/pci_config_test.c']),
    ('pci-roots', ['testing/acpi/pci_roots_test.c']),
    ('pci-windows', ['testing/acpi/pci_windows_test.c']),
    ('pci-prt', ['testing/acpi/pci_prt_test.c']),
    ('pci-prt-arm64', ['testing/acpi/pci_prt_test.c']),
]


def compile_dma_abi(output):
    """Keep the existing query and result layout on every supported ABI."""
    source = output / "pci-dma-abi.c"
    source.write_text("""
#include <ddk/pci/acpi.h>
#include <stddef.h>
_Static_assert(sizeof(AcpiPciDma_t) == 16, "DMA result size");
_Static_assert(offsetof(AcpiPciDma_t, CachePolicy) == 8, "cache policy offset");
_Static_assert(offsetof(AcpiPciDma_t, Flags) == 12, "flags offset");
_Static_assert(sizeof(union AcpiPciResult) == 32, "existing result size");
_Static_assert(sizeof(struct AcpiPciQuery) == 528, "existing query size");
_Static_assert(offsetof(struct AcpiPciQuery, Bridges) == 18, "bridge array offset");
_Static_assert(sizeof(struct AcpiPciRoot) == 32, "root fits existing result");
_Static_assert(offsetof(struct AcpiPciRoot, Id) == 0, "root ID prefix");
_Static_assert(offsetof(struct AcpiPciRoot, WindowCount) == 4, "window count prefix");
_Static_assert(offsetof(struct AcpiPciRoot, Segment) == 8, "segment prefix");
_Static_assert(offsetof(struct AcpiPciRoot, BusStart) == 10, "bus start prefix");
_Static_assert(offsetof(struct AcpiPciRoot, BusEnd) == 11, "bus end prefix");
_Static_assert(offsetof(struct AcpiPciRoot, DmaStatus) == 12, "DMA status offset");
_Static_assert(offsetof(struct AcpiPciRoot, Dma) == 16, "DMA policy offset");
_Static_assert(AcpiPciQueryInterrupt == 2, "existing query kinds");
""")
    compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
    for arch in ("i386", "amd64", "aarch64"):
        command = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                   "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-Wall", "-Werror"]
        for directory in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                          "librt/libddk/include", "librt/libds/include", "librt/libacpi/source/include",
                          "librt/libfdt/include", "librt/libgracht/include", "services/deviced", "services/deviced/include"):
            command += ["-I" + str(ROOT / directory)]
        sources = [str(source), str(ROOT / "kernel/acpi/pci-dma.c"), str(ROOT / "kernel/acpi/pci-roots.c"),
                   str(ROOT / "kernel/acpi/irq.c"),
                   str(ROOT / "services/deviced/bus/pci/dma.c"),
                   str(ROOT / "services/deviced/bus/pci/windows.c"),
                   str(ROOT / "services/deviced/bus/pci/discovery.c"),
                   str(ROOT / "services/deviced/bus/pci/hosts/ecam.c"),
                   str(ROOT / "services/deviced/bus/pci/hosts/legacy.c")]
        if arch == "aarch64":
            sources += [str(ROOT / "kernel/arch/aarch64/acpi/pci-dma.c"),
                        str(ROOT / "kernel/arch/aarch64/acpi/pci-irq.c")]
        else:
            sources += [str(ROOT / "kernel/arch/x86/components/acpi-pci.c")]
        for index, path in enumerate(sources):
            # Match the kernel's ACPICA environment. Its userspace typedefs
            # have a different data model on the Vali targets.
            extra = ["-DLIBC_KERNEL", "-D__LIBDS_KERNEL__"] if path.startswith(str(ROOT / "kernel") + "/") else []
            if path.endswith("hosts/legacy.c") and arch != "aarch64":
                extra += ["-D__OSCONFIG_HAS_LEGACY_PCI"]
            subprocess.run(command + extra + ["-c", path, "-o", str(output / f"{arch}-{index}.obj")], check=True)
        print(f"ACPI PCI DMA: {arch} ABI and production compilation passed", flush=True)


def run_mutations(output):
    mutations = (
        ("host unknown flags", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "policy->Flags & ~ACPI_PCI_DMA_DIRECT", "false"),
        ("host direct flag", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "!(policy->Flags & ACPI_PCI_DMA_DIRECT)", "false"),
        ("host DMA status", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "host->AcpiDmaStatus != OS_EOK", "false"),
        ("host valid status", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "!OSERR_VALID(host->AcpiDmaStatus)", "false"),
        ("host cache policy", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "policy->CachePolicy != SHMDeviceCacheCoherent", "false"),
        ("host inclusive limit", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "policy->AddressLimit + 1", "policy->AddressLimit"),
        ("host full-width limit", "services/deviced/bus/pci/dma.c", "PCI_DMA_HOST_SOURCE", "pci_dma_host_test.c",
         "policy->AddressLimit == UINT64_MAX", "false"),
        ("root policy copy", "services/deviced/bus/pci/windows.c", "PCI_ROOT_LOAD_SOURCE", "pci_dma_host_test.c",
         "host->AcpiDma = root->Dma;", "host->AcpiDma.Flags = 0;"),
        ("root status copy", "services/deviced/bus/pci/windows.c", "PCI_ROOT_LOAD_SOURCE", "pci_dma_host_test.c",
         "host->AcpiDmaStatus = root->DmaStatus;", "host->AcpiDmaStatus = OS_EOK;"),
        ("coherency requirement", "kernel/arch/aarch64/acpi/pci-dma.c", "ARM64_PCI_DMA_SOURCE", "pci_dma_test.c",
         "!properties.Coherent ||", "false ||"),
        ("inherited coherency", "kernel/acpi/pci-dma.c", "ACPI_PCI_DMA_SOURCE", "pci_dma_test.c",
         "!checkCoherency || scan->Properties.CoherencyPresent ||", "!checkCoherency ||"),
        ("DMA restriction", "kernel/arch/aarch64/acpi/pci-dma.c", "ARM64_PCI_DMA_SOURCE", "pci_dma_test.c",
         "|| properties.HasDma", "|| false"),
        ("namespace work bound", "kernel/acpi/pci-dma.c", "ACPI_PCI_DMA_SOURCE", "pci_dma_test.c",
         "scan->Visited == ACPI_PCI_DMA_MAX_OBJECTS", "false"),
        ("subtree depth bound", "kernel/acpi/pci-dma.c", "ACPI_PCI_DMA_SOURCE", "pci_dma_test.c",
         "level > ACPI_PCI_DMA_MAX_DEPTH", "false"),
        ("VIOT gate", "kernel/arch/aarch64/acpi/pci-dma.c", "ARM64_PCI_DMA_SOURCE", "pci_dma_test.c",
         'WARNING("ACPI PCI DMA: segment %u requires VIOT support", segment);',
         'WARNING("ACPI PCI DMA: segment %u requires VIOT support", segment); *addressLimitOut = UINT64_MAX; return OS_EOK;'),
        ("stored DMA failure", "kernel/acpi/pci-roots.c", "ACPI_PCI_ROOTS_SOURCE", "pci_roots_test.c",
         "root->Value.DmaStatus = ArchAcpiGetPciDma(handle, &root->Value, &root->Value.Dma);",
         "ArchAcpiGetPciDma(handle, &root->Value, &root->Value.Dma); root->Value.DmaStatus = OS_EOK;"),
    )
    mutations += (("legacy ACPI policy", "services/deviced/bus/pci/hosts/legacy.c", "PCI_LEGACY_DMA_SOURCE", "pci_dma_host_test.c",
                   "host->AcpiRootId != 0", "false"),)
    link_mutations = (
        ("link resource index", "route->SourceIndex != 0", "false"),
        ("link resource type", "resource->Type != ACPI_RESOURCE_TYPE_EXTENDED_IRQ", "false"),
        ("link single descriptor", "link->Found ||", "false ||"),
        ("link resource length", "resource->Length < ACPI_RS_SIZE(ACPI_RESOURCE_EXTENDED_IRQ)", "false"),
        ("link interrupt count", "irq->InterruptCount != 1 ||", "false ||"),
        ("link consumer", "irq->ProducerConsumer != ACPI_CONSUMER ||", "false ||"),
        ("link trigger", "irq->Triggering != ACPI_LEVEL_SENSITIVE ||", "false ||"),
        ("link polarity", "irq->Polarity != ACPI_ACTIVE_HIGH ||", "false ||"),
        ("link sharing flag", "irq->Shareable > ACPI_SHARED ||", "false ||"),
        ("link resource source", "irq->ResourceSource.StringLength != 0", "false"),
        ("link SPI lower bound", "irq->Interrupts[0] < ACPI_PCI_SPI_MIN ||", "false ||"),
        ("link SPI upper bound", "irq->Interrupts[0] > ACPI_PCI_SPI_MAX", "false"),
        ("link missing descriptor", "!link.Found ||", "false ||"),
        ("link missing end tag", "!link.Ended", "false"),
        ("link fixed GSI", "link->Interrupt.Conform = INTERRUPT_ACPICONFORM_PRESENT | INTERRUPT_ACPICONFORM_FIXED |",
         "link->Interrupt.Conform = INTERRUPT_ACPICONFORM_PRESENT |"),
        ("link preserve sharing", "if (irq->Shareable == ACPI_SHARED)", "if (false)"),
    )
    mutations += tuple((name, "kernel/arch/aarch64/acpi/pci-irq.c", "ARM64_PCI_IRQ_SOURCE", "pci_prt_test.c", before, after)
                       for name, before, after in link_mutations)
    for name, path, macro, fixture, before, after in mutations:
        original = (ROOT / path).read_text()
        assert original.count(before) == 1, name
        changed = original.replace(before, after)
        changed = re.sub(r'#include "([^"]+)"',
                         lambda match: '#include "' + str((ROOT / path).parent / match[1]) + '"', changed)
        mutant = output / "mutant.c"
        mutant.write_text(changed)
        binary = output / "mutant"
        extra = ['-U__amd64__', '-D__aarch64__'] if macro == "ARM64_PCI_IRQ_SOURCE" else []
        if macro == "PCI_LEGACY_DMA_SOURCE":
            extra += ["-D__OSCONFIG_HAS_LEGACY_PCI"]
        command = flags + extra + [f'-D{macro}="{mutant}"', str(ROOT / "testing/acpi" / fixture), "-o", str(binary)]
        subprocess.run(command, check=True)
        result = subprocess.run([str(binary)], capture_output=True, timeout=30,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        sanitizer = b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr
        if result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr or sanitizer:
            raise AssertionError(f"Mutation did not fail an assertion: {name}: {result.stderr.decode()}")
        print("Rejected ACPI PCI mutation:", name, flush=True)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--mutation-check", action="store_true")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-acpi-pci-') as temporary:
    for name, sources in cases:
        binary = Path(temporary) / name
        extra = ['-U__amd64__', '-D__aarch64__'] if name.endswith('-arm64') else []
        if name.endswith('-legacy'):
            extra += ['-D__OSCONFIG_HAS_LEGACY_PCI']
        subprocess.run(flags + extra + [str(ROOT / p) for p in sources] + ['-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
    compile_dma_abi(Path(temporary))
    if args.mutation_check:
        run_mutations(Path(temporary))
