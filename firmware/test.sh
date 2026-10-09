#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-firmware-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
pci_mode=()
if [[ "${1:-}" == "--dma-only" || "${1:-}" == "--bcm2712-only" ]]; then
    pci_mode=("$1")
fi
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= -DSERVICEAPI= -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -fsanitize=address,undefined -g)
for include in librt/libfdt/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
# Each header must provide its own types without depending on include order.
# Only the service root and its public include directory are on the search path.
for header in bus/pci/*.h bus/pci/hosts/*.h bus/pci/hosts/broadcom/*.h bus/rp1/*.h bus/legacy/*.h firmware/dma.h include/device-dma.h core/dma.h; do
    # Expand the service-relative patterns inside the service directory.
    for path in "$root/services/deviced"/$header; do
        printf '#include <%s>\n' "${path#"$root/services/deviced/"}" > "$temporary/header.c"
        clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI \
            -fsyntax-only "$temporary/header.c"
        clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI \
            -D__OSCONFIG_HAS_LEGACY_PCI -fsyntax-only "$temporary/header.c"
    done
done
# Compile the driver files independently too; the test below includes them in
# one translation unit, which can otherwise hide missing headers/declarations.
for source in host discovery dma hosts/ecam hosts/legacy bars resources hosts/broadcom/firmware hosts/broadcom/bcm hosts/broadcom/bcm2711 hosts/broadcom/bcm2712 ../legacy/fixed enumerate functionhandlers io helpers device interrupts strings; do
    clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI -c \
        "$root/services/deviced/bus/pci/$source.c" -o "$temporary/${source//\//_}.o"
    clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI \
        -D__OSCONFIG_HAS_LEGACY_PCI -c \
        "$root/services/deviced/bus/pci/$source.c" -o "$temporary/${source//\//_}-legacy.o"
done
for source in dma firmware rp1 interrupt publish; do
    clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI -fsyntax-only \
        "$root/services/deviced/bus/rp1/$source.c"
done
for source in dma fdt resources gic pci; do
    clang "${flags[@]}" -Wno-unused-command-line-argument -DVALI -fsyntax-only \
        "$root/services/deviced/firmware/$source.c"
done
clang "${flags[@]}" -DVALI "$root/testing/firmware/reader_test.c" \
    "$root/services/deviced/firmware/fdt.c" "$root/services/deviced/firmware/resources.c" \
    "$root/librt/libfdt/parser.c" -o "$temporary/reader"
"$temporary/reader"
clang "${flags[@]}" -DVALI "$root/testing/firmware/dma_test.c" \
    "$root/services/deviced/firmware/dma.c" "$root/services/deviced/firmware/resources.c" \
    "$root/services/deviced/firmware/fdt.c" "$root/librt/libfdt/parser.c" -o "$temporary/dma"
"$temporary/dma"
clang "${flags[@]}" -DVALI "$root/testing/firmware/rp1_interrupt_test.c" -o "$temporary/rp1-irq"
"$temporary/rp1-irq"
clang "${flags[@]}" -DVALI "$root/testing/firmware/pci_host_test.c" -o "$temporary/pci"
"$temporary/pci" "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" \
                 "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" "${pci_mode[@]}"
clang "${flags[@]}" -DVALI -D__OSCONFIG_HAS_LEGACY_PCI \
       "$root/testing/firmware/pci_host_test.c" -o "$temporary/pci-legacy"
"$temporary/pci-legacy" "$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" \
                       "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" "${pci_mode[@]}"
if [[ "${1:-}" == "--dma-only" || "${1:-}" == "--bcm2712-only" ]]; then
    exit 0
fi
clang "${flags[@]}" -DVALI "$root/testing/firmware/interrupt_identity_test.c" -o "$temporary/identity"
"$temporary/identity"
bash "$root/testing/firmware/platform-test.sh"
if [[ "${1:-}" == "--resources-only" ]]; then
    exit 0
fi
clang "${flags[@]}" "$root/testing/firmware/user_copy_test.c" -o "$temporary/copy"
"$temporary/copy"
