"""Exercise real libos pools/tracking and audit fast-path object dependencies."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="vali-buffer-access-") as directory:
        output = Path(directory)
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
                 "-Derrno_t=int", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I" + str(ROOT / "testing/include"),
                 "-I" + str(ROOT / "librt/libddk/include"),
                 "-I" + str(ROOT / "librt/libos/include"),
                 "-idirafter", str(ROOT / "librt/libc/include")]
        for header in ("os/fence_pool.h", "os/buffer_access.h"):
            source = output / "header.c"
            source.write_text('#include <' + header + '>\n')
            subprocess.run(flags + ["-fsyntax-only", str(source)], check=True)
        # No allocator, IPC, mapping or libc sorting may enter these objects.
        # Error diagnostics may call SystemDebug. Audit all imports rather than
        # relying on one exercised path; normal operations must remain quiet.
        allowed = {"memset", "memcmp", "memcpy", "__assert_fail", "assert", "_assert", "__assert",
                   "SystemDebug",
                   "OSFencePoolAcquire", "OSFencePoolRelease", "OSFencePoolPoll",
                   "OSFencePoolSubmit", "OSFencePoolSignal",
                   "__OSFenceStorageInitialize", "__OSFenceStoragePoll",
                   "__OSFenceStorageWait", "__OSFenceStorageSignal"}
        for name in ("fence_pool", "buffer_access"):
            obj = output / (name + ".o")
            subprocess.run(flags + ["-fno-stack-protector", "-c", str(ROOT / "librt/libos" / (name + ".c")),
                                   "-o", str(obj)], check=True)
            symbols = subprocess.check_output(["nm", "-u", str(obj)], text=True)
            unexpected = {line.split()[-1] for line in symbols.splitlines()} - allowed
            assert not unexpected, (name, unexpected)
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g"]
        binary = output / "buffer-access"
        subprocess.run(flags + [str(ROOT / "testing/os_buffer_access_test.c"),
                                str(ROOT / "testing/fence_test_logging.c"),
                                str(ROOT / "librt/libos/fence_storage.c"), "-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)


if __name__ == "__main__":
    main()
