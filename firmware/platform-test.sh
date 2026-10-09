#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-platform-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for service in device file process contracts/filesystem contracts/usbhost contracts/driver; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$service.gr" \
        --out "$temporary" --lang-c --client --server
done
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= -DSERVICEAPI= -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined -Wno-unused-command-line-argument
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g
       -I "$temporary")
for include in kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include librt/libyaml/include librt/libusb/include \
               services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
for source in main core/publication core/devices core/requests core/discover core/configparser core/match bus/rp1/publish; do
    clang "${flags[@]}" -fsyntax-only "$root/services/deviced/$source.c"
done
clang "${flags[@]}" "$root/testing/firmware/platform_device_test.c" -lyaml -o "$temporary/platform"
"$temporary/platform"
clang "${flags[@]}" "$root/testing/firmware/device_provider_test.c" -o "$temporary/providers"
"$temporary/providers"
