"""Exercise filesystem CPU mappings and verify the packed syscall numbering."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    entry = (ROOT / "kernel/api/entry.c").read_text()
    entries = re.findall(r"DefineSyscall\((\d+),\s*(\w+)\)", entry)
    count = int(re.search(r"#define SYSTEM_CALL_COUNT (\d+)", entry)[1])
    assert [int(number) for number, _ in entries] == list(range(count))
    assert dict(entries)["56"] == "ScSHMAttachToken"
    wrappers = (ROOT / "librt/libc/include/internal/_syscalls.h").read_text()
    assert re.search(r"#define Syscall_SHMAttachToken[^\n]+syscall2\(56,", wrappers)
    with tempfile.TemporaryDirectory(prefix="vali-fs-shm-") as directory:
        for service in ("device", "file", "process", "usb", "contracts/driver",
                        "contracts/filesystem", "contracts/storage", "contracts/usbhost", "contracts/usbhub"):
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
                        "librt/libusb/include", "modules/filesystems/common/include",
                        "tools/rd/libvafs/include"):
            flags += ["-I", str(ROOT / include)]
        flags += ["-I", directory]
        binary = str(Path(directory) / "test")
        subprocess.run(flags + [str(ROOT / "testing/fs_shm_test.c"), "-o", binary], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment)
        subprocess.run(flags + [str(ROOT / "testing/fs_directory_shm_test.c"),
                                str(ROOT / "testing/fs_directory_shm_host.c"), "-o", binary], check=True)
        subprocess.run([binary], check=True, env=environment)
        for source in ("modules/filesystems/common/requests.c", "modules/filesystems/common/storage.c",
                       "modules/filesystems/mfs/main.c", "modules/filesystems/valifs/main.c",
                       "modules/filesystems/mfs/directory_operations.c"):
            subprocess.run(flags + ["-Wno-unused-command-line-argument", "-fsyntax-only", str(ROOT / source)], check=True)
    print("Syscalls: contiguous table and SHMAttachToken at 56 passed")


if __name__ == "__main__":
    main()
