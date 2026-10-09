#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-arm-thread_lifecycle-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
compiler=${ARM64_CLANG:-/usr/local/valicc/bin/clang}
flags=(--target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -static
       -ffreestanding -fno-builtin -fno-stack-protector
       -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-e,_start
       -O2 -fms-extensions -DMOLLENOS -D_INTEGRAL_MAX_BITS=64
       -DLIBC_KERNEL -D__LIBDS_KERNEL__)
for include in librt/libfdt/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include; do
    flags+=(-I "$root/$include")
done
for test in thread_lifecycle thread_context; do
    "$compiler" "${flags[@]}" "$root/testing/arm64-kernel/${test}_test.c" -o "$temporary/$test"
    timeout 10s qemu-aarch64 "$temporary/$test"
done
printf 'ARM64 thread retirement and stack checks passed\n'
