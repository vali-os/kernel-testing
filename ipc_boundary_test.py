"""Build memory/futex syscall forwarding and supported guards with backend fixtures."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-ipc-boundary-") as directory:
    (Path(directory) / "internal").mkdir()
    (Path(directory) / "internal/_utils.h").write_text("/* No libc internals needed by these syscalls. */\n")
    (Path(directory) / "event.h").write_text((ROOT / "librt/libc/include/event.h").read_text())
    command = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions", "-fasm-blocks", "-DVALI", "-DKERNELAPI=", "-DKERNELABI=",
               '-DPRIxIN="lx"', "-I" + directory,
               "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
               "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
    command += ["-I" + str(ROOT / path) for path in
                ("testing/include", "kernel/include", "librt/libos/include")]
    command += ["-isystem" + str(ROOT / path) for path in
                ("librt/libds/include", "librt/libddk/include", "boot/include")]
    command += [str(ROOT / path) for path in
                ("testing/ipc_boundary_test.c", "kernel/api/memory.c", "kernel/api/sync.c")]
    command += ["-Wl,--gc-sections", "-o", str(Path(directory) / "test")]
    subprocess.run(command, check=True)
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(Path(directory) / "test")], check=True, env=environment)

