#!/usr/bin/env bash
# Compile provider APIs and their syscall boundary for every supported word size.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=$(mktemp -d -t vali-provider-arch-XXXXXX)
trap 'rm -rf "$output"' EXIT
compiler=${VALI_CLANG:-/usr/local/valicc/bin/clang}
includes=()
for include in kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libfdt/include librt/libacpi/source/include; do
    includes+=(-I "$root/$include")
done
for arch in i386 amd64 aarch64; do
    flags=(--target="$arch-uml-vali" -std=gnu11 -fms-extensions -ffreestanding
           -nostdlibinc -O2 -Wall -Werror -Wno-unused-function -Wno-unused-but-set-variable
           -Wno-address-of-packed-member -Wno-self-assign
           -D__OSCONFIG_ENABLE_MULTIPROCESSORS)
    if [[ "$arch" == aarch64 ]]; then
        flags+=(-march=armv8-a -mno-outline-atomics -D__OSCONFIG_HAS_MMIO -D__OSCONFIG_DEVICETREE_SUPPORT)
    else
        flags+=(-D__OSCONFIG_ACPI_SUPPORT -D__OSCONFIG_HAS_LEGACY_PCI)
    fi
    for source in kernel/handle_provider.c kernel/handle.c kernel/handle_grants.c \
                  kernel/handle_set.c kernel/api/handles.c kernel/api/entry.c \
                  kernel/memory/space/context.c kernel/memory/shm/shm.c; do
        "$compiler" "${flags[@]}" "${includes[@]}" -DLIBC_KERNEL -D__LIBDS_KERNEL__ \
            -c "$root/$source" -o "$output/$(basename "$source").o"
    done
    for source in librt/libos/handles.c librt/libos/handle_provider.c librt/libos/uthreads/scheduler.c; do
        "$compiler" "${flags[@]}" "${includes[@]}" -D__OSLIB_C_IMPLEMENTATION \
            -c "$root/$source" -o "$output/$(basename "$source").o"
    done
    echo "Provider handles: $arch compilation passed"
done
