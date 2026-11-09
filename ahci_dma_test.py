"""Run AHCI failure and teardown regressions with Linux sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="vali-ahci-dma-") as directory:
        for service in ("device", "file", "process", "storage", "contracts/driver", "contracts/storage",
                        "contracts/filesystem", "contracts/usbhost"):
            subprocess.run([
                "python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                "--service", str(ROOT / "protocols" / (service + ".gr")),
                "--out", directory, "--lang-c", "--client", "--server",
            ], check=True)
        binary = Path(directory) / "ahci-dma-test"
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DMOLLENOS", "-DVALI", "-DTESTING",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=", "-DSERVICEABI=", "-DNDEBUG",
                 "-D__time64_t=int64_t", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        for include in ("modules/storage/sata", "kernel/include", "boot/include", "librt/libc/include",
                        "librt/libos/include", "librt/libddk/include", "librt/libds/include",
                        "librt/libgracht/include", "librt/libusb/include", "modules/filesystems/common/include"):
            flags += ["-I", str(ROOT / include)]
        flags += ["-I", directory]
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        for sources in (("ahci_dma_test.c", "ahci_dma_host.c"), ("ahci_identify_test.c",),
                        ("ahci_mfs_alignment_test.c", "ahci_dma_host.c")):
            subprocess.run(flags + [str(ROOT / "testing" / source) for source in sources]
                           + ["-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env=environment)
