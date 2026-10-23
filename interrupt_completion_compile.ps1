param(
    [string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe",
    [string]$OutputDirectory = "$PSScriptRoot/../build/interrupt-tests/completion-compile"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$output = (Resolve-Path $OutputDirectory).Path
$includes = @()
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libfdt/include', 'librt/libacpi/source/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols')) {
    $includes += @('-I', (Join-Path $root $include))
}

# Compile both sides of the callback boundary and the syscall layout. These
# checks complement the host behavior tests; they do not simulate hardware.
foreach ($arch in @('i386', 'x86_64', 'aarch64')) {
    $flags = @("--target=$arch-uml-vali", '-std=gnu11', '-fms-extensions',
        '-ffreestanding', '-nostdlibinc', '-O2', '-Wall', '-Werror',
        '-Wno-unused-function', '-Wno-address-of-packed-member', '-Wno-self-assign',
        '-D__OSCONFIG_ENABLE_MULTIPROCESSORS', '-D__OSCONFIG_HAS_MMIO')
    if ($arch -eq 'aarch64') {
        $flags += @('-march=armv8-a', '-mno-outline-atomics', '-D__OSCONFIG_DEVICETREE_SUPPORT')
    } else {
        $flags += '-D__OSCONFIG_ACPI_SUPPORT'
    }
    foreach ($source in @('kernel/interrupts/chain.c', 'kernel/interrupts/interrupts.c',
        'kernel/interrupts/group.c', 'kernel/api/priv.c', 'kernel/api/entry.c',
        'librt/libddk/interrupt.c', 'librt/libddk/interrupt-provider.c',
        'testing/interrupt_contract_compile.c')) {
        $sourceFlags = @()
        if ($source.StartsWith('kernel/')) {
            $sourceFlags += @('-DLIBC_KERNEL', '-D__LIBDS_KERNEL__')
        }
        & $Compiler @flags @sourceFlags @includes -c (Join-Path $root $source) `
            -o (Join-Path $output "$arch-$($source.Replace('/','_')).obj")
        if ($LASTEXITCODE -ne 0) { throw "Completion boundary compilation failed: $arch $source" }
    }
    Write-Output "Completion boundary and ABI: $arch passed"
}
