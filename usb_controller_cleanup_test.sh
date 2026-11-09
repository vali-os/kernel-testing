#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-usb-cleanup-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for protocol in device file process usb contracts/driver contracts/filesystem contracts/usbhost; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$protocol.gr" \
        --out "$temporary" --lang-c --client --server >/dev/null
done
flags=(-std=gnu11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DTESTING -DMOLLENOS -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -fsanitize=address,undefined -g)
for include in librt/libfdt/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include services/deviced services/deviced/include librt/libusb/include; do
    flags+=(-I "$root/$include")
done
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/usb_controller_cleanup_test.c" \
    "$root/testing/usb_controller_cleanup_host.c" "$root/librt/libos/syscalls.c" -o "$temporary/test"
"$temporary/test"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/usb_registration_test.c" -o "$temporary/registration"
"$temporary/registration"

clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/usbd_string_test.c" -o "$temporary/strings"
"$temporary/strings"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/usbd_hub_configuration_test.c" -o "$temporary/hubs"
"$temporary/hubs"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/hub_power_test.c" -o "$temporary/hub-power"
"$temporary/hub-power"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/dma_pool_isolation_test.c" -o "$temporary/dma-pool"
"$temporary/dma-pool"
