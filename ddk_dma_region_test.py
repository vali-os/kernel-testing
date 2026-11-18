"""Check the DDK device-region contract through the real syscall forwarding."""
import os
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-ddk-regions-") as directory:
    binary = str(Path(directory) / "regions")
    flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING", "-DSERVICEAPI=extern", "-DSERVICEABI=",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-g", "-ffunction-sections", "-fdata-sections"]
    flags += ["-I" + str(ROOT / path) for path in ("testing/include", "librt/libos/include", "librt/libddk/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    subprocess.run(flags + [str(ROOT / "testing/ddk_dma_region_test.c"), str(ROOT / "librt/libos/syscalls.c"),
                           "-Wl,--gc-sections", "-o", binary], check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([binary], check=True, env=env, timeout=30)
