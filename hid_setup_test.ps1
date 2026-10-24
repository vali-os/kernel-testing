param([string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/hid-setup-test.exe'
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=gnu11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-DTESTING', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined', '-llegacy_stdio_definitions')
foreach ($include in @('librt/libc/include', 'librt/libos/include', 'librt/libddk/include',
    'librt/libds/include', 'librt/libgracht/include', 'librt/libusb/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
& $Compiler @flags "$PSScriptRoot/hid_setup_test.c" -o $output
if ($LASTEXITCODE -ne 0) { throw 'HID setup test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'HID setup test failed' }
