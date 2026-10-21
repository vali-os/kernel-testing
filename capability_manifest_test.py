"""Exercise the production YAML parsers and audited driver capability masks.

Set CC to a host Clang compiler. On Windows, --target selects the host ABI.
--mutation-check proves the service transition and missing driver grant regressions
are detected, using temporary copies without changing repository sources.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# I/O plus interrupts for controllers; the polled IDE driver only needs I/O.
# USB class drivers and filesystems delegate hardware access over RPC.
MASKS = {
    "edu/edu-msi.yaml": 0x28,
    "rocker/rocker-msix.yaml": 0x28,
    "input/ps2/ps2.yaml": 0x28,
    "virtio/blk/virtio-blk.yaml": 0x28,
    "virtio/net/virtio-net.yaml": 0x28,
    "storage/ahci/ahci.yaml": 0x28,
    "storage/ide/ide.yaml": 0x08,
    "serial/usb/ehci/ehci.yaml": 0x28,
    "serial/usb/ohci/ohci.yaml": 0x28,
    "serial/usb/uhci/uhci.yaml": 0x28,
    "serial/usb/xhci/xhci.yaml": 0x28,
    "serial/usb/hub/usbhub.yaml": 0,
    "input/hid/hid.yaml": 0,
    "storage/msd/msd.yaml": 0,
    "filesystems/mfs/mfs.yaml": 0,
    "filesystems/valifs/valifs.yaml": 0,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target")
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    actual = {p.relative_to(ROOT / "modules").as_posix()
              for p in (ROOT / "modules").rglob("*.yaml")}
    if actual != set(MASKS):
        raise RuntimeError(f"Driver audit inventory changed: {actual ^ set(MASKS)}")

    # Windows may keep a just-executed image locked briefly after process exit.
    with tempfile.TemporaryDirectory(prefix="vali-capability-manifests-",
                                     ignore_cleanup_errors=os.name == "nt") as directory:
        out = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions",
                 "-DMOLLENOS", "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-DYAML_DECLARE_STATIC",
                 "-D__time64_t=int64_t", "-DNDEBUG", "-O2", "-Wno-macro-redefined",
                 "-ffunction-sections", "-fdata-sections"]
        if args.target:
            flags += [f"--target={args.target}"]
        for include in ("librt/libc/include", "librt/libos/include", "librt/libddk/include",
                        "librt/libds/include", "librt/libgracht/include", "librt/libyaml/include"):
            flags += ["-I", str(ROOT / include)]
        sources = [
            "testing/capability_manifest_test.c",
            "services/processd/services/parser.c",
            "services/deviced/core/configparser.c",
            "librt/libddk/utils.c",
            "librt/libds/list.c",
            "librt/libds/mstring/core/mstr_new.c",
            "librt/libds/mstring/core/mstr_delete.c",
            "librt/libds/mstring/unicode/mstr_unicode.c",
            "librt/libds/mstring/unicode/mstr_conv.c",
            "librt/libds/mstring/unicode/mstr_digits.c",
            "librt/libds/mstring/unicode/mstr_props.c",
        ] + [f"librt/libyaml/{name}.c" for name in ("api", "parser", "reader", "scanner")]
        objects = []
        service_command = None
        for index, source in enumerate(sources):
            obj = out / f"{index}.obj"
            command = flags.copy()
            if source.startswith("services/processd/"):
                command += ["-I", str(ROOT / "services/processd/include")]
            if source.startswith("services/deviced/"):
                command += ["-I", str(ROOT / "services/deviced/include")]
            if source == "librt/libddk/utils.c":
                # Keep the real CapabilityParse and replace logging with the host stub.
                command += ["-DSystemDebug=UnusedSystemDebug"]
            command += ["-c", str(ROOT / source), "-o", str(obj)]
            subprocess.run(command, check=True)
            if source == "services/processd/services/parser.c":
                service_command = command
            objects.append(str(obj))
        binary = out / ("test.exe" if os.name == "nt" else "test")
        link = flags + ["-fuse-ld=lld"] + objects + ["-o", str(binary)]
        link += (["-Wl,/opt:ref", "-llegacy_stdio_definitions"] if os.name == "nt"
                 else ["-Wl,--gc-sections"])
        subprocess.run(link, check=True)
        subprocess.run([str(binary)], check=True)
        for name, mask in MASKS.items():
            subprocess.run([str(binary), str(ROOT / "modules" / name), hex(mask)], check=True)

        if args.mutation_check:
            source = (ROOT / "services/processd/services/parser.c").read_text()
            start = source.index("        case STATE_CAPS_LIST:")
            end = source.index("        case STATE_SERVICEDEPS:", start)
            changed = source[start:end].replace("STATE_SERVICEKEY", "STATE_SERVICEDEPS")
            assert changed != source[start:end]
            mutant = out / "service-mutant.c"
            mutant.write_text(source[:start] + changed + source[end:])
            command = service_command.copy()
            command[command.index("-c") + 1] = str(mutant)
            subprocess.run(command, check=True)
            subprocess.run(link, check=True)
            result = subprocess.run([str(binary)])
            if result.returncode == 0:
                raise RuntimeError("Service parser regression escaped detection")
            print("Service transition mutation rejected")

            manifest = (ROOT / "modules/virtio/blk/virtio-blk.yaml").read_text()
            mutant_manifest = out / "virtio-blk.yaml"
            mutant_manifest.write_text(manifest.replace("    - deviceio-access\n", ""))
            result = subprocess.run([str(binary), str(mutant_manifest), "0x28"])
            if result.returncode == 0:
                raise RuntimeError("Missing driver capability escaped detection")
            print("Missing driver capability mutation rejected")


if __name__ == "__main__":
    main()
