param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$Target = 'x86_64-pc-windows-msvc',
    [string]$Nasm = "$env:USERPROFILE/Tools/bin/nasm.exe",
    [switch]$AddressSanitizer
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/smp'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$flags = @("--target=$Target", '-fuse-ld=lld', '-std=gnu11', '-fms-extensions',
    '-DMOLLENOS', '-DVALI', '-D__LIBDS_KERNEL__', '-DKERNELAPI=', '-DKERNELABI=', '-DSERVICEAPI=', '-DSERVICEABI=',
    '-D__time64_t=int64_t', '-DNDEBUG', '-Wno-macro-redefined',
    '-ffunction-sections', '-fdata-sections', '-Wl,/opt:ref', '-llegacy_stdio_definitions')
if ($AddressSanitizer) { $flags += @('-fsanitize=address', '-g') }
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libacpi/source/include')) {
    $flags += @('-I', (Join-Path $root $include))
}
foreach ($test in @('futex_smp_test', 'smp_memory_test', 'smp_trampoline_test', 'timer_boot_test', 'timer_clock_test', 'apic_ipi_test', 'handle_set_queue_test', 'memory_sync_smp_test', 'memory_unmap_order_test', 'heap_irq_test', 'mutex_owner_test', 'scheduler_timeout_test')) {
    & $Compiler @flags "$PSScriptRoot/$test.c" -o "$output/$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $test" }
    & "$output/$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $test" }
    if ($test -eq 'memory_sync_smp_test') {
        foreach ($scenario in @('--send-failure', '--missing-ack')) {
            & "$output/$test.exe" $scenario
            if ($LASTEXITCODE -ne 0) { throw "Test failed: $test $scenario" }
        }
    }
}
foreach ($arch in @('i386', 'amd64')) {
    $binary = Join-Path $output "ap-$arch.sys"
    & $Nasm -f bin "-D__$($arch)__" -o $binary "$root/boot/ap.asm"
    if ($LASTEXITCODE -ne 0) { throw "Trampoline assembly failed: $arch" }
    $bytes = [System.IO.File]::ReadAllBytes($binary)
    if ($bytes.Length -gt 4096 -or $bytes.Length -lt 32 -or $bytes.Length % 8) {
        throw "Invalid SIPI page/trailer size: $arch"
    }
    foreach ($byte in $bytes[($bytes.Length - 32)..($bytes.Length - 1)]) {
        if ($byte -ne 0) { throw "Invalid trampoline parameter trailer: $arch" }
    }
    Write-Output "SMP trampoline: $arch assembled, $($bytes.Length) bytes, aligned parameter trailer"
}

