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
    # Keep these ABI expectations independent of the generated table. New calls
    # may extend the table without changing existing numbers or argument counts.
    calls = (
        ("ShmAccessBegin", "ScShmAccessBegin", 88, 3),
        ("ShmAccessEnd", "ScShmAccessEnd", 89, 3),
        ("ResourceRelease", "ScResourceRelease", 90, 1),
        ("ShmAccessCreate", "ScShmAccessCreate", 87, 4),
        ("GrantTokenCreate", "ScGrantTokenCreate", 70, 3),
        ("GrantTokenRevoke", "ScGrantTokenRevoke", 71, 1),
        ("SHMAttachToken", "ScSHMAttachToken", 55, 2),
        ("HandleCreate", "ScCreateHandle", 62, 3),
        ("HandleProviderRegister", "ScHandleRegisterProvider", 67, 2),
        ("HandleImport", "ScHandleImport", 63, 3),
        ("HandleValidate", "ScHandleValidate", 68, 5),
        ("HandleProviderGetEvents", "ScHandleProviderGetEvents", 69, 4),
        ("DmaManagerCreate", "ScDmaManagerCreate", 95, 1),
        ("DmaAttachmentRegister", "ScDmaAttachmentRegister", 96, 3),
        ("DmaManagerReadEvents", "ScDmaManagerReadEvents", 97, 4),
        ("DmaAttachmentRemove", "ScDmaAttachmentRemove", 98, 2),
        ("DmaAttachmentBind", "ScDmaAttachmentBind", 99, 3),
        ("DmaOpenBound", "ScDmaOpenBound", 100, 3),
        ("DmaAttachmentPrepareStop", "ScDmaAttachmentPrepareStop", 101, 3),
        ("DmaAttachmentCompleteStop", "ScDmaAttachmentCompleteStop", 102, 2),
    )
    for wrapper, handler, number, arity in calls:
        match = re.search(r"#define Syscall_" + wrapper + r"\(.*?syscall([1-5])\((\d+),", macros)
        assert match is not None, f"Missing wrapper: {wrapper}"
        assert (int(match[1]), int(match[2])) == (arity, number), wrapper
        assert (str(number), handler) in entries, handler
    assert [int(number) for number, _ in entries] == list(range(count))
    prelude = r'''
#include <stdint.h>
#include <limits.h>
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
typedef unsigned int uuid_t;
typedef struct { size_t Index, Arguments[5], Result; } Context_t;
typedef struct { int Unused; } Thread_t;
typedef struct { int ErrorCode; unsigned int NotificationHandle; } OSAsyncContext_t;
static Thread_t g_thread;
static unsigned int g_signals;
static unsigned int g_called;
static size_t g_expectedArguments[5];
static unsigned int ArchGetProcessorCoreId(void) { return 0; }
static Thread_t* ThreadCurrentForCore(unsigned int core) { return &g_thread; }
static int ThreadFlags(Thread_t* thread) { return 0; }
static OSAsyncContext_t* ThreadSyscallContext(Thread_t* thread) { assert(0); return NULL; }
static int MarkHandle(unsigned int handle, int flags) { assert(0); return 0; }
static _Noreturn void ThreadExit(int exitCode) { assert(0); for (;;) { } }
static void SignalProcessQueued(Thread_t* thread, Context_t* context) { g_signals++; }
'''
    prelude += "#define SYSTEM_CALL_COUNT " + str(count) + "\n"
    expected_handlers = {handler for _, handler, _, _ in calls}
    for number, name in entries:
        body = "assert(0); return 0;"
        if name in expected_handlers:
            body = (
                "assert((uintptr_t)a == g_expectedArguments[0]); "
                "assert((uintptr_t)b == g_expectedArguments[1]); "
                "assert((uintptr_t)c == g_expectedArguments[2]); "
                "assert((uintptr_t)d == g_expectedArguments[3]); "
                "assert((uintptr_t)e == g_expectedArguments[4]); "
                f"g_called = {number}; return 123;"
            )
        prelude += f"static size_t {name}(void* a, void* b, void* c, void* d, void* e) {{ {body} }}\n"
    cases = ",\n".join(
        "    { " + str(number) + ", { "
        + ", ".join(str(i + 1) if i < arity else "0" for i in range(5)) + " } }"
        for _, _, number, arity in calls
    )
    check = r'''
static const struct { size_t Number, Arguments[5]; } g_cases[] = {
TEST_CASES
};
int main(void)
{
    Context_t context = { 0 };
    size_t i;
    size_t argument;
    size_t caseCount = sizeof(g_cases) / sizeof(g_cases[0]);

    for (i = 0; i < SYSTEM_CALL_COUNT; i++) {
        assert(g_systemCallsTable[i].Index == (int)i);
        assert(g_systemCallsTable[i].HandlerAddress != 0);
    }
    for (i = 0; i < caseCount; i++) {
        context.Index = g_cases[i].Number;
        context.Result = 0;
        g_called = UINT_MAX;
        for (argument = 0; argument < 5; argument++) {
            context.Arguments[argument] = g_cases[i].Arguments[argument];
            g_expectedArguments[argument] = g_cases[i].Arguments[argument];
        }
        assert(SyscallHandle(&context) == &context && context.Result == 123);
        assert(g_called == g_cases[i].Number && g_signals == i + 1);
    }
    // Trailing slots must fail before looking up a descriptor.
    g_called = UINT_MAX;
    for (i = SYSTEM_CALL_COUNT; i <= SYSTEM_CALL_COUNT + 3; i++) {
        context.Index = i;
        assert(SyscallHandle(&context) == &context && context.Result == OS_EINVALPARAMS);
    }
    context.Index = SIZE_MAX;
    assert(SyscallHandle(&context) == &context && context.Result == OS_EINVALPARAMS);
    assert(g_called == UINT_MAX && g_signals == caseCount);
    puts("DMA/grant dispatch: stable ABI numbers, table bounds and handler routing passed");
    return 0;
}
'''.replace("TEST_CASES", cases)
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
