"""Run process startup RPC boundaries against production code on the host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-process-startup-") as directory:
        output = Path(directory)
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/process.gr"), "--out", directory,
                        "--lang-c", "--client", "--server"], check=True)
        (output / "internal").mkdir()
        (output / "internal/_io.h").write_text("""
#pragma once
#include <stdio.h>
#include <unistd.h>
#include <os/types/process.h>
struct InheritanceOptions { unsigned int Flags; int StdOutHandle; int StdInHandle; int StdErrHandle; };
typedef struct StdioDescriptor stdio_handle_t;
#define __IO_PRIORITY 0x8000
stdio_handle_t* stdio_handle_get(int);
void stdio_handle_set_flags(stdio_handle_t*, unsigned int);
struct inheritance_export { uuid_t* controls; uint32_t count; uint32_t length; };
struct sys_process_inheritance_input;
oserr_t CRTWriteInheritanceBlock(const struct InheritanceOptions*, void*, uint32_t, struct inheritance_export*);
void CRTReleaseInheritanceBlock(struct inheritance_export*);
oserr_t CRTReadInheritanceBlock(const void*, uint32_t);

""")
        (output / "internal/_tls.h").write_text("""
#pragma once
#include <os/types/async.h>
struct thread_storage { uuid_t job_id; OSAsyncContext_t* async_context; };
typedef struct thread_storage thread_storage_t;
int __tls_initialize(struct thread_storage*, int);
struct thread_storage* __tls_current(void);
int __tls_update_environment(void);
""")
        (output / "internal/_syscalls.h").write_text("""
#include <os/types/thread.h>
#include <os/types/grants.h>
oserr_t Syscall_HandleImport(const GrantToken_t*, uuid_t, uuid_t*);
oserr_t Syscall_ThreadCreate(uintptr_t, void*, OSThreadParameters_t*, uuid_t*);
oserr_t Syscall_ThreadDetach(uuid_t);
oserr_t Syscall_ThreadSignal(uuid_t, int);
""")
        (output / "internal/_utils.h").write_text(
            (ROOT / "librt/libc/include/internal/_utils.h").read_text())
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-DMOLLENOS", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-D_In_Opt_=", "-D_Out_Opt_=", "-DTIME_MONOTONIC=TIME_UTC",
                 "-fms-extensions", "-fasm-blocks", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "services/processd/include", "librt/libos/include",
                   "librt/libddk/include", "librt/libds/include", "librt/libgracht/include")]
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        cases = {
            "server": ["testing/process_startup_server_test.c", "services/processd/startup.c",
                       "librt/libds/hashtable.c"],
            "client": ["testing/process_startup_client_test.c"],
        }
        for name, sources in cases.items():
            binary = output / name
            subprocess.run(flags + [str(ROOT / source) for source in sources] +
                           ["-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env=environment, timeout=10)

        if args.mutation_check:
            mutations = [
                ("truncate caller identity", "services/processd/startup.c",
                 "caller->principal != (uint64_t)process->load_context->SecurityGroupID",
                 "(uint32_t)caller->principal != process->load_context->SecurityGroupID"),
                ("invert output capacity", "services/processd/startup.c",
                 "process->startup_data.Length > capacity", "process->startup_data.Length < capacity"),
            ]
            for name, path, before, after in mutations:
                original = (ROOT / path).read_text()
                assert original.count(before) == 1
                mutant = output / "mutant.c"
                mutant.write_text(original.replace(before, after))
                sources = [str(ROOT / source) for source in cases["server"]]
                extra_flags = []
                if path.endswith("/process.c"):
                    extra_flags = [f'-DPROCESSD_SOURCE="{mutant}"']
                else:
                    sources[1] = str(mutant)
                binary = output / "mutant"
                subprocess.run(flags + extra_flags + sources + ["-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=10)
                if result.returncode == 0 or b"Assertion" not in result.stderr:
                    raise AssertionError(f"Mutation was not rejected by an assertion: {name}: {result}")
                print(f"mutation rejected: {name}")

            original = (ROOT / "librt/libc/os/init.c").read_text()
            for name, before, after in (
                ("run constructors before startup", "    while (!complete) {", "    while (false) {"),
                ("record identity only after adoption", "        g_processId = processID;",
                 "        if (bytesWritten >= sizeof(ProcessStartupInformation_t)) g_processId = processID;"),
                ("keep asynchronous bootstrap I/O",
                 "    __tls_current()->async_context = NULL;",
                 "    __tls_current()->async_context = asyncContext;"),
                ("skip initial thread environment refresh",
                 "!g_isPhoenix && __tls_update_environment() != 0", "false"),
                ("skip inherited descriptor adoption",
                 """    oserr = CRTReadInheritanceBlock(
        sections.Inheritance,
        sections.Header.InheritationLength
    );""", "    oserr = OS_EOK;"),
                ("accept too many libraries", "entries >= PROCESS_MAXMODULES",
                 "entries > PROCESS_MAXMODULES"),
            ):
                assert original.count(before) == 1
                mutant = output / "startup-mutant.c"
                mutant.write_text(original.replace(before, after))
                binary = output / "client-mutant"
                subprocess.run(flags + [f'-DSTARTUP_SOURCE="{mutant}"'] +
                               [str(ROOT / source) for source in cases["client"]] +
                               ["-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=10)
                if result.returncode == 0 or b"Assertion" not in result.stderr:
                    raise AssertionError("Startup mutation escaped: " + name)
                print("mutation rejected:", name)


if __name__ == "__main__":
    main()
