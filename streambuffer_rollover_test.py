"""Exercise production streambuffers at counter rollover, including contention."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-streambuffer-") as directory:
    binary = str(Path(directory) / "test")
    flags = [os.environ.get("CC", "cc"), "-std=c11", "-DVALI", "-DKERNELAPI=", "-DKERNELABI=",
             "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
             "-g", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "librt/libos/include", "librt/libds/include")]
    sources = ("testing/streambuffer_rollover_test.c", "librt/libds/streambuffer.c")
    subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
    subprocess.run([binary], check=True, timeout=60)
