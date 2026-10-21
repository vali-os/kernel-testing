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
    flags = [os.environ.get("CC", "cc"), "-std=c11", "-DVALI", "-DKERNELAPI=", "-DKERNELABI=",
             "-DSERVICEAPI=", "-DSERVICEABI=", "-DIOSETIN=1",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
             "-Wno-unused-function",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-g",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + directory]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "kernel/include", "librt/libos/include")]
    flags += ["-isystem" + str(ROOT / path) for path in
              ("librt/libds/include", "librt/libddk/include", "boot/include")]
    sources = ("testing/ipc_security_test.c", "kernel/ipc.c", "librt/libos/ipc.c",
               "kernel/memory/ms_shm.c", "kernel/memory/ms_shm_device.c",
               "kernel/components/memory.c", "kernel/utils/memory_stack.c", "librt/libds/streambuffer.c")
    subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", str(out / "test")], check=True)
    environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([str(out / "test")], check=True, env=environment)
