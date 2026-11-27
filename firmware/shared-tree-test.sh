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
    "$root/librt/libfdt/parser.c" "$root/services/deviced/firmware/bindings/pci.c" \
    "$root/services/deviced/firmware/boards/bcm.c" -o "$temporary/platform"
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
"$temporary/platform" "$root/boot/rpi/firmware/bcm2712-rpi-cm5-cm5io.dtb" 0 cm5

if [[ $mutation_check == 1 ]]; then
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
source = (root / "librt/libfdt/bcm2712.c").read_text()
mutations = {
    "wrong-sdio-address": ("path->RegisterBase != sdioBase", "0"),
    "ignore-soc": ('!FdtStringListContains(path->RootCompatible, path->RootCompatibleLength, "brcm,bcm2712") ||', ""),
    "ignore-length": ("path->RegisterLength != sdioLength", "0"),
    "ignore-device": ('!FdtStringListContains(path->DeviceCompatible, path->DeviceCompatibleLength, "brcm,bcm2712-sdhci") ||', ""),
    "unlimited-dma": (".ChildLimit = UINT32_MAX", ".ChildLimit = UINT64_MAX"),
    "missing-route": ("        .State = FdtDmaPropertyIdentity", "        .State = FdtDmaPropertyAbsent"),
}
for name, (before, after) in mutations.items():
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in wrong-sdio-address ignore-soc ignore-length ignore-device unlimited-dma missing-route; do
        clang "${flags[@]}" -I "$root/librt/libfdt" "-DFDT_BCM2712_SOURCE=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/platform_bus_test.c" "$root/librt/libfdt/parser.c" \
            "$root/services/deviced/firmware/bindings/pci.c" "$root/services/deviced/firmware/boards/bcm.c" \
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

    # Controller, firmware node, hold-back, regulator and supply mutations.
    # Each one must be caught by a CHECK in platform_controller_test.inc,
    # firmware_node_test.inc, platform_runtime_test.inc or
    # platform_supply_test.inc.
    python3 - "$root" "$temporary" <<'PYTHON'
from pathlib import Path
import sys

