"""Exercise authenticated driver assignment against production deviced code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-driver-identity-") as directory:
        output = Path(directory)
        for service in ("device", "file", "process", "contracts/filesystem",
                        "contracts/usbhost", "contracts/driver"):
            subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                            "--service", str(ROOT / "protocols" / (service + ".gr")),
                            "--out", directory, "--lang-c", "--client", "--server"], check=True)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-D__time64_t=int64_t", "-DNDEBUG", "-DVALI", "-DMOLLENOS",
                 "-Wno-macro-redefined", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-g", "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in (
            "kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
            "librt/libfdt/include", "librt/libddk/include", "librt/libds/include",
            "librt/libgracht/include", "librt/libacpi/source/include", "librt/libyaml/include",
            "librt/libusb/include", "services/deviced", "services/deviced/include")]
        source = str(ROOT / "testing/firmware/driver_identity_test.c")
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        binary = output / "test"
        subprocess.run(flags + [source, "-lyaml", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=10)

        if args.mutation_check:
            for name, path, macro, before, after in (
                ("truncate principal", "services/deviced/core/discover.c", "DISCOVER_SOURCE",
                 "principal != driver->Principal", "(uint32_t)principal != (uint32_t)driver->Principal"),
                ("invert credential source", "services/deviced/core/requests.c", "REQUESTS_SOURCE",
                 "message->security.source != GRACHT_SECURITY_KERNEL",
                 "message->security.source == GRACHT_SECURITY_KERNEL"),
                ("drop input request body", "services/deviced/core/requests.c", "REQUESTS_SOURCE",
                 "memcpy(out, data, data_count);", "memset(out, 0, data_count);"),
                ("retry uncertain spawn", "services/deviced/core/discover.c", "DISCOVER_SOURCE",
                 "    driver->State = DmDriverState_LOADING;",
                 "    driver->State = DmDriverState_NOTLOADED;"),
            ):
                original = (ROOT / path).read_text()
                assert original.count(before) >= 1
                mutant = output / "mutant.c"
                mutant.write_text(original.replace(before, after, 1))
                subprocess.run(flags + [f'-D{macro}="{mutant}"', source, "-lyaml",
                                        "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=10)
                if result.returncode != 1 or b"line " not in result.stdout:
                    raise AssertionError(f"Mutation was not rejected by a check: {name}: {result}")
                print("mutation rejected:", name)


if __name__ == "__main__":
    main()
