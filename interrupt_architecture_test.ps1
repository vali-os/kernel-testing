param([string]$Compiler = "$env:USERPROFILE/ValiCC/bin/clang.exe")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$output = Join-Path $root 'build/interrupt-tests/architecture'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$includes = @()
foreach ($include in @('kernel/include', 'boot/include', 'librt/libc/include',
    'librt/libos/include', 'librt/libddk/include', 'librt/libds/include',
    'librt/libgracht/include', 'librt/libfdt/include', 'librt/libacpi/source/include',
    'services/deviced', 'services/deviced/include', 'build/windows/protocols', 'librt/libusb/include')) {
    $includes += @('-I', (Join-Path $root $include))
}
foreach ($arch in @('i386', 'amd64', 'aarch64')) {
    $flags = @("--target=$arch-uml-vali", '-std=gnu11', '-fms-extensions',
        '-ffreestanding', '-nostdlibinc', '-O2', '-Wall', '-Werror',
        '-Wno-unused-function', '-Wno-address-of-packed-member', '-Wno-self-assign',
        '-D__OSCONFIG_ENABLE_MULTIPROCESSORS', '-D__OSCONFIG_HAS_MMIO')
    if ($arch -eq 'aarch64') {
        $flags += @('-march=armv8-a', '-mno-outline-atomics', '-D__OSCONFIG_DEVICETREE_SUPPORT')
        $sources = @('kernel/arch/aarch64/common/interrupts.c', 'kernel/arch/aarch64/common/timer.c')
    } else {
        $flags += @('-D__OSCONFIG_ACPI_SUPPORT', '-D__OSCONFIG_HAS_LEGACY_PCI')
        $sources = @('kernel/arch/x86/interrupts/api.c', 'kernel/arch/x86/interrupts/init.c',
            'kernel/arch/x86/components/smp.c', 'kernel/arch/x86/interrupts/apic/apicipi.c',
            'kernel/arch/x86/components/pit.c', 'kernel/arch/x86/components/rtc.c',
            'kernel/arch/x86/components/timers.c', 'kernel/arch/x86/components/tsc.c',
            'kernel/arch/x86/interrupts/apic/apictimer.c',
            'kernel/acpi/hpet/comparators.c', 'kernel/tests/interrupt_allocation.c')
    }
    $sources += @('kernel/interrupts.c', 'kernel/interrupts_allocation.c',
        'kernel/interrupts_handlers.c', 'kernel/api/priv.c', 'kernel/api/entry.c',
        'kernel/components/cpu.c', 'kernel/memory/ms_create.c', 'kernel/memory/ms_struct.c')
    $sources += @('kernel/sync/futex.c', 'kernel/components/timer.c', 'kernel/handle_set.c', 'librt/libds/list.c',
        'kernel/memory/ms_sync.c', 'kernel/memory/ms_unmap.c', 'kernel/sync/spinlock.c',
        'kernel/memory/heap.c', 'kernel/sync/mutex.c')
    foreach ($source in $sources) {
        $object = Join-Path $output "$arch-$($source.Replace('/','_')).obj"
        & $Compiler @flags @includes -DLIBC_KERNEL -D__LIBDS_KERNEL__ -c (Join-Path $root $source) -o $object
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $arch $source" }
    }
    & $Compiler @flags @includes -c "$root/librt/libddk/interrupt.c" -o "$output/$arch-ddk.obj"
    if ($LASTEXITCODE -ne 0) { throw "DDK compilation failed: $arch" }
    foreach ($source in @('services/deviced/bus/pci/msi.c', 'services/deviced/bus/pci/device.c',
        'services/deviced/bus/pci/discovery.c', 'services/deviced/bus/pci/host.c', 'modules/edu/main.c')) {
        & $Compiler @flags @includes -c "$root/$source" -o "$output/$arch-$($source.Replace('/','_')).obj"
        if ($LASTEXITCODE -ne 0) { throw "PCI MSI compilation failed: $arch $source" }
    }
    Write-Output "Interrupt boundary: $arch compilation passed"
}
