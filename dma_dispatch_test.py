"""Compile the real syscall table and dispatcher against a small host context."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = (ROOT / "kernel/api/entry.c").read_text()
    count = int(re.search(r"#define SYSTEM_CALL_COUNT (\d+)", source)[1])
    # Keep the actual table, bound and dispatcher. The declarations preceding
    # these pull in architecture/thread internals unrelated to dispatch routing.
    source = source[source.index("typedef size_t(*SystemCallHandlerFn)"):]
    entries = re.findall(r"DefineSyscall\((\d+), (\w+)\)", source)
    macros = (ROOT / "librt/libc/include/internal/_syscalls.h").read_text()
    numbers = {}
    for name in ("ShmAccessBegin", "ShmAccessEnd", "ResourceRelease", "ShmAccessCreate",
                 "GrantTokenCreate", "GrantTokenRevoke", "SHMAttachToken", "CreateHandle",
                 "HandleRegisterProvider", "HandleImport", "HandleValidate", "HandleReadCleanup"):
        numbers[name] = int(re.search(r"#define Syscall_" + name + r"\(.*?syscall[12345]\((\d+),", macros)[1])
    prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define _In_
#define TRACE(...) ((void)0)
#define THREADING_FORKED 1
#define OS_EINVALPARAMS 4
#define CONTEXT_SC_FUNC(c) ((c)->Index)
#define CONTEXT_SC_ARG0(c) ((c)->Arguments[0])
#define CONTEXT_SC_ARG1(c) ((c)->Arguments[1])
#define CONTEXT_SC_ARG2(c) ((c)->Arguments[2])
#define CONTEXT_SC_ARG3(c) ((c)->Arguments[3])
#define CONTEXT_SC_ARG4(c) ((c)->Arguments[4])
#define CONTEXT_SC_RET0(c) ((c)->Result)
typedef int oserr_t;
typedef struct { size_t Index, Arguments[5], Result; } Context_t;
typedef struct { int Unused; } Thread_t;
typedef struct { int ErrorCode; unsigned int NotificationHandle; } OSAsyncContext_t;
static Thread_t g_thread;
static unsigned int g_signals;
static unsigned int g_called;
static unsigned int ArchGetProcessorCoreId(void) { return 0; }
static Thread_t* ThreadCurrentForCore(unsigned int core) { return &g_thread; }
static int ThreadFlags(Thread_t* thread) { return 0; }
static OSAsyncContext_t* ThreadSyscallContext(Thread_t* thread) { assert(0); return NULL; }
static int MarkHandle(unsigned int handle, int flags) { assert(0); return 0; }
static _Noreturn void ThreadExit(int exitCode) { assert(0); for (;;) { } }
static void SignalProcessQueued(Thread_t* thread, Context_t* context) { g_signals++; }
'''
    prelude += "#define SYSTEM_CALL_COUNT " + str(count) + "\n"
    for number, name in entries:
        body = "assert(0); return 0;"
        if name == "ScShmAccessEnd":
            body = "assert((uintptr_t)a == 0x1234 && (uintptr_t)b == 2 && (uintptr_t)c == 7 && !d && !e); g_called = 4; return 123;"
        if name == "ScResourceRelease":
            body = "assert((uintptr_t)a == 0x1234 && !b && !c && !d && !e); g_called = 7; return 123;"
        if name == "ScShmAccessBegin":
            body = "assert((uintptr_t)a == 1 && (uintptr_t)b == 2 && (uintptr_t)c == 3 && !d && !e); g_called = 5; return 123;"
        if name == "ScShmAccessCreate":
            body = "assert((uintptr_t)a == 1 && (uintptr_t)b == 2 && (uintptr_t)c == 3 && (uintptr_t)d == 4 && !e); g_called = 6; return 123;"
        if name == "ScGrantTokenCreate":
            body = "assert((uintptr_t)a == 1 && (uintptr_t)b == 2 && (uintptr_t)c == 3 && !d && !e); g_called = 8; return 123;"
        if name == "ScGrantTokenRevoke":
            body = "assert((uintptr_t)a == 0x1234 && !b && !c && !d && !e); g_called = 9; return 123;"
        if name == "ScSHMAttachToken":
            body = "assert((uintptr_t)a == 1 && (uintptr_t)b == 2 && !c && !d && !e); g_called = 10; return 123;"
        if name in ("ScCreateHandle", "ScHandleRegisterProvider", "ScHandleImport",
                    "ScHandleValidate", "ScHandleProviderGetEvents"):
            body = "assert((uintptr_t)a == 1 && (uintptr_t)b == 2 && (uintptr_t)c == 3 && (uintptr_t)d == 4 && (uintptr_t)e == 5); g_called = " + number + "; return 123;"
        prelude += ("static size_t " + name + "(void* a, void* b, void* c, void* d, void* e) { " + body + " }\n")
    check = r'''
int main(void)
{
    Context_t context = { .Arguments = { 0x1234, 2, 7 } };
    size_t i;
    assert(SYSTEM_CALL_COUNT == 91);
    for (i = 0; i < SYSTEM_CALL_COUNT; i++) {
        assert(g_systemCallsTable[i].Index == (int)i);
    }
    for (i = 78; i < SYSTEM_CALL_COUNT; i++) {
        assert(g_systemCallsTable[i].HandlerAddress != 0);
    }
    context.Index = END_NUMBER;
    assert(SyscallHandle(&context) == &context && context.Result == 123);
    assert(g_called == 4 && g_signals == 1);
    context.Arguments[3] = 0;
    context.Index = WAIT_NUMBER;
    context.Arguments[0] = 1;
    context.Arguments[1] = 2;
    context.Arguments[2] = 3;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 5);
    context.Index = OPEN_NUMBER;
    context.Arguments[3] = 4;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 6);
    context.Index = RELEASE_NUMBER;
    context.Arguments[0] = 0x1234;
    context.Arguments[1] = 0;
    context.Arguments[2] = 0;
    context.Arguments[3] = 0;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 7);
    context.Index = GRANT_CREATE_NUMBER;
    context.Arguments[0] = 1;
    context.Arguments[1] = 2;
    context.Arguments[2] = 3;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 8);
    context.Index = GRANT_REVOKE_NUMBER;
    context.Arguments[0] = 0x1234;
    context.Arguments[1] = 0;
    context.Arguments[2] = 0;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 9);
    context.Index = ATTACH_TOKEN_NUMBER;
    context.Arguments[0] = 1;
    context.Arguments[1] = 2;
    assert(SyscallHandle(&context) == &context && context.Result == 123 && g_called == 10);
    context.Arguments[0] = 1;
    context.Arguments[1] = 2;
    context.Arguments[2] = 3;
    context.Arguments[3] = 4;
    context.Arguments[4] = 5;
    for (i = 0; i < 5; i++) {
        static const size_t calls[] = { PROVIDER_CALLS };
        context.Index = calls[i];
        assert(SyscallHandle(&context) == &context && context.Result == 123);
        assert(g_called == calls[i]);
    }
    // Trailing slots must fail before looking up a descriptor.
    for (i = SYSTEM_CALL_COUNT; i <= SYSTEM_CALL_COUNT + 3; i++) {
        context.Index = i;
        assert(SyscallHandle(&context) == &context && context.Result == OS_EINVALPARAMS);
    }
    context.Index = SYSTEM_CALL_COUNT;
    assert(SyscallHandle(&context) == &context && context.Result == OS_EINVALPARAMS);
    context.Index = SIZE_MAX;
    assert(SyscallHandle(&context) == &context && context.Result == OS_EINVALPARAMS);
    assert(g_signals == 12);
    puts("DMA/grant dispatch: runtime syscall numbers, table bounds and actual handler routing passed");
    return 0;
}
'''
    check = check.replace("WAIT_NUMBER", str(numbers["ShmAccessBegin"]))
    check = check.replace("OPEN_NUMBER", str(numbers["ShmAccessCreate"]))
    check = check.replace("END_NUMBER", str(numbers["ShmAccessEnd"]))
    check = check.replace("RELEASE_NUMBER", str(numbers["ResourceRelease"]))
    check = check.replace("GRANT_CREATE_NUMBER", str(numbers["GrantTokenCreate"]))
    check = check.replace("GRANT_REVOKE_NUMBER", str(numbers["GrantTokenRevoke"]))
    check = check.replace("ATTACH_TOKEN_NUMBER", str(numbers["SHMAttachToken"]))
    check = check.replace("PROVIDER_CALLS", ", ".join(str(numbers[name]) for name in
                          ("CreateHandle", "HandleRegisterProvider", "HandleImport",
                           "HandleValidate", "HandleReadCleanup")))
    with tempfile.TemporaryDirectory(prefix="vali-dma-dispatch-") as directory:
        path = Path(directory)
        (path / "dispatch.c").write_text(prelude + source + check)
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                        str(path / "dispatch.c"), "-o", str(path / "dispatch")], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(path / "dispatch")], check=True, env=environment)


if __name__ == "__main__":
    main()
