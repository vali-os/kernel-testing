param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc',
    [switch]$AddressSanitizer
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/pci-msi'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
if ($AddressSanitizer) { $flags += @('-fsanitize=address', '-g') }
foreach ($include in @('services/deviced', 'librt/libc/include', 'librt/libos/include',
    'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
foreach ($name in @('pci_msi_test', 'pci_msi_ddk_test', 'pci_msi_rpc_test')) {
    & $Compiler @flags "$PSScriptRoot/$name.c" -o "$output/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name compilation failed" }
    & "$output/$name.exe"
    if ($LASTEXITCODE -ne 0) { throw "$name failed" }
}
