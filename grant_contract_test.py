"""Exercise grant conversions and real generated Gracht request/dispatch code."""
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
    with tempfile.TemporaryDirectory(prefix="vali-grant-contract-") as directory:
        output = Path(directory)
        fixture = output / "grant_test.gr"
        fixture.write_text(f'''import "{ROOT / 'protocols/types'}"
namespace sys
service grant_test : message {{
    func transfer(shm_transfer description) : () = 1;
    func token(grant_token description) : () = 2;
}}
''')
        for service in (ROOT / "protocols/process.gr", fixture):
            subprocess.run([
                "python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                "--service", str(service), "--out", directory,
                "--lang-c", "--client", "--server",
            ], check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11",
                 "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include",
                   "librt/libgracht/include")]
        sources = [str(ROOT / "testing/grant_contract_test.c"),
                   str(output / "sys_grant_test_service_client.c"),
                   str(output / "sys_grant_test_service_server.c")]
        binary = str(output / "grant-contract")
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run(flags + sources + ["-o", binary], check=True)
        subprocess.run([binary], check=True, timeout=10, env=environment)
        if args.mutation_check:
            header = ROOT / "librt/libddk/include/ddk/convert/grants.h"
            source = header.read_text()
            mutations = {
                "ignore retry token flags": (
                    "left->token.flags == right->token.flags", "true"),
                "reject valid boundary": (
                    "transfer->RequestedLength <=", "transfer->RequestedLength <"),
                "allow wrapping subrange": (
                    "transfer->RequestedLength <=\n           transfer->Token.Options.Data.SHM.Length - transfer->RelativeOffset",
                    "transfer->RelativeOffset + transfer->RequestedLength <= transfer->Token.Options.Data.SHM.Length"),
                "reverse secret word bytes": (
                    "(uint32_t)bytes[0] |", "(uint32_t)bytes[3] |"),
            }
            local = output / "ddk/convert/grants.h"
            local.parent.mkdir(parents=True)
            for name, (before, after) in mutations.items():
                assert source.count(before) == 1, name
                local.write_text(source.replace(before, after))
                subprocess.run(flags + sources + ["-o", binary], check=True)
                result = subprocess.run([binary], stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=10, env=environment)
                if result.returncode == 0:
                    raise AssertionError(f"mutation survived: {name}")
                if b"Assertion" not in result.stdout:
                    raise AssertionError(result.stdout.decode())
                print(f"mutation rejected: {name}")


if __name__ == "__main__":
    main()
