param([string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=gnu11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-DTESTING', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-Wl,/errorlimit:0', '-llegacy_stdio_definitions')
if ($Target.StartsWith('x86_64')) {
    $flags += @('-include', 'intrin.h')
}
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libusb/include', 'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
foreach ($test in @('shm_release_test', 'usb_shm_release_test', 'dma_syscall_test', 'ddk_dma_test', 'usb_controller_cleanup_test', 'dwc3_test', 'xhci_dma_test', 'xhci_dma_manager_test', 'usb_scheduler_memory_test', 'device_match_test', 'usb_dma_manager_test', 'hub_events_test', 'usb_detach_test')) {
    $binary = Join-Path $output "$test.exe"
    $sources = @("$PSScriptRoot/$test.c")
    $testFlags = @()
    # Exercise the real kernel logging macros through the fixture's capture.
    if ($test -eq 'dma_syscall_test') { $testFlags += '-UTESTING' }
    if ($test -eq 'usb_controller_cleanup_test') { $sources += @("$PSScriptRoot/usb_controller_cleanup_host.c", "$root/librt/libos/syscalls.c") }
    if ($test -eq 'usb_shm_release_test') { $sources += "$PSScriptRoot/usb_shm_release_host.c" }
    if ($test -eq 'usb_dma_manager_test') { $sources += "$PSScriptRoot/usb_dma_manager_host.c" }
    if ($test -eq 'usb_detach_test') { $sources += "$PSScriptRoot/usb_detach_host.c" }
    if ($test -eq 'xhci_dma_test') { $sources += "$PSScriptRoot/xhci_dma_host.c" }
    if ($test -eq 'xhci_dma_manager_test') { $sources += "$PSScriptRoot/xhci_dma_manager_host.c" }
    if ($test -in @('ddk_dma_test', 'shm_release_test')) { $sources += "$root/librt/libos/syscalls.c" }
    & $Compiler @flags @testFlags @sources -o $binary
    if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}
