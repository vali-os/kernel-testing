"""Exercise production APIC initialization with checked hardware-register mocks."""
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
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-fms-extensions",
             "-DMOLLENOS", "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=",
             "-DKERNELABI=", "-DSERVICEAPI=", "-DSERVICEABI=", "-D__time64_t=int64_t",
             "-DNDEBUG", "-Wno-macro-redefined", "-ffunction-sections", "-fdata-sections",
             "-Wl,--gc-sections", "-fsanitize=undefined", "-fno-sanitize-recover=all"]
    for include in ("kernel/include", "boot/include", "librt/libc/include",
                    "librt/libos/include", "librt/libddk/include", "librt/libds/include",
                    "librt/libgracht/include", "librt/libacpi/source/include"):
        flags += ["-I", str(ROOT / include)]
    with tempfile.TemporaryDirectory(prefix="vali-apic-init-") as directory:
        binary = Path(directory) / "apic-init"
        subprocess.run(flags + [str(ROOT / "testing/apic_init_test.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=10)
        if args.mutation_check:
            original = (ROOT / "kernel/arch/x86/interrupts/apic/apicinit.c").read_text()
            writes = "    if (!ApicIsX2Apic()) {"
            enable = "        (void)ApicEnableX2Apic();"
            assert original.count(writes) == 1
            assert original.count(enable) == 1
            mutations = {
                "x2apic-logical-destination": original.replace(writes, "    if (1) {"),
                "secondary-mode-enable": original.replace(enable, "        // Missing per-CPU enable."),
            }
            for name, source in mutations.items():
                mutant = Path(directory) / (name + ".c")
                mutant.write_text(source)
                subprocess.run(flags + [f'-DAPIC_INIT_SOURCE="{mutant.as_posix()}"',
                                        str(ROOT / "testing/apic_init_test.c"), "-o", str(binary)],
                               check=True)
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                if result.returncode == 0 or "APIC initialization check failed" not in result.stdout:
                    raise RuntimeError(f"Mutation was not rejected by a register check: {name}")
                print(f"{name}: mutation rejected")


if __name__ == "__main__":
    main()
