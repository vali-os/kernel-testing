#!/usr/bin/env bash
# Regulator and SD host decoders: a synthetic fixture checked node by node,
# and every bundled Raspberry Pi 4 and 5 tree compared with its expected
# decoding in supply_expected/. Run with --update to rewrite the expected
# files after a reviewed change, and with --mutation-check to make sure the
# checks catch broken decoders.
set -euo pipefail
mode=check
if [[ ${1:-} == --mutation-check && $# == 1 ]]; then
    mode=mutation
elif [[ ${1:-} == --update && $# == 1 ]]; then
    mode=update
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--mutation-check|--update]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-supply-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined -Wno-unused-command-line-argument
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g)
for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
               librt/libddk/include librt/libds/include librt/libgracht/include librt/libacpi/source/include \
               services/deviced services/deviced/include; do
    flags+=(-I "$root/$include")
done
for source in services/deviced/firmware/node.c services/deviced/firmware/regulators.c \
              librt/libddk/devices/supply.c librt/libddk/devices/clock.c librt/libddk/devices/firmware-node.c \
              modules/storage/sd/binding.c; do
    clang "${flags[@]}" -fsyntax-only "$root/$source"
done

# The bundled check below uses the controller keys of the firmware driver's
# manifest. Fail when the manifest no longer declares them.
python3 - "$root/modules/platform/rpi-firmware/rpi-firmware.yaml" <<'PYTHON'
from pathlib import Path
import re
import sys

text = Path(sys.argv[1]).read_text()
compatibles = re.search(r"controller-compatibles:\s*\n\s*- (\S+)\s*\n\s*- (\S+)\s*\n", text)
if compatibles is None or compatibles.groups() != ("raspberrypi,firmware-gpio", "raspberrypi,firmware-clocks"):
    raise SystemExit("rpi-firmware.yaml no longer names the firmware GPIO and clock nodes")
route = re.search(r"clock-routes:\s*\n\s*- compatible: (\S+)\s*\n\s*clock: (\d+)\s*\n\s*controller-clock: (\d+)", text)
if route is None or route.groups() != ("brcm,bcm2711-cprman", "51", "12"):
    raise SystemExit("rpi-firmware.yaml no longer routes cprman clock 51 to firmware clock 12")
PYTHON
clang "${flags[@]}" "$root/testing/firmware/supply_decode_test.c" "$root/librt/libfdt/parser.c" \
    -o "$temporary/decode"
python3 "$root/testing/firmware/supply_fixtures.py" "$temporary/supply.dtb"
"$temporary/decode" synthetic "$temporary/supply.dtb"

trees=(bcm2711-rpi-4-b bcm2711-rpi-400 bcm2711-rpi-cm4 bcm2711-rpi-cm4-io bcm2711-rpi-cm4s
       bcm2712-rpi-5-b bcm2712d0-rpi-5-b bcm2712-d-rpi-5-b bcm2712-rpi-500 bcm2712-rpi-cm5-cm4io
       bcm2712-rpi-cm5-cm5io bcm2712-rpi-cm5l-cm4io bcm2712-rpi-cm5l-cm5io)
for tree in "${trees[@]}"; do
    expected="$root/testing/firmware/supply_expected/$tree.txt"
    "$temporary/decode" bundled "$root/boot/rpi/firmware/$tree.dtb" > "$temporary/$tree.txt"
    if [[ $mode == update ]]; then
        cp "$temporary/$tree.txt" "$expected"
    elif ! diff -u "$expected" "$temporary/$tree.txt"; then
        echo "Decoding of $tree changed" >&2
        exit 1
    fi
    echo "Regulator and SD decoding: $tree matches"
done

if [[ $mode == mutation ]]; then
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
mutations = {
    "sd-allow-contradiction": ("modules/storage/sd/binding.c",
                               "if ((nonRemovable && (cdPresent || brokenCd)) || (brokenCd && cdPresent)) {",
                               "if (0) {"),
    "sd-width-default-one": ("modules/storage/sd/binding.c",
                             "*busWidth = role == SDFirmwareRoleMemory ? 4 : 1;", "*busWidth = 1;"),
    "sd-ignore-function": ("modules/storage/sd/binding.c",
                           "            *role = SDFirmwareRoleSdio;", "            *role = SDFirmwareRoleMemory;"),
    "sd-disabled-function": ("modules/storage/sd/binding.c",
                             "if ((child.Flags & DEVICE_FIRMWARE_CHILD_ENABLED) && (child.Flags & DEVICE_FIRMWARE_CHILD_HAS_REG)) {",
                             "if (child.Flags & DEVICE_FIRMWARE_CHILD_HAS_REG) {"),
    "sd-second-line": ("modules/storage/sd/binding.c",
                       "    if (length != __GPIO_ENTRY_SIZE) {", "    if (0) {"),
    "plan-input-line-unchecked": ("services/deviced/firmware/platform-runtime.c",
                                  "            status = __PlatformInputLine(tree, resource, name);",
                                  "            status = OS_EOK;"),
    "regulator-accept-anything": ("services/deviced/firmware/regulators.c",
                                  "                         node->Name, name);\n        return OS_ENOTSUPPORTED;",
                                  "                         node->Name, name);\n        continue;"),
    "regulator-flag-polarity": ("services/deviced/firmware/regulators.c",
                                "    regulator->Enable.ActiveLow = activeHigh ? 0 : 1;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\"",
                                "    (void)activeHigh;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\""),
}
for name, (filename, before, after) in mutations.items():
    source = (root / filename).read_text()
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in sd-allow-contradiction sd-width-default-one sd-ignore-function sd-disabled-function \
                    sd-second-line plan-input-line-unchecked regulator-accept-anything regulator-flag-polarity; do
        case "$mutation" in
            sd-*) macro=BINDING_SOURCE ;;
            plan-*) macro=PLATFORM_RUNTIME_SOURCE ;;
            *) macro=REGULATORS_SOURCE ;;
        esac
        clang "${flags[@]}" -I "$root/services/deviced/firmware" -I "$root/modules/storage/sd" \
            "-D$macro=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/supply_decode_test.c" "$root/librt/libfdt/parser.c" -o "$temporary/$mutation"
        if "$temporary/$mutation" synthetic "$temporary/supply.dtb" > "$temporary/$mutation.log" 2>&1; then
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
