"""Exercise file inheritance with production codecs, local tables and processd preparation."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="vali-inheritance-") as directory:
    output = Path(directory)
    subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                    "--service", str(ROOT / "protocols/process.gr"), "--out", directory,
                    "--lang-c", "--client", "--server"], check=True)
    flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-DVALI", "-DTESTING", "-DMOLLENOS",
             "-D__time64_t=int64_t", "-DNDEBUG", "-fms-extensions", "-Wno-macro-redefined",
             "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-mllvm", "-asan-globals=0",
             "-g", "-I" + directory]
    flags += ["-I" + str(ROOT / path) for path in
              ("librt/libc/include", "librt/libos/include", "librt/libds/include", "librt/libddk/include",
               "librt/libgracht/include", "services/processd/include")]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    command = flags + [str(ROOT / "testing/inheritance_test.c"), "-o", str(output / "test")]
    subprocess.run(command, check=True)
    subprocess.run([str(output / "test")], check=True, env=environment)
    tls_binary = output / "bootstrap-tls"
    subprocess.run(flags + [str(ROOT / "testing/bootstrap_tls_test.c"), "-o", str(tls_binary)], check=True)
    subprocess.run([str(tls_binary)], check=True, env=environment)
    source = (ROOT / "librt/libc/stdio/core/import.c").read_text()
    for name, before, after in (
        ("ignore occupied descriptor", "stdio_handle_get((int)output->descriptors[i].descriptor) != NULL", "false"),
        ("omit alias reference", "status = OSHandlesAcquire(&resources[resource].Handle);", "status = OS_EOK;"),
        ("accept trailing payload", "wire.error || wire.index != length", "wire.error"),
    ):
        mutant = output / "import-mutant.c"
        mutant.write_text(source.replace(before, after).replace('#include "private.h"',
                          '#include "' + str(ROOT / "librt/libc/stdio/core/private.h") + '"'))
        subprocess.run(command + [f'-DINHERITANCE_IMPORT_SOURCE="{mutant}"'], check=True)
        result = subprocess.run([str(output / "test")], capture_output=True, env=environment)
        if result.returncode == 0:
            raise AssertionError("Mutation escaped: " + name)
        print("mutation rejected:", name)
