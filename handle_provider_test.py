"""Exercise provider references with production handles, grants and syscalls."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-provider-test-") as directory:
        output = Path(directory)
        shims = dict(SHIMS)
        shims["spinlock.h"] = shims["spinlock.h"].replace(
            "#define OS_SPINLOCK_INIT {0}",
            "#define OS_SPINLOCK_INIT {0}\n"
            "void TestBeforeNotificationLock(Spinlock_t*);\n"
            "void TestAfterNotificationUnlock(Spinlock_t*);")
        shims["spinlock.h"] = shims["spinlock.h"].replace(
            "assert(!l->held); l->held=1;",
            "TestBeforeNotificationLock(l); assert(!l->held); l->held=1;")
        shims["spinlock.h"] = shims["spinlock.h"].replace(
            "assert(l->held); l->held=0;",
            "assert(l->held); l->held=0; TestAfterNotificationUnlock(l);")
        shims["memoryspace.h"] += "\noserr_t MSContextAcquireSecurity(uint64_t, MSContextSecurity_t**);\n"
        shims["memoryspace.h"] += """
#define MEMORY_SPACE_APPLICATION 2
#define MEMORY_SPACE_INHERIT 1
typedef struct MemorySpace {
    uuid_t GroupGlobalHandleID;
    unsigned int Flags;
} MemorySpace_t;
"""
        shims["ioset.h"] = (ROOT / "librt/libc/include/ioset.h").read_text()
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
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel", "kernel/include", "librt/libos/include", "librt/libds/include", "librt/libddk/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        binary = str(output / "provider-test")
        sources = ("testing/handle_provider_test.c", "librt/libds/hashtable.c", "librt/libds/queue.c",
                   "librt/libds/list.c", "librt/libds/rbtree.c")
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", binary], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment, timeout=10)
        if args.mutation_check:
            mutations = {
                "omit pending-cleanup wakeup": (
                    "if (registration->Open && registration->Pending != NULL)",
                    "if (false)"),
                "ignore restricted access": (
                    "(accessMask & reference->AccessMask) != accessMask", "false"),
                "consume after failed count copy": (
                    "status = MemorySpaceCopyToUser(userCount, &count, sizeof(count));",
                    "(void)MemorySpaceCopyToUser(userCount, &count, sizeof(count)); status = OS_EOK;"),
                "ignore provider group exit": (
                    "if (status != OS_EOK) {\n        __CloseProviderLocked(registration);\n        return false;\n    }",
                    "if (status != OS_EOK) { return true; }"),
            }
            mutations = {
                name: ("kernel/handle_provider.c", "PROVIDER_SOURCE", before, after)
                for name, (before, after) in mutations.items()
            }
            mutations.update({
                "release a reference after failed lookup": (
                    "kernel/handle_set.c", "HANDLE_SET_SOURCE",
                    ".Reference = UUID_INVALID,", ".Reference = handle,"),
                "omit destination capability": (
                    "kernel/api/handles.c", "HANDLES_API_SOURCE",
                    "status = CapabilityVerify(CAPABILITY_MEMORYSPACE_MANAGEMENT);",
                    "status = OS_EOK;"),
                "accept ungranted destination": (
                    "kernel/api/handles.c", "HANDLES_API_SOURCE",
                    "status = AcquireHandleReference(\n            destinationMemorySpace,",
                    "status = AcquireGlobalHandle(\n            destinationMemorySpace,"),
                "check caller instead of destination recipient": (
                    "kernel/handle_grants.c", "GRANTS_SOURCE",
                    "receiver != NULL ? receiver : MemorySpaceGetSecurityContext()",
                    "MemorySpaceGetSecurityContext()"),
                "rollback in the caller instead of destination": (
                    "kernel/api/handles.c", "HANDLES_API_SOURCE",
                    "receiver != NULL ? receiver : MemorySpaceGetSecurityContext(), reference",
                    "MemorySpaceGetSecurityContext(), reference"),
                "dispatch service references by R": (
                    "kernel/handle_set.c", "HANDLE_SET_SOURCE",
                    "source->Resource = reference->Resource->ID;",
                    "source->Resource = handle;"),
                "bind service subscription to initial reference": (
                    "kernel/handle_set.c", "HANDLE_SET_SOURCE",
                    "grant = source->Registration;", "grant = source->Reference;"),
                "omit closing-grant subscription cleanup": (
                    "kernel/handle.c", "HANDLES_SOURCE",
                    "HandleSetsRemoveGrant(securityContext->HandleTable, handleId);", "(void)handleId;"),
                "omit group-exit subscription cleanup": (
                    "kernel/handle.c", "HANDLES_SOURCE",
                    "HandleSetsRemoveGrant(table, UUID_INVALID);", "(void)table;"),
                "leave removed event counted": (
                    "kernel/handle_set.c", "HANDLE_SET_SOURCE",
                    "atomic_fetch_sub(&element->set->EventsPending, 1);", "(void)element;"),
                "allow asynchronous wait to fork twice": (
                    "kernel/handle_set.c", "HANDLE_SET_SOURCE",
                    "asyncContext = NULL;", "(void)asyncContext;"),
            })
            for name, (path, define, before, after) in mutations.items():
                source = (ROOT / path).read_text()
                assert source.count(before) == 1, name
                mutant = output / "provider-mutant.c"
                mutant.write_text(source.replace(before, after))
                command = flags + ['-D' + define + '="' + str(mutant) + '"']
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
