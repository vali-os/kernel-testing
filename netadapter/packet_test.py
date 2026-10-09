"""Test TX construction and retained RX against the real pool, queue and close implementation."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix="net-packet-test-") as directory:
    subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                    "--service", str(ROOT / "protocols/contracts/netadapter.gr"),
                    "--out", directory, "--lang-c"], check=True)
    # The pool header includes target logging/TLS declarations, unused here.
    (Path(directory) / "ddk").mkdir()
    (Path(directory) / "ddk/utils.h").write_text("/* Host test: no target TLS or logging. */\n")
    includes = ["testing/include", "testing", "services/netd/adapters",
                "librt/libos/include", "librt/libgracht/include"]
    sources = ["testing/netadapter/packet_test.c", "testing/net_shm_mock.c"]
    sources += [f"services/netd/adapters/{name}.c" for name in ("adapter", "buffers", "queue", "rx", "tx")]
    command = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-DSERVICEAPI=", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
               "-isystem", directory]
    command += ["-I" + str(ROOT / path) for path in includes]
    command += [str(ROOT / path) for path in sources]
    command += shlex.split(os.environ.get("NET_PACKET_TEST_CFLAGS", ""))
    command += ["-o", str(Path(directory) / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(Path(directory) / "test")], check=True)
