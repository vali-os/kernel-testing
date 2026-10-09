#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-arm-memcpy-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
compiler=${ARM64_CLANG:-/usr/local/valicc/bin/clang}
flags=(--target=aarch64-linux-gnu -fuse-ld=lld -nostdlib -static
       -ffreestanding -fno-builtin -fno-stack-protector
       -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,-e,_start
       -O2 -fms-extensions -DMOLLENOS -D_INTEGRAL_MAX_BITS=64)
for include in librt/libfdt/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include; do
    flags+=(-I "$root/$include")
done

run_case() {
    local name=$1 cpu=$2
    shift 2
    "$compiler" "${flags[@]}" "$@" \
        "$root/testing/arm64-runtime/memcpy_test.c" \
        "$root/librt/libc/arch/aarch64/_memcpy.S" -o "$temporary/$name"
    timeout 30s qemu-aarch64 -cpu "$cpu" "$temporary/$name"
    printf '%s: passed\n' "$name"
}

run_case base cortex-a57 -DTEST_FEATURES=0
run_case neon cortex-a57 -DTEST_FEATURES=1
run_case mops max -DTEST_FEATURES=2
run_case priority max -DTEST_FEATURES=3
run_case query_error cortex-a57 -DTEST_FEATURES=3 -DTEST_QUERY_ERROR=1
run_case short_query cortex-a57 -DTEST_FEATURES=3 -DTEST_SHORT_QUERY=1
run_case kernel cortex-a57 -DLIBC_KERNEL -mgeneral-regs-only