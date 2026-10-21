"""Build actual memory/futex syscall guards with inert backend fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-ipc-boundary-") as directory:
    (Path(directory) / "internal").mkdir()
    (Path(directory) / "internal/_utils.h").write_text("/* No libc internals needed by these syscalls. */\n")
    (Path(directory) / "event.h").write_text((ROOT / "librt/libc/include/event.h").read_text())
    command = [os.environ.get("CC", "cc"), "-std=c11", "-DVALI", "-DKERNELAPI=", "-DKERNELABI=",
               '-DPRIxIN="lx"', "-I" + directory,
               "-DSERVICEAPI=", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    command += ["-I" + str(ROOT / path) for path in
                ("testing/include", "kernel/include", "librt/libos/include")]
    command += ["-isystem" + str(ROOT / path) for path in
                ("librt/libds/include", "librt/libddk/include", "boot/include")]
    command += [str(ROOT / path) for path in
                ("testing/ipc_boundary_test.c", "kernel/api/memory.c", "kernel/api/sync.c", "kernel/memory/ms_utils.c")]
    command += ["-Wl,--gc-sections", "-o", str(Path(directory) / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(Path(directory) / "test")], check=True)
