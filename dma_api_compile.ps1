param([string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/dma-api-compile'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$includes = @()
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libfdt/include', 'librt/libacpi/source/include',
    'librt/libusb/include', 'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $includes += @('-I', (Join-Path $root $include))
}

# Compile production translation units separately: host fixtures include sources
# together and can otherwise hide a missing include or a mismatched declaration.
foreach ($arch in @('i386', 'x86_64', 'aarch64')) {
    $flags = @("--target=$arch-uml-vali", '-std=gnu11', '-fms-extensions',
        '-ffreestanding', '-nostdlibinc', '-O2', '-Wall', '-Werror',
        '-Wno-unused-function', '-Wno-address-of-packed-member',
        '-D__OSCONFIG_ENABLE_MULTIPROCESSORS', '-D__OSCONFIG_HAS_MMIO')
    if ($arch -eq 'aarch64') {
        $flags += @('-march=armv8-a', '-mno-outline-atomics', '-D__OSCONFIG_DEVICETREE_SUPPORT')
    } else {
        $flags += @('-D__OSCONFIG_ACPI_SUPPORT', '-D__OSCONFIG_HAS_LEGACY_PCI')
    }
    foreach ($source in @('librt/libddk/dma.c', 'librt/libddk/dma_context.c',
        'librt/libos/handles.c', 'librt/libos/shm.c', 'kernel/memory/ms_shm.c', 'kernel/api/dma.c', 'kernel/api/entry.c', 'kernel/memory/ms_shm_device.c', 'services/deviced/core/dma.c',
        'modules/serial/usb/common/manager.c', 'modules/serial/usb/common/service.c',
        'modules/serial/usb/ehci/controller.c', 'modules/serial/usb/ohci/controller.c',
        'modules/serial/usb/uhci/controller.c', 'modules/serial/usb/common/transfer.c',
        'modules/serial/usb/common/transfer_dma.c', 'modules/serial/usb/common/transfer_work.c',
        'modules/serial/usb/xhci/dma.c', 'modules/serial/usb/xhci/controller.c',
        'modules/serial/usb/xhci/dwc3.c',
        'modules/serial/usb/xhci/command.c', 'modules/serial/usb/xhci/endpoint.c',
        'modules/serial/usb/xhci/interrupt.c', 'modules/serial/usb/xhci/queue.c',
        'modules/serial/usb/xhci/ring.c', 'modules/serial/usb/xhci/transfer.c',
        'modules/serial/usb/xhci/port.c', 'testing/dma_api_compile.c')) {
        $sourceFlags = @()
        if ($source.StartsWith('kernel/')) {
            $sourceFlags += @('-DLIBC_KERNEL', '-D__LIBDS_KERNEL__')
        }
        & $Compiler @flags @sourceFlags @includes -c (Join-Path $root $source) `
            -o (Join-Path $output "$arch-$($source.Replace('/','_')).obj")
        if ($LASTEXITCODE -ne 0) { throw "DMA API compilation failed: $arch $source" }
    }
    Write-Output "DMA API, kernel, provider and USB production sources: $arch passed"
}
