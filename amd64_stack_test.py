"""Check production amd64 stack creation and fork backing under ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_GNU_SOURCE",
             "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
             "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
             "-fms-extensions", "-fasm-blocks", "-ffunction-sections", "-fdata-sections",
             "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
             "-Wno-address-of-packed-member", "-g"]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "kernel/include", "librt/libos/include")]
    flags += ["-isystem" + str(ROOT / path) for path in
              ("librt/libds/include", "librt/libddk/include", "boot/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    source = args.source or ROOT / "kernel/arch/x86/x64/context.c"
    with tempfile.TemporaryDirectory(prefix="vali-amd64-stacks-") as directory:
        output = Path(directory)
        binary = output / "amd64-stack"

        def build(path):
            subprocess.run(flags + [f'-DAMD64_CONTEXT_SOURCE="{path.resolve().as_posix()}"',
                                    str(ROOT / "testing/amd64_stack_test.c"),
                                    "-o", str(binary)], check=True)

        build(source)
        subprocess.run([str(binary)], check=True, env=environment, timeout=10)
        if args.mutation_check:
            original = source.read_text()
            create = "committedSize = contextType == THREADING_CONTEXT_LEVEL0 ? contextSize : PAGE_SIZE;"
            fork = "committedSize = contextType == THREADING_CONTEXT_LEVEL0\n        ? contextSize : stackUsage + sizeof(Context_t);"
            array = "kmalloc((contextComittedSize / PAGE_SIZE) * sizeof(paddr_t))"
            assert all(original.count(text) == 1 for text in (create, fork, array))
            mutations = {
                "one-page-kernel-stack": original.replace(create, "committedSize = PAGE_SIZE;"),
                "copied-frames-only": original.replace(fork, "committedSize = stackUsage + sizeof(Context_t);"),
                "one-address-output": original.replace(array, "kmalloc(sizeof(paddr_t))"),
                "kernel-selection-inverted": original.replace(create, create.replace("==", "!=")),
            }
            for name, text in mutations.items():
                mutant = output / (name + ".c")
                mutant.write_text(text)
                build(mutant)
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=environment, timeout=10)
                expected = "heap-buffer-overflow" if name == "one-address-output" else "length == g_expectedCommit"
                if result.returncode == 0 or expected not in result.stderr:
                    raise RuntimeError("Mutation was not detected: " + name + "\n" + result.stderr)
                print("AMD64 stacks: " + name + " mutation rejected")


if __name__ == "__main__":
    main()
