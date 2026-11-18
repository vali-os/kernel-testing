#!/usr/bin/env bash
set -euo pipefail
mutation_check=0
if [[ ${1:-} == --mutation-check && $# == 1 ]]; then
    mutation_check=1
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--mutation-check]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-shared-tree-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for service in device file process contracts/driver contracts/filesystem contracts/usbhost; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$service.gr" \
        --out "$temporary" --lang-c --client --server
done
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
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

if [[ $mutation_check == 1 ]]; then
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "librt/libfdt/dma.c").read_text()
mutations = {
    "wrong-sdio-address": ("registerBase != sdioBase", "0"),
    "ignore-board": ('!FdtDmaHasString(rootCompatible, rootLength, "raspberrypi,5-model-b") ||', ""),
    "unlimited-dma": (".ChildLimit = UINT32_MAX", ".ChildLimit = UINT64_MAX"),
    "missing-route": ("        .State = FdtDmaPropertyIdentity", "        .State = FdtDmaPropertyAbsent"),
}
for name, (before, after) in mutations.items():
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in wrong-sdio-address ignore-board unlimited-dma missing-route; do
        clang "${flags[@]}" "-DFDT_DMA_SOURCE=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/platform_bus_test.c" "$root/librt/libfdt/parser.c" \
            -o "$temporary/$mutation"
        if "$temporary/$mutation" "$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" 0 pi5 \
            > "$temporary/$mutation.log" 2>&1; then
            echo "Mutation survived: $mutation" >&2
            exit 1
        fi
        if ! grep -Eq '^line [0-9]+:' "$temporary/$mutation.log"; then
            cat "$temporary/$mutation.log" >&2
            echo "Mutation did not reach a test assertion: $mutation" >&2
            exit 1
        fi
        echo "Mutation rejected: $mutation"
    done
fi