root = Path(sys.argv[1])
output = Path(sys.argv[2])
mutations = {
    "route-wrong-clock": ("services/deviced/firmware/bindings/controllers.c",
                          ".ControllerClock = rules[i].ControllerClock", ".ControllerClock = rules[i].ControllerClock - 1"),
    "route-accepts-ambiguous": ("services/deviced/firmware/bindings/controllers.c",
                                "        if (search.Count > 1) {\n            ERROR(\"__ResolveRoutes",
                                "        if (0) {\n            ERROR(\"__ResolveRoutes"),
    "client-ignores-mboxes": ("services/deviced/firmware/bindings/controllers.c",
                              "        if (mailbox.NodeOffset != search->Device) {", "        if (1) {"),
    "client-child-accepts-ambiguous": ("services/deviced/firmware/bindings/controllers.c",
                                       "    if (search.Count > 1) {\n        ERROR(\"__FindClientChild",
                                       "    if (0) {\n        ERROR(\"__FindClientChild"),
    "node-offset-unchecked": ("services/deviced/firmware/tree/node.c",
                              "    if (offset > measure.Position) {", "    if (0) {"),
    "node-no-size-limit": ("services/deviced/firmware/tree/node.c",
                           "    if (measure.Position > DEVICE_FIRMWARE_NODE_MAX_LENGTH) {", "    if (0) {"),
    "node-disabled-child-enabled": ("services/deviced/firmware/tree/node.c",
                                    "    if (!child->Disabled) {", "    if (1) {"),
    "read-accepts-length-change": ("librt/libddk/devices/firmware-node.c",
                                   "    if (request->Length != length) {", "    if (0) {"),
    "read-keeps-failed-description": ("librt/libddk/devices/firmware-node.c",
                                      "error:\n    free(data);\n    return status;",
                                      "error:\n    return status;"),
    "parse-accepts-duplicates": ("librt/libddk/devices/firmware-node.c",
                                 "            if (!strcmp(__RecordName(&result, earlier), __RecordName(&result, offset))) {",
                                 "            if (0) {"),
    "no-hold-back": ("services/deviced/bus/platform/lifecycle/waiting.c",
                     "    return missing != 0;\n}", "    return 0;\n}"),
    "disable-ignores-users": ("services/deviced/bus/platform/controller/controller.c",
                              "    if ((kinds & DEVICE_CONTROLLER_GPIO) && controller->GpioUsers != 0) {", "    if (0) {"),
    "stop-running-clock": ("services/deviced/bus/platform/controller/clock-owner.c",
                           "        if (!enabled) {", "        if (1) {"),
    "call-ended-controller": ("services/deviced/bus/platform/controller/calls.c",
                              "    if (controller->State == PlatformControllerEnded) {\n        return OS_ENOTCONNECTED;\n    }\n    return OS_EOK;",
                              "    return OS_EOK;"),
    "no-deadline": ("services/deviced/bus/platform/controller/calls.c",
                    "if (!DmTimeBefore(&now, &deadline)) {", "if (0) {"),
    "supply-ignore-opt-out": ("services/deviced/bus/platform/requests/prepare.c",
                              "!(managed & (1U << index))", "1"),
    "regulator-flag-polarity": ("services/deviced/firmware/bindings/regulators.c",
                                "    regulator->Enable.ActiveLow = activeHigh ? 0 : 1;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\"",
                                "    (void)activeHigh;\n\n    return __RegulatorOptionalScalar(node, \"off-on-delay-us\""),
    "regulator-no-settling": ("services/deviced/bus/platform/owners/regulator.c",
                              "return __RegulatorWaitUs(regulator, description->SettlingTimeUs);", "return OS_EOK;"),
    "regulator-no-off-on-delay": ("services/deviced/bus/platform/owners/regulator.c",
                                  "if (on && description->OffOnDelayUs != 0) {", "if (0) {"),
    "regulator-unknown-off-time-no-delay": ("services/deviced/bus/platform/owners/regulator.c",
                                            "            return __RegulatorWaitUs(regulator, regulator->Description.OffOnDelayUs);",
                                            "            return OS_EOK;"),
    "regulator-off-fails-without-clock": ("services/deviced/bus/platform/owners/regulator.c",
                                          "            regulator->OffTime = PlatformRegulatorOffTimeUnknown;\n            return OS_EOK;",
                                          "            regulator->OffTime = PlatformRegulatorOffTimeUnknown;\n            return OS_EUNKNOWN;"),
    "release-without-binding-job": ("services/deviced/bus/platform/lifecycle/waiting.c",
                                    "                  entry->DeviceId);\n            continue;\n        }",
                                    "                  entry->DeviceId);\n        }"),
    "free-controller-with-clock-users": ("services/deviced/bus/platform/controller/controller.c",
                                         "        __ControllerEnd(controller, \"its clock nodes could not be unregistered\");\n        return status;\n    }",
                                         "    }"),
    "unregister-ignores-clock-users": ("services/deviced/bus/platform/controller/clock-owner.c",
                                       "        if (users != 0) {", "        if (0) {"),
    "fixed-clock-zero-rate": ("services/deviced/firmware/bindings/dependencies.c",
                              "    if (frequency == 0) {", "    if (0) {"),
    "fixed-clock-any-node": ("services/deviced/firmware/bindings/dependencies.c",
                             "    if (!FdtCompatible(node, \"fixed-clock\")) {", "    if (0) {"),
    "pins-without-default": ("services/deviced/firmware/bindings/dependencies.c",
                             "        if (names != NULL) {\n            FDT_DECODE_ERROR(\"__PlatformPins: node %u has no default pinctrl state",
                             "        if (0) {\n            FDT_DECODE_ERROR(\"__PlatformPins: node %u has no default pinctrl state"),
    "regulator-accepts-early": ("services/deviced/bus/platform/owners/regulator.c",
                                "    if (regulator->Description.Enable.Present &&\n        !PlatformGpioIsReady(",
                                "    if (0 &&\n        !PlatformGpioIsReady("),
    "release-leaves-on": ("services/deviced/bus/platform/owners/regulator.c",
                          "if (regulatorToken->On && regulator->OnRequests == 1 && __RegulatorCanSwitchOff(regulator)) {", "if (0) {"),
    "loud-plan-collect": ("services/deviced/bus/platform/lifecycle/plan.c",
                          "    FdtDecodeQuietBegin();\n    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &plan);\n    FdtDecodeQuietEnd();",
                          "    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &plan);"),
    "broken-plan-no-reason": ("services/deviced/bus/platform/lifecycle/plan.c",
                              "    status = FdtPlatformDependencies(entry->Bus->Tree, entry->Firmware.FirmwareNode, &discarded);",
                              "    (void)discarded;\n    status = entry->PlanStatus;"),
    "stored-plan-short": ("services/deviced/bus/platform/lifecycle/plan.c",
                          "    entry->PlanCount = plan.Count;", "    entry->PlanCount = plan.Count - 1;"),
    "notice-every-waiting": ("services/deviced/bus/platform/lifecycle/waiting.c",
                             "if (!DmDriverAnyMatches(&identification)) {", "if (0) {"),
    "regulator-gpio-unsupported": ("services/deviced/bus/platform/owners/regulator.c",
                                   "            return PlatformOwnerDriverResult(status);\n        }\n        return OS_EOK;\n    }\n\n"
                                   "    status = PlatformGpioSetValue(regulator->Bus, line->Line.Controller, line->Line.Line, value);\n"
                                   "    if (status != OS_EOK) {\n        return PlatformOwnerDriverResult(status);",
                                   "            return status;\n        }\n        return OS_EOK;\n    }\n\n"
                                   "    status = PlatformGpioSetValue(regulator->Bus, line->Line.Controller, line->Line.Line, value);\n"
                                   "    if (status != OS_EOK) {\n        return status;"),
    "regulator-selector-unsupported": ("services/deviced/bus/platform/owners/regulator.c",
                                       "regulator->Name, status, PlatformOwnerDriverResult(status));\n        return PlatformOwnerDriverResult(status);",
                                       "regulator->Name, status, PlatformOwnerDriverResult(status));\n        return status;"),
    "clock-unsupported-passed": ("services/deviced/bus/platform/controller/clock-owner.c",
                                 "    if (status != OS_EOK) {\n        return PlatformOwnerDriverResult(status);\n    }",
                                 "    if (status != OS_EOK) {\n        return status;\n    }"),
    "interrupt-parent-controller-only": ("services/deviced/firmware/tree/resources.c",
                                         "        node->InterruptParent = nodes[index - 1].HasInterruptCells ?",
                                         "        node->InterruptParent = nodes[index - 1].IsInterruptController ?"),
    "manage-after-prepare": ("services/deviced/bus/platform/requests/supply.c",
                             "    if (entry->Preparation != NULL) {\n        WARNING(\"__SupplyRecordManaged",
                             "    if (0) {\n        WARNING(\"__SupplyRecordManaged"),
}
for name, (filename, before, after) in mutations.items():
    source = (root / filename).read_text()
    if source.count(before) != 1:
        raise SystemExit(f"mutation anchor missing: {name}")
    (output / f"{name}.c").write_text(source.replace(before, after))
