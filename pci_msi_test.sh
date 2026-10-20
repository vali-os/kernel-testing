#!/usr/bin/env bash
# Exercise the real PCI MSI engine and libddk rollback on a host with sanitizers.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-pci-msi-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -D__time64_t=int64_t -DNDEBUG -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -fsanitize=address,undefined -fno-sanitize-recover=all -g)
for include in services/deviced librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include; do
    flags+=(-I "$root/$include")
done
flags+=(-I "${VALI_PROTOCOLS:-$root/build/protocols}")
for name in pci_msi_test pci_msi_ddk_test pci_msi_rpc_test; do
    "${CC:-clang}" "${flags[@]}" "$root/testing/$name.c" -o "$temporary/$name"
    "$temporary/$name"
done
