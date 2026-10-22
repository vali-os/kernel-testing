#!/usr/bin/env bash
# Compile the real interrupt boundary for every supported CPU architecture.
# Host mocks cannot catch pointer-width or architecture-header regressions.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-interrupt-arch-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
compiler=${VALI_CLANG:-/usr/local/valicc/bin/clang}
includes=()
for include in kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libfdt/include librt/libacpi/source/include; do
    includes+=(-I "$root/$include")
done
for arch in i386 amd64 aarch64; do
    flags=(--target="$arch-uml-vali" -std=gnu11 -fms-extensions -ffreestanding
           -nostdlibinc -O2 -Wall -Werror -Wno-unused-function
           -Wno-address-of-packed-member -Wno-self-assign
           -D__OSCONFIG_ENABLE_MULTIPROCESSORS -D__OSCONFIG_HAS_MMIO)
    if [[ "$arch" == aarch64 ]]; then
        flags+=(-march=armv8-a -mno-outline-atomics -D__OSCONFIG_DEVICETREE_SUPPORT)
        sources=(kernel/arch/aarch64/common/interrupts.c kernel/arch/aarch64/common/timer.c
                 kernel/arch/aarch64/rpi/platform.c)
    else
        flags+=(-D__OSCONFIG_ACPI_SUPPORT -D__OSCONFIG_HAS_LEGACY_PCI)
        sources=(kernel/arch/x86/interrupts/api.c kernel/arch/x86/interrupts/init.c
                 kernel/arch/x86/interrupts/apic/apicinit.c kernel/arch/x86/interrupts/apic/apicio.c
                 kernel/arch/x86/components/smp.c kernel/arch/x86/interrupts/apic/apicipi.c
                 kernel/arch/x86/components/pit.c kernel/arch/x86/components/rtc.c
                 kernel/arch/x86/components/timers.c kernel/arch/x86/components/tsc.c
                 kernel/arch/x86/interrupts/apic/apictimer.c
                 kernel/acpi/hpet/comparators.c kernel/tests/interrupt_group.c)
    fi
    for source in kernel/interrupts/interrupts.c kernel/interrupts/group.c kernel/components/ic.c \
                  kernel/interrupts/handlers.c kernel/api/priv.c \
                  kernel/api/entry.c kernel/deviceio.c kernel/components/cpu.c kernel/components/timer.c \
                  kernel/memory/ms_create.c kernel/memory/ms_struct.c kernel/memory/ms_sync.c \
                  kernel/memory/ms_unmap.c kernel/sync/futex.c kernel/sync/spinlock.c \
                  kernel/memory/heap.c kernel/sync/mutex.c \
                  kernel/handle_set.c librt/libds/list.c "${sources[@]}"; do
        source_flags=()
        if [[ "$source" == kernel/arch/x86/interrupts/apic/apicinit.c ]]; then
            # Existing PIC routing state is recorded but not yet consumed.
            source_flags+=(-Wno-unused-but-set-global)
        fi
        "$compiler" "${flags[@]}" "${source_flags[@]}" "${includes[@]}" -DLIBC_KERNEL -D__LIBDS_KERNEL__ \
            -c "$root/$source" -o "$temporary/$arch-${source//\//_}.o"
    done
    "$compiler" "${flags[@]}" "${includes[@]}" -c "$root/librt/libddk/interrupt.c" \
        -o "$temporary/$arch-ddk.o"
    "$compiler" "${flags[@]}" "${includes[@]}" -c "$root/testing/interrupt_contract_compile.c" \
        -o "$temporary/$arch-contract.o"
    for source in services/deviced/bus/pci/msi.c services/deviced/bus/pci/msix.c \
                  services/deviced/bus/pci/msix-device.c librt/libddk/deviceio.c; do
        "$compiler" "${flags[@]}" "${includes[@]}" -I "$root/services/deviced" \
            -I "$root/services/deviced/include" -I "$root/librt/libusb/include" \
            -c "$root/$source" -o "$temporary/$arch-${source//\//_}.o"
    done
    for header in ddk/interrupt.h ddk/types/interrupt.h interrupts_group.h; do
        printf '#include <%s>\n' "$header" > "$temporary/header.c"
        "$compiler" "${flags[@]}" "${includes[@]}" -fsyntax-only "$temporary/header.c"
    done
    printf 'Interrupt boundary: %s compilation passed\n' "$arch"
done
