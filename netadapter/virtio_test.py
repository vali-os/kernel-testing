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
    parser.add_argument("--mutation-check", action="store_true",
                        help="verify initialization and DMA address regressions are detected")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="virtio-net-test-") as directory:
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/contracts/netadapter.gr"),
                        "--out", directory, "--lang-c", "--client", "--server"], check=True)
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/process.gr"),
                        "--out", directory, "--lang-c"], check=True)
        includes = ["testing/netadapter/include", "testing/include", "testing", "services/netd/adapters",
                    "services/netd", "modules/virtio/net", "modules/virtio/common/include",
                    "librt/libos/include", "librt/libgracht/include", "librt/libddk/include", "librt/libds/include"]
        sources = ["testing/netadapter/virtio_test.c", "testing/net_shm_mock.c"]
        if not args.sessions_only:
            sources += [f"services/netd/adapters/{name}.c" for name in
                        ("buffers", "adapter", "queue", "rx", "tx", "scheduler", "events")]
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
        if args.mutation_check:
            device_source = (ROOT / "modules/virtio/net/device.c").read_text()
            anchor = "    for (offset = 0; offset < DMABufferSize(device->Metadata); offset += VIRTIO_NET_METADATA_STRIDE) {"
            assert anchor in device_source
            device_mutant = Path(directory) / "device-mutant.c"
            device_mutant.write_text(device_source.replace(anchor,
                "    if (device->MetadataSegmentCount != 1) {\n"
                "        status = OS_ENOTSUPPORTED;\n        goto error;\n    }\n" + anchor))
            queue_source = (ROOT / "modules/virtio/net/queue.c").read_text()
            anchor = "status = VirtioNetMetadataAddress(device, slot->MetadataOffset, &metadata);"
            assert anchor in queue_source
            queue_mutant = Path(directory) / "queue-mutant.c"
            queue_mutant.write_text(queue_source.replace(anchor,
                "metadata = device->MetadataSegments[0].Address + slot->MetadataOffset;\n"
                "    status = OS_EOK;"))
            pool_source = (ROOT / "modules/virtio/net/pools.c").read_text()
            pool_check = "(transfer.Token.Options.Scope & scope) != scope"
            assert pool_check in pool_source
            pool_mutant = Path(directory) / "pool-mutant.c"
            pool_mutant.write_text(pool_source.replace(pool_check,
                "(transfer.Token.Options.Scope & scope) == scope"))
            mutations = {
                "contiguous allocation": command + [f'-DVIRTIO_NET_DEVICE_SOURCE="{device_mutant}"'],
                "pool token scope": [str(pool_mutant) if arg == str(ROOT / "modules/virtio/net/pools.c")
                                     else arg for arg in command],
                "first-segment DMA address": [str(queue_mutant) if arg == str(ROOT / "modules/virtio/net/queue.c")
                                              else arg for arg in command],
            }
            if not args.sessions_only:
                buffer_source = (ROOT / "services/netd/adapters/buffers.c").read_text()
                anchor = "        slotAlignment,\n        &stride"
                assert buffer_source.count(anchor) == 1
                buffer_mutant = Path(directory) / "buffers-mutant.c"
                buffer_mutant.write_text(buffer_source.replace(
                    anchor, "        info->buffer_alignment,\n        &stride"))
                mutations["adjacent RX cache lines"] = [
                    str(buffer_mutant) if arg == str(ROOT / "services/netd/adapters/buffers.c")
                    else arg for arg in command]
            if args.sessions_only:
                mutations.pop("first-segment DMA address")
                mutations.pop("pool token scope")
            for name, mutant_command in mutations.items():
                subprocess.run(mutant_command, check=True)
                result = subprocess.run([executable])
                if result.returncode == 0:
                    raise RuntimeError(f"Undetected regression: {name}")
                print(f"Rejected {name} mutation", flush=True)

if __name__ == "__main__":
    main()
