"""Test UserEventSignal with the real handle table, I/O sets and a sleeping reader."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]
SOURCE = "kernel/sync/userevent.c"

# Each mutation brings back one of the behaviours this change removed, or
# breaks the rule the tests check.
MUTATIONS = {
    "wake no reader after a raise": (
        "    status = FutexWake(event->KernelMapping, 1, 0);\n",
        "    status = OS_ENOENT;\n"),
    "mark the I/O set for a merged signal": (
        "            return OS_EOK;\n        }\n        if (atomic_compare_exchange_weak",
        "            (void)MarkHandle(handle, IOSETSYN);\n"
        "            return OS_EOK;\n        }\n        if (atomic_compare_exchange_weak"),
    "mark only when the counter rises from 0": (
        "    status = MarkHandle(handle,",
        "    status = currentValue != 0 ? OS_ENOENT : MarkHandle(handle,"),
    "report a merged signal as a raise": (
        "            return OS_EOK;\n        }\n        if (atomic_compare_exchange_weak",
        "            return OS_EINCOMPLETE;\n        }\n        if (atomic_compare_exchange_weak"),
    "raise the counter above its maximum": (
        "(unsigned int)currentValue >= event->InitialValue",
        "(unsigned int)currentValue > event->InitialValue"),
    "log a wake that found nobody": (
        "status != OS_EOK && status != OS_ENOENT) {\n        WARNING(\"UserEventSignal: waking",
        "status != OS_EOK) {\n        WARNING(\"UserEventSignal: waking"),
}


def write_shims(output):
    shims = dict(SHIMS)
    shims["memoryspace.h"] += """
#define MEMORY_SPACE_APPLICATION 2
#define MEMORY_SPACE_INHERIT 1
typedef struct MemorySpace {
    uuid_t GroupGlobalHandleID;
    unsigned int Flags;
} MemorySpace_t;
oserr_t MSContextAcquireSecurity(uint64_t, MSContextSecurity_t**);
"""
    # Warnings are counted, so the tests can check that normal results are
    # not logged.
    shims["debug.h"] = """#pragma once
void TestLogWarning(const char*, ...);
#define WARNING(...) TestLogWarning(__VA_ARGS__)
#define ERROR(...) TestLogWarning(__VA_ARGS__)
#define DEBUG(...) ((void)0)
#define TRACE(...) ((void)0)
"""
    shims["ioset.h"] = (ROOT / "librt/libc/include/ioset.h").read_text()
    shims["event.h"] = (ROOT / "librt/libc/include/event.h").read_text()
    shims["mutex.h"] = """#pragma once
#include <assert.h>
typedef struct { int held; } Mutex_t;
#define MUTEX_FLAG_PLAIN 0
#define OS_MUTEX_INIT(flags) {0}
static inline void MutexLock(Mutex_t* m) { assert(!m->held); m->held=1; }
static inline void MutexUnlock(Mutex_t* m) { assert(m->held); m->held=0; }
"""
    for name, source in shims.items():
        path = output / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-userevent-test-") as directory:
        output = Path(directory)
        write_shims(output)
        # The shared handle fixtures carry helpers this suite does not use.
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-unused-function", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel", "kernel/include", "librt/libos/include", "librt/libds/include",
                   "librt/libddk/include")]
        flags += ["-iquote", str(ROOT / "kernel/handles")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        binary = str(output / "userevent-test")
        sources = ("testing/userevent_test.c", "librt/libds/hashtable.c", "librt/libds/queue.c",
                   "librt/libds/list.c", "librt/libds/rbtree.c")
        command = flags + [str(ROOT / path) for path in sources] + ["-o", binary]
        subprocess.run(command, check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment, timeout=30)
        if not args.mutation_check:
            return

        source = (ROOT / SOURCE).read_text()
        mutant = output / "userevent-mutant.c"
        for name, (before, after) in MUTATIONS.items():
            assert source.count(before) == 1, name
            mutant.write_text(source.replace(before, after))
            subprocess.run(command[:-2] + ['-DUSEREVENT_SOURCE="' + str(mutant) + '"'] + command[-2:],
                           check=True)
            result = subprocess.run([binary], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    env=environment, timeout=30)
            if result.returncode == 0:
                raise AssertionError("mutation survived: " + name)
            if b"Assertion" not in result.stdout:
                raise AssertionError("mutation failed without an assertion: " + name + "\n" +
                                     result.stdout.decode())
            print("mutation rejected: " + name)


if __name__ == "__main__":
    main()
