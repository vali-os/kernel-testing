#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-arm-exceptions-XXXXXX)
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
"$compiler" "${flags[@]}" "$root/testing/arm64-kernel/exceptions_test.c" -o "$temporary/exceptions"
timeout 10s qemu-aarch64 "$temporary/exceptions"
printf 'ARM64 fault and signal-frame checks passed\n'
