"""Check the real DMA wrappers and syscall gate with deterministic transport failures."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    mutation_check = parser.parse_args().mutation_check
    # Use the real syscall macros and runtime forwarding. Only the final machine
    # trap is replaced; the test checks each syscall number and argument layout.
    with tempfile.TemporaryDirectory(prefix="vali-ddk-dma-") as directory:
        output = Path(directory)
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/device.gr"), "--out", str(output),
                        "--lang-c", "--client"], check=True)
        (output / "ddk").mkdir()
        (output / "ddk/convert.h").write_text(
            "#pragma once\n#include <ddk/devices/device.h>\nstruct sys_device;\n"
            "oserr_t to_sys_device(Device_t*, struct sys_device*);\n")
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DVALI", "-DTESTING", "-DSERVICEAPI=extern", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-Wno-unused-function",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread", "-ffunction-sections", "-fdata-sections",
                 "-I" + str(output)]
        flags += ["-I" + str(ROOT / p) for p in
                  ("testing/include", "librt/libos/include", "librt/libddk/include", "librt/libgracht/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        for header in ("os/fence.h", "ddk/dma.h", "os/types/dma.h", "os/types/syscall.h"):
            source = output / "header.c"
            source.write_text('#include <' + header + '>\n')
            subprocess.run(flags + ["-fsyntax-only", str(source)], check=True)
        subprocess.run(flags + ["-fsyntax-only", str(ROOT / "testing/dma_api_compile.c")], check=True)
        # Buffers are continuously shared; the compiler must reject accidental
        # use of the streaming handoff API before it reaches the kernel.
        source.write_text('#include <ddk/dma.h>\n'
                          'oserr_t misuse(struct DMABuffer* buffer) {\n'
                          '    return DMABeginTransfer(buffer, 0, 1, SHMWaitNonBlocking, NULL);\n}\n')
        misuse = subprocess.run(flags + ["-fsyntax-only", str(source)], capture_output=True)
        if misuse.returncode == 0:
            raise AssertionError("DMA buffer unexpectedly accepted as a streaming mapping")
        for parameter, expression in (
                ("struct DMAFencedUse* use", "DMAEndUse(use)"),
                ("struct DMAUse* use", "DMAEndUseFenced(use, OS_EOK)")):
            source.write_text('#include <ddk/dma.h>\n'
                              'oserr_t misuse(' + parameter + ') {\n'
                              '    return ' + expression + ';\n}\n')
            misuse = subprocess.run(flags + ["-fsyntax-only", str(source)], capture_output=True)
            if misuse.returncode == 0:
                raise AssertionError("Fenced/unfenced use types unexpectedly interchangeable")
        binary = output / "ddk-dma-test"
        subprocess.run(flags + [str(ROOT / "testing/ddk_dma_test.c"),
                                str(ROOT / "librt/libos/syscalls.c"), "-Wl,--gc-sections", "-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment)
        if not mutation_check:
            return

        # A refused direct-only map must not be logged as a warning. The
        # mutant logs every failed map syscall as a warning again.
        mutations = (
            ("warn on a refused direct-only map",
             "if (flags == DMA_MAP_REQUIRE_DIRECT && (status == OS_ENOENT || status == OS_ENOTSUPPORTED)) {",
             "if (false) {"),
        )
        original = (ROOT / "librt/libddk/dma/dma.c").read_text()
        test_source = (ROOT / "testing/ddk_dma_test.c").read_text()
        include = '#include "../librt/libddk/dma/dma.c"'
        assert test_source.count(include) == 1
        for name, old, new in mutations:
            assert original.count(old) == 1, name
            mutant = output / "dma-mutant.c"
            mutant.write_text(original.replace(old, new).replace(
                '#include "private.h"',
                '#include "' + str(ROOT / "librt/libddk/dma/private.h") + '"'))
            mutant_test = output / "ddk_dma_test_mutant.c"
            mutant_test.write_text(test_source.replace(include, '#include "' + str(mutant) + '"'))
            mutant_binary = output / "ddk-dma-mutant"
            # The copied test still includes its neighbours relative to testing/.
            subprocess.run(flags + ["-iquote", str(ROOT / "testing"),
                                    str(mutant_test), str(ROOT / "librt/libos/syscalls.c"),
                                    "-Wl,--gc-sections", "-o", str(mutant_binary)], check=True)
            result = subprocess.run([str(mutant_binary)], env=environment, capture_output=True, timeout=60)
            if result.returncode == 0:
                raise AssertionError("Undetected mutation: " + name)
            print("Rejected mutation:", name, flush=True)


if __name__ == "__main__":
    main()
