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
        sources=(kernel/arch/aarch64/common/interrupts.c kernel/arch/aarch64/common/timer.c)
    else
        flags+=(-D__OSCONFIG_ACPI_SUPPORT -D__OSCONFIG_HAS_LEGACY_PCI)
        sources=(kernel/arch/x86/interrupts/api.c kernel/arch/x86/interrupts/init.c
                 kernel/arch/x86/components/smp.c kernel/arch/x86/interrupts/apic/apicipi.c
                 kernel/arch/x86/components/pit.c kernel/arch/x86/components/rtc.c
                 kernel/arch/x86/components/timers.c kernel/arch/x86/components/tsc.c
                 kernel/arch/x86/interrupts/apic/apictimer.c
                 kernel/acpi/hpet/comparators.c kernel/tests/interrupt_allocation.c)
    fi
    for source in kernel/interrupts.c kernel/interrupts_allocation.c kernel/interrupts_handlers.c kernel/api/priv.c \
                  kernel/api/entry.c kernel/components/cpu.c kernel/components/timer.c \
                  kernel/memory/ms_create.c kernel/memory/ms_struct.c kernel/memory/ms_sync.c \
                  kernel/memory/ms_unmap.c kernel/sync/futex.c kernel/sync/spinlock.c \
                  kernel/memory/heap.c kernel/sync/mutex.c \
                  kernel/handle_set.c librt/libds/list.c "${sources[@]}"; do
        "$compiler" "${flags[@]}" "${includes[@]}" -DLIBC_KERNEL -D__LIBDS_KERNEL__ \
            -c "$root/$source" -o "$temporary/$arch-${source//\//_}.o"
    done
    "$compiler" "${flags[@]}" "${includes[@]}" -c "$root/librt/libddk/interrupt.c" \
        -o "$temporary/$arch-ddk.o"
    "$compiler" "${flags[@]}" "${includes[@]}" -I "$root/services/deviced" \
        -c "$root/services/deviced/bus/pci/msi.c" -o "$temporary/$arch-pci-msi.o"
    for header in ddk/interrupt.h ddk/types/interrupt.h interrupts_allocation.h; do
        printf '#include <%s>\n' "$header" > "$temporary/header.c"
        "$compiler" "${flags[@]}" "${includes[@]}" -fsyntax-only "$temporary/header.c"
    done
    printf 'Interrupt boundary: %s compilation passed\n' "$arch"
done
