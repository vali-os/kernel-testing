# Graphics buffer acceptance harness

This Linux host harness builds the real graphics core, Gracht runtime, generated
bindings and libos fence/reservation implementations from this source tree. It
uses a software backend with deterministic delayed byte-fill commands and both
host allocation and native SHM storage. No download or GPU is required.

Run out of tree:

```sh
cmake -S /path/to/vali/testing/graphics -B /tmp/graphics-build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/graphics-build -j 4
ctest --test-dir /tmp/graphics-build --output-on-failure
```

Use a separate build for AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S /path/to/vali/testing/graphics -B /tmp/graphics-sanitized -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/graphics-sanitized -j 4
ctest --test-dir /tmp/graphics-sanitized --output-on-failure
```

The socket tests need permission to create local Linux abstract Unix sockets.
They do not create socket paths in the filesystem. The process fixture uses a
unique name, observes server readiness, applies timeouts, and terminates only its
own children on failure. Assertions remain enabled in Release test builds.

## Acceptance coverage

| Test | Behavior checked |
| --- | --- |
| `graphics-core` | Original negotiation, authentication, context retry, quotas, stale generations and cleanup tests. |
| `graphics-buffers` | Zeroed allocation, buffer replay, range overflow, byte/slot/job limits, stable completion snapshots, ordered fills, independent session progress, released dependency capacity, failed preparation/start/poll/write and post-disconnect draining. |
| `graphics-protocol` | Two real connections, generated codecs, malformed request rejection, authenticated buffer/fill/completion calls, range/count limits, duplicate submission, pending readback, empty error replies and stale references. |
| `graphics-shm` | Real libos SHM wrappers and native adapter: stable mappings, explicit CPU modes, protected End failures, stale tokens, active-view disconnect and retryable close/partial release. |
| `graphics-shm-process-clients` | The same two-process client flow through native SHM storage with a host kernel boundary. |
| `graphics-process-clients` | Separate server and two client processes upload, submit/retry, poll and verify partial-fill readback. Both leave work pending after releasing public references; the server timer finishes all four jobs and frees every resource. |

The delayed fill covers only its requested range; clients verify surrounding bytes
remain unchanged. Completion A remains complete while B on the same context is
pending. Poll errors leave contexts and storage valid; pending work cannot be read
or freed. A full CPU overwrite repairs failed writer history through real libos
reservation operations. The capacity regression keeps old dependency fences after
their original completion slots are recycled.

## Test-specific boundaries

`graphics_test.gr` is compiled and registered only here. It provides bounded
read/write copies, a typed fill command and deterministic device progress. The
common production contract carries buffer management and completion polling, not
fill command semantics or diagnostic byte arrays. No hardware extension is enabled.

The fixture's event loop also advances work without client messages, including
after both clients disconnect. Core tests can freeze execution, inject an
observation error, and later resume it without changing public completion state.
Execution, storage failures and context cleanup have separate controls.

The original storage fixture uses host allocations. The SHM fixture compiles
`graphics-shm` unchanged together with actual libos `shm.c`, `shm_access.c` and
`syscalls.c`. `shm_host.c` replaces only the kernel boundary and the single-reference
local handle registry. It checks SHM flags, whole-buffer session bounds, nonblocking
read/write requests, and release ordering. It can fail creation, session open,
begin, end, close and release after an unmap. It does not test native page/cache
behavior, kernel grant authorization, or multiple local attachment aliases.

The actual libos `fence_pool.c`, `buffer_access.c` and `fence_storage.c` are also
compiled into the harness.
`sync_host.c` supplies TLS and deliberately fails on a blocking/futex call: pools
are private, no shared waiters exist, and this executor must never wait for itself.
This tests libos tracking logic without claiming native cache/device transitions.

The native library can be compiled separately:

```sh
cmake -S /path/to/vali -B /tmp/graphics-aarch64 -G Ninja \
    -DVALI_ARCH=aarch64 -DVALI_PLATFORM=rpi
cmake --build /tmp/graphics-aarch64 --target graphics-common graphics-shm -j 4
```

That uses the installed Vali toolchain and builds `graphics-core.lib` and
`graphics-common.lib` and `graphics-shm.lib`, including native authentication
and native SHM/access imports. It does not build or
install the Linux fixture. Cross-process mapping/sharing, DMA access, hardware stop,
blocking wait registration and a Vali module/process-death integration test remain
separate work. See [the service notes](../../modules/graphics/README.md).
