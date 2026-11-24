"""Run the userspace asynchronous queue and handle set retry under ASan/UBSan.

--mutation-check also builds copies of librt/libos/async_queue.c and
librt/libos/notification_queue.c with one known mistake each, and requires
every copy to fail at an assertion.
"""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
QUEUE = "librt/libos/async_queue.c"
LISTEN = "librt/libos/notification_queue.c"
SOURCES = ("testing/async_completion_ring_test.c", "librt/libos/syscalls.c", QUEUE, LISTEN)

MUTATIONS = (
    ("leave ReadIndex where it was", QUEUE,
     "    atomic_store_explicit(&ring->ReadIndex, readIndex, memory_order_release);\n",
     ""),
    ("read past the end of the ring", QUEUE,
     "&ring->Entries[readIndex % ring->Capacity];",
     "&ring->Entries[readIndex % (ring->Capacity + 1)];"),
    ("return a record result without events", LISTEN,
     "if (oserr != OS_EOK || *numEventsOut != 0) {",
     "if (oserr != OS_EOK || *numEventsOut >= 0) {"),
    ("drop the deadline before the scheduler", LISTEN,
     "oserr = __SyscallAwaitForkUntil(oserr, asyncContext, deadline);",
     "oserr = __SyscallAwaitForkUntil(oserr, asyncContext, NULL);"),
    ("cancel through the buffer handle", QUEUE,
     "return Syscall_AsyncQueueCancel(queue->Handle.ID, context);",
     "return Syscall_AsyncQueueCancel(queue->Buffer.ID, context);"),
    ("call again with the finished context", LISTEN,
     "        OSAsyncContextInitialize(asyncContext);\n    }",
     "    }"),
)


def build_and_run(sources, mutation=None):
    with tempfile.TemporaryDirectory(prefix="vali-async-ring-") as directory:
        binary = Path(directory) / "ring"
        (Path(directory) / "ioset.h").write_text((ROOT / "librt/libc/include/ioset.h").read_text())
        flags = [os.environ.get("CC", "clang"), "-std=c11", "-DVALI", "-DTESTING",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=extern", "-DSERVICEABI=",
                 "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
                 "-Wl,--gc-sections", "-g", "-pthread", "-I" + directory, "-fms-extensions",
                 "-fasm-blocks"]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "librt/libos/include", "librt/libddk/include", "kernel/include")]
        flags += ["-isystem" + str(ROOT / "librt/libds/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        result = subprocess.run([str(binary)], env=environment, timeout=30,
                                capture_output=mutation is not None)
        if mutation is None:
            result.check_returncode()
        elif result.returncode != -signal.SIGABRT or b"Assertion" not in result.stderr:
            raise AssertionError(f"Mutation did not fail at an assertion: {mutation}\n"
                                 f"Exit status: {result.returncode}\n"
                                 + (result.stdout + result.stderr).decode(errors="replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    build_and_run(SOURCES)
    if not args.mutation_check:
        return

    for name, path, old, new in MUTATIONS:
        original = (ROOT / path).read_text()
        assert original.count(old) == 1, name
        with tempfile.TemporaryDirectory(prefix="vali-async-ring-mutation-") as directory:
            mutant = Path(directory) / Path(path).name
            mutant.write_text(original.replace(old, new))
            sources = tuple(str(mutant) if source == path else source for source in SOURCES)
            build_and_run(sources, mutation=name)
            print("Rejected mutation:", name, flush=True)


if __name__ == "__main__":
    main()
