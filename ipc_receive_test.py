"""Build the actual libos IPC receive function with only its OS dependencies stubbed."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-ipc-receive-") as directory:
    output = Path(directory)
    (output / "ddk").mkdir()
    (output / "internal").mkdir()
    (output / "ddk/utils.h").write_text("""
#include <errno.h>
#define TRACE(...)
#define _set_errno(value) (errno=(value))
""")
    (output / "internal/_syscalls.h").write_text("""
#include <os/ipc.h>
oserr_t Syscall_IPCSend(IPCMessage_t**, int, OSTimestamp_t*, OSAsyncContext_t*);
""")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-Wall", "-Wextra", "-Werror", "-UNDEBUG", "-ffunction-sections", "-fdata-sections",
               "-I" + directory, "-I" + str(ROOT / "testing/include"),
               "-I" + str(ROOT / "librt/libos/include"), "-I" + str(ROOT / "librt/libds/include"),
               str(ROOT / "testing/ipc_receive_test.c"), str(ROOT / "librt/libos/ipc.c"),
               "-Wl,--gc-sections", "-o", str(output / "test")]
    command.extend(shlex.split(os.environ.get("IPC_TEST_CFLAGS", "")))
    subprocess.run(command, check=True)
    subprocess.run([str(output / "test")], check=True)
