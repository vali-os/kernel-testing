"""Run checked user copies and firmware syscall boundaries with host-backed pages."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-user-copy-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-fms-extensions",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-D__time64_t=int64_t", "-DNDEBUG", "-Wno-macro-redefined",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("kernel/include", "kernel/memory/space", "boot/include", "librt/libc/include",
                   "librt/libos/include", "librt/libddk/include", "librt/libds/include",
                   "librt/libgracht/include", "librt/libacpi/source/include")]
        test = str(ROOT / "testing/firmware/user_copy_test.c")
        binary = str(output / "user-copy")
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        for config in ([], ["-D__OSCONFIG_ACPI_SUPPORT"]):
            subprocess.run(flags + config + [test, "-o", binary], check=True)
            subprocess.run([binary], check=True, env=environment, timeout=10)

        if args.mutation_check:
            source = (ROOT / "kernel/memory/space/usercopy.c").read_text()
            mutations = {
                "write permission": ("if (toUser && (attributes & MAPPING_READONLY))",
                                     "if (!toUser && (attributes & MAPPING_READONLY))"),
                "device memory": ("MAPPING_TRAPPAGE | MAPPING_DEVICE", "MAPPING_TRAPPAGE"),
                "address ranges": ("if (!__IsUserCopyRange(address, length))", "if (false)"),
            }
            for name, (before, after) in mutations.items():
                assert source.count(before) == 1, name
                mutant = output / "usercopy-mutant.c"
                mutant.write_text(source.replace(before, after))
                define = '-DUSER_COPY_SOURCE="' + str(mutant) + '"'
                subprocess.run(flags + [define, test, "-o", binary], check=True)
                result = subprocess.run([binary], capture_output=True, env=environment, timeout=10)
                if result.returncode == 0:
                    raise AssertionError("mutation survived: " + name)
                print("mutation rejected:", name)


if __name__ == "__main__":
    main()
