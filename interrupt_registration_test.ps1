param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc',
    [string]$OutputDirectory = "$PSScriptRoot/../build/interrupt-tests",
    [switch]$AddressSanitizer
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = Join-Path (Resolve-Path $OutputDirectory).Path 'interrupt-registration.exe'
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DINTERRUPT_WINDOWS_HOST_TEST', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
if ($AddressSanitizer) { $flags += @('-fsanitize=address', '-g') }
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libacpi/source/include')) {
    $flags += @('-I', (Join-Path $root $include))
}
& $Compiler @flags "$PSScriptRoot/interrupt_registration_test.c" `
    "$PSScriptRoot/interrupt_host_windows.c" -o $output
if ($LASTEXITCODE -ne 0) { throw 'Interrupt test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Interrupt tests failed' }
