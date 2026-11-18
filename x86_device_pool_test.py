#!/usr/bin/env python3
"""Check the x86 pool selection and saved failure using production memory code."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(mutation_check):
    with tempfile.TemporaryDirectory(prefix="vali-x86-pool-") as temporary:
        out = Path(temporary)
        source = ROOT / "kernel/arch/x86/components/memory.c"
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-D__amd64__",
                 '-DPRIxIN="lx"', "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-fms-extensions",
                 "-fasm-blocks", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-no-pie"]
        for path in ("testing/include", "kernel/include", "librt/libos/include",
                     "librt/libddk/include", "boot/include", "librt/libds/include"):
            flags += ["-I", str(ROOT / path)]
        environment = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}

        def build(memory, name):
            binary = out / name
            subprocess.run(flags + [str(ROOT / "testing/x86_device_pool_test.c"), str(memory),
                           str(ROOT / "kernel/memory/memory_region.c"), "-o", str(binary)], check=True)
            return binary

        binary = build(source, "pool")
        for mode in ("success", "failure"):
            subprocess.run([str(binary), mode], env=environment, check=True, timeout=10)
        if mutation_check:
            content = source.read_text()
            anchor = "if (!g_devicePoolSelected)"
            assert content.count(anchor) == 1
            mutant = out / "recompute.c"
            mutant.write_text(content.replace(anchor, "if (true)"))
            binary = build(mutant, "recompute")
            for mode in ("success", "failure"):
                result = subprocess.run([str(binary), mode], env=environment, capture_output=True, timeout=10)
                assert result.returncode == -signal.SIGABRT and b"Assertion" in result.stderr, result.stderr
            print("Rejected mutation: recompute x86 pool selection and failure", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    run(parser.parse_args().mutation_check)
