"""Build a disposable Virtio network test image from a configured, built tree.

Copies deployment files before excluding Virtio storage. This isolates the NIC
acceptance test from storage mounting; ordinary deployed files and disk.img are
never modified. Use the regular mkimage target for full-system integration tests.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    output = args.output.resolve()
    cache = (build / "CMakeCache.txt").read_text()
    if "VALI_VIRTIO_NET_TESTS:BOOL=ON" not in cache:
        parser.error("configure with -DVALI_VIRTIO_NET_TESTS=ON and rebuild first")
    if output.exists():
        parser.error("output already exists; choose a new test image path")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vali-virtio-image-") as directory:
        temporary = Path(directory)
        system = temporary / "system"
        boot = temporary / "boot"
        shutil.copytree(build / "deploy/hdd/system", system)
        shutil.copytree(build / "deploy/hdd/boot", boot)
        # Discovery expects a manifest for every module binary: omit the pair.
        for name in ("virtio-blk.dll", "virtio-blk.yaml"):
            (system / "modules" / name).unlink(missing_ok=True)
        subprocess.run([str(build / "tools/host/bin/mkvafs"), "--arch", "amd64",
                        "--compression", "aplib", "--out", str(boot / "initrd.mos"),
                        str(system)], check=True)
        subprocess.run([str(build / "tools/host/bin/lzss"), "c", str(build / "bin/kernel.mos"),
                        str(boot / "kernel.mos")], check=True)
        shutil.copy2(build / "bin/phoenix.mos", boot)
        model = (ROOT / "cmake/models/vali-gpt.yaml").read_text()
        model = model.replace("source: deploy/hdd/boot", "source: " + json.dumps(str(boot)))
        model = model.replace("source: deploy/hdd/system", "source: " + json.dumps(str(system)))
        model_path = temporary / "image.yaml"
        model_path.write_text(model)
        subprocess.run(["mkcdk", "-o", str(output), str(model_path)], cwd=build, check=True)
    print(f"Isolated network test image: {output}")


if __name__ == "__main__":
    main()
