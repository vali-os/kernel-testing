param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../..").Path
$output = Join-Path $root 'build/interrupt-tests/shared-tree'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
foreach ($include in @('librt/libfdt/include', 'kernel/include', 'boot/include',
    'librt/libc/include', 'librt/libos/include', 'librt/libddk/include', 'librt/libusb/include',
    'librt/libds/include', 'librt/libgracht/include', 'librt/libacpi/source/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
& $Compiler @flags "$PSScriptRoot/reader_host_windows.c" -o "$output/reader.exe"
if ($LASTEXITCODE -ne 0) { throw 'Shared reader compilation failed' }
& "$output/reader.exe"
if ($LASTEXITCODE -ne 0) { throw 'Shared reader tests failed' }
& $Compiler @flags "$PSScriptRoot/platform_bus_test.c" "$PSScriptRoot/platform_bus_host_windows.c" `
    "$root/librt/libfdt/parser.c" -o "$output/platform.exe"
if ($LASTEXITCODE -ne 0) { throw 'Platform test compilation failed' }
& $Python "$PSScriptRoot/platform_fixtures.py" $output
if ($LASTEXITCODE -ne 0) { throw 'Fixture generation failed' }
foreach ($fixture in @('alias', 'absolute')) {
    & "$output/platform.exe" "$output/$fixture.dtb" 0 synthetic
    if ($LASTEXITCODE -ne 0) { throw "$fixture tests failed" }
}
foreach ($fixture in @('bad-reg', 'bad-tail')) {
    & "$output/platform.exe" "$output/$fixture.dtb" invalid malformed
    if ($LASTEXITCODE -ne 0) { throw "$fixture tests failed" }
}
& "$output/platform.exe" "$output/missing-alias.dtb" 0 missing-alias
if ($LASTEXITCODE -ne 0) { throw 'Missing console alias test failed' }
& "$output/platform.exe" "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4
if ($LASTEXITCODE -ne 0) { throw 'Pi 4 platform tests failed' }
& "$output/platform.exe" "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" 0 pi5
if ($LASTEXITCODE -ne 0) { throw 'Pi 5 platform tests failed' }
