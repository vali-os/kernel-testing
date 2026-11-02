"""Build and exercise processd's actual capability authorization policy."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-process-security-") as directory:
    binary = Path(directory) / "test"
    (Path(directory) / "internal").mkdir()
    (Path(directory) / "internal/_io.h").write_text("#include <stdio.h>\n")
    (Path(directory) / "internal/_syscalls.h").write_text("""
#include <os/types/thread.h>
oserr_t Syscall_ThreadCreate(uintptr_t, void*, OSThreadParameters_t*, uuid_t*);
oserr_t Syscall_ThreadDetach(uuid_t);
oserr_t Syscall_ThreadSignal(uuid_t, int);
""")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-DVALI", "-DMOLLENOS", "-DTIME_MONOTONIC=TIME_UTC",
               "-D_POSIX_C_SOURCE=200809L", "-D_In_Opt_=", "-D_Out_Opt_=",
               "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare",
               "-I" + directory,
               "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]
    command += ["-I" + str(ROOT / path) for path in
                ("testing/include", "services/processd/include", "librt/libgracht/include",
                 "librt/libos/include", "librt/libddk/include", "librt/libds/include")]
    command += [str(ROOT / "testing/process_security_test.c"),
                str(ROOT / "services/processd/process.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
