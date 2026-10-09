"""Compile the real Virtio net pool/session/queue code against a host DMA model."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions-only", action="store_true",
                        help="test open/close without the netd integration harness")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="virtio-net-test-") as directory:
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/contracts/netadapter.gr"),
                        "--out", directory, "--lang-c", "--client", "--server"], check=True)
        includes = ["testing/netadapter/include", "testing/include", "testing", "services/netd/adapters",
                    "services/netd", "modules/virtio/net", "modules/virtio/common/include",
                    "librt/libos/include", "librt/libgracht/include", "librt/libddk/include", "librt/libds/include"]
        sources = ["testing/netadapter/virtio_test.c", "testing/net_shm_mock.c"]
        if not args.sessions_only:
            sources += [f"services/netd/adapters/{name}.c" for name in
                        ("buffers", "adapter", "queue", "rx", "scheduler", "events")]
        sources += [f"modules/virtio/net/{name}.c" for name in ("pools", "session", "queue", "requests")]
        command = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DVALI", "-DTESTING",
                   "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-fms-extensions", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections"]
        if args.sessions_only:
            command += ["-DVIRTIO_NET_SESSION_TEST_ONLY", "-Wno-unused-variable"]
        command += ["-I" + str(ROOT / path) for path in includes] + ["-isystem", directory]
        command += [str(ROOT / path) for path in sources]
        command += shlex.split(os.environ.get("VIRTIO_NET_TEST_CFLAGS", ""))
        executable = str(Path(directory) / "virtio-net-test")
        command += ["-Wl,--gc-sections", "-o", executable]
        subprocess.run(command, check=True)
        subprocess.run([executable], check=True)

if __name__ == "__main__":
    main()
