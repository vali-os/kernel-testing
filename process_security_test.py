"""Build and exercise processd's actual capability authorization policy."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-process-security-") as directory:
    binary = Path(directory) / "test"
    command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
               "-DSERVICEAPI=", "-DSERVICEABI="]
    command += ["-I" + str(ROOT / path) for path in
                ("testing/include", "services/processd/include", "librt/libgracht/include",
                 "librt/libos/include", "librt/libddk/include", "librt/libds/include")]
    command += [str(ROOT / "testing/process_security_test.c"),
                str(ROOT / "services/processd/process_security.c"), "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True)
