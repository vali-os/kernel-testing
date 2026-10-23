param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/virtio-msi.exe'
New-Item -ItemType Directory -Force -Path (Split-Path $output) | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
foreach ($include in @('modules/virtio/common/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include')) {
    $flags += @('-I', (Join-Path $root $include))
}
& $Compiler @flags "$PSScriptRoot/virtio_msi_test.c" -o $output
if ($LASTEXITCODE -ne 0) { throw 'Virtio interrupt test compilation failed' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Virtio interrupt tests failed' }

# Fast handlers are independently remapped code in production. Compile their
# exact function bodies with the real driver headers, without module RPC glue.
$sources = @()
foreach ($driver in @('net', 'blk')) {
    $source = Get-Content -Raw (Join-Path $root "modules/virtio/$driver/main.c")
    $handler = [regex]::Match($source, '(?ms)^irqstatus_t\s+OnFastInterrupt\(.*?^}')
    if (-not $handler.Success) { throw "Missing $driver fast handler" }
    $name = if ($driver -eq 'net') { 'NetFastInterrupt' } else { 'BlkFastInterrupt' }
    $header = (Join-Path $root "modules/virtio/$driver/virtio-$driver.h").Replace('\', '/')
    $path = Join-Path (Split-Path $output) "virtio-$driver-handler.c"
    $body = "#define OnFastInterrupt $name`n#include `"$header`"`n" + $handler.Value
    [IO.File]::WriteAllText($path, $body)
    $sources += $path
}
foreach ($include in @('librt/libgracht/include', 'build/windows/protocols')) {
    $flags += @('-I', (Join-Path $root $include))
}
$handlerOutput = Join-Path (Split-Path $output) 'virtio-handlers.exe'
& $Compiler @flags "$PSScriptRoot/virtio_handler_test.c" @sources -o $handlerOutput
if ($LASTEXITCODE -ne 0) { throw 'Virtio handler test compilation failed' }
& $handlerOutput
if ($LASTEXITCODE -ne 0) { throw 'Virtio handler tests failed' }
