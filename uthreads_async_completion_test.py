"""Check the real syscall completion path's publication and context lifetime."""
import argparse
import os
from pathlib import Path
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#define _In_
#define TRACE(...) ((void)0)
#define THREADING_FORKED 1
#define OS_EINVALPARAMS 4
#define SYSTEM_CALL_COUNT 1
#define CONTEXT_SC_FUNC(c) 0
#define CONTEXT_SC_ARG0(c) 0
#define CONTEXT_SC_ARG1(c) 0
#define CONTEXT_SC_ARG2(c) 0
#define CONTEXT_SC_ARG3(c) 0
#define CONTEXT_SC_ARG4(c) 0
#define CONTEXT_SC_RET0(c) ((c)->Result)
typedef int oserr_t;
typedef unsigned int uuid_t;
typedef struct { size_t Result; } Context_t;
typedef struct { int Unused; } Thread_t;
typedef struct { uuid_t NotificationHandle; oserr_t ErrorCode; } OSAsyncContext_t;
typedef size_t (*SystemCallHandlerFn)(void*, void*, void*, void*, void*);
struct SystemCallDescriptor { void* HandlerAddress; const char* Name; };
static jmp_buf g_exit;
static Thread_t g_thread;
static OSAsyncContext_t* g_async;
static size_t g_pageSize;
static unsigned int g_posts;
static size_t __Handler(void* a, void* b, void* c, void* d, void* e) { return 37; }
static struct SystemCallDescriptor g_systemCallsTable[] = { { __Handler, "test" } };
static unsigned int ArchGetProcessorCoreId(void) { return 0; }
static Thread_t* ThreadCurrentForCore(unsigned int core) { return &g_thread; }
static int ThreadFlags(Thread_t* thread) { return THREADING_FORKED; }
static OSAsyncContext_t* ThreadSyscallContext(Thread_t* thread) { return g_async; }
static int MarkHandle(uuid_t handle, int flags) {
    assert(handle == 123 && flags == 0x8);
    g_posts++;
    return 0;
}
static _Noreturn void ThreadExit(int status) { assert(status == 0); longjmp(g_exit, 1); }
static void SignalProcessQueued(Thread_t* thread, Context_t* context) { abort(); }
static void __Publish(oserr_t* result, oserr_t value, int order) {
    assert(result == &g_async->ErrorCode && order == __ATOMIC_RELEASE);
    __atomic_store_n(result, value, __ATOMIC_RELEASE);
    assert(__atomic_load_n(result, __ATOMIC_ACQUIRE) == 37);
    // Model a resumed job immediately releasing its context. A later read by
    // the kernel completion path now faults instead of reading stale memory.
    assert(mprotect(g_async, g_pageSize, PROT_NONE) == 0);
}
#define __atomic_store_n(address, value, order) __Publish(address, value, order)
'''

MAIN = r'''
int main(void) {
    Context_t context = { 0 };
    g_pageSize = (size_t)sysconf(_SC_PAGESIZE);
    g_async = mmap(NULL, g_pageSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(g_async != MAP_FAILED);
    g_async->NotificationHandle = 123;
    g_async->ErrorCode = -1;
    if (setjmp(g_exit) == 0) {
        SyscallHandle(&context);
        abort();
    }
    assert(g_posts == 1);
    assert(munmap(g_async, g_pageSize) == 0);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutation-check', action='store_true')
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    source = (ROOT / 'kernel/api/entry.c').read_text()
    source = source[source.index('Context_t*\nSyscallHandle('):]
    cases = {'normal': source}
    if args.mutation_check:
        before = '(void)MarkHandle(notificationHandle, 0x8);'
        assert source.count(before) == 1
        cases['late-context-read'] = source.replace(
            before, '(void)MarkHandle(asyncContext->NotificationHandle, 0x8);')
        assert source.count('__ATOMIC_RELEASE') == 1
        cases['missing-release'] = source.replace('__ATOMIC_RELEASE', '__ATOMIC_RELAXED')
    with tempfile.TemporaryDirectory(prefix='vali-async-completion-') as directory:
        out = Path(directory)
        for name, body in cases.items():
            path = out / (name + '.c')
            binary = out / name
            path.write_text(PRELUDE + body + MAIN)
            subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-Wno-unused-but-set-variable', str(path), '-o', str(binary)],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, timeout=10)
            if (result.returncode == 0) != (name == 'normal'):
                raise RuntimeError(f'{name}: unexpected exit {result.returncode}\n{result.stderr.decode()}')
            print(name + ': ' + ('PASS' if name == 'normal' else 'REJECTED'))


if __name__ == '__main__':
    main()
