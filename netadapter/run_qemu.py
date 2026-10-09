"""Run the guest acceptance test headlessly on a disposable disk overlay.

The serial completion marker is the oracle: reaching a boot prompt or a timeout
is never a pass. A private firmware-vars copy and qcow2 overlay preserve both the
input image and the host's OVMF variables. No host network interface is attached.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--log", type=Path, default=Path("netadapter-serial.log"))
    parser.add_argument("--virtio", action="store_true", help="Run real Virtio NIC tests with an isolated QEMU gateway")
    parser.add_argument("--storage", action="store_true",
                        help="Check full-image Virtio block mount/read/write progress instead of networking")
    parser.add_argument("--settle", type=float, default=0,
                        help="Observe the guest for this many seconds after success markers")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--ovmf-code", type=Path, default=Path("/usr/share/OVMF/x64/OVMF_CODE.4m.fd"))
    parser.add_argument("--ovmf-vars", type=Path, default=Path("/usr/share/OVMF/x64/OVMF_VARS.4m.fd"))
    args = parser.parse_args()
    if args.timeout <= 0 or args.settle < 0 or args.settle >= args.timeout:
        parser.error("require 0 <= --settle < --timeout")
    for path in (args.image, args.ovmf_code, args.ovmf_vars):
        if not path.is_file():
            parser.error(f"file not found: {path}")
    log = args.log.resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text("")
    errors = log.with_suffix(log.suffix + ".qemu.log")
    result = 1
    reason = "timeout waiting for acceptance markers and observation period"
    failures = ["FATAL EXCEPTION OCCURRED", "DebugPanic("]
    if args.storage:
        # These ordered boot operations require mounted MFS, completed block
        # reads and writes, and successful creation of the service directories.
        # A network-workload result is deliberately not a storage-test oracle.
        markers = ["__BuildIteration type=0,", "__BuildIteration type=1,",
                   "FsCreate returned 0", "InstallBundledApplications()"]
        failures += ["virtio-blk.dll: Crashed", "mfs.dll: Crashed", "filed.dll: Crashed",
                     "FsInitialize failed", "Failed to read sector", "Failed to allocate Virtio block"]
    else:
        markers = ["VIRTIO NET PASS all:" if args.virtio else "NETADAPTER IPC PASS all:"]
        failures += ["VIRTIO NET FAIL", "virtio-net.dll: Crashed", "NETADAPTER IPC FAIL",
                     "NETADAPTER FAKE startup/executor failure", "NETADAPTER FAKE thread failure",
                     "netd.dll: Crashed", "netadapter_fake.dll: Crashed"]
    with tempfile.TemporaryDirectory(prefix="vali-netadapter-qemu-") as directory:
        root = Path(directory)
        variables = root / "OVMF_VARS.fd"
        disk = root / "disk.qcow2"
        shutil.copyfile(args.ovmf_vars, variables)
        subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw",
                        "-b", str(args.image.resolve()), str(disk)], check=True)
        command = ["qemu-system-x86_64", "-accel", "tcg", "-display", "none", "-monitor", "none",
                   "-smp", "1", "-m", "4096", "-net", "none", "-no-reboot",
                   "-drive", f"if=pflash,format=raw,readonly=on,file={args.ovmf_code.resolve()}",
                   "-drive", f"if=pflash,format=raw,file={variables}",
                   "-drive", f"if=virtio,format=qcow2,file={disk}", "-serial", f"file:{log}"]
        if args.virtio:
            index = command.index("-net")
            del command[index:index + 2]
            command.extend(["-netdev", "user,id=net0,restrict=on",
                            "-device", "virtio-net-pci,netdev=net0,disable-legacy=on,mac=52:54:00:12:34:56"])
        with errors.open("w") as output:
            process = subprocess.Popen(command, stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + args.timeout
                completed_at = None
                while time.monotonic() < deadline:
                    data = log.read_text(errors="replace")
                    if any(marker in data for marker in failures):
                        reason = "guest reported a failure"
                        break
                    if all(marker in data for marker in markers):
                        if completed_at is None:
                            completed_at = time.monotonic()
                        if time.monotonic() - completed_at >= args.settle:
                            result = 0
                            break
                    if process.poll() is not None:
                        reason = f"QEMU exited with status {process.returncode} before completion"
                        break
                    time.sleep(0.2)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    data = log.read_text(errors="replace")
    if args.storage:
        print("\n".join(line for line in data.splitlines()
                        if "FsCreate returned" in line or "InstallBundledApplications" in line
                        or "DebugPanic" in line))
    else:
        print("\n".join(line for line in data.splitlines()
                        if "NETADAPTER" in line or "VIRTIO NET" in line or "virtio-net" in line))
    if result:
        print(f"FAIL: {reason}\nSerial: {log}\nQEMU diagnostics: {errors}")
    else:
        print(f"PASS: {log}")
    return result


if __name__ == "__main__":
    raise SystemExit(main())
