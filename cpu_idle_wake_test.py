"""Run production AP startup and verify subsequent same-core idle wake scheduling.

Set CC to the compiler. On Windows the default target is x86_64-pc-windows-msvc.
--mutation-check restores the old ARM-only guard in an isolated source copy.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="x86_64-pc-windows-msvc" if os.name == "nt" else None)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-fms-extensions",
             "-DMOLLENOS", "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=",
             "-DKERNELABI=", "-DSERVICEAPI=", "-DSERVICEABI=", "-D__time64_t=int64_t",
             "-DNDEBUG", "-Wno-macro-redefined", "-fuse-ld=lld"]
    if args.target:
        flags += [f"--target={args.target}"]
    for include in ("kernel/include", "kernel/components", "boot/include",
                    "librt/libc/include", "librt/libos/include", "librt/libddk/include",
                    "librt/libds/include", "librt/libgracht/include", "librt/libacpi/source/include"):
        flags += ["-I", str(ROOT / include)]
    if os.name == "nt":
        flags += ["-llegacy_stdio_definitions"]

    with tempfile.TemporaryDirectory(prefix="vali-cpu-idle-wake-",
                                     ignore_cleanup_errors=os.name == "nt") as directory:
        out = Path(directory)
        suffix = ".exe" if os.name == "nt" else ""
        binary = out / f"test{suffix}"
        source = str(ROOT / "testing/cpu_idle_wake_test.c")
        subprocess.run(flags + [source, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        if args.mutation_check:
            original = (ROOT / "kernel/components/cpu.c").read_text()
            call = "    SchedulerEnable();"
            assert original.count(call) == 1
            changed = original.replace(call, "#if defined(__aarch64__)\n" + call + "\n#endif")
            mutant = out / "cpu-arm-only.c"
            mutant.write_text(changed)
            mutant_binary = out / f"cpu-arm-only{suffix}"
            subprocess.run(flags + [f'-DCPU_SOURCE="{mutant.as_posix()}"', source,
                                    "-o", str(mutant_binary)], check=True)
            result = subprocess.run([str(mutant_binary)], capture_output=True, text=True)
            expected = "g_yields == yieldsBeforeWake + 1"
            if result.returncode == 0 or expected not in result.stdout:
                raise RuntimeError(f"ARM-only startup mutation did not fail the local wake check: {result}")
            print(result.stdout.strip())
            print("ARM-only startup mutation rejected at the local wake scheduling check")


if __name__ == "__main__":
    main()
