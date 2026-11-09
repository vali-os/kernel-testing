#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d -t vali-genet-XXXXXX)
trap 'rm -rf "$temporary"' EXIT
for protocol in contracts/netadapter contracts/driver device file process contracts/filesystem contracts/usbhost; do
    python3 "$root/librt/libgracht/generator/parser.py" --service "$root/protocols/$protocol.gr" \
        --out "$temporary" --lang-c --client --server >/dev/null
done
flags=(-std=c11 -fms-extensions -DVALI -DMOLLENOS -D_POSIX_C_SOURCE=200809L
       '-DSERVICEAPI=static inline' -DSERVICEABI= -Wall -Wextra -Werror
       -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -Wno-ignored-attributes
       -ffunction-sections -fdata-sections -fsanitize=address,undefined -g)
for include in testing/netadapter/include testing/include librt/libos/include librt/libgracht/include \
               librt/libddk/include librt/libds/include librt/libusb/include; do
    flags+=(-I "$root/$include")
done
if [[ ${ETHERNET_DIAGNOSTICS:-0} == 1 ]]; then
    flags+=(-DVALI_ETHERNET_DIAGNOSTICS=1)
fi
flags+=(-isystem "$temporary" -idirafter "$root/librt/libc/include")
clang "${flags[@]}" "$root/testing/ethernet/phy_test.c" "$root/testing/ethernet/logging.c" \
    "$root/modules/eth/common/mdio.c" "$root/modules/eth/common/phy.c" \
    "$root/modules/eth/common/phy-broadcom.c" -Wl,--gc-sections -o "$temporary/phy"
"$temporary/phy"
clang "${flags[@]}" "$root/testing/ethernet/genet_test.c" "$root/testing/ethernet/logging.c" \
    "$root/modules/eth/common/mdio.c" "$root/modules/eth/genet/mdio.c" \
    "$root/modules/eth/genet/integration.c" "$root/modules/eth/genet/hardware.c" \
    "$root/modules/eth/genet/queue.c" "$root/modules/eth/genet/diagnostics.c" -Wl,--gc-sections -o "$temporary/genet"
"$temporary/genet"
clang "${flags[@]}" "$root/testing/ethernet/session_test.c" \
    "$root/modules/eth/common/netadapter.c" "$root/modules/eth/common/session.c" \
    "$root/modules/eth/common/pools.c" "$root/modules/eth/common/requests.c" \
    "$root/librt/libds/list.c" -Wl,--gc-sections -o "$temporary/session"
"$temporary/session"
clang "${flags[@]}" -DTIME_MONOTONIC=1 "$root/testing/ethernet/lifecycle_test.c" \
    "$root/modules/eth/genet/controller.c" -Wl,--gc-sections -o "$temporary/lifecycle"
"$temporary/lifecycle"
clang "${flags[@]}" -I "$root/services/netd" \
    "$root/testing/ethernet/hardware_test.c" "$root/testing/ethernet/logging.c" \
    -Wl,--gc-sections -o "$temporary/hardware"
"$temporary/hardware"
