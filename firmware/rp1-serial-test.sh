#!/usr/bin/env bash
# Host test of the RP1 UART setup in deviced (services/deviced/bus/rp1/serial.c)
# on the bundled Raspberry Pi 5 and Compute Module 5 trees. With
# --mutation-check, every listed change to serial.c must make the test fail.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-rp1-serial-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
flags=(-std=c11 -fms-extensions -DKERNELAPI= -DKERNELABI= '-DSERVICEAPI=static inline' -DSERVICEABI=
       -D__time64_t=int64_t -DNDEBUG -DVALI -Wno-macro-redefined
       -ffunction-sections -fdata-sections -Wl,--gc-sections -fsanitize=address,undefined -g)
includes=()
for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
               librt/libddk/include librt/libds/include librt/libgracht/include librt/libacpi/source/include \
               services/deviced services/deviced/include; do
    includes+=(-I "$root/$include")
done
trees=("$root/boot/rpi/firmware/bcm2712-rpi-5-b.dtb" "$root/boot/rpi/firmware/bcm2712-rpi-cm5-cm5io.dtb")

clang "${flags[@]}" "${includes[@]}" -Wall -Wextra -Wno-unused-parameter -Werror -Wno-unused-command-line-argument \
    -fsyntax-only "$root/services/deviced/bus/rp1/serial.c"
clang "${flags[@]}" "${includes[@]}" "$root/testing/firmware/rp1_serial_test.c" "$root/librt/libfdt/parser.c" \
    -o "$temporary/rp1-serial"
for tree in "${trees[@]}"; do
    "$temporary/rp1-serial" "$tree"
done

if [[ "${1:-}" != "--mutation-check" ]]; then
    exit 0
fi

# Each mutation is: name, the exact text in serial.c, its replacement.
# Removing the prepare check of the clock rate request reads setup state that
# does not exist yet. The sanitizer reports that before any check, so it is
# not listed here.
mutations=(
    "clock not stopped before its input changes"
    "    __SerialAddStep(state, Rp1SerialClockMap, RP1_CLK_UART_CTRL, RP1_CLK_CTRL_ENABLE, 0);"
    ""
    "clock left on the old input"
    "RP1_CLK_UART_CTRL, RP1_CLK_CTRL_AUXSRC_MASK, RP1_CLK_UART_AUXSRC_XOSC);"
    "RP1_CLK_UART_CTRL, 0, 0);"
    "pad output driver left off"
    "        uint32_t                   clear = RP1_PADS_OUTPUT_DISABLE;"
    "        uint32_t                   clear = 0;"
    "bias not applied"
    "            set |= pulls[pin->Bias];"
    "            (void)pulls;"
    "output override kept"
    "            RP1_GPIO_CTRL_FUNCSEL_MASK | RP1_GPIO_CTRL_OUTOVER_MASK | RP1_GPIO_CTRL_OEOVER_MASK,"
    "            RP1_GPIO_CTRL_FUNCSEL_MASK,"
    "restore runs forward"
    "        i = state->Changed - 1;"
    "        i = state->StepCount - state->Changed;"
    "failed step not restored"
    "        state->Changed = i + 1;"
    "        state->Changed = i;"
    "stop with an interrupt source held"
    "    if (Rp1RuntimeChildHoldsSource(child)) {"
    "    if (0) {"
    "other functions accepted"
    "        strcmp(function, \"uart0\") != 0) {"
    "        0) {"
    "lines outside UART0 accepted"
    "    if (number < RP1_GPIO_UART0_FIRST || number > RP1_GPIO_UART0_LAST) {"
    "    if (0) {"
    "repeated line accepted"
    "            if (walk->Pins[i].Gpio == gpio) {"
    "            if (0) {"
    "second pin group ignored"
    "        if (pins->Kind != FdtPlatformPins) {"
    "        if (pins->Kind != FdtPlatformPins || controller != NULL) {"
    "clock names not checked"
    "    if (status != OS_EOK || index != RP1_SERIAL_UART_CLOCK_INDEX) {"
    "    if (0) {"
    "clock rate for every position"
    "    if (request->Index != RP1_SERIAL_UART_CLOCK_INDEX) {"
    "    if (0) {"
    "another process accepted"
    "    if (state != NULL && state->Principal != principal) {"
    "    if (0) {"
    "second UART accepted on the shared clock"
    "        if (sibling != child && sibling->Serial != NULL) {"
    "        if (0) {"
    "revision not checked"
    "    if (child->Bus->Parent->Header->Revision != RP1_REVISION_C0) {
        WARNING(\"__SerialPrepare"
    "    if (0) {
        WARNING(\"__SerialPrepare"
)
for ((i = 0; i < ${#mutations[@]}; i += 3)); do
    name=${mutations[i]}
    mutant="$temporary/mutant"
    rm -rf "$mutant"
    mkdir -p "$mutant/services/deviced/bus/rp1" "$mutant/testing/firmware"
    cp -r "$root/services" "$mutant/" 2>/dev/null || true
    cp "$root/testing/firmware/rp1_serial_test.c" "$mutant/testing/firmware/"
    MUTANT_FILE="$mutant/services/deviced/bus/rp1/serial.c" BEFORE="${mutations[i + 1]}" AFTER="${mutations[i + 2]}" \
        python3 -c '
import os
path = os.environ["MUTANT_FILE"]
text = open(path).read()
assert text.count(os.environ["BEFORE"]) == 1, os.environ["BEFORE"]
open(path, "w").write(text.replace(os.environ["BEFORE"], os.environ["AFTER"]))
'
    mutant_includes=()
    for include in kernel/include boot/include librt/libc/include librt/libos/include librt/libfdt/include \
                   librt/libddk/include librt/libds/include librt/libgracht/include librt/libacpi/source/include; do
        mutant_includes+=(-I "$root/$include")
    done
    mutant_includes+=(-I "$mutant/services/deviced" -I "$mutant/services/deviced/include")
    clang "${flags[@]}" "${mutant_includes[@]}" "$mutant/testing/firmware/rp1_serial_test.c" \
        "$root/librt/libfdt/parser.c" -o "$temporary/mutant-test" 2>/dev/null
    failed=0
    for tree in "${trees[@]}"; do
        if ! output=$("$temporary/mutant-test" "$tree" 2>&1); then
            # Only a failed check counts. A sanitizer report is not evidence
            # that a check saw the change.
            if grep -q "^line [0-9]*:" <<<"$output" && ! grep -q "Sanitizer\|runtime error:" <<<"$output"; then
                failed=1
                break
            fi
            echo "Mutation crashed instead of failing a check: $name" >&2
            echo "$output" >&2
            exit 1
        fi
    done
    if [[ $failed -ne 1 ]]; then
        echo "Mutation survived: $name" >&2
        exit 1
    fi
    echo "RP1 UART mutation rejected: $name"
done
