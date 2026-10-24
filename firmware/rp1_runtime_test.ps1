param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$outputDirectory = Join-Path $root 'build/interrupt-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
foreach ($include in @('librt/libfdt/include', 'kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libacpi/source/include', 'librt/libusb/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
foreach ($test in @('rp1_interrupt_test', 'rp1_runtime_test', 'rp1_client_test', 'device_provider_test')) {
    $output = Join-Path $outputDirectory "$test.exe"
    $sources = @("$PSScriptRoot/$test.c")
    if ($test -eq 'device_provider_test') { $sources += "$PSScriptRoot/device_provider_host_windows.c" }
    & $Compiler @flags @sources -o $output
    if ($LASTEXITCODE -ne 0) { throw "$test compilation failed" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}