PYTHON
    for mutation in route-wrong-clock route-accepts-ambiguous client-ignores-mboxes \
                    client-child-accepts-ambiguous node-offset-unchecked node-no-size-limit \
                    node-disabled-child-enabled read-accepts-length-change read-keeps-failed-description \
                    parse-accepts-duplicates no-hold-back disable-ignores-users stop-running-clock \
                    call-ended-controller no-deadline supply-ignore-opt-out regulator-flag-polarity \
                    regulator-no-settling regulator-no-off-on-delay regulator-accepts-early \
                    release-leaves-on regulator-gpio-unsupported regulator-selector-unsupported \
                    clock-unsupported-passed loud-plan-collect broken-plan-no-reason stored-plan-short \
                    notice-every-waiting manage-after-prepare interrupt-parent-controller-only \
                    regulator-unknown-off-time-no-delay regulator-off-fails-without-clock \
                    release-without-binding-job free-controller-with-clock-users \
                    unregister-ignores-clock-users fixed-clock-zero-rate fixed-clock-any-node \
                    pins-without-default; do
        case "$mutation" in
            route-wrong-clock) macro=CONTROLLERS_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            route-accepts-ambiguous|client-ignores-mboxes|client-child-accepts-ambiguous)
                macro=CONTROLLERS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            node-*) macro=NODE_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            read-*|parse-*) macro=DDK_FIRMWARE_NODE_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            stop-running-clock) macro=CONTROLLER_CLOCK_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            call-ended-controller) macro=CONTROLLER_CALLS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            no-hold-back|release-without-binding-job)
                macro=WAITING_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            unregister-ignores-clock-users)
                macro=CONTROLLER_CLOCK_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            fixed-clock-zero-rate|fixed-clock-any-node|pins-without-default)
                macro=DEPENDENCIES_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            no-deadline) macro=CONTROLLER_CALLS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            supply-ignore-opt-out) macro=PREPARE_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            regulator-flag-polarity) macro=REGULATORS_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            regulator-gpio-unsupported|regulator-selector-unsupported)
                macro=REGULATOR_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            clock-unsupported-passed)
                macro=CONTROLLER_CLOCK_SOURCE; fixture=("$root/boot/rpi/firmware/bcm2711-rpi-4-b.dtb" 0 pi4) ;;
            regulator-*|release-leaves-on) macro=REGULATOR_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            loud-plan-collect|broken-plan-no-reason|stored-plan-short)
                macro=PLAN_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            notice-every-waiting) macro=WAITING_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            manage-after-prepare) macro=SUPPLY_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            interrupt-parent-controller-only)
                macro=TREE_RESOURCES_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
            *) macro=CONTROLLER_SOURCE; fixture=("$temporary/alias.dtb" 0 synthetic) ;;
        esac
        clang "${flags[@]}" "-D$macro=\"$temporary/$mutation.c\"" \
            "$root/testing/firmware/platform_bus_test.c" "$root/librt/libfdt/parser.c" \
            "$root/services/deviced/firmware/bindings/pci.c" "$root/services/deviced/firmware/boards/bcm.c" \
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
