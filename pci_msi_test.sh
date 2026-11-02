#!/usr/bin/env bash
# Exercise the real PCI MSI engine and libddk rollback on a host with sanitizers.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-pci-msi-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -D__time64_t=int64_t -DNDEBUG -Wno-macro-redefined
       -DSERVICEAPI= -DSERVICEABI=
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -fsanitize=address,undefined -fno-sanitize-recover=all -g)
for include in services/deviced services/deviced/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include librt/libusb/include; do
    flags+=(-I "$root/$include")
done
if [[ -n "${VALI_PROTOCOLS:-}" ]]; then
    flags+=(-I "$VALI_PROTOCOLS")
else
    for service in device file process contracts/filesystem contracts/usbhost contracts/driver; do
        python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$service.gr" \
            --out "$temporary" --lang-c --client --server
    done
    flags+=(-I "$temporary")
fi
for name in device_bus_test pci_msi_test pci_msix_test pci_msix_ddk_test pci_msi_ddk_test pci_msi_rpc_test pci_msi_provider_test; do
    "${CC:-clang}" "${flags[@]}" "$root/testing/$name.c" -o "$temporary/$name"
    "$temporary/$name"
done
