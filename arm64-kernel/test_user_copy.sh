#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-arm-user-copy-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
compiler=${ARM64_CLANG:-/usr/local/valicc/bin/clang}
flags=(--target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -static
       -ffreestanding -fno-builtin -fno-stack-protector
       -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-e,_start
       -O2 -fms-extensions -DMOLLENOS -D_INTEGRAL_MAX_BITS=64
       -DLIBC_KERNEL -D__LIBDS_KERNEL__)
for include in kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include; do
    flags+=(-I "$root/$include")
done
"$compiler" "${flags[@]}" "$root/testing/arm64-kernel/user_copy_test.c" \
    "$root/kernel/arch/aarch64/common/usercopy.S" -o "$temporary/user-copy"
timeout 10s qemu-aarch64 "$temporary/user-copy"
printf 'ARM64 user copies: exact ranges, unaligned addresses and guard pages passed\n'
