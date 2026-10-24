param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'aarch64-pc-windows-msvc'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/ipc-publication'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @("--target=$Target", '-std=c11', '-fms-extensions',
    '-DMOLLENOS', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=inline', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-D_Out_Opt_=', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections')
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include')) {
    $flags += @('-I', (Join-Path $root $include))
}
# Rename the userspace wrapper so it can call the actual kernel SHMCreate.
& $Compiler @flags '-DSHMCreate=IpcTestSHMCreate' -c "$root/librt/libos/ipc.c" -o "$output/ipc.obj"
if ($LASTEXITCODE -ne 0) { throw 'IPC creation compilation failed' }
& $Compiler @flags '-fuse-ld=lld' '-Wl,/opt:ref' '-llegacy_stdio_definitions' `
    "$PSScriptRoot/ipc_publication_test.c" "$PSScriptRoot/ipc_publication_host_windows.c" `
    "$root/kernel/memory/ms_shm.c" "$root/librt/libds/streambuffer.c" "$output/ipc.obj" -o "$output/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'IPC publication test compilation failed' }
& "$output/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'IPC publication test failed' }
