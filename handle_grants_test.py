"""Run grant tokens against production handles with deterministic kernel stubs."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def grant_syscall_flags():
    """Keep both grant harnesses aligned with the generated syscall wrappers."""
    calls = {}
    for line in (ROOT / 'syscalls.table').read_text().splitlines():
        fields = line.partition('#')[0].split()
        if fields:
            calls[fields[1]] = int(fields[0])
    return [f"-DTEST_GRANT_CREATE_NUMBER={calls['GrantTokenCreate']}",
            f"-DTEST_GRANT_REVOKE_NUMBER={calls['GrantTokenRevoke']}"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-handle-grants-") as directory:
        output = Path(directory)
        for name, source in SHIMS.items():
            if name == "memoryspace.h":
                continue
            if name == "threading.h":
                source = "#pragma once\n" + source
                source += "\n#include <os/types/time.h>\ntypedef struct Thread Thread_t;\n"
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        (output / "ioset.h").write_text((ROOT / "librt/libc/include/ioset.h").read_text())
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-fms-extensions", "-fasm-blocks", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-g", '-DPRIxIN="zx"', "-I" + directory]
        flags += grant_syscall_flags()
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel/include", "librt/libos/include")]
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        binary = str(output / "handle-grants")
        sources = ("testing/handle_grants_test.c", "librt/libds/hashtable.c",
                   "librt/libds/queue.c")
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment, timeout=10)
        if args.mutation_check:
            source = (ROOT / "kernel/handle_grants.c").read_text()
            mutations = {
                "leave consumed token active": (
                    "record->Status = GrantTokenStatusRevoked;\n            oserr = OS_EOK;",
                    "record->Status = GrantTokenStatusActive;\n            oserr = OS_EOK;"),
                "take a new reference for single-use imports": (
                    "if (record->Options.Flags & GrantTokenFlagsSingleUse) {",
                    "if (false) {"),
                "import revoked tokens": (
                    "if (record->Status == GrantTokenStatusActive) {\n        resourceHandleID",
                    "if (true) {\n        resourceHandleID"),
                "ignore copied lifetime flags": (
                    "token->Options.Flags != record->Options.Flags",
                    "false"),
            }
            for name, (before, after) in mutations.items():
                assert source.count(before) == 1, name
                mutant = output / "grants-mutant.c"
                mutant.write_text(source.replace(before, after))
                command = flags + ['-DGRANTS_SOURCE="' + str(mutant) + '"']
                subprocess.run(command + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
                result = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        env=environment, timeout=10)
                if result.returncode == 0:
                    raise AssertionError("mutation survived: " + name)
                if b"Assertion" not in result.stdout and b"Sanitizer" not in result.stdout:
                    raise AssertionError(result.stdout.decode())
                print("mutation rejected: " + name)


if __name__ == "__main__":
    main()
