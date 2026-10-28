param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc',
    [switch]$AddressSanitizer,
    [switch]$PublicationOnly
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$outputDirectory = Join-Path $root 'build/interrupt-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
if ($AddressSanitizer) { $flags += @('-fsanitize=address', '-g') }
foreach ($include in @('librt/libfdt/include', 'kernel/include', 'boot/include',
    'librt/libc/include', 'librt/libos/include', 'librt/libddk/include',
    'librt/libds/include', 'librt/libgracht/include', 'librt/libacpi/source/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
$output = Join-Path $outputDirectory 'publication-service.exe'
& $Compiler @flags "$PSScriptRoot/publication_service_test.c" -o $output
if ($LASTEXITCODE -ne 0) { throw 'Publication service test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Publication service test failed' }
foreach ($legacy in @($false, $true)) {
    $output = Join-Path $outputDirectory "pci-host-$legacy.exe"
    $modeFlags = @()
    if ($legacy) { $modeFlags += '-D__OSCONFIG_HAS_LEGACY_PCI' }
    & $Compiler @flags @modeFlags "$PSScriptRoot/pci_host_test.c" -o $output
    if ($LASTEXITCODE -ne 0) { throw 'PCI host test compilation failed' }
    if ($PublicationOnly) {
        & $output --publication-only
    } else {
        & $output "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb"
    }
    if ($LASTEXITCODE -ne 0) { throw 'PCI host tests failed' }
}
