"""Build the real handle table and containers against deterministic kernel stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIMS = {
    "ddk/io.h": "#define READ_VOLATILE(x) (*(volatile __typeof__(x)*)&(x))\n#define WRITE_VOLATILE(x,v) (*(volatile __typeof__(x)*)&(x) = (v))\n",
    "debug.h": "#define ERROR(...) ((void)0)\n#define DEBUG(...) ((void)0)\n#define TRACE(...) ((void)0)\n",
    "heap.h": "#include <stddef.h>\nvoid* kmalloc(size_t);\nvoid kfree(void*);\n",
    "spinlock.h": """#pragma once
#include <assert.h>
typedef struct { int held; } Spinlock_t;
#define OS_SPINLOCK_INIT {0}
static inline void SpinlockConstruct(Spinlock_t* l) { l->held=0; }
static inline void SpinlockAcquireIrq(Spinlock_t* l) { assert(!l->held); l->held=1; }
static inline void SpinlockReleaseIrq(Spinlock_t* l) { assert(l->held); l->held=0; }
""",
    "threading.h": """#include <spinlock.h>
#include <os/osdefs.h>
typedef int Semaphore_t;
#define SEMAPHORE_INIT(a,b) 0
static inline void SemaphoreSignal(Semaphore_t* s, int n) { (void)s; (void)n; }
static inline void SemaphoreWait(Semaphore_t* s, void* t) { (void)s; (void)t; }
static inline oserr_t ThreadCreate(const char* n, void(*f)(void*), void* a, unsigned b,
    uuid_t c, size_t d, size_t e, uuid_t* out) {
    (void)n;(void)f;(void)a;(void)b;(void)c;(void)d;(void)e;(void)out; return OS_ENOTSUPPORTED;
}
""",
    "ds/mstring.h": """#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef char mstring_t;
static inline mstring_t* mstr_new_u8(const char* s) { return strdup(s); }
static inline size_t mstr_len(mstring_t* s) { return s ? strlen(s) : 0; }
static inline void mstr_delete(mstring_t* s) { free(s); }
static inline int mstr_cmp(mstring_t* a, mstring_t* b) { return strcmp(a,b); }
static inline uint64_t mstr_hash(mstring_t* s) { uint64_t h=0; while(*s) h=h*31+(unsigned char)*s++; return h; }
""",
}


def main():
    with tempfile.TemporaryDirectory(prefix="vali-handle-test-") as directory:
        output = Path(directory)
        for name, source in SHIMS.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        command = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                   "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                   "-I" + str(output)]
        command += ["-I" + str(ROOT / path) for path in ("testing/include", "kernel/include",
                    "librt/libos/include", "librt/libds/include", "librt/libddk/include")]
        command += [str(ROOT / path) for path in ("testing/handle_lifetime_test.c",
                    "librt/libds/hashtable.c", "librt/libds/queue.c")]
        binary = str(output / "handle-test")
        subprocess.run(command + ["-o", binary], check=True)
        # This sandbox cannot run LeakSanitizer; the test counts live records.
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([binary], check=True, env=environment)


if __name__ == "__main__":
    main()
