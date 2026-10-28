param([string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$includes = @()
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libfdt/include', 'librt/libacpi/source/include', 'librt/libusb/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $includes += @('-I', (Join-Path $root $include))
}
foreach ($arch in @('i386', 'amd64', 'aarch64')) {
    $flags = @("--target=$arch-uml-vali", '-std=gnu11', '-fms-extensions',
        '-ffreestanding', '-nostdlibinc', '-O2', '-Wall', '-Werror',
        '-Wno-unused-function', '-Wno-address-of-packed-member',
        '-D__OSCONFIG_ENABLE_MULTIPROCESSORS', '-D__OSCONFIG_HAS_MMIO')
    if ($arch -eq 'aarch64') {
        $flags += @('-march=armv8-a', '-mno-outline-atomics', '-D__OSCONFIG_DEVICETREE_SUPPORT')
    } else {
        $flags += @('-D__OSCONFIG_ACPI_SUPPORT', '-D__OSCONFIG_HAS_LEGACY_PCI')
    }
    foreach ($source in @('kernel/interrupts/group.c', 'kernel/components/ic.c', 'kernel/api/priv.c',
        'librt/libddk/interrupts/interrupt.c', 'librt/libddk/interrupts/pci.c', 'librt/libddk/interrupts/provider.c',
        'services/deviced/firmware/platform-runtime.c', 'services/deviced/firmware/fdt.c',
        'services/deviced/firmware/pci.c', 'services/deviced/firmware/bcm.c',
        'services/deviced/firmware/mapping.c', 'services/deviced/bus/platform/discovery.c',
        'services/deviced/bus/pci/functionhandlers.c', 'services/deviced/bus/rp1/rp1.c',
        'services/deviced/bus/platform/activation.c', 'services/deviced/bus/platform/dma.c',
        'services/deviced/bus/platform/interrupts.c', 'services/deviced/bus/platform/platform.c',
        'services/deviced/bus/rp1/resources.c', 'services/deviced/bus/rp1/publish.c',
        'services/deviced/bus/pci/device.c', 'services/deviced/bus/pci/publish.c',
        'librt/libddk/resources.c',
        'services/deviced/core/io.c', 'services/deviced/main.c',
        'services/deviced/bus/pci/discovery.c', 'services/deviced/bus/pci/enumerate.c',
        'services/deviced/bus/pci/host.c', 'services/deviced/bus/pci/hosts/ecam.c',
        'services/deviced/bus/pci/hosts/legacy.c',
        'services/deviced/bus/pci/resources.c', 'services/deviced/bus/pci/hosts/broadcom/bcm.c',
        'services/deviced/bus/pci/hosts/broadcom/host.c',
        'services/deviced/bus/pci/hosts/broadcom/bcm2711.c',
        'services/deviced/bus/pci/hosts/broadcom/bcm2712.c',
        'services/deviced/core/devices.c', 'services/deviced/core/dma.c',
        'testing/interrupt_contract_compile.c')) {
        & $Compiler @flags @includes -fsyntax-only (Join-Path $root $source)
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $arch $source" }
    }
    Write-Output "$arch native platform sources and wire layouts passed"
}
