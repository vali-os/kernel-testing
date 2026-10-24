#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-shared-tree-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for service in device contracts/driver; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$service.gr" \
        --out "$temporary" --lang-c --client --server
done
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= -DSERVICEAPI= -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g
       -I "$temporary")
for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
               librt/libddk/include librt/libds/include librt/libgracht/include librt/libusb/include \
               librt/libacpi/source/include services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
clang "${flags[@]}" "$root/testing/firmware/platform_bus_test.c" \
    "$root/librt/libfdt/parser.c" -o "$temporary/platform"
python3 "$root/testing/firmware/platform_fixtures.py" "$temporary"
for fixture in alias absolute; do
    "$temporary/platform" "$temporary/$fixture.dtb" 0 synthetic
done
for fixture in bad-reg bad-tail; do
    "$temporary/platform" "$temporary/$fixture.dtb" invalid malformed
done
"$temporary/platform" "$temporary/missing-alias.dtb" 0 missing-alias
"$temporary/platform" "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4
"$temporary/platform" "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" 0 pi5
