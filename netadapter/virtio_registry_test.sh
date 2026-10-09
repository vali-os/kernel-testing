#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
directory=$(mktemp -d)
trap 'rm -rf "$directory"' EXIT

for protocol in contracts/netadapter contracts/driver device; do
    python3 "$root/librt/libgracht/generator/parser.py" \
        --service "$root/protocols/$protocol.gr" \
        --out "$directory" --lang-c --client --server
done

includes=(testing/netadapter/include testing/include modules/virtio/net
          modules/virtio/common/include librt/libos/include librt/libgracht/include
          librt/libddk/include librt/libds/include)
flags=()
for include in "${includes[@]}"; do
    flags+=("-I$root/$include")
done

"${CC:-clang}" -std=c11 -D_POSIX_C_SOURCE=200809L -DVALI -DTESTING \
    '-DSERVICEAPI=static inline' -DSERVICEABI= -fms-extensions \
    -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter \
    -Wno-sign-compare -ffunction-sections -fdata-sections \
    "${flags[@]}" -isystem "$directory" -idirafter "$root/librt/libc/include" \
    "$root/testing/netadapter/virtio_registry_test.c" "$root/librt/libds/list.c" \
    -Wl,--gc-sections -o "$directory/test"
"$directory/test"