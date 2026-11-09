"""Exercise UHCI bounce and legacy USB geometry with real code and sanitizers."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-uhci-bounce-") as directory:
        for service in ("device", "file", "process", "usb", "contracts/driver",
                        "contracts/filesystem", "contracts/usbhost", "contracts/usbhub"):
            subprocess.run([
                "python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                "--service", str(ROOT / "protocols" / (service + ".gr")),
                "--out", directory, "--lang-c", "--client", "--server",
            ], check=True)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-fms-extensions",
                 "-DMOLLENOS", "-DVALI", "-DTESTING", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=", "-DSERVICEABI=", "-D__time64_t=int64_t", "-DNDEBUG",
                 "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
                 "-Wno-sign-compare", "-Wno-missing-field-initializers",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        for include in ("kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
                        "librt/libddk/include", "librt/libds/include", "librt/libgracht/include",
                        "librt/libusb/include"):
            flags += ["-I", str(ROOT / include)]
        flags += ["-I", directory]
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        tests = [("bounce", ["uhci_bounce_test.c"], [])]
        for controller in ("UHCI", "OHCI", "EHCI"):
            tests.append((controller, ["legacy_usb_dma_test.c", "legacy_usb_dma_host.c",
                                       "usb_transfer_bounce_host.c"], ["-DTEST_" + controller, "-Wno-unused-variable",
                                        "-Wno-unused-but-set-variable", "-Wno-unused-but-set-global"]))
        for name, host in (("legacy_usb_manager_test", "usb_dma_manager_host.c"),
                           ("usb_dma_manager_test", "usb_dma_manager_host.c"),
                           ("usb_submission_test", "usb_submission_host.c"),
                           ("usb_shm_release_test", "usb_shm_release_host.c")):
            tests.append((name, [name + ".c", host, "usb_transfer_bounce_host.c"],
                          ["-Wno-unused-but-set-variable"]))
        for name, sources, defines in tests:
            # Existing OHCI/EHCI register macros shift signed 1 into bit 31.
            # Keep this compatibility run focused on bounce; UHCI and the
            # new helper retain the full undefined-behavior sanitizer.
            if name in ("OHCI", "EHCI"):
                defines = defines + ["-fno-sanitize=shift"]
            binary = str(Path(directory) / name)
            subprocess.run(flags + defines + [str(ROOT / "testing" / source) for source in sources]
                           + ["-o", binary], check=True)
            subprocess.run([binary], check=True, env=environment)
        # The fixtures use selected source files. Compile integration points too
        # so header and startup/teardown mistakes cannot hide in discarded code.
        for source in ("common/transfer/bounce.c", "common/transfer/dma.c", "common/transfer/transfer.c",
                       "common/manager.c", "uhci/transfer.c", "uhci/queue.c", "uhci/controller.c"):
            subprocess.run(flags + ["-Wno-unused-command-line-argument", "-fsyntax-only",
                           str(ROOT / "modules/serial/usb" / source)], check=True)


if __name__ == "__main__":
    main()
