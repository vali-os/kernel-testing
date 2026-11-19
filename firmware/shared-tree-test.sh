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
for service in device file process contracts/driver contracts/filesystem contracts/usbhost \
               contracts/gpio contracts/clock; do
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
    "$root/librt/libfdt/parser.c" "$root/services/deviced/firmware/pci.c" \
    "$root/services/deviced/firmware/bcm.c" -o "$temporary/platform"
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
source = (root / "librt/libfdt/bcm2712.c").read_text()
mutations = {
    "wrong-sdio-address": ("path->RegisterBase != sdioBase", "0"),
    "ignore-board": ('!FdtStringListContains(path->RootCompatible, path->RootCompatibleLength, "raspberrypi,5-model-b") ||', ""),
    "unlimited-dma": (".ChildLimit = UINT32_MAX", ".ChildLimit = UINT64_MAX"),
    "missing-route": ("        .State = FdtDmaPropertyIdentity", "        .State = FdtDmaPropertyAbsent"),
}
for name, (before, after) in mutations.items():
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in wrong-sdio-address ignore-board unlimited-dma missing-route; do
        clang "${flags[@]}" -I "$root/librt/libfdt" "-DFDT_BCM2712_SOURCE=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/platform_bus_test.c" "$root/librt/libfdt/parser.c" \
            "$root/services/deviced/firmware/pci.c" "$root/services/deviced/firmware/bcm.c" \
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

    # Controller, hold-back, regulator and supply mutations. Each one must be
    # caught by a CHECK in platform_controller_test.inc,
    # platform_runtime_test.inc or platform_supply_test.inc.
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
mutations = {
    "board-wrong-clock": ("services/deviced/firmware/board-clocks.c",
                          "#define RPI_FIRMWARE_CLOCK_EMMC2  12", "#define RPI_FIRMWARE_CLOCK_EMMC2  11"),
    "no-hold-back": ("services/deviced/bus/platform/waiting.c",
                     "    return missing != 0;\n}", "    return 0;\n}"),
    "disable-ignores-users": ("services/deviced/bus/platform/controller.c",
                              "((kinds & DEVICE_CONTROLLER_CLOCK) && clockUsers != 0)", "0"),
    "stop-running-clock": ("services/deviced/bus/platform/controller.c",
                           "        if (!enabled) {", "        if (1) {"),
    "call-ended-controller": ("services/deviced/bus/platform/controller.c",
                              "    if (controller->Ended) {\n        return OS_ENOTCONNECTED;\n    }\n    *controllerOut",
                              "    *controllerOut"),
    "no-deadline": ("services/deviced/bus/platform/controller-calls.c",
                    "if (!__TimespecBefore(&now, &deadline)) {", "if (0) {"),
    "supply-ignore-opt-out": ("services/deviced/bus/platform/activation.c",
                              "!(managed & (1U << i))", "1"),
    "regulator-flag-polarity": ("services/deviced/firmware/regulators.c",
                                "    regulator->Enable.ActiveLow = activeHigh ? 0 : 1;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\"",
                                "    (void)activeHigh;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\""),
    "regulator-no-settling": ("services/deviced/bus/platform/regulator.c",
                              "return __RegulatorWaitUs(regulator, description->SettlingTimeUs);", "return OS_EOK;"),
    "regulator-no-off-on-delay": ("services/deviced/bus/platform/regulator.c",
                                  "if (on && regulator->OffAtKnown && description->OffOnDelayUs != 0) {", "if (0) {"),
    "regulator-accepts-early": ("services/deviced/bus/platform/regulator.c",
                                "    if (regulator->Description.Enable.Present &&\n        !PlatformGpioIsReady(",
                                "    if (0 &&\n        !PlatformGpioIsReady("),
    "release-leaves-on": ("services/deviced/bus/platform/regulator.c",
                          "if (hold->On && regulator->OnRequests == 1 && __RegulatorCanSwitchOff(regulator)) {", "if (0) {"),
    "loud-waiting-check": ("services/deviced/bus/platform/waiting.c",
                           "    FdtDecodeQuietBegin();\n    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &dependencies);\n    FdtDecodeQuietEnd();",
                           "    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &dependencies);"),
    "notice-every-waiting": ("services/deviced/bus/platform/waiting.c",
                             "if (!DmDiscoverHasDriver(&identification)) {", "if (0) {"),
    "manage-after-prepare": ("services/deviced/bus/platform/supply.c",
                             "    if (entry->Activation != NULL) {\n        WARNING(\"__SupplyRecordManaged",
                             "    if (0) {\n        WARNING(\"__SupplyRecordManaged"),
}
for name, (filename, before, after) in mutations.items():
    source = (root / filename).read_text()
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in board-wrong-clock no-hold-back disable-ignores-users stop-running-clock \
                    call-ended-controller no-deadline supply-ignore-opt-out regulator-flag-polarity \
                    regulator-no-settling regulator-no-off-on-delay regulator-accepts-early \
                    release-leaves-on loud-waiting-check notice-every-waiting manage-after-prepare; do
        case "$mutation" in
            board-wrong-clock) macro=BOARD_CLOCKS_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            no-hold-back) macro=WAITING_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            no-deadline) macro=CONTROLLER_CALLS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            supply-ignore-opt-out) macro=ACTIVATION_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            regulator-flag-polarity) macro=REGULATORS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            regulator-*|release-leaves-on) macro=REGULATOR_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            loud-waiting-check|notice-every-waiting) macro=WAITING_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            manage-after-prepare) macro=SUPPLY_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            *) macro=CONTROLLER_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
        esac
        clang "${flags[@]}" -I "$root/services/deviced/bus/platform" -I "$root/services/deviced/firmware" \
            "-D$macro=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/platform_bus_test.c" "$root/librt/libfdt/parser.c" \
            "$root/services/deviced/firmware/pci.c" "$root/services/deviced/firmware/bcm.c" \
            -o "$temporary/$mutation"
        if "$temporary/$mutation" "${fixture[@]}" > "$temporary/$mutation.log" 2>&1; then
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
