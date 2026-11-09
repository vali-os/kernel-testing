#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-ethernet-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g)
for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
               librt/libddk/include librt/libds/include librt/libgracht/include librt/libacpi/source/include \
               services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
for source in services/deviced/firmware/ethernet.c services/deviced/bus/rp1/ethernet.c \
              services/deviced/bus/rp1/resources.c librt/libddk/resources.c; do
    clang "${flags[@]}" -Wno-unused-command-line-argument -fsyntax-only "$root/$source"
done
clang "${flags[@]}" "$root/testing/firmware/ethernet_test.c" "$root/librt/libfdt/parser.c" -o "$temporary/ethernet"
"$temporary/ethernet" "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0
"$temporary/ethernet" "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" 1
