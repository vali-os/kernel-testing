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
temporary=$(mktemp -d -t vali-platform-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for service in device file process contracts/filesystem contracts/usbhost contracts/driver; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$service.gr" \
        --out "$temporary" --lang-c --client --server
done
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -DMOLLENOS -Wno-macro-redefined -Wno-unused-command-line-argument
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g
       -I "$temporary")
for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include librt/libyaml/include librt/libusb/include \
               services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
for source in main core/publication core/devices core/dma-device core/requests core/discover core/configparser core/match bus/rp1/publish; do
    clang "${flags[@]}" -fsyntax-only "$root/services/deviced/$source.c"
done
clang "${flags[@]}" "$root/testing/firmware/platform_device_test.c" -lyaml -o "$temporary/platform"
"$temporary/platform"
clang "${flags[@]}" "$root/testing/firmware/device_provider_test.c" -o "$temporary/providers"
"$temporary/providers"

if [[ $mutation_check == 1 ]]; then
    # Build isolated copies. The working tree stays unchanged during mutation checks.
    python3 - "$root/services/deviced/core/dma-device.c" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
output = Path(sys.argv[2])
mutations = {
    "skip-removal": ("if (device->Removing)", "if (0)"),
    "lose-address-limit": ("*descriptionOut = description;",
                           "description.AddressLimit = UINT64_MAX; *descriptionOut = description;"),
}
for name, (before, after) in mutations.items():
    if before not in source:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after, 1))
PYTHON
    for mutation in skip-removal lose-address-limit; do
        clang "${flags[@]}" -I "$root/services/deviced/core" \
            "-DDMA_DEVICE_SOURCE=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/device_provider_test.c" -o "$temporary/$mutation"
        if "$temporary/$mutation" > "$temporary/$mutation.log" 2>&1; then
            echo "Mutation survived: $mutation" >&2
            exit 1
        fi
        # A CHECK failure proves the assertion rejected the mutant. A crash or
        # sanitizer setup failure alone does not count as a passing mutation check.
        if ! grep -Eq '^line [0-9]+:' "$temporary/$mutation.log"; then
            cat "$temporary/$mutation.log" >&2
            echo "Mutation did not reach a test assertion: $mutation" >&2
            exit 1
        fi
        echo "Mutation rejected: $mutation"
    done
fi
