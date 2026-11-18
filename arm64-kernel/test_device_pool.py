#!/usr/bin/env python3
"""Exercise early firmware DMA pool selection and its saved boot handoff."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def tree(body, root_properties=''):
    return '/dts-v1/; / { #address-cells = <2>; #size-cells = <2>; ' + root_properties + body + ' };'


def bus(name, ranges, children='', extra=''):
    prop = '' if ranges is None else ('dma-ranges;' if ranges == '' else f'dma-ranges = <{ranges}>;')
    return f'{name} {{ compatible = "simple-bus"; #address-cells = <2>; #size-cells = <2>; {prop} {extra} {children} }};'


def run(args):
    # Status values come from the production error-code ABI.
    ok, invalid, unsupported, missing = 0, 4, 8, 3
    low = bus('bus', '0 0xc0000000 0 0 0 0x40000000')
    pi5 = 'compatible = "raspberrypi,5-model-b", "brcm,bcm2712";'
    sdio = ('soc { compatible = "simple-bus"; #address-cells = <1>; #size-cells = <1>; '
            'ranges = <0x1000 0x10 0xfff000 0x1000>; '
            'sdio { compatible = "brcm,bcm2712-sdhci"; reg = <0x1000 0x260>; }; };')
    fixtures = [
        ('low-alias', tree(low), 0x3fc00000, ok, 0),
        ('expanded-route', tree(bus('bus', '0 0 0 0 0 0x80000000')), 0x7fc00000, ok, 0),
        ('high-physical', tree(bus('bus', '0 0 1 0 1 0')), 0x1ffc00000, ok, 0),
        ('nested', tree(bus('bus', '0 0xc0000000 0 0 0 0x40000000',
                           bus('child', '0 0x10000000 0 0xc0000000 0 0x4000000'))), 0x3c00000, ok, 0),
        ('intersection', tree(bus('a', '0 0 0 0x80000000 0 0x40000000') +
                              bus('b', '0 0 0 0xa0000000 0 0x40000000')), 0xbfc00000, ok, 0),
        ('holes', tree(bus('bus', '0 0 0 0 0 0x8000000 0 0x80000000 0 0x20000000 0 0x8000000')), 0x27c00000, ok, 0),
        ('translation-boundary', tree(bus('bus', '0 0 0 0x1000000 0 0x200000 0 0x10000000 0 0x1200000 0 0x200000')), 0, missing, 0),
        ('disjoint', tree(low + bus('other', '0 0 1 0 0 0x10000000')), 0, missing, 0),
        ('identity', tree(bus('bus', '')), 0x1ffc00000, ok, 0),
        ('missing', tree(bus('bus', None)), 0, unsupported, 0),
        ('missing-parent', tree(bus('bus', None, bus('child', '0 0 0 0 0 0x40000000'))), 0, unsupported, 0),
        ('disabled', tree(low + bus('other', '0 0 1 0 0 0x10000000', extra='status = "disabled";')), 0x3fc00000, ok, 0),
        ('disabled-parent', tree(low + bus('other', None, bus('child', '1'), extra='status = "disabled";')), 0x3fc00000, ok, 0),
        ('inactive-bus', tree(low + bus('other', '1', 'device { status = "disabled"; };')), 0x3fc00000, ok, 0),
        ('active-malformed-bus', tree(low + bus('other', '1', 'device {};')), 0, invalid, 0),
        ('dma-parent', tree(low + bus('other', '1', extra='dma-parent = <1>;')), 0x3fc00000, ok, 0),
        ('iommu-map', tree(low + bus('other', '1', extra='iommu-map = <1>;')), 0x3fc00000, ok, 0),
        ('iommu-input', tree(low + bus('other', '1', extra='iommus = <1>;')), 0x3fc00000, ok, 0),
        ('memory-region', tree(low + bus('other', '1', extra='memory-region = <1>;')), 0x3fc00000, ok, 0),
        ('memory-region-empty', tree(low + bus('other', '1', extra='memory-region;')), 0x3fc00000, ok, 0),
        ('memory-region-child', tree(low + bus('other', '1', 'device { memory-region = <1>; };')), 0x3fc00000, ok, 0),
        ('truncated', tree(bus('bus', '0 0 0')), 0, invalid, 0),
        ('overflow', tree(bus('bus', '0 0 0xffffffff 0xfffff000 0 0x2000')), 0, invalid, 0),
        ('overlap', tree(bus('bus', '0 0 0 0 0 0x40000000 0 0 1 0 0 0x40000000')), 0, invalid, 0),
        ('marked-unreachable', tree(low), 0x3fc00000, ok, 1),
        ('marked-crossing', tree(low), 0x3fa00000, ok, 7),
        ('marked-reachable', tree(bus('bus', '0 0 0 0x80000000 0 0x40000000')), 0x90000000, ok, 1),
        ('unreserved-tree', tree(low), 0, invalid, 2),
        ('nomap-tree', tree(low), 0, invalid, 3),
        ('bad-tree-address', tree(low), 0, invalid, 4),
        ('split-tree-storage', tree(low), 0x3fc00000, ok, 5),
        ('no-low-ram', tree(low), 0, missing, 6),
        ('pci', tree('pci { device_type = "pci"; #address-cells = <3>; #size-cells = <2>; '
                     'dma-ranges = <0x02000000 0 0xc0000000 0 0 0 0x40000000>; };'), 0x3fc00000, ok, 0),
        ('pci-io', tree('pci { device_type = "pci"; #address-cells = <3>; #size-cells = <2>; '
                        'dma-ranges = <0x01000000 0 0 0 0 0 0x40000000>; };'), 0, unsupported, 0),
        ('pci-disabled-child', tree('pci { device_type = "pci"; #address-cells = <3>; #size-cells = <2>; '
                     'dma-ranges = <0x02000000 0 0xc0000000 0 0 0 0x40000000>; '
                     'device { status = "disabled"; }; };' + bus('bus', '')), 0x3fc00000, ok, 0),
        ('sdio-compatibility', tree(sdio + bus('bus', ''), pi5), 0xffc00000, ok, 0),
        ('sdio-other-board', tree(sdio + bus('bus', '')), 0x1ffc00000, ok, 0),
        ('sdio-other-register', tree(sdio.replace('reg = <0x1000', 'reg = <0x1004') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-explicit-route', tree(sdio.replace('ranges = ', 'dma-ranges = <0 0 0 0x20000000>; ranges = '), pi5), 0x1fc00000, ok, 0),
        ('sdio-disabled', tree(sdio.replace('reg = ', 'status = "disabled"; reg = ') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-inactive-children', tree(sdio.replace('reg = <0x1000 0x260>;',
            'reg = <0x1000 0x260>; child { status = "disabled"; };') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-memory-region', tree(sdio.replace('reg = ', 'memory-region = <1>; reg = ') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-nested', tree(bus('outer', '', sdio) + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-mmio-truncated', tree(sdio.replace('0xfff000 0x1000>', '0xfff000>') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-mmio-parent-overflow', tree(sdio.replace('0x10 0xfff000 0x1000',
            '0xffffffff 0xfffffff0 0x1000') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-mmio-child-overflow', tree(sdio.replace('ranges = <0x1000',
            'ranges = <0xfffffff0') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-mmio-ambiguous', tree(sdio.replace('0xfff000 0x1000>',
            '0xfff000 0x1000 0x1000 0x10 0xfff000 0x1000>') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-mmio-partial-overlap', tree(sdio.replace('0xfff000 0x1000>',
            '0xfff000 0x1000 0x1100 0 0x2000 0x100>') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-pci-device', tree(sdio.replace('reg = ', 'device_type = "pci"; reg = ') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('sdio-pci-bus', tree(sdio.replace('soc {', 'soc { device_type = "pci";') + bus('bus', ''), pi5), 0x1ffc00000, ok, 0),
        ('bad-root-cells', '/dts-v1/; / { #address-cells = <3>; #size-cells = <2>; };', 0, unsupported, 0),
        ('window-capacity', tree(bus('bus', ' '.join(
            f'0 {i * 0x400000} 0 {i * 0x400000} 0 0x400000' for i in range(17)))), 0, unsupported, 0),
    ]
    # Text the single failure log must contain, naming what rejected the pool.
    reasons = {
        'missing-parent': 'DMA route rejected by node child',
        'active-malformed-bus': 'DMA route rejected by node other',
        'missing': 'no node in the device tree has a DMA route',
        'bad-root-cells': 'DMA route rejected by node /',
        'unreserved-tree': 'the device tree could not be scanned',
        'no-low-ram': 'DMA route rejected by node bus',
        'translation-boundary': 'no RAM reachable by every DMA route',
    }
    with tempfile.TemporaryDirectory(prefix='vali-dt-pool-') as temporary:
        out = Path(temporary)
        flags = [os.environ.get('CC', 'clang'), '-std=c11', '-DVALI', '-DTESTING',
                 '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
                 '-fms-extensions', '-fasm-blocks', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g', '-no-pie',
                 '-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc']
        for path in ('testing/include', 'kernel/include', 'librt/libos/include',
                     'librt/libfdt/include', 'boot/include', 'librt/libds/include'):
            flags += ['-I', str(ROOT / path)]
        sources = ['testing/arm64-kernel/device_pool_test.c',
                   'kernel/arch/aarch64/common/device-pool.c',
                   'kernel/arch/aarch64/common/boot-memory.c',
                   'kernel/memory/memory_region.c']
        dt_sources = ['kernel/arch/aarch64/devicetree/device-pool-fdt.c',
                      'librt/libfdt/parser.c', 'librt/libfdt/dma.c', 'librt/libfdt/board.c', 'librt/libfdt/bcm2712.c']
        binary = out / 'device-pool'
        subprocess.run(flags + ['-D__OSCONFIG_DEVICETREE_SUPPORT'] +
                       [str(ROOT / path) for path in sources + dt_sources] + ['-o', str(binary)], check=True)
        environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'}
        cases = []
        for name, source, base, status, mode in fixtures:
            dts, dtb = out / (name + '.dts'), out / (name + '.dtb')
            dts.write_text(source)
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(dtb), str(dts)], check=True)
            command = [str(dtb), hex(base), str(status), str(mode)]
            if name in reasons:
                command.append(reasons[name])
            subprocess.run([str(binary), *command], env=environment, check=True, timeout=10)
            cases.append((name, command))
        for board in ('bcm2711-rpi-4-b', 'bcm2712-rpi-5-b'):
            dtb = ROOT / 'boot/rpi/firmware' / (board + '.dtb')
            base = 0x3fc00000 if '2711' in board else 0xffc00000
            subprocess.run([str(binary), str(dtb), hex(base), str(ok), '0'], env=environment, check=True, timeout=10)
        print(f'DT pool: {len(fixtures)} fixtures and both bundled boards passed', flush=True)
        acpi = out / 'acpi-pool'
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ['-o', str(acpi)], check=True)
        subprocess.run([str(acpi), cases[0][1][0], '0x7fc00000', str(ok), '0'], env=environment, check=True)
        print('ACPI pool: unchanged 2 GiB placement and saved handoff passed', flush=True)
        if args.mutation_check:
            mutations = [
                ('ignore-common-ranges', 'kernel/arch/aarch64/devicetree/device-pool-fdt.c',
                 'status = __IntersectRoute(scan, &node->Map);', '(void)__IntersectRoute; scan->Routes++; status = OS_EOK;', 'low-alias'),
                ('ignore-memory-region', 'librt/libfdt/dma.c',
                 'node->HasMemoryRegion', 'false', 'memory-region'),
                ('ignore-board-pool-limit', 'kernel/arch/aarch64/devicetree/device-pool-fdt.c',
                 'if (node->LimitsPool)', 'if (node->LimitsPool && node->State != __PoolNodeBoardRoute)', 'sdio-compatibility'),
                ('ignore-loader-reachability', 'kernel/memory/memory_region.c',
                 'if (entry->Length - 1 > limit - entry->PhysicalBase)', 'if (false)', 'marked-crossing'),
            ]
            for name, source, before, after, case in mutations:
                content = (ROOT / source).read_text()
                assert content.count(before) == 1, name
                mutant = out / (name + '.c')
                mutant.write_text(content.replace(before, after))
                selected = [str(mutant) if path == source else str(ROOT / path) for path in sources + dt_sources]
                target = out / name
                subprocess.run(flags + ['-D__OSCONFIG_DEVICETREE_SUPPORT', '-I', str((ROOT / source).parent)] +
                               selected + ['-o', str(target)], check=True)
                command = next(command for label, command in cases if label == case)
                result = subprocess.run([str(target), *command], env=environment, capture_output=True, timeout=10)
                assert result.returncode == -signal.SIGABRT and b'Assertion' in result.stderr, (name, result.stderr)
                print('Rejected mutation:', name, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    run(parser.parse_args())
