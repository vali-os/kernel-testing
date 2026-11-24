"""Compile production IPC/SHM and libos with host memory/syscall fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory(prefix="vali-ipc-security-") as directory:
    out = Path(directory)
    (out / "internal").mkdir()
    (out / "internal/_syscalls.h").write_text("""
#include <os/types/ipc.h>
#include <os/types/async.h>
#include <os/types/time.h>
oserr_t Syscall_IPCSend(IPCMessage_t**, int, OSTimestamp_t*, OSAsyncContext_t*);
void usched_wait_async(void);
#define _set_errno(value) (errno = (value))
""")
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions", "-fasm-blocks", "-DVALI", "-DKERNELAPI=", "-DKERNELABI=",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-DIOSETIN=1",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
             "-Wno-unused-function",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-g", "-pthread", "-D__LIBDS_KERNEL__",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + directory]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "kernel/include", "librt/libos/include")]
    flags += ["-isystem" + str(ROOT / path) for path in
              ("librt/libds/include", "librt/libddk/include", "boot/include")]
    sources = ("testing/ipc_security_test.c", "kernel/ipc.c", "librt/libos/ipc.c",
               "kernel/memory/shm/shm.c", "kernel/memory/shm/device/device.c", "kernel/memory/shm/device/context.c", "kernel/memory/dma/address.c", "kernel/memory/dma/direct.c", "kernel/memory/shm/view.c",
               "kernel/memory/shm/device/layout.c", "kernel/memory/space/mapping.c",
               "kernel/memory/dma/unit.c", "kernel/memory/dma/translated.c",
               "kernel/memory/dma/iova.c", "librt/libds/rbtree.c",
               "kernel/memory/shm/device/cleanup.c", "kernel/memory/shm/device/attachment.c",
               # The device context reaches the region code through attachment
               # and cleanup, as in shm_device_test.run_host_test.
               "kernel/memory/shm/device/region_mapping.c", "kernel/memory/shm/device/region_session.c",
               "kernel/memory/shm/region_usage.c", "kernel/memory/shm/region.c",
               "kernel/memory/shm/access.c", "testing/shm_access_futex.c",
               "kernel/components/memory.c", "kernel/utils/memory_stack.c", "librt/libds/streambuffer.c")
    subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", str(out / "test")], check=True)
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([str(out / "test")], check=True, env=environment)

