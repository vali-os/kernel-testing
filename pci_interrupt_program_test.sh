#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d -t vali-pci-interrupt-test-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= -DSERVICEAPI= -DSERVICEABI=
    -D__time64_t=int64_t -DNDEBUG -Wno-macro-redefined
    -ffunction-sections -fdata-sections -Wl,--gc-sections
    -fsanitize=address,undefined -fno-sanitize-recover=all -g)
for include in services/deviced services/deviced/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include; do
    flags+=(-I "$root/$include")
done
"${CC:-clang}" "${flags[@]}" "$root/testing/pci_interrupt_program_test.c" \
    -o "$temporary/pci-interrupt-program"
ASAN_OPTIONS="${ASAN_OPTIONS:-}:detect_leaks=0" "$temporary/pci-interrupt-program"
"${CC:-clang}" "${flags[@]}" -DTEST_MSIX "$root/testing/pci_interrupt_program_test.c" \
    -o "$temporary/pci-msix-program"
ASAN_OPTIONS="${ASAN_OPTIONS:-}:detect_leaks=0" "$temporary/pci-msix-program"
