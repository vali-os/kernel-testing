#!/usr/bin/env bash
# Checks that deviced leaves the PCI host behind the kernel's log port alone in
# a VALI_RPI5_EARLY_SERIAL build (__OSCONFIG_RPI5_EARLY_SERIAL), and starts
# every host as before without it. The tests are in pci_serial_host_test.inc.
set -euo pipefail
mutation_check=0
if [[ ${1:-} == --mutation-check && $# == 1 ]]; then
    mutation_check=1
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--mutation-check]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-serial-host-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections
       -fsanitize=address,undefined -g)
for include in librt/libfdt/include kernel/include boot/include librt/libc/include librt/libos/include \
               librt/libddk/include librt/libds/include librt/libgracht/include \
               librt/libacpi/source/include services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
# Every bundled BCM2712 tree: Pi 5, Pi 500 and CM5. RP1 sits behind
# /axi/pcie@1000120000 in all of them.
trees=("$root"/boot/rpi/firmware/bcm2712*.dtb)

# The changed files must also build on their own with the define.
for source in bus/pci/discovery-dt firmware/tree/tree; do
    clang "${flags[@]}" -Wno-unused-command-line-argument -D__OSCONFIG_RPI5_EARLY_SERIAL -fsyntax-only \
        "$root/services/deviced/$source.c"
done

clang "${flags[@]}" "$root/testing/firmware/pci_host_test.c" -o "$temporary/serial-off"
"$temporary/serial-off" --early-serial "${trees[@]}"
clang "${flags[@]}" -D__OSCONFIG_RPI5_EARLY_SERIAL "$root/testing/firmware/pci_host_test.c" \
    -o "$temporary/serial-on"
"$temporary/serial-on" --early-serial "${trees[@]}"

if [[ $mutation_check == 1 ]]; then
    # Each mutation of discovery-dt.c must be caught by a CHECK in
    # pci_serial_host_test.inc.
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "services/deviced/bus/pci/discovery-dt.c").read_text()
mutations = {
    # The host behind the log port is started anyway.
    "serial-host-started": [
        ("    if (g_serialHostSearch == __SerialHostFound && firmwareHost->NodeOffset == g_serialHostNode) {",
         "    if (0) {"),
    ],
    # Plain string prefixes: every leading part is looked up, not only the
    # parts that end before a "/".
    "serial-prefix-without-separator": [
        ("        if (path[end] != '/') {\n            continue;\n        }", ""),
        ("            // A missing node has no children, so no longer part can exist.\n            break;",
         "            continue;"),
    ],
    # The node the alias names counts as its own host.
    "serial-alias-names-host": [
        ("    for (end = 1; path[end] != '\\0'; end++) {\n        if (path[end] != '/') {",
         "    for (end = 1; end < length; end++) {\n        if (path[end] != '/' && path[end] != '\\0') {"),
    ],
    # The search, and its WARNING, runs again for every host.
    "serial-search-every-host": [
        ("    if (g_serialHostSearch == __SerialHostNotSearched) {", "    if (1) {"),
    ],
    # The wrong node is remembered as the host.
    "serial-wrong-node": [
        ("            g_serialHostNode = node->NodeOffset;", "            g_serialHostNode = aliases->NodeOffset;"),
    ],
    # A value without a leading "/" is used as a path.
    "serial-relative-path": [
        (" || path[0] != '/') {", ") {"),
    ],
}
for name, edits in mutations.items():
    mutated = source
    for before, after in edits:
        if mutated.count(before) != 1:
            raise SystemExit(f"mutation anchor missing: {name}")
        mutated = mutated.replace(before, after)
    (output / f"{name}.c").write_text(mutated)
PYTHON
    for mutation in serial-host-started serial-prefix-without-separator serial-alias-names-host \
                    serial-search-every-host serial-wrong-node serial-relative-path; do
        clang "${flags[@]}" -D__OSCONFIG_RPI5_EARLY_SERIAL "-DDISCOVERY_DT_SOURCE=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/pci_host_test.c" -o "$temporary/$mutation"
        if "$temporary/$mutation" --early-serial "${trees[@]}" > "$temporary/$mutation.log" 2>&1; then
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
