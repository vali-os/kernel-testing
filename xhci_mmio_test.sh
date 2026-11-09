#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-xhci-mmio-XXXXXX)
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
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/xhci_dma_test.c" \
    "$root/testing/xhci_dma_host.c" "$root/testing/usb_transfer_work_host.c" "$root/testing/usb_transfer_bounce_host.c" -o "$temporary/test"
"$temporary/test"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/xhci_dma_manager_test.c" \
    "$root/testing/xhci_dma_manager_host.c" "$root/testing/usb_transfer_bounce_host.c" -o "$temporary/manager"
"$temporary/manager"
clang "${flags[@]}" -I "$temporary" -DVALI "$root/testing/xhci_port_power_diagnostics_test.c" \
    "$root/testing/xhci_dma_host.c" "$root/testing/usb_transfer_work_host.c" "$root/testing/usb_transfer_bounce_host.c" -o "$temporary/port-power"
"$temporary/port-power"
